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

/// @file savedc_restoredc_dc_state_stack.cpp
/// @brief SaveDC / RestoreDC — DC state stack management
///
/// Closed cut from gdi32.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// SaveDC / RestoreDC — DC state stack management
// =============================================================================

i32 GdiManager::save_dc(Handle hdc) {
    ApiLock api_lock(*this);
    // "savedc(NULL)=0 err=57005": a bad DC fails with the error untouched.
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return 0;
    DcState& dc = it->second;
    DcState saved = dc;
    saved.state_stack.clear();
    dc.state_stack.push_back(std::move(saved));
    return static_cast<i32>(dc.state_stack.size());  // 1-based save level
}

bool GdiManager::restore_dc(Handle hdc, i32 nSavedDC) {
    ApiLock api_lock(*this);
    // s3gdi_region_probe: -1 is the latest save, a positive level restores
    // that save and discards everything above it; a level that does not
    // exist fails with the error untouched ("restoredc(5)=0 err=57005").
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return false;
    DcState& dc = it->second;
    if (dc.state_stack.empty() || nSavedDC == 0) return false;
    const i32 depth = static_cast<i32>(dc.state_stack.size());
    const i32 index = nSavedDC < 0 ? depth + nSavedDC : nSavedDC - 1;
    if (index < 0 || index >= depth) return false;
    std::vector<DcState> stack = std::move(dc.state_stack);
    DcState restored = std::move(stack[static_cast<usize>(index)]);
    stack.resize(static_cast<usize>(index));
    // The DC keeps what it is: its window, kind and identity.
    restored.associated_hwnd = dc.associated_hwnd;
    restored.screen_dc = dc.screen_dc;
    restored.memory_dc = dc.memory_dc;
    if (!dc.memory_dc) {
        restored.bitmap = dc.bitmap;
        restored.selected_bitmap_handle = dc.selected_bitmap_handle;
    } else if (restored.selected_bitmap_handle != dc.selected_bitmap_handle) {
        m_bitmap_owner_dc.erase(dc.selected_bitmap_handle);
        if (restored.selected_bitmap_handle != 0) m_bitmap_owner_dc[restored.selected_bitmap_handle] = hdc;
    }
    dc = std::move(restored);
    dc.state_stack = std::move(stack);
    return true;
}

} // namespace vitreo::winapi
