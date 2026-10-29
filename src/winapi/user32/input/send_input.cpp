// SPDX-License-Identifier: LGPL-2.1-only

/// @file send_input.cpp
/// @brief Synthetic input injection: SendInput, keybd_event and the keyboard and
///        mouse injectors they share. Split out of the former mixed USER32 stubs file
///        as verbatim code motion.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/user32/windows/kbd_layout.hpp"
#include "winapi/kernel32/system/time.hpp"

namespace vitreo::winapi {

bool User32ExtManager::inject_keyboard(u16 vk, u16 scan, u32 flags, u32 time,
                                       u64 extra_info) {
    // KEYBDINPUT (Learn): KEYEVENTF_EXTENDEDKEY 1, KEYUP 2, UNICODE 4,
    // SCANCODE 8. The keystroke reaches the focus window of this process
    // (Windows: the foreground thread's) as hardware input, so WH_KEYBOARD_LL,
    // WH_KEYBOARD, TranslateMessage and GetKeyState all see it.
    constexpr u32 KEYEVENTF_EXTENDEDKEY = 0x1, KEYEVENTF_KEYUP = 0x2,
                  KEYEVENTF_UNICODE = 0x4, KEYEVENTF_SCANCODE = 0x8;
    constexpr u16 VK_PACKET = 0xE7, VK_MENU = 0x12, VK_CONTROL = 0x11,
                  VK_F10 = 0x79, VK_LMENU = 0xA4, VK_RMENU = 0xA5;
    const bool up = (flags & KEYEVENTF_KEYUP) != 0;
    const bool extended = (flags & KEYEVENTF_EXTENDEDKEY) != 0;
    u16 packet = 0;
    if ((flags & KEYEVENTF_UNICODE) != 0) {
        packet = scan;
        vk = VK_PACKET;
        scan = 0;
    } else if ((flags & KEYEVENTF_SCANCODE) != 0) {
        u32 mapped = 0;
        (void)kbd_us::map_virtual_key(extended ? (0xE000u | scan) : scan, 3, mapped);
        vk = static_cast<u16>(mapped);
    } else if (scan == 0) {
        u32 mapped = 0;
        (void)kbd_us::map_virtual_key(vk, 0, mapped);
        scan = static_cast<u16>(mapped);
    }
    const bool was_down = vk < 256 && (m_injected_keys[vk].load() & 0x80u) != 0;
    if (vk < 256) {
        m_injected_keys[vk].store(up ? 0 : 0x80);
        // The generic modifier follows its left/right forms.
        if (vk == VK_LMENU || vk == VK_RMENU) m_injected_keys[VK_MENU].store(up ? 0 : 0x80);
        if (vk == 0xA2 || vk == 0xA3) m_injected_keys[VK_CONTROL].store(up ? 0 : 0x80);
        if (vk == 0xA0 || vk == 0xA1) m_injected_keys[0x10].store(up ? 0 : 0x80);
    }
    const auto down_now = [this](u32 key) {
        return (m_injected_keys[key].load() & 0x80u) != 0 ||
               (m_host.get_native_async_key_state(key) & 0x8000) != 0;
    };
    const bool alt = down_now(VK_MENU);
    const bool ctrl = down_now(VK_CONTROL);
    // WM_SYSKEY*: Alt held without Ctrl, or F10 (Learn WM_SYSKEYDOWN).
    const bool system = (alt && !ctrl) || vk == VK_F10 || vk == VK_MENU ||
                        vk == VK_LMENU || vk == VK_RMENU;
    windowing::HWND target = m_focus_hwnd;
    if (target == windowing::NULL_HWND) target = static_cast<windowing::HWND>(m_active_window);
    if (target == windowing::NULL_HWND || !m_hwnd_mgr.is_valid(target)) return false;
    windowing::MSG msg{};
    msg.hwnd = target;
    msg.message = up ? (system ? windowing::WM::SYSKEYUP : windowing::WM::KEYUP)
                     : (system ? windowing::WM::SYSKEYDOWN : windowing::WM::KEYDOWN);
    msg.wParam = vk;
    u64 lparam = 1u | (static_cast<u64>(scan & 0xFFu) << 16);
    if (extended) lparam |= 1ull << 24;
    if (alt) lparam |= 1ull << 29;
    if (up || was_down) lparam |= 1ull << 30;
    if (up) lparam |= 1ull << 31;
    msg.lParam = lparam;
    msg.time = time != 0 ? time : tick_count32();
    msg.packet_char = packet;
    msg.extra_info = extra_info;
    if (const auto owner = m_hwnd_mgr.owner_thread_id(target)) msg.target_thread_id = *owner;
    return m_queue.post_input(msg);
}

bool User32ExtManager::inject_mouse(i32 dx, i32 dy, u32 mouse_data, u32 flags, u32 time,
                                    u64 extra_info) {
    // MOUSEINPUT (Learn). The cursor moves (relative pixels, or 0..65535
    // normalized over the primary screen with MOUSEEVENTF_ABSOLUTE), then
    // each button/wheel event goes to the capture window or the window under
    // the cursor, in client coordinates.
    constexpr u32 MOVE = 0x1, LEFTDOWN = 0x2, LEFTUP = 0x4, RIGHTDOWN = 0x8,
                  RIGHTUP = 0x10, MIDDLEDOWN = 0x20, MIDDLEUP = 0x40,
                  XDOWN = 0x80, XUP = 0x100, WHEEL = 0x800, HWHEEL = 0x1000,
                  ABSOLUTE = 0x8000;
    i32 x = 0;
    i32 y = 0;
    cursor_host().get_native_cursor_pos(x, y);
    if ((flags & MOVE) != 0) {
        if ((flags & ABSOLUTE) != 0) {
            const windowing::Rect screen = guest_virtual_screen_rect();
            const i32 width = screen.right - screen.left;
            const i32 height = screen.bottom - screen.top;
            x = screen.left + static_cast<i32>((static_cast<i64>(dx) * width) / 65536);
            y = screen.top + static_cast<i32>((static_cast<i64>(dy) * height) / 65536);
        } else {
            x += dx;
            y += dy;
        }
        (void)cursor_host().set_native_cursor_pos(x, y);
    }
    windowing::HWND target = m_hwnd_mgr.get_capture();
    if (target == windowing::NULL_HWND) target = m_hwnd_mgr.window_from_point({x, y});
    if (target == windowing::NULL_HWND || !m_hwnd_mgr.is_valid(target)) return true;
    const windowing::Point origin = client_origin_to_desktop(m_hwnd_mgr, target);
    const u64 client = (static_cast<u64>(static_cast<u16>(y - origin.y)) << 16) |
                       static_cast<u16>(x - origin.x);
    const u64 screen_lp = (static_cast<u64>(static_cast<u16>(y)) << 16) | static_cast<u16>(x);
    const auto key_down = [this](u32 key) {
        return (m_injected_keys[key].load() & 0x80u) != 0 ||
               (m_host.get_native_async_key_state(key) & 0x8000) != 0;
    };
    const auto post = [&](u32 message, u64 wparam, u64 lparam) {
        windowing::MSG msg{};
        msg.hwnd = target;
        msg.message = message;
        msg.wParam = wparam;
        msg.lParam = lparam;
        msg.time = time != 0 ? time : tick_count32();
        msg.extra_info = extra_info;
        if (const auto owner = m_hwnd_mgr.owner_thread_id(target)) msg.target_thread_id = *owner;
        (void)m_queue.post_input(msg);
    };
    const auto set_button = [this](u32 vk, bool down) { m_injected_keys[vk].store(down ? 0x80 : 0); };
    if ((flags & LEFTDOWN) != 0) set_button(0x01, true);
    if ((flags & RIGHTDOWN) != 0) set_button(0x02, true);
    if ((flags & MIDDLEDOWN) != 0) set_button(0x04, true);
    const auto mk = [&]() {
        u64 w = 0;
        if (key_down(0x01)) w |= 0x0001;  // MK_LBUTTON
        if (key_down(0x02)) w |= 0x0002;  // MK_RBUTTON
        if (key_down(0x10)) w |= 0x0004;  // MK_SHIFT
        if (key_down(0x11)) w |= 0x0008;  // MK_CONTROL
        if (key_down(0x04)) w |= 0x0010;  // MK_MBUTTON
        if (key_down(0x05)) w |= 0x0020;  // MK_XBUTTON1
        if (key_down(0x06)) w |= 0x0040;  // MK_XBUTTON2
        return w;
    };
    if ((flags & MOVE) != 0) post(windowing::WM::MOUSEMOVE, mk(), client);
    if ((flags & LEFTDOWN) != 0) post(windowing::WM::LBUTTONDOWN, mk(), client);
    if ((flags & LEFTUP) != 0) { set_button(0x01, false); post(windowing::WM::LBUTTONUP, mk(), client); }
    if ((flags & RIGHTDOWN) != 0) post(windowing::WM::RBUTTONDOWN, mk(), client);
    if ((flags & RIGHTUP) != 0) { set_button(0x02, false); post(windowing::WM::RBUTTONUP, mk(), client); }
    if ((flags & MIDDLEDOWN) != 0) post(windowing::WM::MBUTTONDOWN, mk(), client);
    if ((flags & MIDDLEUP) != 0) { set_button(0x04, false); post(windowing::WM::MBUTTONUP, mk(), client); }
    if ((flags & (XDOWN | XUP)) != 0) {
        const u32 which = mouse_data & 0x3u;  // XBUTTON1 1, XBUTTON2 2
        const u32 vk = which == 2 ? 0x06u : 0x05u;
        if ((flags & XDOWN) != 0) {
            set_button(vk, true);
            post(0x020B /* WM_XBUTTONDOWN */, mk() | (static_cast<u64>(which) << 16), client);
        }
        if ((flags & XUP) != 0) {
            set_button(vk, false);
            post(0x020C /* WM_XBUTTONUP */, mk() | (static_cast<u64>(which) << 16), client);
        }
    }
    // Wheel messages carry screen coordinates.
    if ((flags & WHEEL) != 0) {
        post(windowing::WM::MOUSEWHEEL, mk() | (static_cast<u64>(mouse_data & 0xFFFFu) << 16), screen_lp);
    }
    if ((flags & HWHEEL) != 0) {
        post(windowing::WM::MOUSEHWHEEL, mk() | (static_cast<u64>(mouse_data & 0xFFFFu) << 16), screen_lp);
    }
    return true;
}

void User32ExtManager::impl_SendInput(Marshaler& m) {
    // UINT SendInput(UINT cInputs, LPINPUT pInputs, int cbSize).
    // s2msg_input_probe: cInputs 0 or cbSize != sizeof(INPUT) -> 0 /
    // ERROR_INVALID_PARAMETER; pInputs NULL -> 0 / ERROR_NOACCESS on x64,
    // ERROR_INVALID_PARAMETER on x86; an INPUT whose type is none of
    // MOUSE/KEYBOARD/HARDWARE is still counted (sendinput_bad_type = 1).
    // Input goes to this process's windows only: Vitreo has no other
    // Windows desktop to deliver to.
    const u32 count = static_cast<u32>(m.arg(0));
    const GuestAddr inputs = m.arg_ptr(1);
    const i32 size = static_cast<i32>(m.arg(2));
    const bool is64 = m.cpu_is_64bit();
    const i32 expected = is64 ? 40 : 28;
    if (count == 0 || size != expected) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    if (inputs == 0) {
        set_last_error(is64 ? error_codes::ERROR_NOACCESS
                            : error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    const GuestAddr body = is64 ? 8 : 4;
    u32 sent = 0;
    for (u32 i = 0; i < count; ++i) {
        const GuestAddr in = inputs + static_cast<GuestAddr>(i) * static_cast<u32>(expected);
        const u32 type = m.read_u32(in).value_or(0xFFFFFFFFu);
        if (type == 1) {  // INPUT_KEYBOARD
            const u16 vk = m.read_u16(in + body).value_or(0);
            const u16 scan = m.read_u16(in + body + 2).value_or(0);
            const u32 flags = m.read_u32(in + body + 4).value_or(0);
            const u32 time = m.read_u32(in + body + 8).value_or(0);
            // KEYBDINPUT.dwExtraInfo (ULONG_PTR): offset 16 on x64, 12 on x86.
            const u64 extra = is64 ? m.read_u64(in + body + 16).value_or(0)
                                   : m.read_u32(in + body + 12).value_or(0);
            (void)inject_keyboard(vk, scan, flags, time, extra);
        } else if (type == 0) {  // INPUT_MOUSE
            const i32 dx = static_cast<i32>(m.read_u32(in + body).value_or(0));
            const i32 dy = static_cast<i32>(m.read_u32(in + body + 4).value_or(0));
            const u32 data = m.read_u32(in + body + 8).value_or(0);
            const u32 flags = m.read_u32(in + body + 12).value_or(0);
            const u32 time = m.read_u32(in + body + 16).value_or(0);
            // MOUSEINPUT.dwExtraInfo (ULONG_PTR): offset 24 on x64, 20 on x86.
            const u64 extra = is64 ? m.read_u64(in + body + 24).value_or(0)
                                   : m.read_u32(in + body + 20).value_or(0);
            (void)inject_mouse(dx, dy, data, flags, time, extra);
        }
        // INPUT_HARDWARE: no non-keyboard/mouse device exists here.
        ++sent;
    }
    VITREO_TRACE("[u32ext] SendInput cInputs=%u -> %u\n", count, sent);
    m.set_return(sent);
}

void User32ExtManager::impl_keybd_event(Marshaler& m) {
    // void keybd_event(BYTE bVk, BYTE bScan, DWORD dwFlags, ULONG_PTR):
    // SendInput with one KEYBDINPUT (Learn: superseded by SendInput).
    const u16 vk = static_cast<u8>(m.arg(0));
    const u16 scan = static_cast<u8>(m.arg(1));
    const u32 flags = static_cast<u32>(m.arg(2));
    const u64 extra = m.cpu_is_64bit() ? m.arg(3) : static_cast<u32>(m.arg(3));
    (void)inject_keyboard(vk, scan, flags, 0, extra);
}

} // namespace vitreo::winapi
