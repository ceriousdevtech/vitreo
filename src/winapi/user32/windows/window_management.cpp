// SPDX-License-Identifier: LGPL-2.1-only

/// @file window_management.cpp
/// @brief Layered windows, Z-order, placement, activation, and WindowFromPoint.
///
/// UpdateLayeredWindow*, SetWindowRgn, SetWindowPlacement, DeferWindowPos,
/// GetWindow/GetTopWindow, and the activation/focus helpers that keep the
/// active HWND coherent with Cocoa's key window.
///
/// Closed cut from user32_ext.cpp: nothing these functions define is used
/// elsewhere in that file, and they use nothing file-local from elsewhere —
/// verified in both directions with tools/architecture/plan_extraction.py
/// before moving.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "winapi/user32/messages/message_loop.hpp"
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

// ----- Window Management APIs -----

namespace {

// Shared body for UpdateLayeredWindow / UpdateLayeredWindowIndirect.
// Returns the BOOL Windows would write into EAX; sets LastError on failure.
bool apply_update_layered_window(
    windowing::HwndManager& hwnd_mgr,
    GdiManager* gdi_mgr,
    Marshaler& m,
    u32 hwnd,
    Handle hdc_src,
    GuestAddr ppt_dst,
    GuestAddr psize,
    GuestAddr ppt_src,
    GuestAddr pblend,
    u32 flags,
    const char* api_tag) {
    constexpr u32 ULW_ALPHA  = 0x00000002;
    constexpr u32 ULW_OPAQUE = 0x00000004;

    if (hwnd == 0) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }
    // Windows allows a NULL hdcSrc only when the caller is merely moving or
    // resizing the window (no new pixels). Nothing to blit in that case.
    if (hdc_src == 0) {
        VITREO_TRACE("[u32ext] %s hwnd=0x%x -> TRUE (no source, position/size only)\n",
                     api_tag, hwnd);
        return true;
    }
    if (gdi_mgr == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }

    i32 src_x = 0, src_y = 0;
    if (ppt_src != 0) {
        auto x = m.read_u32(ppt_src);
        auto y = m.read_u32(ppt_src + 4);
        if (x.is_ok()) src_x = static_cast<i32>(x.value());
        if (y.is_ok()) src_y = static_cast<i32>(y.value());
    }

    i32 width = 0, height = 0;
    if (psize != 0) {
        auto cx = m.read_u32(psize);
        auto cy = m.read_u32(psize + 4);
        if (cx.is_ok()) width = static_cast<i32>(cx.value());
        if (cy.is_ok()) height = static_cast<i32>(cy.value());
    }
    if (width <= 0 || height <= 0) {
        if (const windowing::WindowData* win =
                hwnd_mgr.find(static_cast<windowing::HWND>(hwnd))) {
            width = win->rect.right - win->rect.left;
            height = win->rect.bottom - win->rect.top;
        }
    }
    if (width <= 0 || height <= 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }

    u8 const_alpha = 255;
    bool per_pixel_alpha = false;
    if ((flags & ULW_ALPHA) != 0 && pblend != 0) {
        auto bf = m.read_u32(pblend);
        if (bf.is_ok()) {
            const_alpha = static_cast<u8>((bf.value() >> 16) & 0xFF);
            per_pixel_alpha = (((bf.value() >> 24) & 0xFF) & 0x01) != 0;
        }
    }
    if ((flags & ULW_OPAQUE) != 0) {
        const_alpha = 255;
        per_pixel_alpha = false;
    }

    Handle hdc_win = gdi_mgr->get_dc(static_cast<Handle>(hwnd));
    if (hdc_win == 0) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }
    // Learn UpdateLayeredWindow: hdcSrc's pixels REPLACE the window's
    // content; the system composes them onto the screen with pblend
    // (premultiplied with AC_SRC_ALPHA). Nothing of the previous update or of
    // the unpainted window survives, so transparent source pixels are holes.
    if (std::getenv("VITREO_ULW_DIAG")) gdi_mgr->debug_report_dc_alpha(hdc_src);
    const bool opaque = (flags & ULW_OPAQUE) != 0 || (flags & ULW_ALPHA) == 0;
    const bool ok = gdi_mgr->compose_layered_window(
        hdc_win, hdc_src, src_x, src_y, width, height,
        const_alpha, per_pixel_alpha, opaque);
    if (ok) gdi_mgr->flush_window(hwnd);
    (void)gdi_mgr->release_dc(static_cast<Handle>(hwnd), hdc_win);

    VITREO_TRACE("[u32ext] %s hwnd=0x%x src=0x%x at(%d,%d)"
                 " %dx%d flags=0x%x ca=%u per_pixel=%d -> %s\n",
                 api_tag, hwnd, static_cast<u32>(hdc_src), src_x, src_y,
                 width, height, flags, static_cast<unsigned>(const_alpha),
                 per_pixel_alpha ? 1 : 0, ok ? "TRUE" : "FALSE");
    (void)ppt_dst;  // Window placement is owned by SetWindowPos here.
    if (!ok) set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    return ok;
}

}  // namespace

namespace {

constexpr u32 kWsExLayered = 0x00080000u;
constexpr u32 kLwaColorKey = 0x00000001u;
constexpr u32 kLwaAlpha = 0x00000002u;
constexpr u32 kUlwColorKey = 0x00000001u;
constexpr u32 kUlwValidFlags = 0x0000000Fu;  // COLORKEY|ALPHA|OPAQUE|EX_NORESIZE

void report_layered_unimplemented(const char* api, const char* what) {
    static std::atomic_flag reported_slwa = ATOMIC_FLAG_INIT;
    static std::atomic_flag reported_ulw = ATOMIC_FLAG_INIT;
    std::atomic_flag& flag =
        std::strcmp(api, "SetLayeredWindowAttributes") == 0 ? reported_slwa : reported_ulw;
    if (!flag.test_and_set()) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!%s: %s\n", api, what);
    }
}

}  // namespace

void User32ExtManager::layered_style_cleared(windowing::HWND hwnd) {
    bool had_alpha = false;
    {
        std::lock_guard lock(m_layered_mutex);
        auto it = m_layered.find(hwnd);
        if (it == m_layered.end()) return;
        had_alpha = it->second.mode == LayeredState::Mode::attributes &&
                    (it->second.flags & kLwaAlpha) != 0;
        m_layered.erase(it);
    }
    (void)m_host.set_window_per_pixel_alpha(hwnd, false);
    if (had_alpha) (void)m_host.set_window_opacity(hwnd, 255);
}

bool User32ExtManager::update_layered_window(Marshaler& m, u32 hwnd, u32 hdc_src,
                                             GuestAddr ppt_dst, GuestAddr psize,
                                             GuestAddr ppt_src, GuestAddr pblend,
                                             u32 flags, const char* api_tag) {
    // Measured (s1win_dwm_probe "-- layered"): an unknown window fails with
    // ERROR_INVALID_WINDOW_HANDLE; a window without WS_EX_LAYERED, an
    // undefined flag, or a window whose layering was set by
    // SetLayeredWindowAttributes fails with ERROR_INVALID_PARAMETER.
    const auto h = static_cast<windowing::HWND>(hwnd);
    const windowing::WindowData* win = m_hwnd_mgr.find(h);
    if (win == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }
    if ((win->ex_style & kWsExLayered) == 0 || (flags & ~kUlwValidFlags) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    {
        std::lock_guard lock(m_layered_mutex);
        auto& state = m_layered[h];
        if (state.mode == LayeredState::Mode::attributes) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return false;
        }
    }
    if ((flags & kUlwColorKey) != 0 && hdc_src != 0) {
        // The composed pixels would need crKey keyed out before
        // presentation; the window surface has no such stage.
        report_layered_unimplemented(api_tag, "ULW_COLORKEY");
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        return false;
    }
    {
        std::lock_guard lock(m_layered_mutex);
        m_layered[h].mode = LayeredState::Mode::update;
    }

    // Learn: pptDst / psize give the new screen position and size; NULL
    // means unchanged.
    if (ppt_dst != 0 || psize != 0) {
        const windowing::Rect rect = win->rect;
        i32 x = rect.left, y = rect.top;
        i32 cx = rect.right - rect.left, cy = rect.bottom - rect.top;
        if (ppt_dst != 0) {
            x = static_cast<i32>(m.read_u32(ppt_dst).value_or(static_cast<u32>(x)));
            y = static_cast<i32>(m.read_u32(ppt_dst + 4).value_or(static_cast<u32>(y)));
        }
        if (psize != 0) {
            cx = static_cast<i32>(m.read_u32(psize).value_or(static_cast<u32>(cx)));
            cy = static_cast<i32>(m.read_u32(psize + 4).value_or(static_cast<u32>(cy)));
        }
        if (x != rect.left || y != rect.top ||
            cx != rect.right - rect.left || cy != rect.bottom - rect.top) {
            constexpr u32 kSwpNoZOrder = 0x0004u;
            constexpr u32 kSwpNoActivate = 0x0010u;
            (void)apply_set_window_pos(h, windowing::NULL_HWND, x, y, cx, cy,
                                       kSwpNoZOrder | kSwpNoActivate);
        }
    }

    // The window now shows only what UpdateLayeredWindow composes, alpha
    // included; arm that before the first composed frame is presented.
    if (hdc_src != 0) (void)m_host.set_window_per_pixel_alpha(h, true);
    return apply_update_layered_window(
        m_hwnd_mgr, m_gdi_mgr, m, hwnd, static_cast<Handle>(hdc_src), ppt_dst,
        psize, ppt_src, pblend, flags, api_tag);
}

void User32ExtManager::impl_UpdateLayeredWindow(Marshaler& m) {
    // BOOL UpdateLayeredWindow(HWND hWnd, HDC hdcDst, POINT *pptDst,
    //     SIZE *psize, HDC hdcSrc, POINT *pptSrc, COLORREF crKey,
    //     BLENDFUNCTION *pblend, DWORD dwFlags)
    // 9 args, stdcall_bytes=36.
    //
    // This is the only way a layered window's pixels reach the screen: the
    // caller composites its frame into hdcSrc and hands it over here. A
    // success return that does not present hdcSrc leaves the window blank.
    const auto hwnd = static_cast<u32>(m.arg(0));
    const auto hdc_src = static_cast<u32>(m.arg(4));
    const GuestAddr ppt_dst = m.arg_ptr(2);
    const GuestAddr psize = m.arg_ptr(3);
    const GuestAddr ppt_src = m.arg_ptr(5);
    const GuestAddr pblend = m.arg_ptr(7);
    const auto flags = static_cast<u32>(m.arg32(8));

    const bool ok = update_layered_window(m, hwnd, hdc_src, ppt_dst, psize,
                                          ppt_src, pblend, flags,
                                          "UpdateLayeredWindow");
    m.set_return(ok ? 1u : 0u);
}

void User32ExtManager::impl_UpdateLayeredWindowIndirect(Marshaler& m) {
    // BOOL UpdateLayeredWindowIndirect(HWND hwnd,
    //     const UPDATELAYEREDWINDOWINFO *pULWInfo)
    // stdcall_bytes=8.
    //
    // Same contract as UpdateLayeredWindow; arguments are packed in a struct.
    // prcDirty is accepted but ignored for now (full-window update).
    const auto hwnd = static_cast<u32>(m.arg(0));
    const GuestAddr info = m.arg_ptr(1);
    if (hwnd == 0 || info == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const bool is64 = m.cpu_is_64bit();
    // MSVC layout: after DWORD cbSize, pointer-sized fields are pointer-aligned.
    const GuestAddr off_hdc_src = is64 ? 32 : 16;
    const GuestAddr off_ppt_dst = is64 ? 16 : 8;
    const GuestAddr off_psize = is64 ? 24 : 12;
    const GuestAddr off_ppt_src = is64 ? 40 : 20;
    const GuestAddr off_pblend = is64 ? 56 : 28;
    const GuestAddr off_flags = is64 ? 64 : 32;

    auto cb = m.read_u32(info);
    if (cb.is_err() || cb.value() < (is64 ? 72u : 36u)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const u32 hdc_src = static_cast<u32>(m.read_ptr(info + off_hdc_src).value_or(0));
    const GuestAddr ppt_dst = static_cast<GuestAddr>(m.read_ptr(info + off_ppt_dst).value_or(0));
    const GuestAddr psize = static_cast<GuestAddr>(m.read_ptr(info + off_psize).value_or(0));
    const GuestAddr ppt_src = static_cast<GuestAddr>(m.read_ptr(info + off_ppt_src).value_or(0));
    const GuestAddr pblend = static_cast<GuestAddr>(m.read_ptr(info + off_pblend).value_or(0));
    const u32 flags = m.read_u32(info + off_flags).value_or(0);

    const bool ok = update_layered_window(m, hwnd, hdc_src, ppt_dst, psize,
                                          ppt_src, pblend, flags,
                                          "UpdateLayeredWindowIndirect");
    m.set_return(ok ? 1u : 0u);
}

void User32ExtManager::impl_SetLayeredWindowAttributes(Marshaler& m) {
    // BOOL SetLayeredWindowAttributes(HWND, COLORREF crKey, BYTE bAlpha,
    //                                 DWORD dwFlags)
    // Measured (s1win_dwm_probe "-- layered", x64 and x86 identical):
    //  * unknown window: FALSE, ERROR_INVALID_WINDOW_HANDLE;
    //  * no WS_EX_LAYERED, or a flag other than LWA_COLORKEY|LWA_ALPHA:
    //    FALSE, ERROR_INVALID_PARAMETER;
    //  * success leaves the last error alone; bAlpha is kept only when
    //    LWA_ALPHA is given, crKey only when LWA_COLORKEY is given, and the
    //    flags are replaced (alpha 128 survives a later LWA_COLORKEY call);
    //  * it also works after UpdateLayeredWindow, switching the window to
    //    attribute layering.
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const u32 key = static_cast<u32>(m.arg32(1));
    const u8 alpha = static_cast<u8>(m.arg32(2) & 0xFFu);
    const u32 flags = static_cast<u32>(m.arg32(3));
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (win == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    if ((win->ex_style & kWsExLayered) == 0 ||
        (flags & ~(kLwaColorKey | kLwaAlpha)) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    if ((flags & kLwaColorKey) != 0) {
        // Keying crKey out of the window's pixels needs a presentation stage
        // the window surface does not have.
        report_layered_unimplemented("SetLayeredWindowAttributes", "LWA_COLORKEY");
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(0u);
        return;
    }
    bool had_alpha = false;
    {
        std::lock_guard lock(m_layered_mutex);
        const auto it = m_layered.find(hwnd);
        had_alpha = it != m_layered.end() &&
                    it->second.mode == LayeredState::Mode::attributes &&
                    (it->second.flags & kLwaAlpha) != 0;
    }
    const u8 opacity = (flags & kLwaAlpha) != 0 ? alpha : 255;
    if (((flags & kLwaAlpha) != 0 || had_alpha) &&
        !m_host.set_window_opacity(hwnd, opacity)) {
        // A layered child window (Windows 8+) has no native window of its
        // own to fade.
        report_layered_unimplemented("SetLayeredWindowAttributes",
                                     "LWA_ALPHA on a window without a native surface");
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(0u);
        return;
    }
    {
        std::lock_guard lock(m_layered_mutex);
        auto& state = m_layered[hwnd];
        state.mode = LayeredState::Mode::attributes;
        if ((flags & kLwaColorKey) != 0) state.key = key;
        if ((flags & kLwaAlpha) != 0) state.alpha = alpha;
        state.flags = flags;
    }
    // Attribute layering presents ordinary painting, which is opaque.
    (void)m_host.set_window_per_pixel_alpha(hwnd, false);
    VITREO_TRACE("[u32ext] SetLayeredWindowAttributes hwnd=0x%x key=0x%x alpha=%u flags=0x%x -> TRUE\n",
                 static_cast<u32>(hwnd), key, alpha, flags);
    m.set_return(1u);
}

void User32ExtManager::impl_GetLayeredWindowAttributes(Marshaler& m) {
    // BOOL GetLayeredWindowAttributes(HWND, COLORREF*, BYTE*, DWORD*)
    // Measured (s1win_dwm_probe): no WS_EX_LAYERED -> FALSE,
    // ERROR_INVALID_PARAMETER; layered but never given attributes (or
    // composed with UpdateLayeredWindow) -> FALSE with the last error
    // untouched; NULL outputs are allowed.
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const GuestAddr key_out = m.arg_ptr(1);
    const GuestAddr alpha_out = m.arg_ptr(2);
    const GuestAddr flags_out = m.arg_ptr(3);
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (win == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    if ((win->ex_style & kWsExLayered) == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    LayeredState state;
    {
        std::lock_guard lock(m_layered_mutex);
        const auto it = m_layered.find(hwnd);
        if (it != m_layered.end()) state = it->second;
    }
    if (state.mode != LayeredState::Mode::attributes) {
        m.set_return(0u);
        return;
    }
    if (key_out != 0) (void)m.write_u32(key_out, state.key);
    if (alpha_out != 0) (void)m.write_u8(alpha_out, state.alpha);
    if (flags_out != 0) (void)m.write_u32(flags_out, state.flags);
    m.set_return(1u);
}


void User32ExtManager::impl_BringWindowToTop(Marshaler& m) {
    // Learn (nf-winuser-bringwindowtotop): activates the window and places it
    // at the top of the Z order among windows of its type.
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    if (m_hwnd_mgr.find(hwnd) == nullptr) {
        if (!ipc::hwnd_registry_lookup(static_cast<u32>(hwnd))) {
            set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        }
        m.set_return(0u);
        return;
    }
    // Learn BringWindowToTop: top of the Z order among windows of its type;
    // a top-level window is also activated — SetWindowPos(HWND_TOP,
    // SWP_NOMOVE | SWP_NOSIZE) with its WM_WINDOWPOSCHANGING/CHANGED.
    constexpr u32 kSwpNoSize = 0x0001;
    constexpr u32 kSwpNoMove = 0x0002;
    const bool ok = apply_set_window_pos(hwnd, windowing::HWND_TOP, 0, 0, 0, 0,
                                         kSwpNoSize | kSwpNoMove, true);
    VITREO_TRACE("[u32ext] BringWindowToTop hwnd=0x%x -> %u\n", hwnd, ok ? 1u : 0u);
    m.set_return(ok ? 1u : 0u);
}

void User32ExtManager::impl_SetWindowRgn(Marshaler& m) {
    // MS Learn: SetWindowRgn(hWnd, hRgn, bRedraw) — the system owns hRgn after
    // success; nothing outside the region is displayed. An empty region
    // (CreateRectRgn(0,0,0,0) on a WS_POPUP) hides the window until a later
    // region is applied.
    const u32 hwnd = static_cast<u32>(m.arg(0));
    const u32 hrgn = static_cast<u32>(m.arg(1));
    const bool redraw = m.arg(2) != 0;

    // Not a window: FALSE, error untouched ("SetWindowRgn(bad)=0").
    if (hwnd == 0 || !m_hwnd_mgr.is_valid(static_cast<windowing::HWND>(hwnd))) {
        m.set_return(0u);
        return;
    }

    if (hrgn == 0) {
        {
            std::lock_guard lock(m_window_state_mutex);
            m_window_regions.erase(hwnd);
        }
        m_host.set_window_rgn(
            static_cast<windowing::HWND>(hwnd),
            /*has_rgn=*/false, 0, 0, 0, 0, redraw, {});
        VITREO_TRACE("[u32ext] SetWindowRgn hwnd=0x%x hrgn=NULL redraw=%u -> TRUE\n",
                     hwnd, redraw ? 1u : 0u);
        m.set_return(1u);
        return;
    }

    ClipRect rgn{};
    if (!m_gdi_mgr || !m_gdi_mgr->get_region_rect(hrgn, rgn)) {
        VITREO_TRACE(
            "[u32ext] SetWindowRgn hwnd=0x%x hrgn=0x%x -> FALSE (unknown region)\n",
            hwnd, hrgn);
        m.set_return(0u);
        return;
    }

    // The system owns the region from here: GetWindowRgn copies it, and the
    // caller's handle is gone ("DeleteObject(owned rgn)=0").
    {
        std::lock_guard lock(m_window_state_mutex);
        m_window_regions[hwnd] = WindowRegion{
            {rgn.left, rgn.top, rgn.right, rgn.bottom}, rgn.rectangles};
    }
    (void)m_gdi_mgr->delete_object(hrgn);

    // The whole region, not just its bounding box: a shaped window (a
    // splash image's opaque pixels, a rounded skin) is a union of rectangles.
    m_host.set_window_rgn(
        static_cast<windowing::HWND>(hwnd),
        /*has_rgn=*/true, rgn.left, rgn.top, rgn.right, rgn.bottom, redraw,
        rgn.rectangles);
    const bool win_diag = windowing::diag::win_diag();
    if (win_diag) {
        std::fprintf(stderr,
                     "[hwnd] SetWindowRgn hwnd=0x%x bounds=(%d,%d)-(%d,%d) "
                     "rects=%zu redraw=%u\n",
                     hwnd, rgn.left, rgn.top, rgn.right, rgn.bottom,
                     rgn.rectangles.size(), redraw ? 1u : 0u);
    }
    VITREO_TRACE(
        "[u32ext] SetWindowRgn hwnd=0x%x hrgn=0x%x rect=(%d,%d)-(%d,%d) "
        "empty=%u redraw=%u -> TRUE\n",
        hwnd, hrgn, rgn.left, rgn.top, rgn.right, rgn.bottom,
        rgn.is_empty() ? 1u : 0u, redraw ? 1u : 0u);
    m.set_return(1u);
}

void User32ExtManager::impl_GetWindowRgn(Marshaler& m) {
    // A copy of the window's region and its type; 0 (ERROR, error untouched)
    // without one or for a bad window ("GetWindowRgn(none)=0",
    // "GetWindowRgn=3").
    const u32 hwnd = static_cast<u32>(m.arg(0));
    const auto hrgn = static_cast<Handle>(m.arg(1));
    if (m_gdi_mgr == nullptr) {
        m.set_return(0u);
        return;
    }
    ClipRect r;
    {
        std::lock_guard lock(m_window_state_mutex);
        const auto it = m_window_regions.find(hwnd);
        if (it == m_window_regions.end()) {
            m.set_return(0u);
            return;
        }
        r.left = it->second.bounds[0];
        r.top = it->second.bounds[1];
        r.right = it->second.bounds[2];
        r.bottom = it->second.bounds[3];
        r.rectangles = it->second.rects;
    }
    r.valid = true;
    if (!m_gdi_mgr->set_region(hrgn, r)) {
        m.set_return(0u);
        return;
    }
    m.set_return(r.is_empty() ? 1u : (r.rectangles.empty() ? 2u : 3u));
}

void User32ExtManager::impl_SetWindowPlacement(Marshaler& m) {
    constexpr u32 kWindowPlacementSize = 44;
    constexpr u32 kKnownFlags = 0x0001u | 0x0002u | 0x0004u;

    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const GuestAddr placement = m.arg_ptr(1);
    if (!m_hwnd_mgr.is_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    if (placement == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }

    const auto length = m.read_u32(placement + 0);
    const auto flags = m.read_u32(placement + 4);
    const auto show_cmd = m.read_u32(placement + 8);
    const auto min_x = m.read_u32(placement + 12);
    const auto min_y = m.read_u32(placement + 16);
    const auto max_x = m.read_u32(placement + 20);
    const auto max_y = m.read_u32(placement + 24);
    const auto left = m.read_u32(placement + 28);
    const auto top = m.read_u32(placement + 32);
    const auto right = m.read_u32(placement + 36);
    const auto bottom = m.read_u32(placement + 40);
    if (length.is_err() || flags.is_err() || show_cmd.is_err() ||
        min_x.is_err() || min_y.is_err() || max_x.is_err() ||
        max_y.is_err() || left.is_err() || top.is_err() ||
        right.is_err() || bottom.is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0u);
        return;
    }
    if (length.value() != kWindowPlacementSize ||
        (flags.value() & ~kKnownFlags) != 0 ||
        show_cmd.value() > static_cast<u32>(windowing::SW::FORCEMINIMIZE)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    if (!m_show_mgr) {
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        m.set_return(0u);
        return;
    }

    const bool applied = m_show_mgr->set_window_placement(
        hwnd, flags.value(), static_cast<int>(show_cmd.value()),
        windowing::Point{static_cast<i32>(min_x.value()),
                         static_cast<i32>(min_y.value())},
        windowing::Point{static_cast<i32>(max_x.value()),
                         static_cast<i32>(max_y.value())},
        windowing::Rect{static_cast<i32>(left.value()),
                        static_cast<i32>(top.value()),
                        static_cast<i32>(right.value()),
                        static_cast<i32>(bottom.value())});
    if (!applied) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }

    VITREO_TRACE(
        "[u32ext] SetWindowPlacement hwnd=0x%x flags=0x%x show=%u "
        "normal=(%d,%d)-(%d,%d) -> TRUE\n",
        hwnd, flags.value(), show_cmd.value(),
        static_cast<i32>(left.value()), static_cast<i32>(top.value()),
        static_cast<i32>(right.value()), static_cast<i32>(bottom.value()));
    m.set_return(1u);
}

void User32ExtManager::impl_GetTopWindow(Marshaler& m) {
    // Deliberately model-only: z-order is owned by HwndManager. This query
    // must not consult the native host or make AppKit ordering observable.
    auto hwnd = static_cast<windowing::HWND>(m.arg(0));

    windowing::HWND parent = hwnd;
    if (parent == 0) {
        parent = windowing::HWND_DESKTOP;
    }

    auto children = m_hwnd_mgr.enum_children(parent);
    windowing::HWND result = children.empty() ? 0u : children.front();
    VITREO_TRACE("[u32ext] GetTopWindow hwnd=0x%x -> 0x%x\n", hwnd, result);
    m.set_return(result);
}

void User32ExtManager::impl_GetWindow(Marshaler& m) {
    auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    auto uCmd = static_cast<u32>(m.arg(1));

    constexpr u32 GW_HWNDFIRST    = 0;
    constexpr u32 GW_HWNDLAST     = 1;
    constexpr u32 GW_HWNDNEXT     = 2;
    constexpr u32 GW_HWNDPREV     = 3;
    constexpr u32 GW_OWNER        = 4;
    constexpr u32 GW_CHILD        = 5;
    constexpr u32 GW_ENABLEDPOPUP = 6;

    windowing::HWND result = 0;

    // Measured (user32_windows_probe gw_invalid_hwnd / gw_bad_cmd): a bad
    // window is ERROR_INVALID_WINDOW_HANDLE, a uCmd above GW_ENABLEDPOPUP is
    // ERROR_INVALID_GW_COMMAND (1443); every other NULL leaves the error.
    if (m_hwnd_mgr.find(hwnd) == nullptr) {
        // Another capsule process's window is a valid HWND; its
        // relationships live in that process, so none is reported here.
        if (!ipc::hwnd_registry_lookup(static_cast<u32>(hwnd))) {
            set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        }
        m.set_return(0);
        return;
    }
    if (uCmd > GW_ENABLEDPOPUP) {
        set_last_error(1443u);  // ERROR_INVALID_GW_COMMAND (winerror.h)
        m.set_return(0);
        return;
    }

    auto get_siblings = [&](windowing::HWND target) {
        std::vector<windowing::HWND> siblings;
        const auto* wd = m_hwnd_mgr.find(target);
        if (!wd) {
            return siblings;
        }
        windowing::HWND parent = wd->parent;
        if (parent == 0) {
            parent = windowing::HWND_DESKTOP;
        }
        siblings = m_hwnd_mgr.enum_children(parent);
        return siblings;
    };

    switch (uCmd) {
    case GW_OWNER: {
        const auto* wd = m_hwnd_mgr.find(hwnd);
        if (wd) {
            bool is_child = (wd->style & windowing::WS::CHILD) != 0;
            if (!is_child) {
                result = wd->owner;
            }
        }
        break;
    }
    case GW_CHILD: {
        auto children = m_hwnd_mgr.enum_children(hwnd);
        if (!children.empty()) {
            result = children.front();
        }
        break;
    }
    case GW_ENABLEDPOPUP: {
        // The first visible, enabled window owned by hWnd, in GW_HWNDNEXT
        // order. Measured (user32_windows_probe gw_enabledpopup_*): NULL --
        // never hWnd itself -- when there is none, including when the owned
        // popup is hidden or disabled.
        for (const auto top : m_hwnd_mgr.enum_children(windowing::HWND_DESKTOP)) {
            const auto* wd = m_hwnd_mgr.find(top);
            if (wd != nullptr && top != hwnd && wd->owner == hwnd &&
                (wd->style & windowing::WS::CHILD) == 0 && wd->visible &&
                wd->enabled) {
                result = top;
                break;
            }
        }
        break;
    }
    case GW_HWNDFIRST:
    case GW_HWNDLAST:
    case GW_HWNDNEXT:
    case GW_HWNDPREV: {
        auto siblings = get_siblings(hwnd);
        auto it = std::find(siblings.begin(), siblings.end(), hwnd);
        if (it == siblings.end()) {
            break;
        }
        if (uCmd == GW_HWNDFIRST) {
            result = siblings.front();
        } else if (uCmd == GW_HWNDLAST) {
            result = siblings.back();
        } else if (uCmd == GW_HWNDNEXT) {
            auto next = it;
            ++next;
            if (next != siblings.end()) {
                result = *next;
            }
        } else {
            if (it != siblings.begin()) {
                --it;
                result = *it;
            }
        }
        break;
    }
    default:
        break;
    }

    VITREO_TRACE("[u32ext] GetWindow hwnd=0x%x cmd=%u -> 0x%x\n", hwnd, uCmd, result);
    m.set_return(result);
}

void User32ExtManager::impl_GetLastActivePopup(Marshaler& m) {
    u32 hwnd = static_cast<u32>(m.arg(0));
    // lastactivepopup_invalid: NULL / ERROR_INVALID_WINDOW_HANDLE.
    if (!m_hwnd_mgr.is_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    windowing::HWND result = hwnd;
    auto top = m_hwnd_mgr.enum_children(windowing::HWND_DESKTOP);
    for (windowing::HWND candidate : top) {
        const windowing::WindowData* wd = m_hwnd_mgr.find(candidate);
        if (!wd || wd->owner != hwnd) {
            continue;
        }
        if (wd->visible && wd->enabled) {
            result = candidate;
        }
    }
    VITREO_TRACE("[u32ext] GetLastActivePopup hwnd=0x%x -> 0x%x\n", hwnd, result);
    m.set_return(result);
}

void User32ExtManager::impl_SetActiveWindow(Marshaler& m) {
    // Measured by activation_order_probe: NULL returns NULL and changes
    // nothing; an invalid handle fails with ERROR_INVALID_WINDOW_HANDLE; a
    // child window returns the current active window and changes nothing.
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const windowing::HWND active = m_active_window;
    const windowing::HWND prev = window_in_caller_queue(active) ? active : 0;
    if (hwnd == windowing::NULL_HWND) {
        m.set_return(0u);
        return;
    }
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (win == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    // Another (unattached) thread's window: NULL with the last error
    // untouched, nothing activated (s2msg_input_probe
    // setactive_other_thread_null).
    if (!window_in_caller_queue(hwnd)) {
        m.set_return(0u);
        return;
    }
    if (win->parent == windowing::HWND_DESKTOP ||
        win->parent == windowing::NULL_HWND) {
        (void)change_active_window(hwnd, windowing::WA::ACTIVE);
        set_last_error(0);  // setactive_own err=0
    }
    VITREO_TRACE("[u32ext] SetActiveWindow hwnd=0x%x -> prev=0x%x\n", hwnd, prev);
    m.set_return(prev);
}

void User32ExtManager::impl_GetActiveWindow(Marshaler& m) {
    // Learn GetActiveWindow: the active window "attached to the calling
    // thread's message queue", else NULL.
    const windowing::HWND active = m_active_window;
    const windowing::HWND result = window_in_caller_queue(active) ? active : 0;
    VITREO_TRACE("[u32ext] GetActiveWindow -> 0x%x\n", result);
    m.set_return(result);
}

namespace {

bool is_top_level_window(const windowing::WindowData& win) {
    return win.parent == windowing::HWND_DESKTOP ||
           win.parent == windowing::NULL_HWND;
}

} // namespace

namespace {
// Set by NativeDeliveryScope on the thread delivering an AppKit-originated
// activation or focus change (the host main thread).
thread_local bool t_native_delivery = false;
} // namespace

User32ExtManager::NativeDeliveryScope::NativeDeliveryScope() noexcept
    : m_previous(t_native_delivery) {
    t_native_delivery = true;
}

User32ExtManager::NativeDeliveryScope::~NativeDeliveryScope() {
    t_native_delivery = m_previous;
}

bool User32ExtManager::change_active_window(windowing::HWND next, u32 state,
                                            u32 other_thread_id) {
    if (next != windowing::NULL_HWND) {
        const windowing::WindowData* win = m_hwnd_mgr.find(next);
        if (win == nullptr || !is_top_level_window(*win)) {
            return false;
        }
    }
    // Native (Cocoa main thread) and guest-thread activation can race for the
    // same transition. Claim it atomically so the loser sees it as done
    // instead of sending a second activation sequence.
    windowing::HWND prev = windowing::NULL_HWND;
    bool gaining_application = false;
    bool losing_application = false;
    {
        std::lock_guard<std::mutex> lock(m_activation_mutex);
        prev = m_active_window;
        if (prev != windowing::NULL_HWND && !m_hwnd_mgr.is_valid(prev)) {
            prev = windowing::NULL_HWND;
        }
        gaining_application =
            next != windowing::NULL_HWND && !m_application_active;
        losing_application =
            next == windowing::NULL_HWND && m_application_active;
        if (next == prev && !gaining_application && !losing_application) {
            return false;
        }
        m_active_window = next;
        m_application_active = next != windowing::NULL_HWND;
    }

    // Returns true when a guest procedure received the message and returned
    // zero (for a dialog procedure: did not process it). Under a
    // NativeDeliveryScope a window of another thread gets the message queued
    // as a sent message instead: the result is not known here, so
    // @p on_unprocessed (the caller's "returned zero" handling) runs with the
    // message on the receiving thread and this returns false.
    const auto send = [&](windowing::HWND target, u32 message, u64 wparam,
                          u64 lparam,
                          std::function<void()> on_unprocessed = {}) {
        const windowing::WindowData* win = m_hwnd_mgr.find(target);
        if (win == nullptr) return false;
        if (t_native_delivery && m_msgsend_mgr && win->wndproc != 0) {
            const auto owner = m_hwnd_mgr.owner_thread_id(target);
            const u32 caller = ThreadManager::get_current_thread_id_static();
            if (owner && *owner != caller) {
                MessageSendManager* msm = m_msgsend_mgr;
                (void)m_queue.notify_thread_send(
                    caller, *owner,
                    [msm, target, message, wparam, lparam,
                     on_unprocessed = std::move(on_unprocessed)] {
                        const u64 result =
                            msm->send_message(target, message, wparam, lparam);
                        if (result == 0 && on_unprocessed) on_unprocessed();
                        return result;
                    });
                return false;
            }
        }
        if (m_msgsend_mgr && win->wndproc != 0) {
            const u64 result =
                m_msgsend_mgr->send_message(target, message, wparam, lparam);
            if (windowing::diag::focus_diag()) {
                std::fprintf(stderr,
                             "[focus] sent msg=0x%x hwnd=0x%x wparam=0x%llx "
                             "-> 0x%llx\n",
                             message, target,
                             static_cast<unsigned long long>(wparam),
                             static_cast<unsigned long long>(result));
            }
            return result == 0;
        }
        m_queue.post(windowing::MSG{target, message, wparam, lparam, 0, {}});
        return false;
    };
    const auto is_dialog = [&](windowing::HWND hwnd) {
        const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
        return win != nullptr && win->is_dialog;
    };
    const auto send_activate_app = [&](bool active) {
        const auto top_levels =
            m_hwnd_mgr.enum_children(windowing::HWND_DESKTOP);
        for (windowing::HWND target : top_levels) {
            send(target, windowing::WM::ACTIVATEAPP, active ? 1u : 0u,
                 other_thread_id);
        }
        VITREO_TRACE(
            "[u32ext] sent WM_ACTIVATEAPP(%u) top_levels=%zu\n",
            active ? 1u : 0u, top_levels.size());
    };
    const auto minimized_bit = [&](windowing::HWND hwnd) -> u64 {
        const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
        return win != nullptr && win->minimized ? 0x10000u : 0u;
    };

    if (gaining_application) {
        (void)m_host.exchange_app_active(true);
        if (m_msgloop_mgr) {
            m_msgloop_mgr->note_synchronous_application_activation();
        }
        send_activate_app(true);
    }

    // Dialog windows run the guest DLGPROC directly, so USER32 supplies the
    // DefDlgProc WM_ACTIVATE default when the procedure does not process it
    // (Learn DefDlgProc): save the focused control on deactivation, restore
    // it on activation.
    if (prev != windowing::NULL_HWND && prev != next) {
        (void)send(prev, windowing::WM::NCACTIVATE, 0u, next);
        const bool unprocessed = send(prev, windowing::WM::ACTIVATE,
             windowing::WA::INACTIVE | minimized_bit(prev), next,
             [this, prev, is_dialog] {
                 if (is_dialog(prev)) dialog_activate_default(prev, false);
             });
        if (unprocessed && is_dialog(prev)) {
            dialog_activate_default(prev, false);
        }
    }

    if (next != windowing::NULL_HWND) {
        m_host.set_active_hwnd(next);
        if (next != prev) {
            constexpr u32 HSHELL_WINDOWACTIVATED = 4;
            notify_shell_event(HSHELL_WINDOWACTIVATED, next);
            // Measured (winevent_sequence_probe): a window becoming the
            // foreground window is brought to the top, REORDER on the
            // desktop (OBJID_CLIENT), then EVENT_SYSTEM_FOREGROUND.
            constexpr u32 EVENT_SYSTEM_FOREGROUND = 0x0003u;
            constexpr u32 EVENT_OBJECT_REORDER = 0x8004u;
            constexpr i32 OBJID_CLIENT = -4;
            notify_win_event(EVENT_OBJECT_REORDER, windowing::HWND_DESKTOP, OBJID_CLIENT);
            notify_win_event(EVENT_SYSTEM_FOREGROUND, next);
        }
        (void)send(next, windowing::WM::NCACTIVATE, 1u, prev);
        const bool unprocessed = send(next, windowing::WM::ACTIVATE,
                                      state | minimized_bit(next), prev,
                                      [this, next, is_dialog] {
                                          if (is_dialog(next)) {
                                              dialog_activate_default(next, true);
                                          }
                                      });
        if (unprocessed && is_dialog(next)) {
            dialog_activate_default(next, true);
        }
    }

    if (losing_application) {
        (void)m_host.exchange_app_active(false);
        send_activate_app(false);
        if (m_focus_hwnd != windowing::NULL_HWND) {
            (void)set_keyboard_focus(m_focus_hwnd, false);
        }
    }

    VITREO_TRACE("[u32ext] activation 0x%x -> 0x%x state=%u app=%d\n",
                 prev, next, state, m_application_active ? 1 : 0);
    if (windowing::diag::focus_diag()) {
        std::fprintf(stderr,
                     "[focus] activation 0x%x -> 0x%x state=%u gain_app=%d "
                     "lose_app=%d\n",
                     prev, next, state, gaining_application ? 1 : 0,
                     losing_application ? 1 : 0);
    }
    return true;
}

void User32ExtManager::dialog_activate_default(windowing::HWND dialog,
                                               bool activating) {
    if (activating) {
        const auto saved = m_saved_focus_by_top_level.find(dialog);
        if (saved == m_saved_focus_by_top_level.end()) return;
        const windowing::HWND control = saved->second;
        m_saved_focus_by_top_level.erase(saved);
        if (m_hwnd_mgr.is_valid(control)) {
            (void)set_keyboard_focus(control, true);
        }
        return;
    }
    windowing::HWND h = m_focus_hwnd;
    for (u32 depth = 0; depth < 64 && h != windowing::NULL_HWND; ++depth) {
        const windowing::WindowData* w = m_hwnd_mgr.find(h);
        if (w == nullptr) return;
        if (w->parent == dialog) {
            m_saved_focus_by_top_level[dialog] = m_focus_hwnd;
            return;
        }
        h = w->parent;
    }
}

void User32ExtManager::release_activation(windowing::HWND hwnd,
                                          bool destroying) {
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (win == nullptr) return;

    const auto contains = [&](windowing::HWND ancestor, windowing::HWND h) {
        for (u32 depth = 0; depth < 64 && h != windowing::NULL_HWND; ++depth) {
            if (h == ancestor) return true;
            const windowing::WindowData* w = m_hwnd_mgr.find(h);
            if (w == nullptr) return false;
            h = w->parent;
        }
        return false;
    };

    if (hwnd == m_active_window) {
        const auto eligible = [&](windowing::HWND candidate) {
            if (candidate == hwnd) return false;
            const windowing::WindowData* c = m_hwnd_mgr.find(candidate);
            return c != nullptr && is_top_level_window(*c) && c->visible &&
                   c->enabled && !c->minimized && c->owner != hwnd;
        };
        windowing::HWND next = windowing::NULL_HWND;
        if (win->owner != windowing::NULL_HWND &&
            win->owner != windowing::HWND_DESKTOP && eligible(win->owner)) {
            next = win->owner;
        } else {
            for (windowing::HWND candidate :
                 m_hwnd_mgr.enum_children(windowing::HWND_DESKTOP)) {
                if (eligible(candidate)) {
                    next = candidate;
                    break;
                }
            }
        }
        if (next != windowing::NULL_HWND) {
            (void)change_active_window(next, windowing::WA::ACTIVE);
            return;
        }
        // Hiding the last window deactivates the application: WM_NCACTIVATE,
        // WM_ACTIVATE, WM_ACTIVATEAPP(FALSE), WM_KILLFOCUS (measured,
        // s1win_msgseq2_probe showwindow_hide_visible_popup).
        if (!destroying) {
            (void)change_active_window(windowing::NULL_HWND, windowing::WA::INACTIVE);
            return;
        }
        // Measured: destroying the last window sends it nothing and leaves
        // no active or focus window. The application is no longer active,
        // so the next activation sends WM_ACTIVATEAPP(TRUE) again
        // (s1win_msgseq2_probe showwindow_show_popup).
        {
            std::lock_guard<std::mutex> lock(m_activation_mutex);
            m_active_window = windowing::NULL_HWND;
            m_application_active = false;
        }
        if (contains(hwnd, m_focus_hwnd)) {
            m_focus_hwnd = windowing::NULL_HWND;
            m_host.set_keyboard_focus_hwnd(windowing::NULL_HWND);
        }
        return;
    }

    if (destroying && m_focus_hwnd != windowing::NULL_HWND &&
        contains(hwnd, m_focus_hwnd)) {
        m_focus_hwnd = windowing::NULL_HWND;
        m_host.set_keyboard_focus_hwnd(windowing::NULL_HWND);
    }
}

bool User32ExtManager::activate_application(windowing::HWND hwnd,
                                            u32 other_thread_id) {
    return change_active_window(hwnd, windowing::WA::ACTIVE, other_thread_id);
}

bool User32ExtManager::set_application_active(windowing::HWND hwnd,
                                              bool active,
                                              u32 other_thread_id,
                                              bool by_mouse) {
    if (active) {
        return change_active_window(
            hwnd, by_mouse ? windowing::WA::CLICKACTIVE : windowing::WA::ACTIVE,
            other_thread_id);
    }
    if (!m_application_active) {
        return false;
    }
    return change_active_window(windowing::NULL_HWND, windowing::WA::INACTIVE,
                                other_thread_id);
}

u32 User32ExtManager::native_mouse_activate(windowing::HWND top, u32 mouse_message,
                                            std::function<void(windowing::HWND)> on_declined) {
    // Learn WM_MOUSEACTIVATE: sent when the cursor is in an inactive window
    // and a mouse button is pressed; wParam is the top-level window being
    // activated, LOWORD(lParam) the hit-test code at the cursor, HIWORD(lParam)
    // the mouse message. The window under the cursor gets it (DefWindowProc
    // passes a child's to its parent). MA_ACTIVATE = 1, MA_ACTIVATEANDEAT = 2,
    // MA_NOACTIVATE = 3, MA_NOACTIVATEANDEAT = 4; anything else activates.
    constexpr u32 MA_ACTIVATE = 1, MA_ACTIVATEANDEAT = 2, MA_NOACTIVATE = 3,
                  MA_NOACTIVATEANDEAT = 4;
    if (top == m_active_window) return MA_ACTIVATE;
    const windowing::WindowData* top_win = m_hwnd_mgr.find(top);
    if (top_win == nullptr || top_win->wndproc == 0 || m_msgsend_mgr == nullptr) return MA_ACTIVATE;
    i32 x = 0, y = 0;
    cursor_host().get_native_cursor_pos(x, y);
    windowing::HWND target = m_hwnd_mgr.window_from_point(windowing::Point{x, y});
    // Only a window inside the clicked top-level answers for it.
    bool inside = false;
    for (windowing::HWND h = target; h != windowing::NULL_HWND && h != windowing::HWND_DESKTOP;) {
        if (h == top) { inside = true; break; }
        const windowing::WindowData* w = m_hwnd_mgr.find(h);
        if (w == nullptr || (w->style & windowing::WS::CHILD) == 0) break;
        h = w->parent;
    }
    if (!inside) target = top;
    const auto ask = [this, top, target, mouse_message, x, y]() -> u32 {
        const u64 point = (static_cast<u64>(static_cast<u16>(y)) << 16) | static_cast<u16>(x);
        const u32 hit = static_cast<u32>(
            m_msgsend_mgr->send_message(target, windowing::WM::NCHITTEST, 0, point));
        const u64 lparam = (static_cast<u64>(mouse_message & 0xFFFFu) << 16) | (hit & 0xFFFFu);
        const u32 answer = static_cast<u32>(
            m_msgsend_mgr->send_message(target, windowing::WM::MOUSEACTIVATE, top, lparam));
        VITREO_TRACE("[u32ext] WM_MOUSEACTIVATE hwnd=0x%x top=0x%x hit=%u msg=0x%x -> %u\n",
                     target, top, hit, mouse_message, answer);
        return answer >= MA_ACTIVATE && answer <= MA_NOACTIVATEANDEAT ? answer : MA_ACTIVATE;
    };
    const auto owner = m_hwnd_mgr.owner_thread_id(top);
    const u32 caller = ThreadManager::get_current_thread_id_static();
    if (!owner || *owner == caller) return ask();
    // The click arrived on the host thread. The owner thread answers it as a
    // sent message, which it handles before the click's queued input, so the
    // decision precedes the button-down exactly as on Windows.
    const windowing::HWND stays_active = m_active_window;
    (void)m_queue.notify_thread_send(
        caller, *owner,
        [this, ask, top, target, mouse_message, stays_active,
         on_declined = std::move(on_declined)]() -> u64 {
            const u32 answer = ask();
            if (answer == MA_ACTIVATEANDEAT || answer == MA_NOACTIVATEANDEAT) {
                m_queue.remove_by_type(target, mouse_message);  // the click is discarded
            }
            if (answer == MA_NOACTIVATE || answer == MA_NOACTIVATEANDEAT) {
                if (on_declined) on_declined(stays_active);
                return 0;
            }
            (void)set_application_active(top, true, 0, true);
            return 0;
        });
    return kMouseActivateDeferred;
}

bool User32ExtManager::set_keyboard_focus(windowing::HWND hwnd,
                                          bool focused) {
    if (!m_hwnd_mgr.is_valid(hwnd)) {
        return false;
    }

    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        return false;
    }

    const auto send_focus_message = [&](windowing::HWND target,
                                        u32 message, u64 other) {
        const windowing::WindowData* target_window = m_hwnd_mgr.find(target);
        if (m_msgsend_mgr && target_window && target_window->wndproc != 0) {
            // Focus is per thread input queue: a focus change never makes
            // the caller wait on another thread. The other thread's window
            // learns it lost (or gained) focus when that thread next
            // retrieves messages -- a thread blocked in a wait must not
            // stall SetFocus / DefWindowProc(WM_ACTIVATE) here. This also
            // covers NativeDeliveryScope (never block the host main thread).
            const auto owner = m_hwnd_mgr.owner_thread_id(target);
            const u32 caller = ThreadManager::get_current_thread_id_static();
            if (owner && *owner != 0 && *owner != caller) {
                MessageSendManager* msm = m_msgsend_mgr;
                (void)m_queue.notify_thread_send(
                    caller, *owner, [msm, target, message, other] {
                        return msm->send_message(target, message, other, 0);
                    });
                return;
            }
        }
        if (m_msgsend_mgr && target_window && target_window->wndproc != 0) {
            (void)m_msgsend_mgr->send_message(target, message, other, 0);
        } else {
            m_queue.post(windowing::MSG{
                target, message, other, 0, 0, {}});
        }
    };

    if (focused) {
        if (m_focus_hwnd == hwnd) {
            return false;
        }
        const windowing::HWND previous = m_focus_hwnd;
        m_focus_hwnd = hwnd;
        m_host.set_keyboard_focus_hwnd(hwnd);
        if (previous != windowing::NULL_HWND &&
            m_hwnd_mgr.is_valid(previous)) {
            send_focus_message(previous, windowing::WM::KILLFOCUS, hwnd);
            VITREO_TRACE(
                "[u32ext] sent WM_KILLFOCUS hwnd=0x%x next=0x%x\n",
                previous, hwnd);
        }
        send_focus_message(hwnd, windowing::WM::SETFOCUS, previous);
        VITREO_TRACE(
            "[u32ext] sent WM_SETFOCUS hwnd=0x%x previous=0x%x\n",
            hwnd, previous);
        // Measured (winevent_sequence_probe): EVENT_OBJECT_FOCUS on the
        // window gaining focus, OBJID_CLIENT.
        notify_win_event(0x8005u /* EVENT_OBJECT_FOCUS */, hwnd, -4 /* OBJID_CLIENT */);
        if (windowing::diag::focus_diag()) {
            std::fprintf(stderr,
                         "[focus] SetFocus hwnd=0x%x previous=0x%x\n",
                         hwnd, previous);
        }
        return true;
    }

    if (m_focus_hwnd != hwnd) {
        return false;
    }
    m_focus_hwnd = windowing::NULL_HWND;
    m_host.set_keyboard_focus_hwnd(windowing::NULL_HWND);
    send_focus_message(hwnd, windowing::WM::KILLFOCUS, 0);
    VITREO_TRACE("[u32ext] sent WM_KILLFOCUS hwnd=0x%x next=0x0\n", hwnd);
    // Measured (winevent_sequence_probe, minimize): focus moving to nothing
    // raises EVENT_OBJECT_FOCUS with a NULL hwnd.
    notify_win_event(0x8005u /* EVENT_OBJECT_FOCUS */, windowing::NULL_HWND, -4 /* OBJID_CLIENT */);
    if (windowing::diag::focus_diag()) {
        std::fprintf(stderr, "[focus] KillFocus hwnd=0x%x\n", hwnd);
    }
    return true;
}

bool User32ExtManager::set_native_keyboard_focus(
    windowing::HWND top_level, bool focused) {
    if (!m_hwnd_mgr.is_valid(top_level)) {
        return false;
    }

    const auto top_level_of = [&](windowing::HWND hwnd) {
        windowing::HWND current = hwnd;
        for (u32 depth = 0; depth < 64; ++depth) {
            const windowing::WindowData* window = m_hwnd_mgr.find(current);
            if (!window) return windowing::NULL_HWND;
            if (window->parent == windowing::HWND_DESKTOP ||
                window->parent == windowing::NULL_HWND) {
                return current;
            }
            current = window->parent;
        }
        return windowing::NULL_HWND;
    };

    // True when hwnd is top_level, a descendant, or in a window owned by
    // top_level. Learn: owned WS_POPUP/overlapped windows are themselves
    // top-level (parent = desktop); GetWindow(GW_OWNER) returns the owner.
    const auto focus_lives_under = [&](windowing::HWND hwnd) {
        windowing::HWND current = top_level_of(hwnd);
        for (u32 depth = 0; depth < 64; ++depth) {
            if (current == top_level) {
                return true;
            }
            const windowing::WindowData* window = m_hwnd_mgr.find(current);
            if (!window || window->owner == windowing::NULL_HWND ||
                window->owner == windowing::HWND_DESKTOP) {
                return false;
            }
            const windowing::HWND next = top_level_of(window->owner);
            if (next == windowing::NULL_HWND || next == current) {
                return false;
            }
            current = next;
        }
        return false;
    };

    if (!focused) {
        if (m_focus_hwnd == windowing::NULL_HWND ||
            top_level_of(m_focus_hwnd) != top_level) {
            return false;
        }
        const windowing::HWND previous = m_focus_hwnd;
        m_saved_focus_by_top_level[top_level] = previous;
        if (windowing::diag::focus_diag()) {
            std::fprintf(stderr, "[focus] native focus lost top=0x%x\n",
                         top_level);
        }
        return set_keyboard_focus(previous, false);
    }

    // AppKit can re-enter become-key on the owner (pending activation flush,
    // restack) while a child or owned popup already owns GetFocus. Windows
    // does not pull focus back to the owner on that hint; SetFocus(owner)
    // would send WM_KILLFOCUS and drop caret/edit focus. Become-key on the
    // owned popup itself still SetFocus's it — that is activation of a
    // different top-level, which must not be swallowed or clicks go nowhere.
    if (m_focus_hwnd != windowing::NULL_HWND &&
        focus_lives_under(m_focus_hwnd)) {
        m_saved_focus_by_top_level.erase(top_level);
        return false;
    }

    windowing::HWND target = top_level;
    if (const auto saved = m_saved_focus_by_top_level.find(top_level);
        saved != m_saved_focus_by_top_level.end()) {
        if (m_hwnd_mgr.is_valid(saved->second) &&
            top_level_of(saved->second) == top_level) {
            target = saved->second;
        } else {
            m_saved_focus_by_top_level.erase(saved);
        }
    }
    return set_keyboard_focus(target, true);
}

void User32ExtManager::impl_WindowFromPoint(Marshaler& m) {
    // Deliberately model-only: HwndManager applies guest visibility, enabled
    // state, rectangles, and z-order without asking the native host.
    i32 x = 0;
    i32 y = 0;
    if (m.cpu_is_64bit()) {
        // Win64 passes the eight-byte POINT value in RCX.
        const u64 packed = m.arg(0);
        x = static_cast<i32>(static_cast<u32>(packed));
        y = static_cast<i32>(static_cast<u32>(packed >> 32));
    } else {
        // Win32 stdcall expands POINT into two consecutive stack DWORDs.
        x = static_cast<i32>(m.arg(0));
        y = static_cast<i32>(m.arg(1));
    }
    const windowing::HWND result =
        m_hwnd_mgr.window_from_point(windowing::Point{x, y});
    VITREO_TRACE("[u32ext] WindowFromPoint(%d, %d) -> 0x%x\n", x, y, result);
    m.set_return(result);
}

void User32ExtManager::impl_ChildWindowFromPointEx(Marshaler& m) {
    const auto parent = static_cast<windowing::HWND>(m.arg(0));
    i32 x = 0;
    i32 y = 0;
    u32 flags = 0;
    if (m.cpu_is_64bit()) {
        // Win64 passes the eight-byte POINT value in RDX.
        const u64 packed = m.arg(1);
        x = static_cast<i32>(static_cast<u32>(packed));
        y = static_cast<i32>(static_cast<u32>(packed >> 32));
        flags = static_cast<u32>(m.arg(2));
    } else {
        // Win32 stdcall expands POINT into two consecutive stack DWORDs.
        x = static_cast<i32>(m.arg(1));
        y = static_cast<i32>(m.arg(2));
        flags = static_cast<u32>(m.arg(3));
    }
    const windowing::HWND result = m_hwnd_mgr.child_window_from_point(
        parent, windowing::Point{x, y}, flags);
    if (windowing::diag::input_diag()) {
        std::fprintf(stderr,
                     "[input] ChildWindowFromPointEx parent=0x%x "
                     "point=(%d,%d) flags=0x%x -> 0x%x\n",
                     parent, x, y, flags, result);
    }
    m.set_return(result);
}

} // namespace vitreo::winapi
