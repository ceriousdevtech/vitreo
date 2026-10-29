// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32.cpp
/// @brief GDI32 device context stubs (WIN32-063)

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

/// @file pen_line_operations.cpp
/// @brief Pen & Line Operations
///
/// Closed cut from gdi32.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Pen & Line Operations
// =============================================================================

Handle GdiManager::create_pen(i32 style, i32 width, u32 color) {
    ApiLock api_lock(*this);
    Handle h = allocate_gdi_handle(kHandleTypeGdiObject);
    // LOGPEN reports the magnitude of a negative width (s3gdi_bitmap_probe
    // "pen solid-4").
    m_pen_objects[h] = PenInfo{style, width < 0 ? -width : width, color};
    return h;
}

bool GdiManager::move_to_ex(Handle hdc, i32 x, i32 y, i32* prev_x, i32* prev_y) {
    ApiLock api_lock(*this);
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return false;
    if (prev_x) *prev_x = it->second.current_x;
    if (prev_y) *prev_y = it->second.current_y;
    if (path_record_move(hdc, x, y)) return true;  // inside BeginPath: a new figure
    it->second.current_x = x;
    it->second.current_y = y;
    return true;
}

i32 GdiManager::set_rop2(Handle hdc, i32 mode) {
    ApiLock api_lock(*this);
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return 0;
    i32 prev = it->second.rop2;
    it->second.rop2 = mode;
    return prev;
}

} // namespace vitreo::winapi
