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

/// @file clipping_region_operations.cpp
/// @brief Clipping Region Operations
///
/// Closed cut from gdi32.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Clipping Region Operations
// =============================================================================

// Boolean rectangle regions, canonicalized into horizontal bands. The
// coordinate edges partition the plane into cells with constant membership.
// Learn CombineRgn: DIFF excludes B, XOR excludes overlap, and aliases are valid.
ClipRect ClipRect::combine(const ClipRect& a, const ClipRect& b, i32 mode) {
    if (mode == 5) return a;
    const auto ap = a.parts(), bp = b.parts();
    std::vector<i32> xs, ys;
    for (const auto* parts : {&ap, &bp}) for (const auto& r : *parts) {
        xs.insert(xs.end(), {r[0], r[2]});
        ys.insert(ys.end(), {r[1], r[3]});
    }
    for (auto* edges : {&xs, &ys}) {
        std::sort(edges->begin(), edges->end());
        edges->erase(std::unique(edges->begin(), edges->end()), edges->end());
    }
    ClipRect out;
    out.valid = true;
    std::vector<std::array<i32,4>> previous;
    for (size_t y = 1; y < ys.size(); ++y) {
        std::vector<std::array<i32,4>> band;
        for (size_t x = 1; x < xs.size(); ++x) {
            const bool av = !ap.empty() && a.contains(xs[x-1], ys[y-1]);
            const bool bv = !bp.empty() && b.contains(xs[x-1], ys[y-1]);
            const bool keep = mode == 1 ? av && bv : mode == 2 ? av || bv :
                              mode == 3 ? av != bv : av && !bv;
            if (!keep) continue;
            if (!band.empty() && band.back()[2] == xs[x-1]) band.back()[2] = xs[x];
            else band.push_back({xs[x-1], ys[y-1], xs[x], ys[y]});
        }
        bool same = band.size() == previous.size() && !band.empty();
        for (size_t i = 0; same && i < band.size(); ++i)
            same = band[i][0] == previous[i][0] && band[i][2] == previous[i][2] && previous[i][3] == ys[y-1];
        if (same) {
            for (size_t i = 0; i < band.size(); ++i)
                out.rectangles[out.rectangles.size()-band.size()+i][3] = ys[y];
        } else out.rectangles.insert(out.rectangles.end(), band.begin(), band.end());
        previous = std::move(band);
    }
    if (!out.rectangles.empty()) {
        out.left = out.rectangles[0][0]; out.top = out.rectangles[0][1];
        out.right = out.rectangles[0][2]; out.bottom = out.rectangles[0][3];
        for (const auto& r : out.rectangles) {
            out.left = std::min(out.left, r[0]); out.top = std::min(out.top, r[1]);
            out.right = std::max(out.right, r[2]); out.bottom = std::max(out.bottom, r[3]);
        }
        if (out.rectangles.size() == 1) out.rectangles.clear();
    }
    return out;
}

Handle GdiManager::create_rect_rgn(i32 left, i32 top, i32 right, i32 bottom) {
    ApiLock api_lock(*this);
    // Corners are ordered and an empty rectangle is the empty region at
    // 0,0,0,0 (s3gdi_region_probe "inverted", "empty").
    Handle h = allocate_gdi_handle(kHandleTypeGdiObject);
    ClipRect rgn;
    rgn.left = std::min(left, right);
    rgn.right = std::max(left, right);
    rgn.top = std::min(top, bottom);
    rgn.bottom = std::max(top, bottom);
    if (rgn.left == rgn.right || rgn.top == rgn.bottom) rgn.left = rgn.top = rgn.right = rgn.bottom = 0;
    rgn.valid = true;
    m_region_objects[h] = rgn;
    VITREO_TRACE("[gdi] CreateRectRgn(%d,%d,%d,%d) -> 0x%x\n",
                 left, top, right, bottom, h);
    return h;
}

bool GdiManager::get_region_rect(Handle hrgn, ClipRect& out) const {
    ApiLock api_lock(*this);
    auto it = m_region_objects.find(hrgn);
    if (it == m_region_objects.end()) {
        return false;
    }
    out = it->second;
    return true;
}

i32 GdiManager::select_clip_rgn(Handle hdc, Handle hrgn) {
    ApiLock api_lock(*this);
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return RGN_ERROR;

    auto& dc = it->second;

    if (hrgn == 0) {
        // NULL region = remove clip region (full DC visible)
        dc.clip_rect = ClipRect{};
        dc.clip_rect.valid = false;
        return SIMPLEREGION;
    }

    auto rgn_it = m_region_objects.find(hrgn);
    if (rgn_it == m_region_objects.end()) {
        return RGN_ERROR;
    }

    dc.clip_rect = rgn_it->second;

    if (dc.clip_rect.is_empty()) {
        return NULLREGION;
    }
    return dc.clip_rect.rectangles.empty() ? SIMPLEREGION : COMPLEXREGION;
}

namespace {
i32 region_type(const ClipRect& r) {
    if (r.is_empty()) return NULLREGION;
    return r.rectangles.empty() ? SIMPLEREGION : COMPLEXREGION;
}
}  // namespace

ClipRect GdiManager::dc_surface_region(const DcState& dc) const {
    ApiLock api_lock(*this);
    ClipRect whole;
    whole.left = 0;
    whole.top = 0;
    if (dc.bitmap) {
        whole.right = dc.bitmap->width;
        whole.bottom = dc.bitmap->height;
    } else {
        whole.right = 32767;
        whole.bottom = 32767;
    }
    whole.valid = true;
    return whole;
}

i32 GdiManager::ext_select_clip_rgn(Handle hdc, Handle hrgn, i32 fnMode) {
    ApiLock api_lock(*this);
    // s3gdi_region_probe: the result's type ("extselect and fresh=2",
    // "or=3"); a NULL region only works with RGN_COPY, which removes the clip
    // ("copy NULL=2"; "and NULL=0" with the error untouched).
    constexpr i32 RGN_COPY = 5;
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end() || fnMode < 1 || fnMode > 5) return RGN_ERROR;
    if (fnMode == RGN_COPY || hrgn == 0) {
        if (hrgn == 0 && fnMode != RGN_COPY) return RGN_ERROR;
        return select_clip_rgn(hdc, hrgn);
    }
    auto rgn_it = m_region_objects.find(hrgn);
    if (rgn_it == m_region_objects.end()) return RGN_ERROR;
    auto& dc = it->second;
    const ClipRect base = dc.clip_rect.valid ? dc.clip_rect : dc_surface_region(dc);
    dc.clip_rect = ClipRect::combine(base, rgn_it->second, fnMode);
    dc.clip_rect.valid = true;
    return region_type(dc.clip_rect);
}

i32 GdiManager::exclude_clip_rect(Handle hdc, i32 left, i32 top, i32 right, i32 bottom) {
    ApiLock api_lock(*this);
    // Logical coordinates; Windows reports COMPLEXREGION whatever the result
    // ("ret exclude fresh=3", "ret exclude outside=3").
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return RGN_ERROR;
    auto& dc = it->second;
    lp_to_dp_rect(dc, left, top, right, bottom);
    const ClipRect base = dc.clip_rect.valid ? dc.clip_rect : dc_surface_region(dc);
    dc.clip_rect = ClipRect::combine(base, ClipRect{left, top, right, bottom, true, {}}, 4);
    dc.clip_rect.valid = true;
    return COMPLEXREGION;
}

i32 GdiManager::intersect_clip_rect(Handle hdc, i32 left, i32 top, i32 right, i32 bottom) {
    ApiLock api_lock(*this);
    // Logical coordinates, kept in device units ("clip vporg ... getcliprgn=1
    // 10,10,15,15"); the return is COMPLEXREGION even for a simple or empty
    // result ("ret intersect fresh=3", "ret intersect empty=3").
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return RGN_ERROR;
    auto& dc = it->second;
    lp_to_dp_rect(dc, left, top, right, bottom);
    const ClipRect rect{left, top, right, bottom, true, {}};
    dc.clip_rect = dc.clip_rect.valid ? ClipRect::combine(dc.clip_rect, rect, 1) : rect;
    dc.clip_rect.valid = true;
    return COMPLEXREGION;
}

i32 GdiManager::offset_clip_rgn(Handle hdc, i32 dx, i32 dy) {
    ApiLock api_lock(*this);
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return RGN_ERROR;
    auto& dc = it->second;
    if (!dc.clip_rect.valid) return SIMPLEREGION;  // "ret offsetcliprgn none=2"
    i32 ox = 0, oy = 0;
    lp_to_dp_point(dc, ox, oy);
    i32 px = dx, py = dy;
    lp_to_dp_point(dc, px, py);
    const i32 ddx = px - ox, ddy = py - oy;
    ClipRect& c = dc.clip_rect;
    if (!c.is_empty()) {
        c.left += ddx;
        c.right += ddx;
        c.top += ddy;
        c.bottom += ddy;
        for (auto& r : c.rectangles) {
            r[0] += ddx;
            r[2] += ddx;
            r[1] += ddy;
            r[3] += ddy;
        }
    }
    return region_type(c);
}

i32 GdiManager::get_clip_box(Handle hdc, i32& left, i32& top, i32& right, i32& bottom) {
    ApiLock api_lock(*this);
    // The clip's bounds in logical units ("clip rgn vporg clipbox=2
    // -6,-6,10,10"), the surface without a clip.
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return RGN_ERROR;
    auto& dc = it->second;
    const ClipRect box = dc.clip_rect.valid ? dc.clip_rect : dc_surface_region(dc);
    if (!dc.clip_rect.valid && !dc.bitmap) {
        left = top = 0;
        right = 1920;
        bottom = 1080;
    } else {
        double l = 0, t = 0, r = 0, b = 0;
        if (!dp_to_lp_d(dc, box.left, box.top, l, t) || !dp_to_lp_d(dc, box.right, box.bottom, r, b)) {
            return RGN_ERROR;
        }
        left = static_cast<i32>(std::lround(std::min(l, r)));
        right = static_cast<i32>(std::lround(std::max(l, r)));
        top = static_cast<i32>(std::lround(std::min(t, b)));
        bottom = static_cast<i32>(std::lround(std::max(t, b)));
    }
    return dc.clip_rect.valid ? region_type(dc.clip_rect) : SIMPLEREGION;
}

bool GdiManager::pt_visible(Handle hdc, i32 x, i32 y) {
    ApiLock api_lock(*this);
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return false;
    auto& dc = it->second;
    lp_to_dp_point(dc, x, y);  // logical coordinates
    if (!dc.clip_rect.contains(x, y)) return false;
    if (dc.bitmap && !dc.bitmap->in_bounds(x, y)) return false;
    return true;
}

} // namespace vitreo::winapi
