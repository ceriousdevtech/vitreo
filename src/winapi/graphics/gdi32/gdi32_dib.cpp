// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32_dib.cpp
/// @brief Device-independent bitmaps.
///
/// CreateDIBSection, SetDIBits / GetDIBits, StretchDIBits, and the header
/// parsing they share. A DIB is a self-describing pixel buffer: a header giving
/// dimensions, bit depth and compression, an optional palette, then rows.
///
/// Two things about that layout are easy to get wrong and are handled here.
/// Rows are bottom-up unless the height is negative, so a positive height means
/// the first row in memory is the *last* row on screen. And in a BI_RGB 32bpp
/// DIB the fourth byte of each pixel is reserved, not alpha — carrying it
/// through as alpha renders every frame black.
///
/// Split out of gdi32.cpp. Closed in both directions once the ternary raster
/// operation codes moved to gdi32_internal.hpp, which is where the bitmap half
/// of the file needed them from too.

#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "ipc/hwnd_registry.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

namespace vitreo::winapi {



// =============================================================================
// GetBkMode / GetTextColor — read DC state
// =============================================================================

i32 GdiManager::get_bk_mode(Handle hdc) {
    ApiLock api_lock(*this);
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return 0;
    return it->second.bk_mode;
}

u32 GdiManager::get_text_color(Handle hdc) {
    ApiLock api_lock(*this);
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return 0xFFFFFFFF; // CLR_INVALID
    return it->second.text_color;
}




// =============================================================================
// GetPixel — read pixel from DC bitmap
// =============================================================================

// =============================================================================
// GetDCOrgEx — DC origin (stub: always 0,0)
// =============================================================================

bool GdiManager::get_dc_org_ex(Handle hdc, GuestAddr lppt) {
    ApiLock api_lock(*this);
    if (!m_guest_memory || lppt == 0) return false;
    i32 org_x = 0, org_y = 0;
    auto dc_it = m_dc_states.find(hdc);
    if (dc_it != m_dc_states.end() && dc_it->second.associated_hwnd != 0 && m_hwnd_mgr) {
        auto hwnd = static_cast<windowing::HWND>(dc_it->second.associated_hwnd);
        if (const auto* win = m_hwnd_mgr->find(hwnd)) {
            org_x = win->client_rect.left;
            org_y = win->client_rect.top;
        }
    }
    (void)m_guest_memory->write_u32(lppt, static_cast<u32>(org_x));
    (void)m_guest_memory->write_u32(lppt + 4, static_cast<u32>(org_y));
    return true;
}

} // namespace vitreo::winapi
