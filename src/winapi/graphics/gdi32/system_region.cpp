// SPDX-License-Identifier: LGPL-2.1-only
// GetRandomRgn: Microsoft Learn wingdi/nf-wingdi-getrandomrgn; independent
// Windows ABI/error/bitmap measurements in get_random_rgn_probe*.expected.
#include "gdi32.hpp"
#include "winapi/kernel32/error.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <cstdio>
#include <limits>
#include <optional>
#include <set>

namespace vitreo::winapi {
#include "random_rgn_contract.inc"

void GdiManager::set_dc_paint_region(Handle hdc, const ClipRect& region) {
    ApiLock lock(*this);
    if (auto dc=m_dc_states.find(hdc); dc!=m_dc_states.end()) {
        dc->second.paint_region=region;
        refresh_dc_clip(dc->second);
    }
}

Handle GdiManager::get_window_dc(Handle hwnd) {
    ApiLock lock(*this);
    const Handle dc=get_dc(hwnd);
    if (auto state=m_dc_states.find(dc); state!=m_dc_states.end())
        state->second.whole_window_dc=true;
    return dc;
}

i32 GdiManager::get_random_region(Handle hdc, Handle hrgn, i32 selector, bool is64,
                                  const SystemRegionState* scene) {
    ApiLock lock(*this);
    // Native selector5 is the one selector that bypasses handle validation.
    if (selector==random_rgn_contract::EARLY_NULL_SELECTOR) return 0;
    const auto dc=m_dc_states.find(hdc);
    if (dc==m_dc_states.end()) {
        if (is64) set_last_error(random_rgn_contract::ERROR_INVALID_HANDLE);
        return -1;
    }
    const auto destination=m_region_objects.find(hrgn);
    if (destination==m_region_objects.end()) return -1;
    const auto& state=dc->second;
    if (selector==random_rgn_contract::CLIPRGN ||
        selector==random_rgn_contract::METARGN || selector==random_rgn_contract::APIRGN) {
        ClipRect requested=selector==random_rgn_contract::METARGN?state.meta_clip:state.app_clip;
        if (selector==random_rgn_contract::APIRGN && state.meta_clip.valid)
            requested=requested.valid?ClipRect::combine(requested,state.meta_clip,random_rgn_contract::RGN_AND)
                                     :state.meta_clip;
        if (!requested.valid)return 0;
        destination->second=std::move(requested);
        return 1;
    }
    if (selector!=random_rgn_contract::SYSRGN)return 0;
    auto region=dc_system_region(hdc,state,scene);
    if(!region)return -1;
    destination->second=std::move(*region);return 1;
}

std::optional<ClipRect> GdiManager::dc_system_region(Handle hdc,const DcState& state,
    const SystemRegionState* scene,bool device_coordinates) const {
    ClipRect region{0,0,0,0,true,{}};
    i64 device_x=0,device_y=0;
    const char* unavailable_reason="unavailable/remote window geometry";
    const auto unsupported=[&]() -> std::optional<ClipRect> {
        static std::mutex report_mutex;
        std::lock_guard report_lock(report_mutex);
        static std::set<std::string> reported;
        if (reported.insert(unavailable_reason).second)
            std::fprintf(stderr,"[vitreo] gdi32!GetRandomRgn: %s\n",unavailable_reason);
        return std::nullopt;
    };
    if (state.memory_dc) {
        if (!state.bitmap) return unsupported();
        region.right=state.bitmap->width;region.bottom=state.bitmap->height;
    } else if (state.reference_device_dc) {
        region.right=device_caps(hdc,HORZRES);region.bottom=device_caps(hdc,VERTRES);
    } else {
        if (scene && scene->has_snapshot && !scene->snapshot_complete) {
            unavailable_reason="capsule window scene changed or could not be captured";
            return unsupported();
        }
        if (!m_hwnd_mgr && !(scene && scene->has_snapshot)) return unsupported();
        const auto windows=scene && scene->has_snapshot?scene->window_geometry:m_hwnd_mgr->geometry_snapshot();
        using Geometry=windowing::HwndManager::Geometry;
        const auto find=[&](u64 hwnd)->const Geometry* {
            const auto found=std::find_if(windows.begin(),windows.end(),
                [=](const Geometry& w){return w.hwnd==hwnd;});
            return found==windows.end()?nullptr:&*found;
        };
        const auto origin=[&](const Geometry& window)->std::optional<windowing::Point> {
            i64 x=window.rect.left,y=window.rect.top;
            const Geometry* current=&window;
            usize hops=0;
            while ((current->style&windowing::WS::CHILD) && current->parent!=windowing::HWND_DESKTOP) {
                current=find(current->parent);
                if (!current || ++hops>windows.size()) return std::nullopt;
                x+=static_cast<i64>(current->rect.left)+current->client_offset.x;
                y+=static_cast<i64>(current->rect.top)+current->client_offset.y;
            }
            if (x<std::numeric_limits<i32>::min() || x>std::numeric_limits<i32>::max() ||
                y<std::numeric_limits<i32>::min() || y>std::numeric_limits<i32>::max()) return std::nullopt;
            return windowing::Point{static_cast<i32>(x),static_cast<i32>(y)};
        };
        const auto shape=[&](const Geometry& window,bool client)->std::optional<ClipRect> {
            if (!window.visible || window.minimized || window.hidden_by_owner)
                return ClipRect{0,0,0,0,true,{}};
            if (window.ex_style & static_cast<u32>(random_rgn_contract::WS_EX_LAYERED)) {
                // Visible Windows oracle: a newly layered window has an empty
                // SYSRGN until its layering has been initialized successfully.
                bool initialized=false,per_pixel=false;
                if (scene) for (const auto& [hwnd,uses_pixels]:scene->layered_windows)
                    if (hwnd==window.hwnd) {initialized=true;per_pixel=uses_pixels;break;}
                // Per-pixel layering also has an empty GDI SYSRGN, independent
                // of pixel alpha, paint updates, or a SetWindowRgn mask.
                if (!initialized || per_pixel) return ClipRect{0,0,0,0,true,{}};
            }
            auto point=origin(window);if (!point) return std::nullopt;
            i64 x=point->x,y=point->y;
            const auto& bounds=client?window.client_rect:window.rect;
            if (client) {x+=window.client_offset.x;y+=window.client_offset.y;}
            const i64 right=x+static_cast<i64>(bounds.right)-bounds.left;
            const i64 bottom=y+static_cast<i64>(bounds.bottom)-bounds.top;
            for (const auto value:{x,y,right,bottom})
                if (value<std::numeric_limits<i32>::min() || value>std::numeric_limits<i32>::max()) return std::nullopt;
            ClipRect result{static_cast<i32>(x),static_cast<i32>(y),
                static_cast<i32>(right),static_cast<i32>(bottom),true,{}};
            // The client region cannot expose pixels beyond the whole window.
            const i64 outer_right=static_cast<i64>(point->x)+window.rect.right-window.rect.left;
            const i64 outer_bottom=static_cast<i64>(point->y)+window.rect.bottom-window.rect.top;
            if (outer_right<std::numeric_limits<i32>::min() || outer_right>std::numeric_limits<i32>::max() ||
                outer_bottom<std::numeric_limits<i32>::min() || outer_bottom>std::numeric_limits<i32>::max()) return std::nullopt;
            result=ClipRect::combine(result,ClipRect{point->x,point->y,
                static_cast<i32>(outer_right),static_cast<i32>(outer_bottom),true,{}},random_rgn_contract::RGN_AND);
            if (scene) for (const auto& [hwnd,source]:scene->window_shapes) {
                if (hwnd!=window.hwnd) continue;
                ClipRect mask=source;
                const auto convert=[&](std::array<i32,4> rect)->std::optional<std::array<i32,4>> {
                    i64 l=rect[0],r=rect[2];
                    if (window.ex_style&static_cast<u32>(random_rgn_contract::WS_EX_LAYOUTRTL)) {
                        const i64 width=static_cast<i64>(window.rect.right)-window.rect.left;
                        l=width-rect[2];r=width-rect[0];
                    }
                    const std::array<i64,4> mapped{l+point->x,
                        static_cast<i64>(rect[1])+point->y,r+point->x,
                        static_cast<i64>(rect[3])+point->y};
                    for (const auto value:mapped)
                        if (value<std::numeric_limits<i32>::min() || value>std::numeric_limits<i32>::max()) return std::nullopt;
                    return std::array<i32,4>{static_cast<i32>(mapped[0]),static_cast<i32>(mapped[1]),
                        static_cast<i32>(mapped[2]),static_cast<i32>(mapped[3])};
                };
                const auto bounds=convert({mask.left,mask.top,mask.right,mask.bottom});
                if (!bounds)return std::nullopt;
                mask.left=(*bounds)[0];mask.top=(*bounds)[1];mask.right=(*bounds)[2];mask.bottom=(*bounds)[3];
                for (auto& part:mask.rectangles) {auto mapped=convert(part);if (!mapped)return std::nullopt;part=*mapped;}
                result=ClipRect::combine(result,mask,random_rgn_contract::RGN_AND);
                break;
            }
            return result;
        };
        const auto visible=[](const Geometry& w) {return w.visible && !w.minimized && !w.hidden_by_owner;};
        const auto* desktop=find(windowing::HWND_DESKTOP);
        if (!desktop) return unsupported();
        if (state.screen_dc) {
            if (visible(*desktop)) {auto full=shape(*desktop,true);if (!full)return unsupported();region=*full;}
        } else {
            const auto* window=find(state.associated_hwnd);
            if (!window) return unsupported();
            if(device_coordinates) {
                const auto point=origin(*window);if(!point)return unsupported();
                device_x=point->x;device_y=point->y;
                if(!state.whole_window_dc){device_x+=window->client_offset.x;device_y+=window->client_offset.y;}
            }
            // CS_PARENTDC uses the immediate parent's visible region for a
            // plain client DC. Child clipping styles and GetWindowDC opt out.
            // Own masks are ignored in this mode, including during BeginPaint.
            const bool parent_dc=visible(*window) && !state.whole_window_dc &&
                (window->style&windowing::WS::CHILD) &&
                (window->class_style&static_cast<u32>(random_rgn_contract::CS_PARENTDC)) &&
                !(window->style&(windowing::WS::CLIPSIBLINGS|windowing::WS::CLIPCHILDREN));
            const Geometry* current=parent_dc?find(window->parent):window;
            if (!current)return unsupported();
            auto full=shape(*current,parent_dc || !state.whole_window_dc);
            if (!full)return unsupported();region=*full;
            usize hops=0;
            while (current && current->hwnd!=windowing::HWND_DESKTOP) {
                if (++hops>windows.size()) return unsupported();
                if (!visible(*current)) {region={0,0,0,0,true,{}};break;}
                const bool child=(current->style&windowing::WS::CHILD)!=0;
                // DWM redirects top-level surfaces independently; only child
                // siblings participate in this clipping (visible Windows oracle).
                if (child && (current->style&windowing::WS::CLIPSIBLINGS)) {
                    for (const auto& other:windows) {
                        if (other.parent!=current->parent || other.hwnd==current->hwnd ||
                            other.z_order>=current->z_order || !visible(other) ||
                            (other.ex_style & static_cast<u32>(random_rgn_contract::WS_EX_LAYERED |
                                                             random_rgn_contract::WS_EX_TRANSPARENT))) continue;
                        auto cover=shape(other,false);if (!cover)return unsupported();
                        region=ClipRect::combine(region,*cover,random_rgn_contract::RGN_DIFF);
                    }
                }
                const auto* parent=find(child?current->parent:windowing::HWND_DESKTOP);
                if (!parent)return unsupported();
                auto parent_clip=shape(*parent,true);if (!parent_clip)return unsupported();
                region=ClipRect::combine(region,*parent_clip,random_rgn_contract::RGN_AND);
                current=parent;
            }
            if (!visible(*desktop)) region={0,0,0,0,true,{}};
            if (window->style&windowing::WS::CLIPCHILDREN) {
                for (const auto& child:windows) {
                    if (child.parent!=window->hwnd || !visible(child) ||
                        (child.ex_style & static_cast<u32>(random_rgn_contract::WS_EX_LAYERED |
                                                         random_rgn_contract::WS_EX_TRANSPARENT))) continue;
                    auto cover=shape(child,false);if (!cover)return unsupported();
                    region=ClipRect::combine(region,*cover,random_rgn_contract::RGN_DIFF);
                }
            }
            if (state.paint_region.valid) {
                // Update coordinates are relative to the raw client origin,
                // never the bounding box of a SetWindowRgn-clipped shape.
                const auto point=origin(*window);if (!point)return unsupported();
                const i64 client_x=static_cast<i64>(point->x)+window->client_offset.x;
                const i64 client_y=static_cast<i64>(point->y)+window->client_offset.y;
                const i64 width=static_cast<i64>(window->client_rect.right)-window->client_rect.left;
                const i64 height=static_cast<i64>(window->client_rect.bottom)-window->client_rect.top;
                for (const auto value:{client_x,client_y,client_x+width,client_y+height})
                    if (value<std::numeric_limits<i32>::min() || value>std::numeric_limits<i32>::max())
                        return unsupported();
                if (width<0 || height<0 || width>std::numeric_limits<i32>::max() ||
                    height>std::numeric_limits<i32>::max()) return unsupported();
                ClipRect update=ClipRect::combine(state.paint_region,
                    ClipRect{0,0,static_cast<i32>(width),static_cast<i32>(height),true,{}},
                    random_rgn_contract::RGN_AND);
                update.left+=static_cast<i32>(client_x);update.right+=static_cast<i32>(client_x);
                update.top+=static_cast<i32>(client_y);update.bottom+=static_cast<i32>(client_y);
                for (auto& part:update.rectangles) {
                    part[0]+=static_cast<i32>(client_x);part[2]+=static_cast<i32>(client_x);
                    part[1]+=static_cast<i32>(client_y);part[3]+=static_cast<i32>(client_y);
                }
                region=ClipRect::combine(region,update,random_rgn_contract::RGN_AND);
            }
        }
    }
    if(device_coordinates && !region.is_empty()) {
        const auto convert=[&](i32& value,i64 origin) {
            const i64 result=static_cast<i64>(value)-origin;
            if(result<std::numeric_limits<i32>::min() || result>std::numeric_limits<i32>::max())return false;
            value=static_cast<i32>(result);return true;
        };
        if(!convert(region.left,device_x) || !convert(region.right,device_x) ||
           !convert(region.top,device_y) || !convert(region.bottom,device_y))return unsupported();
        for(auto& part:region.rectangles)
            if(!convert(part[0],device_x) || !convert(part[2],device_x) ||
               !convert(part[1],device_y) || !convert(part[3],device_y))return unsupported();
    }
    // An existing empty region is copied with result1, not a missing region0.
    if (region.is_empty()) region={0,0,0,0,true,{}};
    if (std::getenv("VITREO_GDI_DIAG"))
        std::fprintf(stderr,"[gdi-diag] GetRandomRgn dc=0x%x hwnd=0x%llx system=%d,%d,%d,%d parts=%zu paint=%d,%d,%d,%d\n",
            hdc,static_cast<unsigned long long>(state.associated_hwnd),
            region.left,region.top,region.right,region.bottom,region.parts().size(),
            state.paint_region.left,state.paint_region.top,state.paint_region.right,state.paint_region.bottom);
    return region;
}
} // namespace vitreo::winapi
