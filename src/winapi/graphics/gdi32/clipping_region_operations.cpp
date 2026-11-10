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
#include "random_rgn_contract.inc"

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

void GdiManager::refresh_dc_clip(DcState& dc) {
    dc.clip_rect=ClipRect{};
    for (const auto* layer:{&dc.app_clip,&dc.meta_clip,&dc.paint_region})
        if (layer->valid) dc.clip_rect=dc.clip_rect.valid?ClipRect::combine(dc.clip_rect,*layer,1):*layer;
}

i32 GdiManager::set_meta_region(Handle hdc) {
    ApiLock lock(*this);
    const auto found=m_dc_states.find(hdc);
    if (found==m_dc_states.end()) {set_last_error(error_codes::ERROR_INVALID_HANDLE);return RGN_ERROR;}
    auto& dc=found->second;
    const bool simple_inputs=dc.app_clip.valid && dc.meta_clip.valid &&
        !dc.app_clip.is_empty() && !dc.meta_clip.is_empty() &&
        dc.app_clip.parts().size()==1 && dc.meta_clip.parts().size()==1;
    if (dc.app_clip.valid) dc.meta_clip=dc.meta_clip.valid?
        ClipRect::combine(dc.meta_clip,dc.app_clip,1):dc.app_clip;
    dc.app_clip=ClipRect{};
    refresh_dc_clip(dc);
    if (!dc.meta_clip.valid)return SIMPLEREGION;
    // Windows' simple/simple intersection fast path reports SIMPLEREGION
    // once even when disjoint; the stored meta is empty (native 4x4 matrix).
    if (dc.meta_clip.is_empty())return simple_inputs?SIMPLEREGION:NULLREGION;
    return dc.meta_clip.parts().size()==1?SIMPLEREGION:COMPLEXREGION;
}

std::optional<ClipRect> GdiManager::application_clip_base(Handle hdc,const DcState& dc,
    const SystemRegionState* scene) const {
    if(dc.memory_dc) {
        if(!dc.bitmap)return std::nullopt;
        return ClipRect{0,0,dc.bitmap->width,dc.bitmap->height,true,{}};
    }
    if(dc.reference_device_dc)return ClipRect{0,0,device_caps(hdc,HORZRES),device_caps(hdc,VERTRES),true,{}};
    if(scene && scene->has_snapshot && !scene->snapshot_complete)return std::nullopt;
    if(!m_hwnd_mgr && !(scene && scene->has_snapshot)) {
        if(dc.screen_dc)return ClipRect{0,0,device_caps(hdc,HORZRES),device_caps(hdc,VERTRES),true,{}};
        return std::nullopt;
    }
    const auto windows=scene && scene->has_snapshot?scene->window_geometry:m_hwnd_mgr->geometry_snapshot();
    const auto find=[&](u64 hwnd)->const windowing::HwndManager::Geometry* {
        const auto it=std::find_if(windows.begin(),windows.end(),[=](const auto& g){return g.hwnd==hwnd;});
        return it==windows.end()?nullptr:&*it;
    };
    const auto* window=find(dc.screen_dc?windowing::HWND_DESKTOP:dc.associated_hwnd);if(!window)return std::nullopt;
    i64 l=0,t=0,r=0,b=0;
    if(dc.screen_dc){l=window->rect.left;t=window->rect.top;r=window->rect.right;b=window->rect.bottom;}
    else if(dc.whole_window_dc){r=static_cast<i64>(window->rect.right)-window->rect.left;b=static_cast<i64>(window->rect.bottom)-window->rect.top;}
    else if((window->style&windowing::WS::CHILD) && (window->class_style&static_cast<u32>(random_rgn_contract::CS_PARENTDC)) &&
            !(window->style&(windowing::WS::CLIPSIBLINGS|windowing::WS::CLIPCHILDREN))) {
        const auto* parent=find(window->parent);if(!parent)return std::nullopt;
        l=-static_cast<i64>(window->rect.left)-window->client_offset.x;
        t=-static_cast<i64>(window->rect.top)-window->client_offset.y;
        r=l+static_cast<i64>(parent->client_rect.right)-parent->client_rect.left;
        b=t+static_cast<i64>(parent->client_rect.bottom)-parent->client_rect.top;
    } else {r=static_cast<i64>(window->client_rect.right)-window->client_rect.left;b=static_cast<i64>(window->client_rect.bottom)-window->client_rect.top;}
    for(const auto v:{l,t,r,b})if(v<std::numeric_limits<i32>::min() || v>std::numeric_limits<i32>::max())return std::nullopt;
    return ClipRect{static_cast<i32>(l),static_cast<i32>(t),static_cast<i32>(r),static_cast<i32>(b),true,{}};
}
std::optional<i32> GdiManager::clip_complexity(Handle hdc,const DcState& dc,
    const ClipRect& app,const SystemRegionState* scene) const {
    // ExtSelect/SelectClipRgn returns the drawing-region complexity, whereas
    // GetRandomRgn app/meta selectors preserve unbounded application operands.
    auto effective=dc_system_region(hdc,dc,scene,true);if(!effective)return std::nullopt;
    if(dc.meta_clip.valid)*effective=ClipRect::combine(*effective,dc.meta_clip,1);
    if(app.valid)*effective=ClipRect::combine(*effective,app,1);
    if(effective->is_empty())return NULLREGION;
    return effective->parts().size()==1?SIMPLEREGION:COMPLEXREGION;
}

i32 GdiManager::select_clip_rgn(Handle hdc, Handle hrgn,const SystemRegionState* scene) {
    ApiLock lock(*this);
    const auto it=m_dc_states.find(hdc);if(it==m_dc_states.end())return RGN_ERROR;
    ClipRect app;
    if(hrgn) {const auto region=m_region_objects.find(hrgn);if(region==m_region_objects.end())return RGN_ERROR;app=region->second;}
    const auto complexity=clip_complexity(hdc,it->second,app,scene);if(!complexity)return RGN_ERROR;
    it->second.app_clip=std::move(app);refresh_dc_clip(it->second);return *complexity;
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

i32 GdiManager::ext_select_clip_rgn(Handle hdc,Handle hrgn,i32 mode,const SystemRegionState* scene) {
    ApiLock lock(*this);
    const auto it=m_dc_states.find(hdc);
    if(it==m_dc_states.end() || mode<1 || mode>5)return RGN_ERROR;
    if(mode==5 || !hrgn)return mode==5?select_clip_rgn(hdc,hrgn,scene):RGN_ERROR;
    const auto rgn=m_region_objects.find(hrgn);if(rgn==m_region_objects.end())return RGN_ERROR;
    auto& dc=it->second;ClipRect app;
    // Native fresh AND stores the raw operand, including outside the canvas.
    if(mode==1 && !dc.app_clip.valid)app=rgn->second;
    else {
        const auto base=dc.app_clip.valid?std::optional<ClipRect>{dc.app_clip}:application_clip_base(hdc,dc,scene);
        if(!base)return RGN_ERROR;app=ClipRect::combine(*base,rgn->second,mode);
    }
    const auto complexity=clip_complexity(hdc,dc,app,scene);if(!complexity)return RGN_ERROR;
    dc.app_clip=std::move(app);refresh_dc_clip(dc);return *complexity;
}

i32 GdiManager::exclude_clip_rect(Handle hdc, i32 left, i32 top, i32 right, i32 bottom,const SystemRegionState* scene) {
    ApiLock api_lock(*this);
    // Logical coordinates; Windows reports COMPLEXREGION whatever the result
    // ("ret exclude fresh=3", "ret exclude outside=3").
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return RGN_ERROR;
    auto& dc = it->second;
    lp_to_dp_rect(dc, left, top, right, bottom);
    const auto base=dc.app_clip.valid?std::optional<ClipRect>{dc.app_clip}:application_clip_base(hdc,dc,scene);
    if(!base)return RGN_ERROR;
    dc.app_clip = ClipRect::combine(*base, ClipRect{left, top, right, bottom, true, {}}, 4);
    dc.app_clip.valid = true;
    refresh_dc_clip(dc);
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
    dc.app_clip = dc.app_clip.valid ? ClipRect::combine(dc.app_clip, rect, 1) : rect;
    dc.app_clip.valid = true;
    refresh_dc_clip(dc);
    return COMPLEXREGION;
}

i32 GdiManager::offset_clip_rgn(Handle hdc, i32 dx, i32 dy) {
    ApiLock api_lock(*this);
    auto it = m_dc_states.find(hdc);
    if (it == m_dc_states.end()) return RGN_ERROR;
    auto& dc = it->second;
    if (!dc.app_clip.valid) return SIMPLEREGION;  // "ret offsetcliprgn none=2"
    i32 ox = 0, oy = 0;
    lp_to_dp_point(dc, ox, oy);
    i32 px = dx, py = dy;
    lp_to_dp_point(dc, px, py);
    const i32 ddx = px - ox, ddy = py - oy;
    ClipRect& c = dc.app_clip;
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
    refresh_dc_clip(dc);
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
