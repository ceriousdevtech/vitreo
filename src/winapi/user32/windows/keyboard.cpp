// SPDX-License-Identifier: LGPL-2.1-only

/// @file keyboard.cpp
/// @brief Keyboard state, layout, focus, and virtual-key translation.
///
/// Get/SetKeyboardState, MapVirtualKey, ToUnicodeEx, GetAsyncKeyState,
/// GetFocus, and the thread-key-state bookkeeping that message retrieval
/// updates. Pure Win32 keyboard model; Cocoa maps host keys elsewhere.
///
/// Closed cut from user32_ext.cpp: nothing these functions define is used
/// elsewhere in that file, and they use nothing file-local from elsewhere —
/// verified in both directions with tools/architecture/plan_extraction.py
/// before moving.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/windows/kbd_layout.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "winapi/user32/messages/message_loop.hpp"
#include "winapi/user32/messages/message_time.hpp"
#include "winapi/user32/messages/message_send.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "windowing/model/diag_flags.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/hwnd_registry.hpp"
#include "windowing/resources/dialog_template.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "winapi/user32/classes/window_class.hpp"
#include "winapi/user32/windows/window.hpp"
#include "winapi/user32/dialogs/dialog.hpp"
#include "winapi/user32/windows/show_window.hpp"
#include "winapi/user32/resources/resources.hpp"
#include "core/pe/parser/pe_structures.hpp"
#include "vitreo/result.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi {

// ----- Keyboard/Input APIs -----

namespace {

std::array<u8, 256> native_keyboard_state(
    windowing::User32Host& host) {
    std::array<u8, 256> result{};
    for (u32 vk = 0; vk < result.size(); ++vk) {
        const u16 state = static_cast<u16>(host.get_native_key_state(vk));
        result[vk] = static_cast<u8>(
            ((state & 0x8000u) != 0 ? 0x80u : 0u) |
            ((state & 0x0001u) != 0 ? 0x01u : 0u));
    }
    return result;
}

} // namespace

void User32ExtManager::impl_GetKeyboardState(Marshaler& m) {
    const GuestAddr lpKeyState = m.arg_ptr(0);
    if (lpKeyState == 0) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0u);
        return;
    }

    const u32 thread_id = ThreadManager::get_current_thread_id_static();
    std::array<u8, 256> state{};
    {
        std::lock_guard lock(m_keyboard_state_mutex);
        auto [it, inserted] = m_keyboard_states.try_emplace(thread_id);
        if (inserted) {
            it->second = native_keyboard_state(m_host);
        }
        state = it->second;
    }

    if (m.memory().write_bytes(
            lpKeyState, reinterpret_cast<const Byte*>(state.data()),
            state.size()).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        VITREO_TRACE("[u32ext] GetKeyboardState ptr=0x%llx -> FALSE "
                     "gle=%u\n",
                     static_cast<unsigned long long>(lpKeyState),
                     error_codes::ERROR_NOACCESS);
        m.set_return(0u);
        return;
    }

    VITREO_TRACE("[u32ext] GetKeyboardState thread=%u -> TRUE\n", thread_id);
    m.set_return(1u);
}

void User32ExtManager::impl_SetKeyboardState(Marshaler& m) {
    const GuestAddr lpKeyState = m.arg_ptr(0);
    std::array<u8, 256> state{};
    if (lpKeyState == 0 ||
        m.memory().read_bytes(
            lpKeyState, reinterpret_cast<Byte*>(state.data()),
            state.size()).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        VITREO_TRACE("[u32ext] SetKeyboardState ptr=0x%llx -> FALSE "
                     "gle=%u\n",
                     static_cast<unsigned long long>(lpKeyState),
                     error_codes::ERROR_NOACCESS);
        m.set_return(0u);
        return;
    }

    const u32 thread_id = ThreadManager::get_current_thread_id_static();
    {
        std::lock_guard lock(m_keyboard_state_mutex);
        m_keyboard_states[thread_id] = state;
    }
    VITREO_TRACE("[u32ext] SetKeyboardState thread=%u -> TRUE\n", thread_id);
    m.set_return(1u);
}

i16 User32ExtManager::thread_key_state(u32 virtual_key) {
    if (virtual_key >= 256) return 0;

    const u32 thread_id = ThreadManager::get_current_thread_id_static();
    std::lock_guard lock(m_keyboard_state_mutex);
    auto [it, inserted] = m_keyboard_states.try_emplace(thread_id);
    if (inserted) {
        it->second = native_keyboard_state(m_host);
    }
    // GetKeyState returns the state byte sign-extended to a SHORT: a key
    // held down (0x80) reads 0xFF80, not 0x8000 (s2msg_input_probe
    // getkeystate_A_down). Only bits 7 and 0 are defined.
    const u8 value = it->second[virtual_key] & 0x81u;
    return static_cast<i16>(static_cast<i8>(value));
}

std::array<u8, 256> User32ExtManager::thread_keyboard_state() {
    const u32 thread_id = ThreadManager::get_current_thread_id_static();
    std::lock_guard lock(m_keyboard_state_mutex);
    auto [it, inserted] = m_keyboard_states.try_emplace(thread_id);
    if (inserted) {
        it->second = native_keyboard_state(m_host);
    }
    return it->second;
}

void User32ExtManager::note_retrieved_queue_message(
    const windowing::MSG& message, bool removed) noexcept {
    if (removed) {
        note_retrieved_keyboard_message(message);
        note_removed_raw_input_message(message);
    }
    const u32 thread_id = ThreadManager::get_current_thread_id_static();
    {
        std::lock_guard lock(m_message_time_mutex);
        m_last_message_time_by_thread[thread_id] = message.time;
        m_last_message_pos_by_thread[thread_id] =
            (static_cast<u32>(message.pt.y) & 0xffffu) << 16 |
            (static_cast<u32>(message.pt.x) & 0xffffu);
    }
    timer_diag("[timer-diag] GetMessageTime tid=%u value=%u gettick=%u\n",
               thread_id, message.time, tick_count32());
}

u32 User32ExtManager::last_message_time() const noexcept {
    const u32 thread_id = ThreadManager::get_current_thread_id_static();
    std::lock_guard lock(m_message_time_mutex);
    const auto it = m_last_message_time_by_thread.find(thread_id);
    return it == m_last_message_time_by_thread.end() ? 0u : it->second;
}

u32 User32ExtManager::last_message_pos() const noexcept {
    const u32 thread_id = ThreadManager::get_current_thread_id_static();
    std::lock_guard lock(m_message_time_mutex);
    const auto it = m_last_message_pos_by_thread.find(thread_id);
    return it == m_last_message_pos_by_thread.end() ? 0u : it->second;
}

void User32ExtManager::note_retrieved_keyboard_message(
    const windowing::MSG& message) noexcept {
    const bool down = message.message == windowing::WM::KEYDOWN ||
                      message.message == windowing::WM::SYSKEYDOWN;
    const bool up = message.message == windowing::WM::KEYUP ||
                    message.message == windowing::WM::SYSKEYUP;
    const u32 virtual_key = static_cast<u32>(message.wParam);
    if ((!down && !up) || virtual_key >= 256) return;

    const u32 thread_id = ThreadManager::get_current_thread_id_static();
    std::lock_guard lock(m_keyboard_state_mutex);
    auto [it, inserted] = m_keyboard_states.try_emplace(thread_id);
    if (inserted) {
        it->second = native_keyboard_state(m_host);
    }

    u8& value = it->second[virtual_key];
    if (down) {
        value |= 0x80u;
        const bool first_press = (message.lParam & (1ull << 30)) == 0;
        if (first_press &&
            (virtual_key == 0x14 || virtual_key == 0x90 ||
             virtual_key == 0x91)) {
            value ^= 0x01u;
        }
    } else {
        value &= static_cast<u8>(~0x80u);
    }
}

void User32ExtManager::impl_GetKeyboardLayout(Marshaler& m) {
    u32 threadId = static_cast<u32>(m.arg(0));
    VITREO_TRACE("[u32ext] GetKeyboardLayout thread=%u -> 0x04090409\n", threadId);
    m.set_return(0x04090409u);
}

i32 to_unicode_us(u32 vk, u32 scan_code, const u8* key_state, u16* out,
                  i32 capacity, u32 flags) noexcept {
    // Learn ToUnicodeEx: a set high bit of wScanCode is a key break, which
    // translates to nothing (s2msg_input_probe tounicode_keyup_bit = 0)
    // unless bit 1 of wFlags asks for break translation.
    if ((scan_code & 0x8000u) != 0 && (flags & 0x2u) == 0) return 0;
    const kbd_us::ToUnicodeRow* row =
        kbd_us::to_unicode(vk, kbd_us::modifiers(key_state));
    if (row == nullptr) return 0;
    const i32 units = row->ret < 0 ? 1 : row->ret;
    for (i32 i = 0; i < units && i < capacity && i < 4; ++i) out[i] = row->units[i];
    return row->ret;
}

namespace {

/// ToUnicode / ToUnicodeEx share everything but the HKL, which selects
/// nothing: Vitreo has the one layout it reports.
void to_unicode_thunk(Marshaler& m, const char* name) {
    const u32 vk = static_cast<u32>(m.arg(0));
    const u32 scan = static_cast<u32>(m.arg(1));
    const GuestAddr key_state_ptr = m.arg_ptr(2);
    const GuestAddr buff = m.arg_ptr(3);
    const i32 cch = static_cast<i32>(m.arg32(4));
    const u32 flags = m.arg32(5);
    // s2msg_input_probe tounicode_zero_buffer: 0 / ERROR_INVALID_PARAMETER.
    if (buff == 0 || cch <= 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    std::array<u8, 256> key_state{};
    if (key_state_ptr == 0 ||
        m.memory().read_bytes(key_state_ptr, key_state.data(),
                              key_state.size()).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    u16 units[4] = {};
    const i32 result = to_unicode_us(vk, scan, key_state.data(), units, cch, flags);
    const i32 produced = result < 0 ? 1 : result;
    for (i32 i = 0; i < produced && i < cch; ++i) {
        (void)m.write_u16(buff + static_cast<GuestAddr>(i) * 2, units[i]);
    }
    // ToUnicodeEx is called once per virtual key while a layout is built.
    // Logging that under VITREO_FOCUS_DIAG floods I/O; keep it on
    // VITREO_INPUT_DIAG only.
    if (windowing::diag::input_diag()) {
        std::fprintf(stderr, "[input] %s vk=0x%x -> %d U+%04x\n", name, vk,
                     result, produced > 0 ? units[0] : 0u);
    }
    m.set_return(static_cast<u64>(static_cast<u32>(result)));
}

} // namespace

void User32ExtManager::impl_ToUnicodeEx(Marshaler& m) {
    // int ToUnicodeEx(UINT wVirtKey, UINT wScanCode, const BYTE *lpKeyState,
    //                 LPWSTR pwszBuff, int cchBuff, UINT wFlags, HKL dwhkl)
    to_unicode_thunk(m, "ToUnicodeEx");
}

void User32ExtManager::impl_ToUnicode(Marshaler& m) {
    // int ToUnicode(...) — same first six parameters as ToUnicodeEx; no HKL.
    to_unicode_thunk(m, "ToUnicode");
}

void User32ExtManager::impl_MapVirtualKeyW(Marshaler& m) {
    const u32 uCode = static_cast<u32>(m.arg(0));
    const u32 uMapType = static_cast<u32>(m.arg(1));
    u32 result = 0;
    if (!kbd_us::map_virtual_key(uCode, uMapType, result)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    }
    VITREO_TRACE("[u32ext] MapVirtualKeyW code=%u type=%u -> %u\n",
                 uCode, uMapType, result);
    m.set_return(result);
}

void User32ExtManager::impl_GetKeyNameTextW(Marshaler& m) {
    u32 lParam = static_cast<u32>(m.arg(0));
    GuestAddr lpString = m.arg_ptr(1);
    i32 cchSize = static_cast<i32>(m.arg(2));
    (void)lParam;
    if (lpString && cchSize >= 4) {
        (void)m.write_u16(lpString + 0, u'K');
        (void)m.write_u16(lpString + 2, u'e');
        (void)m.write_u16(lpString + 4, u'y');
        (void)m.write_u16(lpString + 6, 0);
        VITREO_TRACE("[u32ext] GetKeyNameTextW -> 3\n");
        m.set_return(3u);
    } else {
        m.set_return(0u);
    }
}

void User32ExtManager::impl_GetAsyncKeyState(Marshaler& m) {
    u32 vKey = static_cast<u32>(m.arg(0));
    i16 state = m_host.get_native_async_key_state(vKey);
    // Keys SendInput / keybd_event hold down read as down.
    if (vKey < 256 && (m_injected_keys[vKey].load() & 0x80u) != 0) {
        state = static_cast<i16>(static_cast<u16>(state) | 0x8000u);
    }
    if (windowing::diag::input_diag()) {
        std::fprintf(stderr, "[input] GetAsyncKeyState vk=0x%02x -> 0x%04x\n",
                     vKey, static_cast<u16>(state));
    }
    VITREO_TRACE("[u32ext] GetAsyncKeyState vk=0x%x -> 0x%04x\n",
                 vKey, static_cast<u16>(state));
    m.set_return(static_cast<u64>(static_cast<u16>(state)));
}

void User32ExtManager::impl_MapVirtualKeyExW(Marshaler& m) {
    const u32 uCode = static_cast<u32>(m.arg(0));
    const u32 uMapType = static_cast<u32>(m.arg(1));
    (void)m.arg(2); // dwhkl — Vitreo exposes a single layout
    u32 result = 0;
    if (!kbd_us::map_virtual_key(uCode, uMapType, result)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    }
    VITREO_TRACE("[u32ext] MapVirtualKeyExW code=%u type=%u -> %u\n",
                 uCode, uMapType, result);
    m.set_return(result);
}

void User32ExtManager::impl_GetFocus(Marshaler& m) {
    // Learn GetFocus: the focus window "attached to the calling thread's
    // message queue"; another queue's focus reads as NULL.
    const windowing::HWND focus = m_focus_hwnd;
    const windowing::HWND result = window_in_caller_queue(focus) ? focus : 0;
    VITREO_TRACE("[u32ext] GetFocus -> 0x%x\n", result);
    m.set_return(static_cast<u32>(result));
}

} // namespace vitreo::winapi
