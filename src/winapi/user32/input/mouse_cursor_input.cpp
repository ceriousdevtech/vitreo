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
#include "windowing/model/diag_flags.hpp"
#include "windowing/model/cursor_confinement.hpp"
#include "winapi/user32/input/cursor_clip_sdk.generated.hpp"

#include <array>
#include <atomic>
#include <cstdio>
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
    const GuestAddr address = m.arg_ptr(0);
    static std::atomic<unsigned> trace_count{0};
    const bool trace = windowing::diag::input_diag() &&
        trace_count.fetch_add(1, std::memory_order_relaxed) < 96;
    std::optional<windowing::Rect> requested;
    const auto finish = [&](bool success, u32 error) {
        if (error) set_last_error(error);
        if (trace) {
            if (requested) {
                std::fprintf(stderr,
                    "[input] ClipCursor rect=0x%llx requested=(%d,%d)-(%d,%d) caller=0x%llx result=%d error=%u\n",
                    static_cast<unsigned long long>(address), requested->left,
                    requested->top, requested->right, requested->bottom,
                    static_cast<unsigned long long>(m.guest_return_address()),
                    success ? 1 : 0, error);
            } else {
                std::fprintf(stderr,
                    "[input] ClipCursor rect=0x%llx request=%s caller=0x%llx result=%d error=%u\n",
                    static_cast<unsigned long long>(address), address ? "unread" : "NULL",
                    static_cast<unsigned long long>(m.guest_return_address()),
                    success ? 1 : 0, error);
            }
        }
        m.set_return(success ? 1u : 0u);
    };
    const windowing::Rect screen = guest_virtual_screen_rect();
    if (!address) {
        const bool applied = m_host.set_cursor_clip(false, 0, 0, 0, 0, screen);
        finish(applied, applied ? 0 : error_codes::ERROR_GEN_FAILURE);
        return;
    }
    windowing::Rect previous{};
    // Windows x86/x64 oracle: a currently degenerate clip rejects every
    // non-NULL request with access denied BEFORE probing that request.
    if (m_host.get_cursor_clip(previous.left, previous.top, previous.right,
                               previous.bottom) &&
        (previous.left == previous.right || previous.top == previous.bottom)) {
        finish(false, error_codes::ERROR_ACCESS_DENIED);
        return;
    }
    std::array<Byte, cursor_clip_sdk::kRectSize> bytes{};
    if (m_memory.read_bytes(address, bytes.data(), bytes.size()).is_err()) {
        finish(false, error_codes::ERROR_NOACCESS);
        return;
    }
    std::array<i32, cursor_clip_sdk::kRectOffsets.size()> values{};
    for (usize field = 0; field < values.size(); ++field) {
        u32 value = 0;
        for (usize byte = 0; byte < sizeof(value); ++byte)
            value |= static_cast<u32>(bytes[cursor_clip_sdk::kRectOffsets[field] + byte]) << (byte * 8);
        values[field] = static_cast<i32>(value);
    }
    requested = windowing::Rect{values[0], values[1], values[2], values[3]};
    const auto effective = windowing::normalize_cursor_clip(*requested, screen);
    const windowing::Rect clip = effective.value_or(windowing::Rect{});
    const bool applied = m_host.set_cursor_clip(effective.has_value(), clip.left,
        clip.top, clip.right, clip.bottom, screen);
    finish(applied, applied ? 0 : error_codes::ERROR_GEN_FAILURE);
}

void User32ExtManager::impl_GetClipCursor(Marshaler& m) {
    const GuestAddr address = m.arg_ptr(0);
    static std::atomic<unsigned> trace_count{0};
    const bool trace = windowing::diag::input_diag() &&
        trace_count.fetch_add(1, std::memory_order_relaxed) < 32;
    const auto fail = [&] {
        set_last_error(error_codes::ERROR_NOACCESS);
        if (trace) std::fprintf(stderr,
            "[input] GetClipCursor rect=0x%llx caller=0x%llx result=0 error=%u\n",
            static_cast<unsigned long long>(address),
            static_cast<unsigned long long>(m.guest_return_address()),
            error_codes::ERROR_NOACCESS);
        m.set_return(0u);
    };
    if (!address) { fail(); return; }
    windowing::Rect clip{};
    if (!m_host.get_cursor_clip(clip.left, clip.top, clip.right, clip.bottom))
        clip = guest_virtual_screen_rect();
    const std::array<i32, cursor_clip_sdk::kRectOffsets.size()> values{
        clip.left, clip.top, clip.right, clip.bottom};
    std::array<Byte, cursor_clip_sdk::kRectSize> bytes{};
    for (usize field = 0; field < values.size(); ++field) {
        const u32 value = static_cast<u32>(values[field]);
        for (usize byte = 0; byte < sizeof(value); ++byte)
            bytes[cursor_clip_sdk::kRectOffsets[field] + byte] =
                static_cast<Byte>(value >> (byte * 8));
    }
    // GuestMemory validates the entire span before any byte is written.
    if (m_memory.write_bytes(address, bytes.data(), bytes.size()).is_err()) {
        fail(); return;
    }
    if (trace) std::fprintf(stderr,
        "[input] GetClipCursor rect=0x%llx output=(%d,%d)-(%d,%d) caller=0x%llx result=1\n",
        static_cast<unsigned long long>(address), clip.left, clip.top,
        clip.right, clip.bottom,
        static_cast<unsigned long long>(m.guest_return_address()));
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
