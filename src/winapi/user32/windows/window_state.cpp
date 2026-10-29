// SPDX-License-Identifier: LGPL-2.1-only

/// @file window_state.cpp
/// @brief Window state queries and small window services: GetWindowInfo,
/// LockWindowUpdate, FlashWindow(Ex), PrintWindow, GetGuiResources,
/// ChildWindowFromPoint, SetLastErrorEx.
///
/// Contracts: the Microsoft Learn page of each function. What Learn leaves
/// open (error codes, the cbSize and NULL-argument cases) is measured in
/// tools/oracle_probe/s1win_api_probe.c ("-- state") on Windows 11.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/user32/user32_errors.hpp"
#include "winapi/user32/classes/window_class.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "ipc/hwnd_registry.hpp"

#include <algorithm>
#include <cstdio>
#include <mutex>

namespace vitreo::winapi {

namespace {

void unimplemented_once(std::once_flag& flag, const char* what) {
    std::call_once(flag, [what] {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED %s\n", what);
    });
}

} // namespace

bool User32ExtManager::window_exists(windowing::HWND hwnd) const {
    return m_hwnd_mgr.is_valid(hwnd) ||
           (hwnd != windowing::NULL_HWND &&
            ipc::hwnd_registry_is_window(static_cast<u32>(hwnd)));
}

// BOOL GetWindowInfo(HWND hwnd, PWINDOWINFO pwi)
//
// WINDOWINFO (winuser.h): cbSize, rcWindow, rcClient, dwStyle, dwExStyle,
// dwWindowStatus, cxWindowBorders, cyWindowBorders, atomWindowType,
// wCreatorVersion — 60 bytes on x86 and x64. Measured
// (s1win_api_probe getwindowinfo_*): a wrong cbSize still succeeds but sets
// ERROR_INVALID_PARAMETER and leaves cbSize as the caller wrote it;
// wCreatorVersion is 0x0500; the border widths are the frame's.
void User32ExtManager::impl_GetWindowInfo(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const GuestAddr pwi = m.arg_ptr(1);
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    if (pwi == 0) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0u);
        return;
    }
    constexpr u32 kWindowInfoSize = 60;
    if (m.read_u32(pwi).value_or(0) != kWindowInfoSize) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    }
    const auto origin = client_origin_to_desktop(m_hwnd_mgr, hwnd);
    windowing::Rect outer{origin.x - win->client_offset.x, origin.y - win->client_offset.y, 0, 0};
    outer.right = outer.left + win->rect.width();
    outer.bottom = outer.top + win->rect.height();
    const windowing::Rect client{origin.x, origin.y, origin.x + win->client_rect.width(),
                                 origin.y + win->client_rect.height()};
    i32 border = 0;
    if (win->style & windowing::WS::THICKFRAME) border = 8;       // SM_CXSIZEFRAME
    else if (win->style & windowing::WS::DLGFRAME) border = 4;    // SM_CXFIXEDFRAME
    else if (win->style & windowing::WS::BORDER) border = 1;      // SM_CXBORDER
    const WindowClassData* cls = window_class(*win);
    constexpr u32 kWsActiveCaption = 0x0001;
    const u32 status = (m_active_window == hwnd)
        ? kWsActiveCaption : 0u;
    const u32 words[14] = {
        static_cast<u32>(outer.left), static_cast<u32>(outer.top),
        static_cast<u32>(outer.right), static_cast<u32>(outer.bottom),
        static_cast<u32>(client.left), static_cast<u32>(client.top),
        static_cast<u32>(client.right), static_cast<u32>(client.bottom),
        win->style, win->ex_style, status,
        static_cast<u32>(border), static_cast<u32>(border),
        static_cast<u32>(cls ? cls->atom : win->class_atom) | (0x0500u << 16)};
    for (u32 i = 0; i < 14; ++i) (void)m.write_u32(pwi + 4 + i * 4, words[i]);
    m.set_return(1u);
}

// BOOL LockWindowUpdate(HWND hWndLock)
//
// Learn: only one window can be locked at a time; NULL unlocks. Measured
// (lockwindowupdate_*): locking while another window is locked, and
// unlocking when nothing is locked, fail with ERROR_INVALID_PARAMETER; an
// invalid window is ERROR_INVALID_WINDOW_HANDLE.
void User32ExtManager::impl_LockWindowUpdate(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    std::lock_guard lock(m_lock_update_mutex);
    if (hwnd == windowing::NULL_HWND) {
        if (m_lock_update_hwnd == windowing::NULL_HWND) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0u);
            return;
        }
        m_lock_update_hwnd = windowing::NULL_HWND;
        m.set_return(1u);
        return;
    }
    if (!window_exists(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    if (m_lock_update_hwnd != windowing::NULL_HWND) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    m_lock_update_hwnd = hwnd;
    // The lock is tracked; suppressing drawing into the locked window's DCs
    // (DCX_LOCKWINDOWUPDATE) is GDI clipping Vitreo does not apply.
    static std::once_flag once;
    unimplemented_once(once, "user32!LockWindowUpdate: drawing into the locked "
                             "window is not suppressed");
    m.set_return(1u);
}

// BOOL FlashWindow(HWND hWnd, BOOL bInvert) / BOOL FlashWindowEx(PFLASHWINFO)
//
// Learn: the return value is the window's state before the call — nonzero
// when its caption was drawn active. The macOS equivalent of a taskbar flash
// is Dock attention (.docs/windowing-native-presentation.md). Measured
// (flashwindow*): a bad window is ERROR_INVALID_PARAMETER for both, a bad
// cbSize is ERROR_INVALID_PARAMETER, a NULL FLASHWINFO is ERROR_NOACCESS.
void User32ExtManager::impl_FlashWindow(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const bool invert = m.arg(1) != 0;
    if (!window_exists(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    const bool was_active = m_active_window == hwnd;
    m_host.request_user_attention(hwnd, invert);
    m.set_return(was_active ? 1u : 0u);
}

void User32ExtManager::impl_FlashWindowEx(Marshaler& m) {
    const GuestAddr info = m.arg_ptr(0);
    if (info == 0) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0u);
        return;
    }
    // FLASHWINFO: cbSize, hwnd, dwFlags, uCount, dwTimeout.
    const bool is64 = m.cpu_is_64bit();
    const u32 expected = is64 ? 32u : 20u;
    const u32 cb = m.read_u32(info).value_or(0);
    const auto hwnd = static_cast<windowing::HWND>(
        is64 ? m.read_u64(info + 8).value_or(0) : m.read_u32(info + 4).value_or(0));
    const u32 flags = m.read_u32(info + (is64 ? 16 : 8)).value_or(0);
    if (cb != expected || !window_exists(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    const bool was_active = m_active_window == hwnd;
    constexpr u32 kFlashwStop = 0;
    m_host.request_user_attention(hwnd, flags != kFlashwStop);
    m.set_return(was_active ? 1u : 0u);
}

// BOOL PrintWindow(HWND hwnd, HDC hdcBlt, UINT nFlags)
//
// Learn: copies the visual window into the DC; PW_CLIENTONLY copies only
// the client area; PW_RENDERFULLCONTENT captures the composed content.
// Vitreo composes a window from its GDI framebuffer, so the copy is a blit of
// that surface. Measured (printwindow_*): an invalid window is
// ERROR_INVALID_WINDOW_HANDLE, unknown flags ERROR_INVALID_PARAMETER, and a
// NULL DC succeeds without drawing.
void User32ExtManager::impl_PrintWindow(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const auto hdc = static_cast<Handle>(m.arg(1));
    const u32 flags = static_cast<u32>(m.arg(2));
    constexpr u32 kPwClientOnly = 0x1;
    constexpr u32 kPwRenderFullContent = 0x2;
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    if ((flags & ~(kPwClientOnly | kPwRenderFullContent)) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    if (hdc == 0 || m_gdi_mgr == nullptr) {
        m.set_return(1u);
        return;
    }
    // The window DC covers the client area; a whole-window print places it
    // at the client offset inside the frame.
    const i32 dx = (flags & kPwClientOnly) ? 0 : win->client_offset.x;
    const i32 dy = (flags & kPwClientOnly) ? 0 : win->client_offset.y;
    const Handle window_dc = m_gdi_mgr->get_dc(static_cast<Handle>(hwnd));
    if (window_dc != 0) {
        constexpr u32 kSrcCopy = 0x00CC0020;
        (void)m_gdi_mgr->bit_blt(hdc, dx, dy, win->client_rect.width(),
                                 win->client_rect.height(), window_dc, 0, 0, kSrcCopy);
        (void)m_gdi_mgr->release_dc(static_cast<Handle>(hwnd), window_dc);
    }
    m.set_return(1u);
}

// DWORD GetGuiResources(HANDLE hProcess, DWORD uiFlags)
//
// Learn: GR_GDIOBJECTS (0), GR_USEROBJECTS (1), GR_GDIOBJECTS_PEAK (2),
// GR_USEROBJECTS_PEAK (4); hProcess may be GR_GLOBAL ((HANDLE)-2) for the
// session totals. Measured (getguiresources_*): other flags and an invalid
// process handle are ERROR_INVALID_PARAMETER.
void User32ExtManager::impl_GetGuiResources(Marshaler& m) {
    const u64 process = m.arg(0);
    const u32 flags = static_cast<u32>(m.arg(1));
    constexpr u32 kGdi = 0, kUser = 1, kGdiPeak = 2, kUserPeak = 4;
    if (flags != kGdi && flags != kUser && flags != kGdiPeak && flags != kUserPeak) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    const u64 current_process = m.cpu_is_64bit() ? ~0ull : 0xFFFFFFFFull;
    const u64 global = m.cpu_is_64bit() ? ~1ull : 0xFFFFFFFEull;
    if (process != current_process && process != global) {
        // Counting another process's objects needs that process's tables.
        static std::once_flag once;
        unimplemented_once(once, "user32!GetGuiResources: a process other than "
                                 "the caller");
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    const u32 gdi = m_gdi_mgr ? static_cast<u32>(m_gdi_mgr->live_object_count()) : 0u;
    const u32 user = static_cast<u32>(m_hwnd_mgr.count() + m_menu_records.size());
    u32 gdi_peak = m_gdi_peak.load(std::memory_order_relaxed);
    while (gdi > gdi_peak && !m_gdi_peak.compare_exchange_weak(gdi_peak, gdi)) {}
    u32 user_peak = m_user_peak.load(std::memory_order_relaxed);
    while (user > user_peak && !m_user_peak.compare_exchange_weak(user_peak, user)) {}
    u32 value = 0;
    switch (flags) {
    case kGdi: value = gdi; break;
    case kUser: value = user; break;
    case kGdiPeak: value = std::max(gdi, m_gdi_peak.load()); break;
    default: value = std::max(user, m_user_peak.load()); break;
    }
    m.set_return(value);
}

// HWND ChildWindowFromPoint(HWND hWndParent, POINT Point)
// Learn: ChildWindowFromPointEx with CWP_ALL.
void User32ExtManager::impl_ChildWindowFromPoint(Marshaler& m) {
    const auto parent = static_cast<windowing::HWND>(m.arg(0));
    i32 x = 0;
    i32 y = 0;
    if (m.cpu_is_64bit()) {
        // Win64 passes the eight-byte POINT value in RDX.
        const u64 packed = m.arg(1);
        x = static_cast<i32>(static_cast<u32>(packed));
        y = static_cast<i32>(static_cast<u32>(packed >> 32));
    } else {
        x = static_cast<i32>(m.arg(1));
        y = static_cast<i32>(m.arg(2));
    }
    m.set_return(m_hwnd_mgr.child_window_from_point(parent, windowing::Point{x, y}, 0));
}

// ---------------------------------------------------------------------------
// ShutdownBlockReasonCreate / Query / Destroy.
//
// Contract: Microsoft Learn (Vista+): a window's reason text shown when the
// system shuts down while the application blocks it; only the thread that
// created the window may set or destroy it (ERROR_ACCESS_DENIED). Measured
// (s1win_misc_probe "-- shutdownblock"): an invalid window is
// ERROR_INVALID_WINDOW_HANDLE, a NULL reason ERROR_INVALID_PARAMETER, a
// query or destroy with no reason set ERROR_INVALID_PARAMETER; a second
// create replaces the text; a query reports the length including the NUL
// and, when the buffer is too small, returns TRUE with that length and
// writes nothing.
// ---------------------------------------------------------------------------

void User32ExtManager::impl_ShutdownBlockReasonCreate(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const GuestAddr reason = m.arg_ptr(1);
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    if (win->owner_thread_id != ThreadManager::get_current_thread_id_static()) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        m.set_return(0u);
        return;
    }
    if (reason == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    std::wstring text;
    if (auto r = m.read_wide_string(reason); r.is_ok()) text = utf8_to_wstring(r.value());
    std::lock_guard lock(m_shutdown_reason_mutex);
    m_shutdown_reasons[hwnd] = std::move(text);
    m.set_return(1u);
}

void User32ExtManager::impl_ShutdownBlockReasonQuery(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const GuestAddr buffer = m.arg_ptr(1);
    const GuestAddr pcch = m.arg_ptr(2);
    if (!m_hwnd_mgr.is_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    std::lock_guard lock(m_shutdown_reason_mutex);
    auto it = m_shutdown_reasons.find(hwnd);
    if (it == m_shutdown_reasons.end() || pcch == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    const u32 need = static_cast<u32>(it->second.size()) + 1;
    const u32 have = m_memory.read_u32(pcch).value_or(0);
    if (buffer != 0 && have >= need) {
        for (u32 i = 0; i + 1 < need; ++i) {
            (void)m_memory.write_u16(buffer + i * 2, static_cast<u16>(it->second[i]));
        }
        (void)m_memory.write_u16(buffer + (need - 1) * 2, 0);
    }
    (void)m_memory.write_u32(pcch, need);
    m.set_return(1u);
}

void User32ExtManager::impl_ShutdownBlockReasonDestroy(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    if (win->owner_thread_id != ThreadManager::get_current_thread_id_static()) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        m.set_return(0u);
        return;
    }
    std::lock_guard lock(m_shutdown_reason_mutex);
    if (m_shutdown_reasons.erase(hwnd) == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    m.set_return(1u);
}

} // namespace vitreo::winapi
