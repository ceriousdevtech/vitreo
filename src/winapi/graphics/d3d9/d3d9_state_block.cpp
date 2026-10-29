// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_state_block.cpp
/// @brief State-block recording and the IDirect3DStateBlock9 interface.
///
/// Vtable slots 59-61 of IDirect3DDevice9, routed here from
/// `D3D9Manager::dispatch_device` in d3d9_device_dispatch.cpp, and the
/// IDirect3DStateBlock9 vtable.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <utility>

namespace vitreo::winapi {

namespace {

[[nodiscard]] bool d3d9_state_block_diag_enabled() noexcept {
    static const bool enabled =
        std::getenv("VITREO_D3D9_STATEBLOCK_DIAG") != nullptr;
    return enabled;
}

[[nodiscard]] usize state_block_map_entry_count(
    const std::array<std::map<u32, u32>, D3D9_MAX_TEXTURE_STAGES>& maps) {
    usize count = 0;
    for (const auto& map : maps) count += map.size();
    return count;
}

[[nodiscard]] usize state_block_map_entry_count(
    const std::array<std::map<u32, u32>, D3D9_MAX_SAMPLERS>& maps) {
    usize count = 0;
    for (const auto& map : maps) count += map.size();
    return count;
}

void diagnose_state_block(const char* operation, u32 object,
                          const D3D9Manager::StateBlockState& block) {
    if (!d3d9_state_block_diag_enabled()) return;
    const usize textures = static_cast<usize>(std::count(
        block.texture_included.begin(), block.texture_included.end(), true));
    const usize vs_constants = static_cast<usize>(std::count(
        block.vs_const_f_included.begin(), block.vs_const_f_included.end(),
        true));
    const usize ps_constants = static_cast<usize>(std::count(
        block.ps_const_f_included.begin(), block.ps_const_f_included.end(),
        true));
    std::fprintf(
        stderr,
        "[d3d9-stateblock-diag] %s object=0x%08x type=%u rs=%zu tex=%zu "
        "tss=%zu samp=%zu xform=%zu fvf=%u decl=%u vs=%u ps=%u "
        "vsc=%zu psc=%zu vp=%u scissor=%u\n",
        operation, object, block.type, block.render_states.size(), textures,
        state_block_map_entry_count(block.stage_states),
        state_block_map_entry_count(block.sampler_states),
        block.transforms.size(), block.has_fvf ? 1u : 0u,
        block.has_vertex_declaration ? 1u : 0u,
        block.has_vertex_shader ? 1u : 0u,
        block.has_pixel_shader ? 1u : 0u, vs_constants, ps_constants,
        block.has_viewport ? 1u : 0u, block.has_scissor ? 1u : 0u);
    if (std::strcmp(operation, "end") == 0 ||
        std::strcmp(operation, "create") == 0) {
        for (const auto& [state, value] : block.render_states) {
            std::fprintf(stderr,
                         "[d3d9-stateblock-diag]   rs[%u]=0x%08x\n",
                         state, value);
        }
        for (u32 stage = 0; stage < D3D9_MAX_SAMPLERS; ++stage) {
            if (block.texture_included[stage]) {
                std::fprintf(stderr,
                             "[d3d9-stateblock-diag]   tex[%u]=0x%08x\n",
                             stage, block.textures[stage]);
            }
            for (const auto& [state, value] : block.stage_states[stage]) {
                std::fprintf(stderr,
                             "[d3d9-stateblock-diag]   tss[%u,%u]=0x%08x\n",
                             stage, state, value);
            }
        }
        for (u32 sampler = 0; sampler < D3D9_MAX_SAMPLERS; ++sampler) {
            for (const auto& [state, value] :
                 block.sampler_states[sampler]) {
                std::fprintf(stderr,
                             "[d3d9-stateblock-diag]   samp[%u,%u]=0x%08x\n",
                             sampler, state, value);
            }
        }
    }
    std::fflush(stderr);
}

} // namespace

/// IDirect3DDevice9 vtable slots: CreateStateBlock, BeginStateBlock and
/// EndStateBlock.
void D3D9Manager::dispatch_device_state_blocks(Marshaler& m, usize slot,
                                               u32 self,
                                               DeviceState& state) {
    switch (slot) {
    case 59: { // CreateStateBlock
        const u32 type = static_cast<u32>(m.arg(1));
        const GuestAddr out = m.arg_ptr(2);
        if (out != 0) (void)m.write_u32(out, 0);
        // D3DSBT_ALL=1, PIXELSTATE=2, VERTEXSTATE=3. Snapshot what we track.
        if (out == 0 || type < 1 || type > 3) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (state.recording_state_block) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const u32 object =
            alloc_com_object(D3D9InterfaceId::StateBlock, self);
        if (object == 0) {
            m.set_return(E_OUTOFMEMORY_D3D);
            return;
        }
        ComObject* block_entry = find(object, D3D9InterfaceId::StateBlock);
        block_entry->state_block = std::make_unique<StateBlockState>();
        StateBlockState& block = *block_entry->state_block;
        block.type = type;
        block.has_render_states = true;
        block.render_states = state.render_states;
        block.has_textures = true;
        block.textures = state.textures;
        block.texture_included.fill(true);
        block.vertex_textures = state.vertex_textures;
        block.vertex_texture_included.fill(true);
        block.has_stage_states = true;
        block.stage_states = state.stage_states;
        block.has_sampler_states = true;
        block.sampler_states = state.sampler_states;
        block.vertex_sampler_states = state.vertex_sampler_states;
        block.has_transforms = true;
        block.transforms = state.transforms;
        block.has_fvf = true;
        block.fvf = state.fvf;
        block.has_vertex_declaration = true;
        block.vertex_declaration = state.vertex_declaration;
        if (block.vertex_declaration != 0) {
            (void)add_ref(block.vertex_declaration);
        }
        block.has_vertex_shader = true;
        block.vertex_shader = state.vertex_shader;
        if (block.vertex_shader != 0) (void)add_ref(block.vertex_shader);
        block.has_pixel_shader = true;
        block.pixel_shader = state.pixel_shader;
        if (block.pixel_shader != 0) (void)add_ref(block.pixel_shader);
        block.has_vs_const_f = true;
        block.vs_const_f = state.vs_const_f;
        block.vs_const_f_included.fill(true);
        block.has_ps_const_f = true;
        block.ps_const_f = state.ps_const_f;
        block.ps_const_f_included.fill(true);
        for (u32 stage = 0; stage < D3D9_MAX_SAMPLERS; ++stage) {
            if (block.texture_included[stage] && block.textures[stage] != 0) {
                (void)add_ref(block.textures[stage]);
            }
        }
        for (u32 stage = 0; stage < D3D9_VERTEX_TEXTURE_SAMPLER_COUNT;
             ++stage) {
            if (block.vertex_texture_included[stage] &&
                block.vertex_textures[stage] != 0) {
                (void)add_ref(block.vertex_textures[stage]);
            }
        }
        block.has_viewport = true;
        block.viewport = state.viewport;
        block.has_scissor = true;
        block.scissor = state.scissor;
        diagnose_state_block("create", object, block);
        if (m.write_u32(out, object).is_err()) {
            destroy_object(object);
            m.set_return(E_POINTER_D3D);
            return;
        }
        VITREO_TRACE("[d3d9] CreateStateBlock type=%u -> 0x%08x\n", type,
                     object);
        m.set_return(D3D_OK);
        return;
    }
    case 60: { // BeginStateBlock
        if (state.recording_state_block) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        state.recording_state_block = true;
        state.recording_block = StateBlockState{};
        state.recording_block.type = 1; // D3DSBT_ALL-shaped delta
        m.set_return(D3D_OK);
        return;
    }
    case 61: { // EndStateBlock
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) (void)m.write_u32(out, 0);
        if (!state.recording_state_block || out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        state.recording_state_block = false;
        const u32 object =
            alloc_com_object(D3D9InterfaceId::StateBlock, self);
        if (object == 0) {
            m.set_return(E_OUTOFMEMORY_D3D);
            return;
        }
        ComObject* block_entry = find(object, D3D9InterfaceId::StateBlock);
        block_entry->state_block =
            std::make_unique<StateBlockState>(std::move(state.recording_block));
        state.recording_block = StateBlockState{};
        // Hold refs on any COM objects the delta captured.
        StateBlockState& block = *block_entry->state_block;
        if (block.has_vertex_shader && block.vertex_shader != 0) {
            (void)add_ref(block.vertex_shader);
        }
        if (block.has_pixel_shader && block.pixel_shader != 0) {
            (void)add_ref(block.pixel_shader);
        }
        if (block.has_vertex_declaration &&
            block.vertex_declaration != 0) {
            (void)add_ref(block.vertex_declaration);
        }
        if (block.has_textures) {
            for (u32 stage = 0; stage < D3D9_MAX_SAMPLERS; ++stage) {
                if (block.texture_included[stage] &&
                    block.textures[stage] != 0) {
                    (void)add_ref(block.textures[stage]);
                }
            }
            for (u32 stage = 0; stage < D3D9_VERTEX_TEXTURE_SAMPLER_COUNT;
                 ++stage) {
                if (block.vertex_texture_included[stage] &&
                    block.vertex_textures[stage] != 0) {
                    (void)add_ref(block.vertex_textures[stage]);
                }
            }
        }
        diagnose_state_block("end", object, block);
        if (m.write_u32(out, object).is_err()) {
            destroy_object(object);
            m.set_return(E_POINTER_D3D);
            return;
        }
        VITREO_TRACE("[d3d9] EndStateBlock -> 0x%08x\n", object);
        m.set_return(D3D_OK);
        return;
    }
    }
}

void D3D9Manager::dispatch_state_block(Marshaler& m, usize slot, u32 self) {
    if (dispatch_unknown(m, slot, self, D3D9InterfaceId::StateBlock)) return;

    ComObject* entry = find(self, D3D9InterfaceId::StateBlock);
    if (entry == nullptr || !entry->state_block) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    StateBlockState& block = *entry->state_block;
    ComObject* device_entry = find(entry->device, D3D9InterfaceId::Device);
    if (device_entry == nullptr || !device_entry->device_state) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    DeviceState& state = *device_entry->device_state;

    switch (slot) {
    case 3: { // GetDevice
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0 || entry->device == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        (void)add_ref(entry->device);
        if (m.write_u32(out, entry->device).is_err()) {
            (void)release(entry->device);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 4: { // Capture — refresh from the current device state
        // Microsoft documents Capture as invalid while the device is between
        // BeginStateBlock and EndStateBlock.  In particular, do not partially
        // mutate the stored block before reporting the failure.
        if (state.recording_state_block) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (block.has_render_states) {
            for (auto& [key, value] : block.render_states) {
                const auto found = state.render_states.find(key);
                value = found != state.render_states.end()
                            ? found->second
                            : d3d9_default_render_state(
                                  key,
                                  state.present_field(
                                      PP_ENABLE_AUTO_DEPTH_STENCIL) != 0);
            }
        }
        if (block.has_textures) {
            for (u32 stage = 0; stage < D3D9_MAX_SAMPLERS; ++stage) {
                if (!block.texture_included[stage]) continue;
                if (block.textures[stage] != 0) {
                    (void)release(block.textures[stage]);
                }
                block.textures[stage] = state.textures[stage];
                if (block.textures[stage] != 0) {
                    (void)add_ref(block.textures[stage]);
                }
            }
            for (u32 stage = 0; stage < D3D9_VERTEX_TEXTURE_SAMPLER_COUNT;
                 ++stage) {
                if (!block.vertex_texture_included[stage]) continue;
                if (block.vertex_textures[stage] != 0) {
                    (void)release(block.vertex_textures[stage]);
                }
                block.vertex_textures[stage] = state.vertex_textures[stage];
                if (block.vertex_textures[stage] != 0) {
                    (void)add_ref(block.vertex_textures[stage]);
                }
            }
        }
        if (block.has_stage_states) {
            for (u32 stage = 0; stage < D3D9_MAX_TEXTURE_STAGES; ++stage) {
                for (auto& [key, value] : block.stage_states[stage]) {
                    const auto found = state.stage_states[stage].find(key);
                    value = found != state.stage_states[stage].end()
                                ? found->second
                                : d3d9_default_texture_stage_state(stage, key);
                }
            }
        }
        if (block.has_sampler_states) {
            for (u32 sampler = 0; sampler < D3D9_MAX_SAMPLERS; ++sampler) {
                for (auto& [key, value] : block.sampler_states[sampler]) {
                    const auto found = state.sampler_states[sampler].find(key);
                    value = found != state.sampler_states[sampler].end()
                                ? found->second
                                : d3d9_default_sampler_state(key);
                }
            }
            for (u32 sampler = 0;
                 sampler < D3D9_VERTEX_TEXTURE_SAMPLER_COUNT; ++sampler) {
                for (auto& [key, value] :
                     block.vertex_sampler_states[sampler]) {
                    const auto found =
                        state.vertex_sampler_states[sampler].find(key);
                    value = found != state.vertex_sampler_states[sampler].end()
                                ? found->second
                                : d3d9_default_sampler_state(key);
                }
            }
        }
        if (block.has_transforms) {
            for (auto& [key, value] : block.transforms) {
                const auto found = state.transforms.find(key);
                if (found != state.transforms.end()) value = found->second;
            }
        }
        if (block.has_fvf) block.fvf = state.fvf;
        if (block.has_vertex_declaration) {
            if (block.vertex_declaration != 0) {
                (void)release(block.vertex_declaration);
            }
            block.vertex_declaration = state.vertex_declaration;
            if (block.vertex_declaration != 0) {
                (void)add_ref(block.vertex_declaration);
            }
        }
        if (block.has_vertex_shader) {
            if (block.vertex_shader != 0) (void)release(block.vertex_shader);
            block.vertex_shader = state.vertex_shader;
            if (block.vertex_shader != 0) (void)add_ref(block.vertex_shader);
        }
        if (block.has_pixel_shader) {
            if (block.pixel_shader != 0) (void)release(block.pixel_shader);
            block.pixel_shader = state.pixel_shader;
            if (block.pixel_shader != 0) (void)add_ref(block.pixel_shader);
        }
        if (block.has_vs_const_f) {
            for (u32 reg = 0; reg < D3D9_MAX_SHADER_CONST_F; ++reg) {
                if (!block.vs_const_f_included[reg]) continue;
                std::copy_n(state.vs_const_f.data() + reg * 4, 4,
                            block.vs_const_f.data() + reg * 4);
            }
        }
        if (block.has_ps_const_f) {
            for (u32 reg = 0; reg < D3D9_MAX_SHADER_CONST_F; ++reg) {
                if (!block.ps_const_f_included[reg]) continue;
                std::copy_n(state.ps_const_f.data() + reg * 4, 4,
                            block.ps_const_f.data() + reg * 4);
            }
        }
        if (block.has_viewport) block.viewport = state.viewport;
        if (block.has_scissor) block.scissor = state.scissor;
        diagnose_state_block("capture", self, block);
        m.set_return(D3D_OK);
        return;
    }
    case 5: { // Apply
        diagnose_state_block("apply", self, block);
        if (block.has_render_states) {
            for (const auto& [key, value] : block.render_states) {
                state.render_states[key] = value;
            }
        }
        if (block.has_textures) {
            for (u32 stage = 0; stage < D3D9_MAX_SAMPLERS; ++stage) {
                if (!block.texture_included[stage]) continue;
                const u32 texture = block.textures[stage];
                if (state.textures[stage] == texture) continue;
                if (texture != 0) (void)add_ref(texture);
                if (state.textures[stage] != 0) {
                    (void)release(state.textures[stage]);
                }
                state.textures[stage] = texture;
            }
            for (u32 stage = 0; stage < D3D9_VERTEX_TEXTURE_SAMPLER_COUNT;
                 ++stage) {
                if (!block.vertex_texture_included[stage]) continue;
                const u32 texture = block.vertex_textures[stage];
                if (state.vertex_textures[stage] == texture) continue;
                if (texture != 0) (void)add_ref(texture);
                if (state.vertex_textures[stage] != 0) {
                    (void)release(state.vertex_textures[stage]);
                }
                state.vertex_textures[stage] = texture;
            }
        }
        if (block.has_stage_states) {
            for (u32 stage = 0; stage < D3D9_MAX_TEXTURE_STAGES; ++stage) {
                for (const auto& [key, value] : block.stage_states[stage]) {
                    state.stage_states[stage][key] = value;
                }
            }
        }
        if (block.has_sampler_states) {
            for (u32 sampler = 0; sampler < D3D9_MAX_SAMPLERS; ++sampler) {
                for (const auto& [key, value] :
                     block.sampler_states[sampler]) {
                    state.sampler_states[sampler][key] = value;
                }
            }
            for (u32 sampler = 0;
                 sampler < D3D9_VERTEX_TEXTURE_SAMPLER_COUNT; ++sampler) {
                for (const auto& [key, value] :
                     block.vertex_sampler_states[sampler]) {
                    state.vertex_sampler_states[sampler][key] = value;
                }
            }
        }
        if (block.has_transforms) {
            for (const auto& [key, value] : block.transforms) {
                state.transforms[key] = value;
            }
        }
        if (block.has_fvf) state.fvf = block.fvf;
        if (block.has_vertex_declaration &&
            state.vertex_declaration != block.vertex_declaration) {
            if (block.vertex_declaration != 0) {
                (void)add_ref(block.vertex_declaration);
            }
            if (state.vertex_declaration != 0) {
                (void)release(state.vertex_declaration);
            }
            state.vertex_declaration = block.vertex_declaration;
        }
        if (block.has_vertex_shader &&
            state.vertex_shader != block.vertex_shader) {
            if (block.vertex_shader != 0) (void)add_ref(block.vertex_shader);
            if (state.vertex_shader != 0) (void)release(state.vertex_shader);
            state.vertex_shader = block.vertex_shader;
        }
        if (block.has_pixel_shader &&
            state.pixel_shader != block.pixel_shader) {
            if (block.pixel_shader != 0) (void)add_ref(block.pixel_shader);
            if (state.pixel_shader != 0) (void)release(state.pixel_shader);
            state.pixel_shader = block.pixel_shader;
        }
        if (block.has_vs_const_f) {
            for (u32 reg = 0; reg < D3D9_MAX_SHADER_CONST_F; ++reg) {
                if (!block.vs_const_f_included[reg]) continue;
                std::copy_n(block.vs_const_f.data() + reg * 4, 4,
                            state.vs_const_f.data() + reg * 4);
            }
        }
        if (block.has_ps_const_f) {
            for (u32 reg = 0; reg < D3D9_MAX_SHADER_CONST_F; ++reg) {
                if (!block.ps_const_f_included[reg]) continue;
                std::copy_n(block.ps_const_f.data() + reg * 4, 4,
                            state.ps_const_f.data() + reg * 4);
            }
        }
        if (block.has_viewport) state.viewport = block.viewport;
        if (block.has_scissor) state.scissor = block.scissor;
        state.invalidate_draw_pipeline_reuse();
        state.vs_const_dirty = true;
        state.ps_const_dirty = true;
        state.ff_uniform_dirty = true;
        m.set_return(D3D_OK);
        return;
    }
    default:
        m.set_return(E_NOTIMPL_D3D);
        return;
    }
}

} // namespace vitreo::winapi
