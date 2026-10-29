// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_descriptor_tables.cpp
/// @brief Descriptor-table binding (D3D11Manager::sync_descriptor_tables).
///        Split out of d3d11_pipeline.cpp as verbatim code motion.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_pipeline_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_state_census.hpp"
#include "winapi/graphics/d3d11/d3d11_stdcall.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/delta_record.hpp"
#include "winapi/graphics/d3d11/d3d11_draw_memo.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "winapi/graphics/common/live_diag.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "winapi/graphics/common/d3d_sampler.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "graphics/shaders/gs_passthrough.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <vitreo/diagnostics.hpp>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <limits>
#include <map>
#include <tuple>
#include <mutex>
#include <vector>
#include <cctype>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <mach/mach_time.h>

namespace vitreo::winapi {

// =============================================================================
// Descriptor-table binding
// =============================================================================

bool D3D11Manager::sync_descriptor_tables(
    ContextState& state, d3d11::StageBindings stage,
    const d3d11::ShaderBindingLayout& layout,
    std::array<const graphics::BackendDescriptorTable*,
               graphics::BACKEND_MAX_DESCRIPTOR_SETS>& out,
    u32 border_twins) {
    if (!layout.uses_descriptor_tables) return true;

    // The register spaces this stage binds from. Which array a stage reads is
    // the only thing that differs between them.
    const std::array<GuestAddr, d3d11::CONSTANT_BUFFER_SLOTS>* cbuffers =
        nullptr;
    const std::array<u32, d3d11::CONSTANT_BUFFER_SLOTS>* constant_first =
        nullptr;
    const d3d11::SrvSlots* resources = nullptr;
    const d3d11::SamplerSlots* samplers = nullptr;
    switch (stage) {
    case d3d11::StageBindings::Vertex:
        cbuffers = &state.vertex_constant_buffers;
        constant_first = &state.vertex_constant_first;
        resources = &state.vertex_shader_resources;
        samplers = &state.vertex_samplers;
        break;
    case d3d11::StageBindings::Pixel:
        cbuffers = &state.pixel_constant_buffers;
        constant_first = &state.pixel_constant_first;
        resources = &state.pixel_shader_resources;
        samplers = &state.pixel_samplers;
        break;
    case d3d11::StageBindings::Compute:
        cbuffers = &state.compute_constant_buffers;
        constant_first = &state.compute_constant_first;
        resources = &state.compute_shader_resources;
        samplers = &state.compute_samplers;
        break;
    default:
        return false;
    }

    auto& tables = state.descriptor_tables[static_cast<usize>(stage)];
    // A table is sized to the whole D3D register space, not to this shader's
    // high-water mark: the same table serves every shader this stage binds, and
    // slot N is descriptor N in all of them. The shader's own struct may be
    // shorter, which is fine -- a larger buffer behind a shorter struct is not.
    const auto ensure = [&](unsigned set, unsigned slots,
                            graphics::BackendDescriptorClass klass)
                            -> graphics::BackendDescriptorTable* {
        if (!tables[set]) {
            tables[set] = m_backend.create_descriptor_table({slots, klass});
            if (!tables[set]) {
                pipeline_diag("descriptor set %u (%u slots) could not be "
                              "created", set, slots);
                return nullptr;
            }
        }
        return tables[set].get();
    };

    if (layout.constant_buffer_slots > 0) {
        auto* table = ensure(0, d3d11::CONSTANT_BUFFER_SLOTS,
                             graphics::BackendDescriptorClass::Buffer);
        if (table == nullptr) return false;
        for (u32 slot = 0; slot < layout.constant_buffer_slots; ++slot) {
            const auto buffer = m_buffers.find((*cbuffers)[slot]);
            const graphics::BackendBuffer* backend =
                buffer != m_buffers.end() ? buffer->second.backend.get()
                                          : nullptr;
            // The *SetConstantBuffers1 window applies in both binding modes: a
            // descriptor is a GPU address, so the offset folds into it.
            if (!table->set_buffer(
                    slot, backend,
                    constant_buffer_bind_offset((*cbuffers)[slot],
                                                (*constant_first)[slot]))) {
                return false;
            }
        }
        out[0] = table;
    }

    if (layout.srv_slots > 0) {
        auto* table = ensure(1, d3d11::SHADER_RESOURCE_SLOTS,
                             graphics::BackendDescriptorClass::Texture);
        if (table == nullptr) return false;
        for (u32 slot = 0; slot < layout.srv_slots; ++slot) {
            const GuestAddr srv = (*resources)[slot];
            // A t# register names a texture, a structured/raw buffer, or a typed
            // buffer -- never more than one. Ask the buffer resolver first,
            // because a buffer SRV has no texture to resolve.
            const graphics::BackendBuffer* srv_buffer = nullptr;
            unsigned srv_offset = 0;
            const graphics::BackendTextureBuffer* typed_buffer = nullptr;
            resolve_buffer_resource(srv, srv_buffer, srv_offset, typed_buffer);
            if (srv_buffer != nullptr) {
                if (!table->set_buffer(slot, srv_buffer, srv_offset)) {
                    return false;
                }
                continue;
            }
            if (typed_buffer != nullptr) {
                // A typed buffer is a texture-buffer view: the table holds it
                // as the texture it is. Leaving it empty handed every
                // table-mode shader zeros -- UE's heavier GPU-scene vertex
                // shaders lost their per-instance data this way while the
                // flat-bound depth prepass kept it.
                if (!table->set_texture_buffer(slot, typed_buffer)) {
                    return false;
                }
                continue;
            }
            const graphics::BackendTexture2D* texture_2d = nullptr;
            const graphics::BackendTexture3D* texture_3d = nullptr;
            graphics::BackendTextureViewDesc view{};
            resolve_shader_resource(srv, texture_2d, view, texture_3d);
            if (texture_2d != nullptr &&
                !d3d11::fit_view_to_declaration(layout, false, slot, view)) {
                texture_2d = nullptr;
            }
            if (texture_3d != nullptr) {
                if (!table->set_texture_3d(slot, texture_3d)) return false;
            } else if (!table->set_texture(slot, texture_2d, view)) {
                return false;
            }
        }
        out[1] = table;
    }

    if (layout.sampler_slots > 0) {
        // The sixteen s#, then each one's border twin at SAMPLER_SLOTS + n
        // (DxbcTranslationOptions::border_twin_binding_base).
        auto* table = ensure(2, 2 * d3d11::SAMPLER_SLOTS,
                             graphics::BackendDescriptorClass::Sampler);
        if (table == nullptr) return false;
        for (u32 slot = 0; slot < layout.sampler_slots; ++slot) {
            // s# is its own register space: never paired with t# of the same
            // number (resolve_sampler).
            graphics::BackendSamplerDesc sampler{};
            (void)resolve_sampler((*samplers)[slot], sampler, nullptr);
            if (!table->set_sampler(slot, sampler)) return false;
        }
        for (u32 slot = 0; slot < d3d11::SAMPLER_SLOTS; ++slot) {
            if ((border_twins & (1u << slot)) == 0) continue;
            graphics::BackendSamplerDesc twin{};
            (void)resolve_sampler((*samplers)[slot], twin, nullptr);
            twin.border_color = 0xFFFFFFFFu;  // opaque white
            if (!table->set_sampler(d3d11::SAMPLER_SLOTS + slot, twin)) {
                return false;
            }
        }
        out[2] = table;
    }

    // u# exists only on the stages that can declare it. A table-mode shader
    // reads its whole u# space from this argument buffer, so leaving it unbound
    // would give the shader nothing to write to -- for a compute shader that is
    // its entire output.
    if (stage == d3d11::StageBindings::Compute) {
        auto* table = ensure(3, d3d11::UAV_SLOTS,
                             graphics::BackendDescriptorClass::Texture);
        if (table == nullptr) return false;
        for (u32 slot = 0; slot < d3d11::UAV_SLOTS; ++slot) {
            const auto view = m_unordered_access_views.find(
                state.compute_unordered_access_views[slot]);
            if (view == m_unordered_access_views.end()) {
                table->clear_slot(slot);
                continue;
            }
            if (view->second.texture_buffer != nullptr) {
                // As for a typed t#: a RWBuffer<T> is a writable
                // texture-buffer view over the buffer's own storage. Its
                // element bias travels in the u# size-table entry, which
                // u8 and above do not have.
                if (view->second.texture_buffer->element_bias() != 0 &&
                    slot >= graphics::kGuardSizeUavSlots) {
                    static bool reported = false;
                    if (!reported) {
                        reported = true;
                        std::fprintf(stderr,
                                     "[vitreo] UNIMPLEMENTED d3d11!CSSetUnordered"
                                     "AccessViews: a typed buffer UAV at u%u "
                                     "whose FirstElement is not on the "
                                     "backend's texture alignment (u8 and "
                                     "above carry no element bias); not "
                                     "bound\n",
                                     slot);
                    }
                    table->clear_slot(slot);
                    continue;
                }
                if (!table->set_texture_buffer(
                        slot, view->second.texture_buffer.get())) {
                    return false;
                }
                continue;
            }
            if (view->second.buffer != 0) {
                const auto buffer = m_buffers.find(view->second.buffer);
                const graphics::BackendBuffer* backend =
                    buffer != m_buffers.end() ? buffer->second.backend.get()
                                              : nullptr;
                const unsigned offset =
                    view->second.first_element * view->second.element_bytes;
                if (!table->set_buffer(slot, backend, offset)) return false;
                continue;
            }
            const auto texture = m_textures.find(view->second.texture);
            if (texture == m_textures.end()) {
                table->clear_slot(slot);
                continue;
            }
            if (texture->second.volume) {
                // As the flat path (volume_uav_binding).
                graphics::BackendTexture3D* volume =
                    volume_uav_binding(view->second, texture->second, slot);
                if (volume == nullptr) {
                    table->clear_slot(slot);
                    continue;
                }
                ++texture->second.diag_uav_binds;
                texture->second.diag_uav_writer = state.compute_shader;
                texture->second.diag_uav_writer_compute = true;
                if (!table->set_texture_3d(slot, volume)) return false;
                continue;
            }
            texture->second.diag_uav_writer = state.compute_shader;
            texture->second.diag_uav_writer_compute = true;
            // The same view the flat path binds: mip, array window, format.
            graphics::BackendTextureViewDesc view_desc =
                uav_texture_view(view->second.view_dimension,
                                 view->second.mip_slice,
                                 view->second.first_array_slice,
                                 view->second.array_size,
                                 backend_format(view->second.format),
                                 texture->second.backend->desc().pixel_format);
            if (!d3d11::fit_view_to_declaration(layout, true, slot,
                                                view_desc)) {
                table->clear_slot(slot);
                continue;
            }
            if (!table->set_texture(slot, texture->second.backend.get(),
                                    view_desc)) {
                return false;
            }
        }
        out[3] = table;
    }

    return true;
}

} // namespace vitreo::winapi
