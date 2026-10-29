// SPDX-License-Identifier: LGPL-2.1-only

/// @file mouse_cursor_input.cpp
/// @brief TrackMouseEvent, ClipCursor/GetClipCursor and GetCursorInfo: the mouse
///        and cursor-state APIs that talk to the cursor host and the window
///        manager. Split out of the former mixed USER32 stubs file
///        as verbatim code motion.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "windowing/model/hwnd_manager.hpp"

#include <array>
#include <vector>

namespace vitreo::winapi {

void User32ExtManager::impl_TrackMouseEvent(Marshaler& m) {
    // BOOL TrackMouseEvent(LPTRACKMOUSEEVENT lpEventTrack)
    //
    //   DWORD cbSize; DWORD dwFlags; HWND hwndTrack; DWORD dwHoverTime;
    //
    // hwndTrack is pointer-sized, so the struct is 16 bytes on x86 and 24 on
    // x64 (tail padding included).
    const GuestAddr address = m.arg_ptr(0);
    const bool is_64bit = m.cpu_is_64bit();
    const u32 size = is_64bit ? 24u : 16u;
    if (!address || m_memory.read_u32(address).value_or(0) != size) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    constexpr u32 TME_HOVER = 0x00000001;
    constexpr u32 TME_LEAVE = 0x00000002;
    constexpr u32 TME_NONCLIENT = 0x00000010;
    constexpr u32 TME_QUERY = 0x40000000;
    constexpr u32 TME_CANCEL = 0x80000000;

    const u32 flags = m_memory.read_u32(address + 4).value_or(0);
    const GuestAddr hwnd_field = address + 8;
    const u64 hwnd_raw = is_64bit
        ? m_memory.read_u64(hwnd_field).value_or(0)
        : m_memory.read_u32(hwnd_field).value_or(0);
    const auto hwnd = static_cast<windowing::HWND>(hwnd_raw);
    const GuestAddr hover_field = address + (is_64bit ? 16u : 12u);
    const u32 hover_ms = m_memory.read_u32(hover_field).value_or(0);

    // s2msg_input_probe trackmouse_invalid_hwnd: FALSE /
    // ERROR_INVALID_WINDOW_HANDLE.
    if (hwnd == 0 || !m_hwnd_mgr.is_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0);
        return;
    }

    if ((flags & TME_QUERY) != 0) {
        // Learn: TME_QUERY overwrites the structure with what is armed and
        // does not change it. dwHoverTime is only meaningful with TME_HOVER.
        bool leave = false;
        bool hover = false;
        u32 armed_ms = 0;
        m_host.query_mouse_tracking(hwnd, leave, hover, armed_ms);
        u32 armed = 0;
        if (leave) armed |= TME_LEAVE;
        if (hover) armed |= TME_HOVER;
        (void)m_memory.write_u32(address + 4, armed);
        (void)m_memory.write_u32(hover_field, armed_ms);
        m.set_return(1);
        return;
    }

    const bool leave = (flags & TME_LEAVE) != 0;
    const bool hover = (flags & TME_HOVER) != 0;
    if ((flags & TME_CANCEL) != 0) {
        m_host.cancel_mouse_tracking(hwnd, leave, hover);
    } else if (leave || hover) {
        m_host.track_mouse_event(hwnd, leave, hover, hover_ms);
    }
    // TME_NONCLIENT asks for WM_NCMOUSELEAVE/WM_NCMOUSEHOVER instead. Vitreo
    // presents the non-client frame natively, so the guest never owns a
    // non-client area the pointer can leave; the client-area request that
    // accompanies it is honoured and the caller is not lied to about failure.
    (void)TME_NONCLIENT;
    m.set_return(1);
}

void User32ExtManager::impl_ClipCursor(Marshaler& m) {
    GuestAddr lpRect = m.arg_ptr(0);
    if (lpRect == 0) {
        m_host.set_cursor_clip(false, 0, 0, 0, 0);
        VITREO_TRACE("[u32ext] ClipCursor(NULL) -> TRUE\n");
        m.set_return(1u);
        return;
    }
    const i32 left = static_cast<i32>(m_memory.read_u32(lpRect + 0).value_or(0));
    const i32 top = static_cast<i32>(m_memory.read_u32(lpRect + 4).value_or(0));
    const i32 right = static_cast<i32>(m_memory.read_u32(lpRect + 8).value_or(0));
    const i32 bottom = static_cast<i32>(m_memory.read_u32(lpRect + 12).value_or(0));
    m_host.set_cursor_clip(true, left, top, right, bottom);
    VITREO_TRACE("[u32ext] ClipCursor (%d,%d)-(%d,%d) -> TRUE\n", left, top,
                 right, bottom);
    m.set_return(1u);
}

void User32ExtManager::impl_GetClipCursor(Marshaler& m) {
    GuestAddr lpRect = m.arg_ptr(0);
    if (lpRect == 0) {
        m.set_return(0u);
        return;
    }
    i32 left = 0, top = 0, right = 0, bottom = 0;
    if (!m_host.get_cursor_clip(left, top, right, bottom)) {
        // Win32 returns the full virtual screen when unrestricted -- the
        // same rectangle SetCursorPos confines to and SM_*VIRTUALSCREEN
        // report (virtual full-screen included).
        const windowing::Rect screen = guest_virtual_screen_rect();
        left = screen.left;
        top = screen.top;
        right = screen.right;
        bottom = screen.bottom;
    }
    (void)m.write_u32(lpRect + 0, static_cast<u32>(left));
    (void)m.write_u32(lpRect + 4, static_cast<u32>(top));
    (void)m.write_u32(lpRect + 8, static_cast<u32>(right));
    (void)m.write_u32(lpRect + 12, static_cast<u32>(bottom));
    VITREO_TRACE("[u32ext] GetClipCursor -> (%d,%d)-(%d,%d)\n", left, top,
                 right, bottom);
    m.set_return(1u);
}

void User32ExtManager::impl_GetCursorInfo(Marshaler& m) {
    // CURSORINFO { cbSize, flags, HCURSOR hCursor, POINT ptScreenPos }: 20
    // bytes on x86, 24 on x64 (hCursor aligned to 8). cbSize must be that
    // size, else FALSE with the error untouched; flags is CURSOR_SHOWING
    // while the display count is not negative (s3gdi_usermisc_probe
    // "GetCursorInfo=1 flags=1 hcursor ok=1", "(bad size)=0 err=57005").
    const GuestAddr pci = m.arg_ptr(0);
    const bool wide = m.cpu_is_64bit();
    const u32 size = wide ? 24u : 20u;
    if (pci == 0 || m.read_u32(pci).value_or(0) != size) {
        m.set_return(0u);
        return;
    }
    i32 x = 0, y = 0;
    m_host.get_native_cursor_pos(x, y);
    const bool showing = m_cursor_display_count >= 0 && m_current_cursor != 0;
    (void)m.write_u32(pci + 4, showing ? 1u : 0u);
    const GuestAddr at = wide ? pci + 8 : pci + 8;
    if (wide) (void)m.memory().write_u64(at, m_current_cursor);
    else (void)m.write_u32(at, m_current_cursor);
    const GuestAddr pt = wide ? pci + 16 : pci + 12;
    (void)m.write_u32(pt, static_cast<u32>(x));
    (void)m.write_u32(pt + 4, static_cast<u32>(y));
    m.set_return(1u);
}

// GESTURECONFIG { DWORD dwID, dwWant, dwBlock } (WinUser.h) is 12 bytes on
// x86 and x64. Measured x86 == x64 in clipformats_gesture_probe:
//  * SetGestureConfig: cbSize must be 12, cIDs non-zero, the array non-NULL
//    and every dwID GID_ZOOM..GID_PRESSANDTAP (3..7) or 0, else
//    ERROR_INVALID_PARAMETER; dwReserved is ignored; a window that is not one
//    of this process's (the desktop included) is ERROR_INVALID_WINDOW_HANDLE.
//    Entries replace that gesture's (want, block); other gestures keep theirs.
//    dwID 0 applies to every gesture, GC_ALLGESTURES in dwWant / dwBlock
//    meaning each gesture's full mask (0x1F for pan, 1 for the rest).
//  * GetGestureConfig: fills dwWant / dwBlock for each requested dwID; an
//    unconfigured gesture is 0 / 0.
namespace {
constexpr u32 kGestureConfigSize = 12;
constexpr u32 kGidZoom = 3, kGidPressAndTap = 7, kGidPan = 4;
constexpr u32 kGcAllGestures = 1, kGcPanAll = 0x1F, kGcfIncludeAncestors = 1;
u32 full_gesture_mask(u32 id) { return id == kGidPan ? kGcPanAll : 1u; }
}  // namespace

void User32ExtManager::impl_SetGestureConfig(Marshaler& m) {
    // BOOL SetGestureConfig(HWND, DWORD dwReserved, UINT cIDs,
    //                       PGESTURECONFIG, UINT cbSize)
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const u32 count = static_cast<u32>(m.arg(2));
    const GuestAddr configs = m.arg_ptr(3);
    const u32 cb = static_cast<u32>(m.arg(4));
    if (cb != kGestureConfigSize || count == 0 || configs == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    std::vector<std::array<u32, 3>> entries(count);
    for (u32 i = 0; i < count; ++i) {
        for (u32 f = 0; f < 3; ++f) {
            auto v = m.memory().read_u32(configs + i * kGestureConfigSize + f * 4);
            if (v.is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0u);
                return;
            }
            entries[i][f] = v.value();
        }
        const u32 id = entries[i][0];
        if (id != 0 && (id < kGidZoom || id > kGidPressAndTap)) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0u);
            return;
        }
    }
    // The desktop window belongs to no process of the caller: 1400 (desktop).
    if (hwnd == windowing::HWND_DESKTOP || m_hwnd_mgr.find(hwnd) == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    std::lock_guard lock(m_gesture_mutex);
    auto& window = m_gesture_configs[hwnd];
    for (const auto& e : entries) {
        if (e[0] != 0) {
            window[e[0]] = GestureConfigEntry{e[1], e[2]};
            continue;
        }
        for (u32 id = kGidZoom; id <= kGidPressAndTap; ++id) {
            window[id] = GestureConfigEntry{(e[1] & kGcAllGestures) ? full_gesture_mask(id) : 0u,
                                            (e[2] & kGcAllGestures) ? full_gesture_mask(id) : 0u};
        }
    }
    m.set_return(1u);
}

void User32ExtManager::impl_GetGestureConfig(Marshaler& m) {
    // BOOL GetGestureConfig(HWND, DWORD dwReserved, DWORD dwFlags,
    //                       PUINT pcIDs, PGESTURECONFIG, UINT cbSize)
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const u32 flags = static_cast<u32>(m.arg(2));
    const GuestAddr pcount = m.arg_ptr(3);
    const GuestAddr configs = m.arg_ptr(4);
    const u32 cb = static_cast<u32>(m.arg(5));
    const u32 count = pcount != 0 ? m.memory().read_u32(pcount).value_or(0) : 0;
    if (cb != kGestureConfigSize || count == 0 || configs == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    const windowing::WindowData* win =
        hwnd == windowing::HWND_DESKTOP ? nullptr : m_hwnd_mgr.find(hwnd);
    if (win == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    std::lock_guard lock(m_gesture_mutex);
    for (u32 i = 0; i < count; ++i) {
        const GuestAddr at = configs + i * kGestureConfigSize;
        const u32 id = m.memory().read_u32(at).value_or(0);
        if (id < kGidZoom || id > kGidPressAndTap) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0u);
            return;
        }
        // GCF_INCLUDE_ANCESTORS (Learn): a gesture this window leaves
        // unconfigured takes the nearest ancestor's configuration.
        // The chain follows WS_CHILD parents and stops at a top-level window
        // (whose parent field is the desktop); the depth bound guards a
        // malformed parent cycle.
        GestureConfigEntry value{};
        const windowing::WindowData* w = win;
        for (u32 depth = 0; w != nullptr && depth < 256; ++depth) {
            auto wit = m_gesture_configs.find(w->handle);
            if (wit != m_gesture_configs.end()) {
                auto git = wit->second.find(id);
                if (git != wit->second.end()) {
                    value = git->second;
                    break;
                }
            }
            const bool child = (w->style & windowing::WS::CHILD) != 0 &&
                               w->parent != windowing::NULL_HWND &&
                               w->parent != windowing::HWND_DESKTOP;
            w = (flags & kGcfIncludeAncestors) && child ? m_hwnd_mgr.find(w->parent) : nullptr;
        }
        (void)m.write_u32(at + 4, value.want);
        (void)m.write_u32(at + 8, value.block);
    }
    m.set_return(1u);
}

} // namespace vitreo::winapi
