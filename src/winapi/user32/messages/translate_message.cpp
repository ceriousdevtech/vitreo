// SPDX-License-Identifier: LGPL-2.1-only

/// @file message_loop.cpp
/// @brief GetMessageW/PeekMessageW/DispatchMessageW/TranslateMessage (WIN32-053)

#include "message_loop.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/user32/messages/message_time.hpp"
#include "winapi/user32/messages/mouse_in_pointer.hpp"
#include <vitreo/virtual_tsc.hpp>
#include "winapi/user32/painting/painting.hpp"
#include "winapi/user32/user32_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "core/memory/guest_memory.hpp"
#include <cstdlib>
#include <chrono>
#include <cstdio>    // fprintf
#include <thread>    // this_thread::sleep_for
#include <unistd.h>
#include <vector>

/// @file translate_message.cpp
/// @brief translate_message
///
/// Closed cut from message_loop.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// -----------------------------------------------------------------------------
// translate_message
// -----------------------------------------------------------------------------

thread_local u16 MessageLoopManager::t_last_packet_char = 0;
thread_local u64 MessageLoopManager::t_message_extra_info = 0;

void MessageLoopManager::finish_retrieved_message(windowing::MSG& msg, bool removed) noexcept {
    stamp_message_time(msg);
    // Every retrieved message -- PM_NOREMOVE included, posted ones carrying
    // 0 -- becomes the thread's GetMessageExtraInfo value; an empty peek or
    // a same-thread send leaves it (s2msg_extrainfo_probe).
    t_message_extra_info = msg.extra_info;
    // The guest gets MSG back through its own memory, which has no room for
    // a VK_PACKET's character: keep it for this thread's TranslateMessage.
    constexpr u64 VK_PACKET = 0xE7;
    if (removed && msg.wParam == VK_PACKET &&
        (msg.message == windowing::WM::KEYDOWN || msg.message == windowing::WM::SYSKEYDOWN)) {
        t_last_packet_char = msg.packet_char;
    }
    // GetKeyboardState advances only when input leaves the queue. A
    // PM_NOREMOVE inspection still records GetMessageTime, but must not
    // release modifiers early or toggle lock keys repeatedly.
    if (removed) {
        note_retrieved_message(msg);
    } else if (m_user32_ext_mgr != nullptr) {
        m_user32_ext_mgr->note_retrieved_queue_message(msg, false);
    }
    (void)mouse_in_pointer::convert(
        msg, ThreadManager::get_current_thread_id_static(), virtual_qpc_ticks());
}

void MessageLoopManager::note_retrieved_message(const windowing::MSG& msg) noexcept {
    if (m_user32_ext_mgr != nullptr) {
        m_user32_ext_mgr->note_retrieved_queue_message(msg);
    }

    const bool down = msg.message == windowing::WM::KEYDOWN ||
                      msg.message == windowing::WM::SYSKEYDOWN;
    const bool up   = msg.message == windowing::WM::KEYUP ||
                      msg.message == windowing::WM::SYSKEYUP;
    if (!down && !up) return;

    u32 bit = 0;
    switch (static_cast<u32>(msg.wParam)) {
    case 0x10: bit = 1u << 0; break;  // VK_SHIFT
    case 0x11: bit = 1u << 1; break;  // VK_CONTROL
    case 0x12: bit = 1u << 2; break;  // VK_MENU
    default: return;
    }
    if (down) {
        m_sync_modifiers |= bit;
    } else {
        m_sync_modifiers &= ~bit;
    }
}

bool MessageLoopManager::sync_modifier_down(u32 vk) const noexcept {
    switch (vk) {
    case 0x10: return (m_sync_modifiers & (1u << 0)) != 0;
    case 0x11: return (m_sync_modifiers & (1u << 1)) != 0;
    case 0x12: return (m_sync_modifiers & (1u << 2)) != 0;
    default:   return false;
    }
}

bool MessageLoopManager::translate_message(const windowing::MSG& msg) {
    // Learn TranslateMessage: "If the message is WM_KEYDOWN, WM_KEYUP,
    // WM_SYSKEYDOWN, or WM_SYSKEYUP, the return value is nonzero, regardless
    // of the translation"; anything else is zero (s2msg_input_probe
    // translate_keyup_a / translate_keydown_f1 ret=1, translate_char_msg 0).
    // A key-down posts the characters ToUnicode gives the key under the
    // THREAD's keyboard state -- WM_CHAR, or WM_SYSCHAR for WM_SYSKEYDOWN,
    // WM_DEADCHAR / WM_SYSDEADCHAR for a dead key -- with the key's lParam.
    const u32 id = msg.message;
    if (id != windowing::WM::KEYDOWN && id != windowing::WM::KEYUP &&
        id != windowing::WM::SYSKEYDOWN && id != windowing::WM::SYSKEYUP) {
        return false;
    }
    if (id == windowing::WM::KEYUP || id == windowing::WM::SYSKEYUP) return true;

    std::array<u8, 256> key_state{};
    if (m_user32_ext_mgr != nullptr) {
        key_state = m_user32_ext_mgr->thread_keyboard_state();
    } else {
        // No USER32 state manager (unit tests): the modifiers this loop saw
        // retrieved, and the host's Caps Lock.
        if (sync_modifier_down(0x10)) key_state[0x10] = 0x80;
        if (sync_modifier_down(0x11)) key_state[0x11] = 0x80;
        if (sync_modifier_down(0x12)) key_state[0x12] = 0x80;
        if ((m_bridge.get_native_key_state(0x14) & 0x0001) != 0) key_state[0x14] = 0x01;
    }
    const u32 vk = static_cast<u32>(msg.wParam);
    const u32 scan = static_cast<u32>((msg.lParam >> 16) & 0xFFu);
    // VK_PACKET carries its character outside the MSG (SendInput
    // KEYEVENTF_UNICODE); a VK_PACKET that did not come from there types
    // nothing (s2msg_input_probe translate_packet: no WM_CHAR).
    constexpr u32 VK_PACKET = 0xE7;
    if (vk == VK_PACKET) {
        const u16 packet_char = t_last_packet_char;
        t_last_packet_char = 0;
        if (packet_char == 0) return true;
        windowing::MSG out{};
        out.hwnd = msg.hwnd;
        out.message = id == windowing::WM::SYSKEYDOWN ? windowing::WM::SYSCHAR
                                                      : windowing::WM::CHAR_MSG;
        out.wParam = packet_char;
        out.lParam = msg.lParam;
        out.time = msg.time;
        out.pt = msg.pt;
        out.target_thread_id = ThreadManager::get_current_thread_id_static();
        m_queue.post(out);
        return true;
    }
    u16 units[4] = {};
    const i32 result = to_unicode_us(vk, scan, key_state.data(), units, 4, 0);
    if (result == 0) {
        VITREO_TRACE("[msgloop] TranslateMessage: VK 0x%x -> no character\n", vk);
        return true;
    }
    const bool system = id == windowing::WM::SYSKEYDOWN;
    const bool dead = result < 0;
    constexpr u32 WM_DEADCHAR = 0x0103;
    constexpr u32 WM_SYSDEADCHAR = 0x0107;
    const u32 char_msg = dead ? (system ? WM_SYSDEADCHAR : WM_DEADCHAR)
                              : (system ? windowing::WM::SYSCHAR
                                        : windowing::WM::CHAR_MSG);
    const i32 count = dead ? 1 : result;
    for (i32 i = 0; i < count && i < 4; ++i) {
        windowing::MSG out{};
        out.hwnd = msg.hwnd;
        out.message = char_msg;
        out.wParam = units[i];
        out.lParam = msg.lParam;
        out.time = msg.time;
        out.pt = msg.pt;
        // Posted to the thread that owns the window (Learn: "posted to the
        // calling thread's message queue").
        out.target_thread_id = ThreadManager::get_current_thread_id_static();
        m_queue.post(out);
    }
    VITREO_TRACE("[msgloop] TranslateMessage: VK 0x%x -> %d unit(s) U+%04x msg=0x%x\n",
                 vk, result, units[0], char_msg);
    if (std::getenv("VITREO_FOCUS_DIAG")) {
        std::fprintf(stderr,
                     "[input] TranslateMessage hwnd=0x%x vk=0x%x -> 0x%x "
                     "U+%04x\n",
                     msg.hwnd, vk, char_msg, units[0]);
    }
    return true;
}

} // namespace vitreo::winapi
