// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_device_scene.cpp
/// @brief IDirect3DDevice9 render-target / depth binding, scene bracketing
///        and Clear.
///
/// Vtable slots 37-43 of IDirect3DDevice9, routed here from
/// `D3D9Manager::dispatch_device` in d3d9_device_dispatch.cpp.

#include "winapi/graphics/d3d9/d3d9_state.hpp"
#include "winapi/graphics/d3d9/d3d9_cs.hpp"
#include "winapi/graphics/d3d9/d3d9_device_internal.hpp"
#include "winapi/graphics/d3d9/d3d9_draw_internal.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <vector>
#include <algorithm>
#include <vitreo/diagnostics.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace vitreo::winapi {

namespace {

[[nodiscard]] bool d3d9_clear_diag_enabled() noexcept {
    static const bool enabled =
        std::getenv("VITREO_D3D9_CLEAR_DIAG") != nullptr;
    return enabled;
}

/// Unpack a D3DCOLOR (ARGB) into the linear RGBA the backend clear takes.
[[nodiscard]] std::array<float, 4> unpack_d3dcolor(u32 color) {
    const auto channel = [](u32 value) {
        return static_cast<float>(value & 0xFF) / 255.0f;
    };
    return {channel(color >> 16), channel(color >> 8), channel(color),
            channel(color >> 24)};
}

} // namespace

/// IDirect3DDevice9 vtable slots: render-target and depth-stencil binding,
/// BeginScene, EndScene and Clear.
void D3D9Manager::dispatch_device_scene(Marshaler& m, usize slot,
                                        DeviceState& state) {
    switch (slot) {
    case 37: { // SetRenderTarget
        const u32 index = static_cast<u32>(m.arg(1));
        const u32 surface = static_cast<u32>(m.arg(2));
        if (index >= state.render_target_surfaces.size()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        // Windows forbids detaching render target 0.
        if (surface == 0 && index == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        ComObject* target = nullptr;
        if (surface != 0) {
            target = find(surface, D3D9InterfaceId::Surface);
            if (target == nullptr || !target->surface ||
                (surface != state.back_buffer_surface &&
                 (target->surface->usage & D3DUSAGE_RENDERTARGET) == 0)) {
                VITREO_TRACE("[d3d9] SetRenderTarget[%u] rejected surface "
                             "0x%08x (back=0x%08x found=%u state=%u "
                             "usage=0x%08x)\n",
                             index, surface, state.back_buffer_surface,
                             target != nullptr ? 1u : 0u,
                             target != nullptr && target->surface ? 1u : 0u,
                             target != nullptr && target->surface
                                 ? target->surface->usage
                                 : 0u);
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
        }
        if (state.render_target_surfaces[index] != surface) {
            flush_render_pass(state);
            // The implicit back buffer's base reference is device ownership,
            // not the slot-0 binding. Other surfaces acquire a binding ref.
            if (surface != 0 && surface != state.back_buffer_surface) {
                (void)add_ref(surface);
            }
            if (state.render_target_surfaces[index] != 0 &&
                state.render_target_surfaces[index] !=
                    state.back_buffer_surface) {
                (void)release(state.render_target_surfaces[index]);
            }
            state.render_target_surfaces[index] = surface;
            if (index == 0) state.render_target_surface = surface;
            state.invalidate_draw_pipeline_reuse();
            if (index == 0 && surface == state.back_buffer_surface) {
                VITREO_TRACE("[d3d9] SetRenderTarget -> back buffer 0x%08x\n",
                             surface);
            } else if (surface != 0) {
                VITREO_TRACE("[d3d9] SetRenderTarget[%u] -> 0x%08x %ux%u fmt=%u "
                             "(texture/offscreen)\n",
                             index, surface, target->surface->width,
                             target->surface->height, target->surface->format);
            }
        }
        // IDirect3DDevice9::SetRenderTarget resets both the viewport and the
        // scissor rectangle to the complete target. This applies to every
        // successful call, including rebinding the surface already current.
        if (target != nullptr) {
            state.viewport = {0u, 0u, target->surface->width,
                              target->surface->height, f32_bits(0.0f),
                              f32_bits(1.0f)};
            state.scissor = {0, 0, static_cast<i32>(target->surface->width),
                             static_cast<i32>(target->surface->height)};
        }
        m.set_return(D3D_OK);
        return;
    }

    case 38: { // GetRenderTarget
        const u32 index = static_cast<u32>(m.arg(1));
        const GuestAddr out = m.arg_ptr(2);
        if (index >= state.render_target_surfaces.size() || out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const u32 surface = state.render_target_surfaces[index];
        if (surface == 0) {
            (void)m.write_ptr(out, 0);
            m.set_return(D3DERR_NOTAVAILABLE);
            return;
        }
        (void)add_ref(surface);
        if (m.write_ptr(out, surface).is_err()) {
            (void)release(surface);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 39: { // SetDepthStencilSurface
        const u32 surface = static_cast<u32>(m.arg(1));
        if (surface != 0) {
            ComObject* target = find(surface, D3D9InterfaceId::Surface);
            if (target == nullptr || !target->surface ||
                (target->surface->usage & D3DUSAGE_DEPTHSTENCIL) == 0) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
        }
        if (state.depth_stencil_surface == surface) {
            m.set_return(D3D_OK);
            return;
        }
        if (surface != 0) (void)add_ref(surface);
        flush_render_pass(state);
        if (state.depth_stencil_surface != 0) {
            (void)release(state.depth_stencil_surface);
        }
        state.depth_stencil_surface = surface;
        state.invalidate_draw_pipeline_reuse();
        m.set_return(D3D_OK);
        return;
    }

    case 40: { // GetDepthStencilSurface
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        (void)m.write_ptr(out, 0);
        if (state.depth_stencil_surface == 0) {
            // Documented: no depth stencil is not an error.
            m.set_return(D3DERR_NOTFOUND_D3D);
            return;
        }
        (void)add_ref(state.depth_stencil_surface);
        if (m.write_ptr(out, state.depth_stencil_surface).is_err()) {
            (void)release(state.depth_stencil_surface);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 41: // BeginScene
        if (state.lost) {
            m.set_return(D3DERR_DEVICELOST);
            return;
        }
        if (state.in_scene) {
            // Windows rejects a nested BeginScene.
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        state.in_scene = true;
        ++state.diagnostic_frame;
        state.diagnostic_draw = 0;
        if (d3d9_sequence_diag_enabled() && state.diagnostic_frame <= 8) {
            std::fprintf(stderr, "[d3d9-sequence] frame=%llu begin\n",
                         static_cast<unsigned long long>(
                             state.diagnostic_frame));
        }
        m.set_return(D3D_OK);
        return;

    case 42: // EndScene
        if (!state.in_scene) {
            // Windows rejects EndScene without a matching BeginScene.
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        state.in_scene = false;
        if (d3d9_sequence_diag_enabled() && state.diagnostic_frame <= 8) {
            std::fprintf(stderr,
                         "[d3d9-sequence] frame=%llu end draws=%u\n",
                         static_cast<unsigned long long>(
                             state.diagnostic_frame),
                         state.diagnostic_draw);
        }
        // Everything the scene recorded becomes visible to Present here.
        flush_render_pass(state);
        m.set_return(D3D_OK);
        return;

    case 43: { // Clear
        const u32 rect_count = static_cast<u32>(m.arg(1));
        const GuestAddr rects = m.arg_ptr(2);
        const u32 flags = static_cast<u32>(m.arg(3));
        const u32 color = static_cast<u32>(m.arg(4));

        if (static const bool xfer = std::getenv("VITREO_D3D9_XFER_LOG") != nullptr;
            xfer && state.render_target_surface != state.back_buffer_surface) {
            // Off-screen clears next to the StretchRect log (thumbnail passes).
            static int shown = 0;
            if (shown++ < 4000) {
                std::fprintf(stderr, "[d3d9-xfer] Clear rt=%08x flags=0x%x color=0x%08x rects=%u\n",
                             state.render_target_surface, flags, color, rect_count);
            }
        }
        if (d3d9_sequence_diag_enabled() && state.diagnostic_frame <= 8) {
            std::fprintf(stderr,
                         "[d3d9-sequence] frame=%llu clear after-draw=%u "
                         "rects=%u flags=0x%x color=0x%08x\n",
                         static_cast<unsigned long long>(
                             state.diagnostic_frame),
                         state.diagnostic_draw, rect_count, flags, color);
        }

        if (state.lost) {
            m.set_return(D3DERR_DEVICELOST);
            return;
        }
        // Windows requires the count and the pointer to agree.
        if ((rect_count == 0) != (rects == 0)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (flags == 0 ||
            (flags & ~(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER |
                       D3DCLEAR_STENCIL)) != 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        // Clearing depth or stencil without one attached is invalid.
        if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) != 0 &&
            state.depth_stencil_surface == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        // Learn IDirect3DDevice9::Clear: pRects == NULL clears the entire
        // VIEWPORT rectangle, and every rectangle is clipped to the viewport;
        // with D3DRS_SCISSORTESTENABLE the scissor rectangle clips it too.
        // The Sims 3 renders a thumbnail twice side by side into one target,
        // clearing the left viewport black and the right one white, and
        // recovers alpha from the pair; clearing the whole target wiped the
        // black half and every portrait came out grey.
        u32 rt_w = state.back_buffer_width();
        u32 rt_h = state.back_buffer_height();
        u32 rt_level = 0;
        if (state.render_target_surface != 0 &&
            state.render_target_surface != state.back_buffer_surface) {
            if (ComObject* rt = find(state.render_target_surface, D3D9InterfaceId::Surface);
                rt != nullptr && rt->surface) {
                rt_w = rt->surface->width;
                rt_h = rt->surface->height;
                rt_level = rt->surface->level;
            }
        }
        using Area = std::array<i64, 4>;  // left, top, right, bottom
        Area region{static_cast<i64>(state.viewport[0]), static_cast<i64>(state.viewport[1]),
                    static_cast<i64>(state.viewport[0]) + state.viewport[2],
                    static_cast<i64>(state.viewport[1]) + state.viewport[3]};
        const auto clip = [](Area& a, const Area& b) {
            a = {std::max(a[0], b[0]), std::max(a[1], b[1]), std::min(a[2], b[2]),
                 std::min(a[3], b[3])};
        };
        clip(region, {0, 0, rt_w, rt_h});
        if (const auto it = state.render_states.find(d3d9_draw_detail::D3DRS_SCISSORTESTENABLE);
            it != state.render_states.end() && it->second != 0) {
            clip(region, {state.scissor[0], state.scissor[1], state.scissor[2],
                          state.scissor[3]});
        }
        std::vector<std::array<u32, 4>> areas;
        const auto add_area = [&](Area a) {
            clip(a, region);
            if (a[2] > a[0] && a[3] > a[1]) {
                areas.push_back({static_cast<u32>(a[0]), static_cast<u32>(a[1]),
                                 static_cast<u32>(a[2] - a[0]),
                                 static_cast<u32>(a[3] - a[1])});
            }
        };
        if (rect_count == 0) {
            add_area(region);
        }
        for (u32 i = 0; i < rect_count; ++i) {
            i32 r[4] = {};
            if (m.memory().read_bytes(rects + i * 16u, reinterpret_cast<Byte*>(r),
                                      sizeof(r)).is_err()) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            add_area({r[0], r[1], r[2], r[3]});
        }
        if (areas.empty()) {
            // Nothing of the target lies inside the clear area.
            m.set_return(D3D_OK);
            return;
        }
        const bool whole_target = areas.size() == 1 && areas[0][0] == 0 &&
                                  areas[0][1] == 0 && areas[0][2] == rt_w &&
                                  areas[0][3] == rt_h;
        const bool to_back_buffer =
            state.render_target_surface == 0 ||
            state.render_target_surface == state.back_buffer_surface;
        graphics::BackendTexture2D* const rt_color =
            resolve_render_target_color(state.render_target_surface,
                                        state.back_buffer_surface);
        std::array<graphics::BackendTexture2D*, 4> rt_colors{};
        rt_colors[0] = rt_color;
        for (u32 slot = 1; slot < rt_colors.size(); ++slot) {
            const u32 surface = state.render_target_surfaces[slot];
            if (surface == 0) continue;
            rt_colors[slot] =
                resolve_render_target_color(surface, state.back_buffer_surface);
        }
        const u32 depth_bits = static_cast<u32>(m.arg(5));
        const u32 stencil = static_cast<u32>(m.arg(6));
        float depth_value = 1.0f;
        std::memcpy(&depth_value, &depth_bits, sizeof(depth_value));

        if ((flags & D3DCLEAR_TARGET) != 0) {
            if (to_back_buffer) {
                if (!state.swap_chain || !state.swap_chain->valid()) {
                    state.lost = true;
                    m.set_return(D3DERR_DEVICELOST);
                    return;
                }
            } else if (rt_color == nullptr || !rt_color->valid()) {
                VITREO_TRACE("[d3d9] Clear: bound render target has no "
                             "clearable shared-core texture\n");
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            for (u32 slot = 1; slot < rt_colors.size(); ++slot) {
                if (state.render_target_surfaces[slot] != 0 &&
                    (rt_colors[slot] == nullptr ||
                     !rt_colors[slot]->valid())) {
                    VITREO_TRACE("[d3d9] Clear: render target %u has no "
                                 "clearable shared-core texture\n",
                                 slot);
                    m.set_return(D3DERR_INVALIDCALL);
                    return;
                }
            }
        }

        // A partial clear is a fill per rectangle, which needs single-sample
        // textures in a format whose clear texel is known. Otherwise widen
        // to the whole target, as before, and say so once.
        bool partial = !whole_target;
        std::array<std::array<u8, 16>, 4> color_texels{};
        std::array<unsigned, 4> color_texel_bytes{};
        std::array<unsigned, 4> color_levels{};
        graphics::BackendTexture2D* depth_target = nullptr;
        const char* widened = nullptr;
        if (partial && to_back_buffer) widened = "the back buffer";
        if (partial && widened == nullptr && (flags & D3DCLEAR_TARGET) != 0) {
            for (u32 slot = 0; slot < rt_colors.size() && widened == nullptr; ++slot) {
                graphics::BackendTexture2D* target = rt_colors[slot];
                if (target == nullptr) continue;
                const u32 surface_id =
                    slot == 0 ? state.render_target_surface : state.render_target_surfaces[slot];
                ComObject* surface = find(surface_id, D3D9InterfaceId::Surface);
                if (target->desc().sample_count > 1) {
                    widened = "a multisampled render target";
                } else if (surface == nullptr || !surface->surface ||
                           !d3d9_encode_clear_texel(surface->surface->format,
                                                    target->desc().pixel_format,
                                                    unpack_d3dcolor(color), color,
                                                    color_texels[slot],
                                                    color_texel_bytes[slot])) {
                    widened = "a render target format without a clear texel";
                } else {
                    color_levels[slot] = slot == 0 ? rt_level : surface->surface->level;
                }
            }
        }
        if (partial && widened == nullptr &&
            (flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) != 0) {
            depth_target = resolve_depth_attachment(state, rt_w, rt_h);
            if (depth_target == nullptr || depth_target->desc().sample_count > 1) {
                widened = "this depth-stencil surface";
            }
        }
        if (widened != nullptr) {
            partial = false;
            static int reported = 0;
            if (reported++ < 4) {
                std::fprintf(stderr,
                             "[d3d9] Clear: a %ux%u partial clear of %s is widened "
                             "to the whole target\n",
                             areas[0][2], areas[0][3], widened);
            }
        }

        if ((flags & D3DCLEAR_ZBUFFER) != 0 && !partial) {
            state.depth_clear_value = depth_value;
            // Next begin_render_pass on T1 consumes this. Worker may also
            // record swap-chain clear_depth; a redundant load-action clear
            // is correct. Test backends reject clear_depth and rely on this.
            state.depth_pending_clear = true;
            state.depth_pending_clear_back_buffer = to_back_buffer;
        }

        D3d9ClearSnapshot snap{};
        snap.manager = this;
        snap.swap_chain = state.swap_chain.get();
        snap.rt_color = rt_color;
        snap.rt_colors = rt_colors;
        const auto slice_for = [&](u32 surface_id) -> unsigned {
            if (surface_id == 0) return 0;
            ComObject* surface_entry =
                find(surface_id, D3D9InterfaceId::Surface);
            if (surface_entry == nullptr || !surface_entry->surface) return 0;
            return surface_entry->surface->face;
        };
        snap.rt_color_slices[0] = slice_for(state.render_target_surface);
        for (u32 slot = 1; slot < snap.rt_color_slices.size(); ++slot) {
            snap.rt_color_slices[slot] =
                slice_for(state.render_target_surfaces[slot]);
        }
        snap.flags = flags;
        snap.color = unpack_d3dcolor(color);
        snap.depth = depth_value;
        snap.stencil = stencil;
        snap.to_back_buffer = to_back_buffer;
        snap.partial = partial;
        if (partial) {
            snap.rects = std::move(areas);
            snap.color_texels = color_texels;
            snap.color_texel_bytes = color_texel_bytes;
            snap.rt_color_levels = color_levels;
            snap.depth_target = depth_target;
        }
        if (state.render_pass) {
            snap.pass_to_end = std::move(state.render_pass);
            state.render_pass_color = nullptr;
        }

        if ((flags & D3DCLEAR_TARGET) != 0 && to_back_buffer) {
            static u32 clear_bb_trace = 0;
            ++clear_bb_trace;
            if (clear_bb_trace <= 32 || (clear_bb_trace & 0xFFu) == 0) {
                VITREO_TRACE(
                    "[d3d9] Clear TARGET backbuffer #%u color=0x%08x "
                    "flags=0x%x depth_pending=%u after-draw=%u "
                    "in_scene=%u frame=%llu\n",
                    clear_bb_trace, color, flags,
                    state.depth_pending_clear ? 1u : 0u,
                    state.diagnostic_draw, state.in_scene ? 1u : 0u,
                    static_cast<unsigned long long>(
                        state.diagnostic_frame));
            }
        } else if ((flags & D3DCLEAR_TARGET) != 0) {
            static u32 clear_tex_trace = 0;
            ++clear_tex_trace;
            if (clear_tex_trace <= 16 || (clear_tex_trace & 0xFFu) == 0) {
                ComObject* rt = find(state.render_target_surface,
                                     D3D9InterfaceId::Surface);
                const u32 w = rt && rt->surface ? rt->surface->width : 0;
                const u32 h = rt && rt->surface ? rt->surface->height : 0;
                const u32 fmt =
                    rt && rt->surface ? rt->surface->format : 0;
                VITREO_TRACE("[d3d9] Clear TARGET texRT #%u color=0x%08x "
                             "rt=%ux%u fmt=%u\n",
                             clear_tex_trace, color, w, h, fmt);
            }
        }

        for (u32 slot = 0; slot < state.render_target_surfaces.size(); ++slot) {
            const u32 surface = state.render_target_surfaces[slot];
            if (surface != 0 && surface != state.back_buffer_surface) {
                (void)add_ref(surface);
                snap.retained.push_back(surface);
            }
        }
        if (state.depth_stencil_surface != 0 &&
            (flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) != 0) {
            (void)add_ref(state.depth_stencil_surface);
            snap.retained.push_back(state.depth_stencil_surface);
        }

        D3d9SubmitQueue::instance().enqueue_clear(std::move(snap));
        m.set_return(D3D_OK);
        return;
    }
    }
}

} // namespace vitreo::winapi
