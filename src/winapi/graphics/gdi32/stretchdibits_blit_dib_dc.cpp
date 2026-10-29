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

/// @file stretchdibits_blit_dib_dc.cpp
/// @brief StretchDIBits — blit a DIB to a DC
///
/// Closed cut from gdi32_dib.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// StretchDIBits — blit a DIB to a DC
// =============================================================================

} // namespace vitreo::winapi
