// SPDX-License-Identifier: LGPL-2.1-only

/// @file rect_util.cpp
/// @brief GetUpdateRect/Rgn and AdjustWindowRectEx.
///
/// Closed cut from user32_ext.cpp — verified with plan_extraction.py.

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

// ----- Rect Utility APIs -----

void User32ExtManager::impl_GetUpdateRect(Marshaler& m) {
    // BOOL GetUpdateRect(HWND hWnd, LPRECT lpRect, BOOL bErase)
    const u32 hwnd = static_cast<u32>(m.arg(0));
    const GuestAddr rect_ptr = m.arg_ptr(1);
    const bool erase = m.arg(2) != 0;

    if (hwnd == 0) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    // "GetUpdateRect(bad hwnd)=0 err=57005".
    if (m_paint_mgr != nullptr && !m_paint_mgr->is_window(hwnd)) {
        m.set_return(0u);
        return;
    }
    if (m_paint_mgr == nullptr) {
        if (rect_ptr != 0) {
            (void)m.write_u32(rect_ptr + 0, 0);
            (void)m.write_u32(rect_ptr + 4, 0);
            (void)m.write_u32(rect_ptr + 8, 0);
            (void)m.write_u32(rect_ptr + 12, 0);
        }
        m.set_return(0u);
        return;
    }

    Rect update{};
    const bool has_update =
        m_paint_mgr->get_update_rect(hwnd, update, erase);
    if (rect_ptr != 0) {
        (void)m.write_u32(rect_ptr + 0, static_cast<u32>(update.left));
        (void)m.write_u32(rect_ptr + 4, static_cast<u32>(update.top));
        (void)m.write_u32(rect_ptr + 8, static_cast<u32>(update.right));
        (void)m.write_u32(rect_ptr + 12, static_cast<u32>(update.bottom));
    }
    VITREO_TRACE(
        "[u32ext] GetUpdateRect hwnd=0x%x erase=%u -> %s {%d,%d,%d,%d}\n",
        hwnd, erase ? 1u : 0u, has_update ? "TRUE" : "FALSE",
        update.left, update.top, update.right, update.bottom);
    // WIN_DIAG: first-paint / Login sizing without enabling full VITREO_TRACE.
    if (has_update && std::getenv("VITREO_WIN_DIAG")) {
        static thread_local u32 s_logged = 0;
        if (s_logged < 16) {
            ++s_logged;
            std::fprintf(stderr,
                "[win-diag] GetUpdateRect hwnd=0x%x erase=%u -> TRUE {%d,%d,%d,%d}\n",
                hwnd, erase ? 1u : 0u,
                update.left, update.top, update.right, update.bottom);
        }
    }
    m.set_return(has_update ? 1u : 0u);
}

void User32ExtManager::impl_GetUpdateRgn(Marshaler& m) {
    // int GetUpdateRgn(HWND hWnd, HRGN hRgn, BOOL bErase)
    constexpr u64 kError = 0;
    constexpr u64 kNullRegion = 1;
    constexpr u64 kSimpleRegion = 2;
    const u32 hwnd = static_cast<u32>(m.arg(0));
    const u32 hrgn = static_cast<u32>(m.arg(1));
    const bool erase = m.arg(2) != 0;
    if (hwnd == 0 || hrgn == 0) {
        m.set_return(kError);
        return;
    }
    // The region itself, copied into @p hrgn ("GetUpdateRgn=3" for two
    // invalidated rectangles).
    u64 result = kNullRegion;
    (void)kSimpleRegion;
    if (m_paint_mgr != nullptr) {
        ClipRect region{};
        result = static_cast<u64>(m_paint_mgr->get_update_region(hwnd, region, erase));
        if (m_gdi_mgr != nullptr && !m_gdi_mgr->set_region(static_cast<Handle>(hrgn), region)) {
            result = kError;
        }
    }
    VITREO_TRACE("[u32ext] GetUpdateRgn hwnd=0x%x hrgn=0x%x erase=%u -> %llu\n",
                 hwnd, hrgn, erase ? 1u : 0u,
                 static_cast<unsigned long long>(result));
    m.set_return(result);
}


void User32ExtManager::impl_AdjustWindowRectEx(Marshaler& m) {
    // BOOL AdjustWindowRectEx(LPRECT, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle)
    const GuestAddr rp = m.arg_ptr(0);
    const auto style = static_cast<u32>(m.arg(1));
    const bool has_menu = m.arg(2) != 0;
    const auto ex_style = static_cast<u32>(m.arg(3));
    if (rp == 0) {
        // Windows returns TRUE for a null RECT and leaves LastError alone.
        m.set_return(1u);
        return;
    }
    const bool ok = adjust_window_rect(memory(), rp, style, has_menu, ex_style);
    VITREO_TRACE("[u32ext] AdjustWindowRectEx style=0x%x ex=0x%x menu=%d -> %u\n",
                 style, ex_style, has_menu ? 1 : 0, ok ? 1u : 0u);
    m.set_return(ok ? 1u : 0u);
}

} // namespace vitreo::winapi
