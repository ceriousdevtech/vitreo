// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_draw_pass.cpp
/// @brief D3D9 render-pass and attachment management, plus the command-stream
///        flush/drain helpers the frontend calls around draws.

#include "winapi/graphics/d3d9/d3d9_state.hpp"
#include "winapi/graphics/d3d9/d3d9_cs.hpp"

#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <array>

namespace vitreo::winapi {

graphics::BackendTexture2D*
D3D9Manager::resolve_render_target_color(u32 surface_id,
                                         u32 back_buffer_surface) {
    if (surface_id == 0 || surface_id == back_buffer_surface) {
        return nullptr;
    }
    ComObject* surface_entry =
        find(surface_id, D3D9InterfaceId::Surface);
    if (surface_entry == nullptr || !surface_entry->surface) {
        return nullptr;
    }
    const SurfaceState& surface = *surface_entry->surface;
    // Standalone CreateRenderTarget surfaces own a colour backend.
    if (surface.backend && surface.backend->valid() &&
        surface.backend->desc().render_target) {
        return surface.backend.get();
    }
    if (surface.kind != SurfaceState::Kind::TextureLevel ||
        surface.container == 0 || surface.level != 0) {
        // mip>0 RT texture levels need a per-level view the shared core does
        // not expose yet.
        return nullptr;
    }
    ComObject* texture_entry =
        find(surface.container, D3D9InterfaceId::Texture);
    if (texture_entry == nullptr) {
        texture_entry =
            find(surface.container, D3D9InterfaceId::CubeTexture);
    }
    if (texture_entry == nullptr || !texture_entry->texture ||
        !texture_entry->texture->backend ||
        !texture_entry->texture->backend->valid()) {
        return nullptr;
    }
    if (!texture_entry->texture->backend->desc().render_target) {
        return nullptr;
    }
    return texture_entry->texture->backend.get();
}

graphics::BackendTexture2D*
D3D9Manager::resolve_depth_attachment(DeviceState& state, u32 color_width,
                                      u32 color_height) {
    if (state.depth_stencil_surface == 0 || color_width == 0 ||
        color_height == 0 || m_backend == nullptr || !m_backend->available()) {
        return nullptr;
    }
    ComObject* ds_entry =
        find(state.depth_stencil_surface, D3D9InterfaceId::Surface);
    if (ds_entry == nullptr || !ds_entry->surface) {
        return nullptr;
    }
    SurfaceState& ds = *ds_entry->surface;
    if (ds.backend && ds.backend->valid() && ds.width == color_width &&
        ds.height == color_height) {
        return ds.backend.get();
    }
    // Texture levels share their parent's allocation. Using scratch depth
    // here would discard the depth values when the guest later samples it.
    if (ds.kind == SurfaceState::Kind::TextureLevel && ds.container != 0) {
        auto* texture = find(ds.container, D3D9InterfaceId::Texture);
        if (ds.level == 0 && ds.width == color_width && ds.height == color_height &&
            texture && texture->texture && texture->texture->backend &&
            texture->texture->backend->valid() &&
            texture->texture->backend->desc().depth_stencil) {
            return texture->texture->backend.get();
        }
        // Subresource views / unequal depth-texture attachments need a real
        // preserving backend path; an unrelated scratch image is incorrect.
        return nullptr;
    }
    // Windows allows DS size >= colour RT. Metal needs equal sizes, so use a
    // scratch depth texture matching the colour target.
    if (ds.width < color_width || ds.height < color_height) {
        return nullptr;
    }
    const u64 key = (static_cast<u64>(color_width) << 32) |
                    static_cast<u64>(color_height);
    const auto found = state.rt_depth_scratch.find(key);
    if (found != state.rt_depth_scratch.end() && found->second &&
        found->second->valid()) {
        return found->second.get();
    }
    graphics::BackendTexture2DDesc desc{};
    desc.width = color_width;
    desc.height = color_height;
    desc.mip_levels = 1;
    desc.array_size = 1;
    desc.pixel_format = graphics::PixelFormat::D32Float_S8Uint;
    desc.depth_stencil = true;
    auto scratch = m_backend->create_texture_2d(desc, nullptr, 0);
    if (!scratch || !scratch->valid()) {
        VITREO_TRACE("[d3d9] Draw: could not allocate %ux%u scratch depth\n",
                     color_width, color_height);
        return nullptr;
    }
    graphics::BackendTexture2D* raw = scratch.get();
    state.rt_depth_scratch[key] = std::move(scratch);
    return raw;
}

void D3D9Manager::d3d9_cs_flush() {
    auto& queue = D3d9SubmitQueue::instance();
    queue.flush();
    for (u32 id : queue.take_completed_retains()) {
        (void)release(id);
    }
}

void D3D9Manager::d3d9_cs_drain(bool clear_pipelines) {
    auto& queue = D3d9SubmitQueue::instance();
    queue.drain();
    for (u32 id : queue.take_completed_retains()) {
        (void)release(id);
    }
    if (clear_pipelines) {
        queue.clear_pipeline_cache();
    }
}

bool D3D9Manager::ensure_draw_render_pass(DeviceState& state,
                                         D3d9DrawSnapshot& snap) {
    if (state.render_pass && state.render_pass->valid()) {
        snap.render_pass = state.render_pass.get();
        return true;
    }
    // Clear/ColorFill transfer the open pass to the worker. begin_render_pass
    // / create_render_pass_standalone wait_encoded — that wait stays on T1,
    // after the worker has recorded EndPass and the GPU clear.
    d3d9_cs_flush();
    if (snap.to_back_buffer) {
        if (!state.swap_chain) {
            return false;
        }
        const bool clear_depth = state.depth_pending_clear &&
                                 state.depth_pending_clear_back_buffer;
        std::array<graphics::BackendTexture2D*,
                   graphics::BACKEND_MAX_RENDER_TARGETS>
            color_targets{};
        std::copy(snap.rt_colors.begin(), snap.rt_colors.end(),
                  color_targets.begin());
        state.render_pass = state.swap_chain->begin_render_pass(
            true, clear_depth, state.depth_clear_value, color_targets);
        state.render_pass_color = nullptr;
        if (clear_depth) {
            state.depth_pending_clear = false;
            state.depth_pending_clear_back_buffer = false;
        }
    } else {
        graphics::BackendRenderPassDesc pass_desc{};
        for (u32 slot = 0; slot < snap.rt_colors.size(); ++slot) {
            pass_desc.color_targets[slot] = snap.rt_colors[slot];
            pass_desc.color_slices[slot] = snap.rt_color_slices[slot];
            pass_desc.color_array_sizes[slot] = 1;
        }
        pass_desc.preserve_color_contents = true;
        pass_desc.depth_stencil_target = snap.rt_depth;
        if (snap.rt_depth != nullptr && state.depth_pending_clear &&
            !state.depth_pending_clear_back_buffer) {
            pass_desc.clear_depth = true;
            pass_desc.depth_clear_value = state.depth_clear_value;
            state.depth_pending_clear = false;
        }
        state.render_pass = m_backend->create_render_pass_standalone(pass_desc);
        state.render_pass_color = snap.rt_color;
    }
    if (!state.render_pass || !state.render_pass->valid()) {
        state.render_pass.reset();
        state.render_pass_color = nullptr;
        return false;
    }
    snap.render_pass = state.render_pass.get();
    return true;
}

void D3D9Manager::flush_render_pass(DeviceState& state) {
    d3d9_cs_flush();
    if (!state.render_pass) return;
    (void)state.render_pass->end();
    state.render_pass.reset();
    state.render_pass_color = nullptr;
}

} // namespace vitreo::winapi
