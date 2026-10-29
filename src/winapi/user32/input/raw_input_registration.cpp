// SPDX-License-Identifier: LGPL-2.1-only

/// @file raw_input_registration.cpp
/// @brief RegisterRawInputDevices and the WM_INPUT producers: the keyboard and
///        mouse transitions the host reports, and the packet lifetime rules.
///        Split out of the former mixed USER32 stubs file
///        as verbatim code motion.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "winapi/user32/input/raw_input_internal.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/kernel32/system/time.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>

namespace vitreo::winapi {

void User32ExtManager::impl_RegisterRawInputDevices(Marshaler& m) {
    constexpr u32 kRidevRemove = 0x00000001u;
    constexpr u16 kGenericDesktop = 0x0001u;
    constexpr u16 kMouse = 0x0002u;
    constexpr u16 kKeyboard = 0x0006u;

    const GuestAddr devices = m.arg_ptr(0);
    const u32 count = static_cast<u32>(m.arg(1));
    const u32 size = static_cast<u32>(m.arg(2));
    const u32 expected_size = m.cpu_is_64bit() ? 16u : 12u;
    if (devices == 0 || count == 0 || size != expected_size) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    std::optional<RawInputRegistration> keyboard_update;
    std::optional<RawInputRegistration> mouse_update;
    bool keyboard_seen = false;
    bool mouse_seen = false;
    for (u32 i = 0; i < count; ++i) {
        const GuestAddr entry = devices + static_cast<GuestAddr>(i) * size;
        const auto usage_page = m.read_u16(entry);
        const auto usage = m.read_u16(entry + 2);
        const auto flags = m.read_u32(entry + 4);
        u64 target_value = 0;
        bool target_ok = false;
        if (m.cpu_is_64bit()) {
            const auto target = m.read_u64(entry + 8);
            target_ok = target.is_ok();
            if (target_ok) target_value = target.value();
        } else {
            const auto target = m.read_u32(entry + 8);
            target_ok = target.is_ok();
            if (target_ok) target_value = target.value();
        }
        if (usage_page.is_err() || usage.is_err() || flags.is_err() ||
            !target_ok) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
        VITREO_TRACE("[u32ext] RegisterRawInputDevices[%u] page=0x%x usage=0x%x flags=0x%x target=0x%llx\n",
                     i, usage_page.value(), usage.value(), flags.value(),
                     static_cast<unsigned long long>(target_value));
        if (raw_input_diag()) {
            std::fprintf(stderr,
                         "[rawinput] Register[%u] page=0x%x usage=0x%x "
                         "flags=0x%x target=0x%llx\n",
                         i, usage_page.value(), usage.value(), flags.value(),
                         static_cast<unsigned long long>(target_value));
        }
        // RAWINPUTDEVICE.dwFlags (winuser.h RIDEV_*): unknown bits fail with
        // ERROR_INVALID_FLAGS (s2msg_input_probe rid_bad_flags); the sink
        // modes need a target window (rid_inputsink_no_hwnd: 87).
        constexpr u32 kRidevValid = 0x00003731u;
        constexpr u32 kRidevNeedsTarget = 0x00000100u /* INPUTSINK */ |
                                          0x00001000u /* EXINPUTSINK */;
        if ((flags.value() & ~kRidevValid) != 0) {
            set_last_error(error_codes::ERROR_INVALID_FLAGS);
            m.set_return(0);
            return;
        }
        if ((flags.value() & kRidevNeedsTarget) != 0 && target_value == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        if (usage_page.value() != kGenericDesktop ||
            (usage.value() != kKeyboard && usage.value() != kMouse)) {
            continue;
        }
        const bool is_mouse = usage.value() == kMouse;
        if (is_mouse) mouse_seen = true;
        else keyboard_seen = true;
        const auto hwnd = static_cast<windowing::HWND>(target_value);
        if ((flags.value() & kRidevRemove) != 0) {
            if (hwnd != windowing::NULL_HWND) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            if (is_mouse) mouse_update.reset();
            else keyboard_update.reset();
        } else {
            if (hwnd != windowing::NULL_HWND && !m_hwnd_mgr.is_valid(hwnd)) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            RawInputRegistration registration{
                flags.value(), hwnd, m.cpu_is_64bit()};
            if (is_mouse) mouse_update = registration;
            else keyboard_update = registration;
        }
    }

    if (keyboard_seen || mouse_seen) {
        std::lock_guard lock(m_raw_input_mutex);
        if (keyboard_seen) {
            if (keyboard_update.has_value()) {
                m_raw_keyboard_registration = keyboard_update;
            } else {
                m_raw_keyboard_registration.reset();
            }
        }
        if (mouse_seen) {
            if (mouse_update.has_value()) {
                m_raw_mouse_registration = mouse_update;
            } else {
                m_raw_mouse_registration.reset();
            }
        }
    }
    VITREO_TRACE("[u32ext] RegisterRawInputDevices count=%u size=%u keyboard=%s target=0x%x -> TRUE\n",
                 count, size, !keyboard_seen ? "unchanged" :
                 keyboard_update.has_value() ? "registered" : "removed",
                 keyboard_update.has_value() ? keyboard_update->target : 0u);
    m.set_return(1u);
}

void User32ExtManager::note_raw_keyboard_transition(u32 virtual_key,
                                                     bool down) {
    constexpr u16 kRiKeyBreak = 0x0001u;
    constexpr u16 kRiKeyE0 = 0x0002u;
    constexpr u32 kRidevInputSink = 0x00000100u;

    RawInputRegistration registration{};
    u64 handle = 0;
    windowing::MSG message{};
    {
        std::lock_guard lock(m_raw_input_mutex);
        if (!m_raw_keyboard_registration.has_value()) return;
        registration = *m_raw_keyboard_registration;
        const windowing::HWND target = registration.target != windowing::NULL_HWND
            ? registration.target : m_active_window;
        if (target == windowing::NULL_HWND || !m_hwnd_mgr.is_valid(target)) return;

        const u32 scan = map_virtual_key(virtual_key, 0);
        u16 raw_flags = down ? 0u : kRiKeyBreak;
        switch (virtual_key) {
            case 0x21: case 0x22: case 0x23: case 0x24:
            case 0x25: case 0x26: case 0x27: case 0x28:
            case 0x2D: case 0x2E:
                raw_flags |= kRiKeyE0;
                break;
            default:
                break;
        }
        handle = registration.is_64bit ? m_next_raw_input_handle++
            : static_cast<u32>(m_next_raw_input_handle++);
        RawKeyboardPacket packet{};
        packet.make_code = static_cast<u16>(scan);
        packet.flags = raw_flags;
        packet.virtual_key = static_cast<u16>(virtual_key);
        packet.message = down ? windowing::WM::KEYDOWN : windowing::WM::KEYUP;
        packet.is_64bit = registration.is_64bit;
        m_raw_keyboard_packets.emplace(handle, packet);
        m_raw_keyboard_order.push_back(handle);

        message.hwnd = target;
        message.message = windowing::WM::INPUT;
        message.wParam = (registration.flags & kRidevInputSink) != 0 ? 1u : 0u;
        message.lParam = handle;
        message.time = tick_count32();
    }
    m_queue.post(message);
    if (raw_input_diag()) {
        std::fprintf(stderr, "[rawinput] post WM_INPUT kbd vk=0x%x %s hwnd=0x%x raw=0x%llx\n",
                     virtual_key, down ? "down" : "up", message.hwnd,
                     static_cast<unsigned long long>(handle));
    }
    VITREO_TRACE("[u32ext] WM_INPUT keyboard vk=0x%x %s raw=0x%llx\n",
                 virtual_key, down ? "down" : "up",
                 static_cast<unsigned long long>(handle));
}

void User32ExtManager::note_raw_mouse_button(u32 button, bool down,
                                             windowing::HWND event_hwnd) {
    u16 button_flags = 0;
    switch (button) {
        case 0: button_flags = down ? 0x0001u : 0x0002u; break;
        case 1: button_flags = down ? 0x0004u : 0x0008u; break;
        case 2: button_flags = down ? 0x0010u : 0x0020u; break;
        default: return;
    }
    post_raw_mouse_buttons(button_flags, 0, event_hwnd);
}

void User32ExtManager::note_raw_mouse_wheel(i32 delta, bool horizontal,
                                            windowing::HWND event_hwnd) {
    // winuser.h: RI_MOUSE_WHEEL 0x0400 / RI_MOUSE_HWHEEL 0x0800; "the wheel
    // delta is stored in usButtonData" as a signed SHORT in WHEEL_DELTA units.
    // One packet per SHORT-sized chunk, like the WM_MOUSEWHEEL split.
    const u16 button_flags = horizontal ? 0x0800u : 0x0400u;
    while (delta != 0) {
        const i32 chunk = std::clamp(delta, -32760, 32760);
        delta -= chunk;
        post_raw_mouse_buttons(button_flags,
                               static_cast<u16>(static_cast<i16>(chunk)),
                               event_hwnd);
    }
}

void User32ExtManager::post_raw_mouse_buttons(u16 button_flags,
                                              u16 button_data,
                                              windowing::HWND event_hwnd) {
    constexpr u32 kRidevInputSink = 0x00000100u;
    RawInputRegistration registration{};
    u64 handle = 0;
    windowing::MSG message{};
    {
        std::lock_guard lock(m_raw_input_mutex);
        if (!m_raw_mouse_registration.has_value()) return;
        registration = *m_raw_mouse_registration;
        windowing::HWND target = registration.target;
        if (target == windowing::NULL_HWND) target = event_hwnd;
        if (target == windowing::NULL_HWND) target = m_active_window;
        if (target == windowing::NULL_HWND || !m_hwnd_mgr.is_valid(target)) return;
        handle = registration.is_64bit ? m_next_raw_input_handle++
            : static_cast<u32>(m_next_raw_input_handle++);
        RawKeyboardPacket packet{};
        packet.mouse = true;
        packet.button_flags = button_flags;
        packet.button_data = button_data;
        packet.is_64bit = registration.is_64bit;
        m_raw_keyboard_packets.emplace(handle, packet);
        m_raw_keyboard_order.push_back(handle);
        message.hwnd = target;
        message.message = windowing::WM::INPUT;
        message.wParam = (registration.flags & kRidevInputSink) != 0 ? 1u : 0u;
        message.lParam = handle;
        message.time = tick_count32();
    }
    m_queue.post(message);
    if (raw_input_diag()) {
        std::fprintf(stderr, "[rawinput] post WM_INPUT mouse buttons=0x%x data=0x%x hwnd=0x%x raw=0x%llx\n",
                     button_flags, button_data, message.hwnd,
                     static_cast<unsigned long long>(handle));
    }
}

void User32ExtManager::note_raw_mouse_motion(double device_dx,
                                             double device_dy,
                                             windowing::HWND event_hwnd) {
    constexpr u32 kRidevInputSink = 0x00000100u;
    RawInputRegistration registration{};
    u64 handle = 0;
    windowing::MSG message{};
    {
        std::lock_guard lock(m_raw_input_mutex);
        if (!m_raw_mouse_registration.has_value()) return;
        // Device motion arrives with sub-count precision on macOS; RAWMOUSE
        // carries integer counts, so carry the remainder to the next event
        // instead of dropping slow motion.
        m_raw_mouse_frac_x += device_dx;
        m_raw_mouse_frac_y += device_dy;
        const i32 dx = static_cast<i32>(std::trunc(m_raw_mouse_frac_x));
        const i32 dy = static_cast<i32>(std::trunc(m_raw_mouse_frac_y));
        m_raw_mouse_frac_x -= dx;
        m_raw_mouse_frac_y -= dy;
        if (dx == 0 && dy == 0) return;
        registration = *m_raw_mouse_registration;
        windowing::HWND target = registration.target;
        if (target == windowing::NULL_HWND) target = event_hwnd;
        if (target == windowing::NULL_HWND) target = m_active_window;
        if (target == windowing::NULL_HWND || !m_hwnd_mgr.is_valid(target)) return;
        handle = registration.is_64bit ? m_next_raw_input_handle++
            : static_cast<u32>(m_next_raw_input_handle++);
        RawKeyboardPacket packet{};
        packet.mouse = true;
        packet.last_x = dx;
        packet.last_y = dy;
        packet.is_64bit = registration.is_64bit;
        m_raw_keyboard_packets.emplace(handle, packet);
        m_raw_keyboard_order.push_back(handle);
        message.hwnd = target;
        message.message = windowing::WM::INPUT;
        message.wParam = (registration.flags & kRidevInputSink) != 0 ? 1u : 0u;
        message.lParam = handle;
        message.time = tick_count32();
    }
    m_queue.post(message);
}

void User32ExtManager::discard_raw_input_for_thread(u32 tid) {
    std::lock_guard lock(m_raw_input_mutex);
    const auto it = m_current_raw_input_by_thread.find(tid);
    if (it == m_current_raw_input_by_thread.end()) return;
    m_raw_keyboard_packets.erase(it->second);
    m_current_raw_input_by_thread.erase(it);
}

// Windows rawinput_lifetime_probe (x86/x64): packet reads and DefWindowProc
// leave the current handle valid. Only the next successful message removal
// retires it; PM_NOREMOVE and an empty PM_REMOVE do not. Buffer membership
// ends when WM_INPUT leaves the queue, separately from packet lifetime.
void User32ExtManager::note_removed_raw_input_message(
    const windowing::MSG& message) {
    const u32 tid = ThreadManager::get_current_thread_id_static();
    std::lock_guard lock(m_raw_input_mutex);
    if (const auto old = m_current_raw_input_by_thread.find(tid);
        old != m_current_raw_input_by_thread.end()) {
        m_raw_keyboard_packets.erase(old->second);
        m_current_raw_input_by_thread.erase(old);
    }
    if (message.message == windowing::WM::INPUT &&
        m_raw_keyboard_packets.contains(message.lParam)) {
        std::erase(m_raw_keyboard_order, message.lParam);
        m_current_raw_input_by_thread.emplace(tid, message.lParam);
    }
}

} // namespace vitreo::winapi
