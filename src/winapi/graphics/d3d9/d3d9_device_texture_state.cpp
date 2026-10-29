// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_device_texture_state.cpp
/// @brief IDirect3DDevice9 texture binding, texture-stage and sampler state,
///        and palettes.
///
/// Vtable slots 64-69 and 71-74 of IDirect3DDevice9, routed here from
/// `D3D9Manager::dispatch_device` in d3d9_device_dispatch.cpp.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>

#include <cstdio>
#include <cstdlib>

namespace vitreo::winapi {

namespace {

[[nodiscard]] bool d3d9_texture_lifetime_diag_enabled() noexcept {
    static const bool enabled =
        std::getenv("VITREO_D3D9_TEXTURE_LIFETIME_DIAG") != nullptr;
    return enabled;
}

} // namespace

/// IDirect3DDevice9 vtable slots: texture binding, texture-stage and
/// sampler state, and palettes.
void D3D9Manager::dispatch_device_texture_state(Marshaler& m, usize slot,
                                                DeviceState& state) {
    switch (slot) {
    case 64: { // GetTexture
        const u32 stage = static_cast<u32>(m.arg(1));
        const GuestAddr out = m.arg_ptr(2);
        const bool pixel_stage = stage < D3D9_MAX_SAMPLERS;
        const bool vertex_stage =
            stage >= D3D9_VERTEX_TEXTURE_SAMPLER_FIRST &&
            stage < D3D9_VERTEX_TEXTURE_SAMPLER_FIRST +
                        D3D9_VERTEX_TEXTURE_SAMPLER_COUNT;
        if ((!pixel_stage && !vertex_stage) || out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const u32 texture =
            pixel_stage
                ? state.textures[stage]
                : state.vertex_textures[stage - D3D9_VERTEX_TEXTURE_SAMPLER_FIRST];
        if (texture != 0) (void)add_ref(texture);
        if (m.write_u32(out, texture).is_err()) {
            if (texture != 0) (void)release(texture);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 65: { // SetTexture
        const u32 stage = static_cast<u32>(m.arg(1));
        const u32 texture = static_cast<u32>(m.arg(2));
        const bool pixel_stage = stage < D3D9_MAX_SAMPLERS;
        const bool vertex_stage =
            stage >= D3D9_VERTEX_TEXTURE_SAMPLER_FIRST &&
            stage < D3D9_VERTEX_TEXTURE_SAMPLER_FIRST +
                        D3D9_VERTEX_TEXTURE_SAMPLER_COUNT;
        if (!pixel_stage && !vertex_stage) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (texture != 0 &&
            find(texture, D3D9InterfaceId::Texture) == nullptr &&
            find(texture, D3D9InterfaceId::CubeTexture) == nullptr &&
            find(texture, D3D9InterfaceId::VolumeTexture) == nullptr) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        u32& bound_texture =
            pixel_stage
                ? state.textures[stage]
                : state.vertex_textures[stage - D3D9_VERTEX_TEXTURE_SAMPLER_FIRST];
        if (bound_texture == texture) {
            if (state.recording_state_block) {
                state.recording_block.has_textures = true;
                if (pixel_stage) {
                    state.recording_block.texture_included[stage] = true;
                    state.recording_block.textures[stage] = texture;
                } else {
                    const u32 vertex_slot =
                        stage - D3D9_VERTEX_TEXTURE_SAMPLER_FIRST;
                    state.recording_block.vertex_texture_included[vertex_slot] =
                        true;
                    state.recording_block.vertex_textures[vertex_slot] = texture;
                }
            }
            m.set_return(D3D_OK);
            return;
        }
        const u32 previous_texture = bound_texture;
        const ComObject* previous_entry = find_any(previous_texture);
        const ComObject* next_entry = find_any(texture);
        const auto large_texture = [](const ComObject* object) {
            return object != nullptr && object->texture != nullptr &&
                   static_cast<u64>(object->texture->width) *
                           static_cast<u64>(object->texture->height) >=
                       1000000u;
        };
        if (d3d9_texture_lifetime_diag_enabled() &&
            (large_texture(previous_entry) || large_texture(next_entry))) {
            std::fprintf(stderr,
                         "[d3d9-texture-life] set-texture stage=%u "
                         "old=0x%08x new=0x%08x new_refs=%u\n",
                         stage, previous_texture, texture,
                         next_entry != nullptr ? next_entry->ref_count : 0u);
        }
        if (texture != 0) (void)add_ref(texture);
        if (bound_texture != 0) (void)release(bound_texture);
        bound_texture = texture;
        if (state.recording_state_block) {
            state.recording_block.has_textures = true;
            if (pixel_stage) {
                state.recording_block.texture_included[stage] = true;
                state.recording_block.textures[stage] = texture;
            } else {
                const u32 vertex_slot =
                    stage - D3D9_VERTEX_TEXTURE_SAMPLER_FIRST;
                state.recording_block.vertex_texture_included[vertex_slot] =
                    true;
                state.recording_block.vertex_textures[vertex_slot] = texture;
            }
        }
        // Learn: paletted textures use the current device palette when
        // sampled — expand on bind if the palette serial moved.
        if (next_entry != nullptr && next_entry->texture &&
            d3d9_is_palettized_format(next_entry->texture->format)) {
            sync_palettized_texture(*next_entry->texture, state, false);
        }
        m.set_return(D3D_OK);
        return;
    }

    case 66: { // GetTextureStageState
        const u32 stage = static_cast<u32>(m.arg(1));
        const u32 type = static_cast<u32>(m.arg(2));
        const GuestAddr out = m.arg_ptr(3);
        if (stage >= D3D9_MAX_TEXTURE_STAGES || out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const auto& stage_map = state.stage_states[stage];
        const auto found = stage_map.find(type);
        const u32 value = found != stage_map.end()
                              ? found->second
                              : d3d9_default_texture_stage_state(stage, type);
        if (m.write_u32(out, value).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 67: { // SetTextureStageState
        const u32 stage = static_cast<u32>(m.arg(1));
        const u32 type = static_cast<u32>(m.arg(2));
        const u32 value = static_cast<u32>(m.arg(3));
        if (stage >= D3D9_MAX_TEXTURE_STAGES) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        record_d3d9_state_use(D3D9StateKind::TextureStageState, type, value);
        const auto found = state.stage_states[stage].find(type);
        if (found == state.stage_states[stage].end() || found->second != value) {
            state.stage_states[stage][type] = value;
            state.invalidate_draw_pipeline_reuse();
            state.ff_uniform_dirty = true;
        }
        if (state.recording_state_block) {
            state.recording_block.has_stage_states = true;
            state.recording_block.stage_states[stage][type] = value;
        }
        if (type == 24 /* D3DTSS_TEXTURETRANSFORMFLAGS */) {
            static u32 ttf_set_trace = 0;
            ++ttf_set_trace;
            if (ttf_set_trace <= 32 || (ttf_set_trace & 0xFFu) == 0) {
                VITREO_TRACE("[d3d9] SetTextureStageState stage=%u TTF=0x%x "
                             "#%u\n",
                             stage, value, ttf_set_trace);
            }
        }
        m.set_return(D3D_OK);
        return;
    }

    case 68: { // GetSamplerState
        const u32 sampler = static_cast<u32>(m.arg(1));
        const u32 type = static_cast<u32>(m.arg(2));
        const GuestAddr out = m.arg_ptr(3);
        const bool pixel_sampler = sampler < D3D9_MAX_SAMPLERS;
        const bool vertex_sampler =
            sampler >= D3D9_VERTEX_TEXTURE_SAMPLER_FIRST &&
            sampler < D3D9_VERTEX_TEXTURE_SAMPLER_FIRST +
                          D3D9_VERTEX_TEXTURE_SAMPLER_COUNT;
        if ((!pixel_sampler && !vertex_sampler) || out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const auto& sampler_map = pixel_sampler
            ? state.sampler_states[sampler]
            : state.vertex_sampler_states[
                  sampler - D3D9_VERTEX_TEXTURE_SAMPLER_FIRST];
        const auto found = sampler_map.find(type);
        const u32 value = found != sampler_map.end()
                              ? found->second
                              : d3d9_default_sampler_state(type);
        if (m.write_u32(out, value).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 69: { // SetSamplerState
        const u32 sampler = static_cast<u32>(m.arg(1));
        const u32 type = static_cast<u32>(m.arg(2));
        const u32 value = static_cast<u32>(m.arg(3));
        const bool pixel_sampler = sampler < D3D9_MAX_SAMPLERS;
        const bool vertex_sampler =
            sampler >= D3D9_VERTEX_TEXTURE_SAMPLER_FIRST &&
            sampler < D3D9_VERTEX_TEXTURE_SAMPLER_FIRST +
                          D3D9_VERTEX_TEXTURE_SAMPLER_COUNT;
        if (!pixel_sampler && !vertex_sampler) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        auto& sampler_map = pixel_sampler
            ? state.sampler_states[sampler]
            : state.vertex_sampler_states[
                  sampler - D3D9_VERTEX_TEXTURE_SAMPLER_FIRST];
        record_d3d9_state_use(D3D9StateKind::SamplerState, type, value);
        const auto found = sampler_map.find(type);
        if (found == sampler_map.end() || found->second != value) {
            sampler_map[type] = value;
            state.invalidate_draw_pipeline_reuse();
        }
        if (state.recording_state_block) {
            state.recording_block.has_sampler_states = true;
            if (pixel_sampler) {
                state.recording_block.sampler_states[sampler][type] = value;
            } else {
                state.recording_block.vertex_sampler_states[
                    sampler - D3D9_VERTEX_TEXTURE_SAMPLER_FIRST][type] = value;
            }
        }
        m.set_return(D3D_OK);
        return;
    }

    case 71: { // SetPaletteEntries
        // Learn: IDirect3DDevice9::SetPaletteEntries — 256 PALETTEENTRY;
        // PaletteNumber in [0, 0xFFFF]. With D3DPTEXTURECAPS_ALPHAPALETTE
        // set, any peFlags/alpha values are allowed (peFlags = alpha).
        const u32 palette_number = static_cast<u32>(m.arg(1));
        const GuestAddr entries_in = m.arg_ptr(2);
        if (palette_number > DeviceState::kMaxPaletteNumber ||
            entries_in == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        DeviceState::TexturePalette entries{};
        if (m.memory()
                .read_bytes(entries_in, entries.data(), entries.size())
                .is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        state.texture_palettes[palette_number] = entries;
        ++state.texture_palette_serial;
        // Learn: the current palette translates all paletted textures; when
        // entries for the active palette change, bound stages must re-expand.
        if (palette_number == state.current_texture_palette) {
            sync_bound_palettized_textures(state);
        }
        m.set_return(D3D_OK);
        return;
    }
    case 72: { // GetPaletteEntries
        // Learn: IDirect3DDevice9::GetPaletteEntries — never-set palettes
        // return the documented 0xFFFFFFFF defaults.
        const u32 palette_number = static_cast<u32>(m.arg(1));
        const GuestAddr entries_out = m.arg_ptr(2);
        if (palette_number > DeviceState::kMaxPaletteNumber ||
            entries_out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        DeviceState::TexturePalette entries{};
        entries.fill(0xFFu);
        if (const auto it = state.texture_palettes.find(palette_number);
            it != state.texture_palettes.end()) {
            entries = it->second;
        }
        if (m.memory()
                .write_bytes(entries_out, entries.data(), entries.size())
                .is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 73: { // SetCurrentTexturePalette
        // Learn: IDirect3DDevice9::SetCurrentTexturePalette — selects which
        // of the up-to-0xFFFF device palettes is current for all stages.
        const u32 palette_number = static_cast<u32>(m.arg(1));
        if (palette_number > DeviceState::kMaxPaletteNumber) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (state.current_texture_palette != palette_number) {
            state.current_texture_palette = palette_number;
            ++state.texture_palette_serial;
            sync_bound_palettized_textures(state);
        }
        m.set_return(D3D_OK);
        return;
    }
    case 74: { // GetCurrentTexturePalette
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0 ||
            m.write_u32(out, state.current_texture_palette).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    }
}

} // namespace vitreo::winapi
