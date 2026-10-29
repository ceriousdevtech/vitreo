// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_dispatch.cpp
/// @brief Compute dispatch: the flat compute bindings, UAV counters and
///        D3D11Manager::execute_dispatch.
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

// The flat compute bindings for one stage's register arrays: b# into the
// constant-buffer window, t# buffers and textures, s#, and u# buffers and
// textures. Shared by the compute stage and the geometry stage, whose shader
// runs as a compute kernel; a stage on descriptor tables binds none of these.
// Learn (imm_atomic_alloc): the counter belongs to a structured buffer UAV
// created with the Count or Append flag. A shader that moves the counter of a
// view without one is undefined on Windows; here it would move it through an
// unbound Metal buffer, which can fault the GPU. So the caller skips the work,
// the one outcome that is safe and still visible.
bool D3D11Manager::bind_uav_counters(
    std::span<const GuestAddr> unordered_access_views,
    const d3d11::ShaderBindingLayout& layout,
    std::span<const graphics::BackendBuffer*,
              graphics::BACKEND_MAX_UAV_COUNTER_BINDINGS> counters) {
    for (u32 k = 0; k < layout.uav_counter_count; ++k) {
        const u32 reg = layout.uav_counter_registers[k];
        const auto view =
            reg < unordered_access_views.size()
                ? m_unordered_access_views.find(unordered_access_views[reg])
                : m_unordered_access_views.end();
        if (view == m_unordered_access_views.end() ||
            !view->second.counter) {
            pipeline_diag("u%u: the shader moves its hidden counter, and the "
                          "view bound there has none (not APPEND/COUNTER)",
                          reg);
            return false;
        }
        counters[k] = view->second.counter.get();
    }
    return true;
}

std::string D3D11Manager::describe_dispatch_inputs(
    std::span<const GuestAddr> constant_buffers,
    std::span<const GuestAddr> shader_resources) const {
    std::string note;
    char buf[200];
    for (usize slot = 0; slot < 2 && slot < shader_resources.size(); ++slot) {
        if (shader_resources[slot] == 0) continue;
        const auto srv = m_shader_resource_views.find(shader_resources[slot]);
        if (srv == m_shader_resource_views.end()) {
            std::snprintf(buf, sizeof(buf), " t%zu=0x%llx(unknown view)", slot,
                          static_cast<unsigned long long>(shader_resources[slot]));
            note += buf;
            continue;
        }
        if (srv->second.texture != 0) {
            const auto tex = m_textures.find(srv->second.texture);
            if (tex != m_textures.end()) {
                const auto& d = tex->second.desc;
                // How much of mip 0 is non-zero right now: a zero input
                // names an upload/ordering defect rather than a shader one.
                double nonzero_fraction = -1.0;
                const auto fmt = graphics::format_table(backend_format(d.format));
                // The readback drains the GPU, which serialises the very
                // dispatches being diagnosed (it hid a zero bloom spectrum),
                // so it is opt-in.
                static const bool readback_inputs =
                    std::getenv("VITREO_D3D11_DIAG_READBACK") != nullptr;
                if (readback_inputs && tex->second.backend && d.width > 0 &&
                    d.height > 0 &&
                    d.sample_count <= 1 && fmt.bytes_per_pixel != 0 &&
                    !fmt.is_compressed) {
                    const usize pitch =
                        static_cast<usize>(d.width) * fmt.bytes_per_pixel;
                    std::vector<u8> raw(pitch * d.height, 0u);
                    if (tex->second.backend->readback(0, raw.data(),
                                                      static_cast<u32>(pitch))) {
                        usize nonzero = 0;
                        for (const u8 b : raw) nonzero += b != 0u;
                        nonzero_fraction =
                            static_cast<double>(nonzero) / static_cast<double>(raw.size());
                    }
                }
                std::snprintf(buf, sizeof(buf),
                              " t%zu=tex 0x%llx fmt=%u %ux%u mips=%u bind=0x%x"
                              " viewfmt=%u draws=%u uav=%u copies=%u nonzero=%.3f",
                              slot,
                              static_cast<unsigned long long>(srv->second.texture),
                              d.format, d.width, d.height, d.mip_levels,
                              d.bind_flags, srv->second.format,
                              tex->second.diag_rt_draws,
                              tex->second.diag_uav_binds,
                              tex->second.diag_copy_dst, nonzero_fraction);
            } else {
                std::snprintf(buf, sizeof(buf), " t%zu=tex 0x%llx (not live)",
                              slot,
                              static_cast<unsigned long long>(srv->second.texture));
            }
            note += buf;
        } else if (srv->second.buffer != 0) {
            std::snprintf(buf, sizeof(buf), " t%zu=buf 0x%llx", slot,
                          static_cast<unsigned long long>(srv->second.buffer));
            note += buf;
        }
    }
    if (!constant_buffers.empty() && constant_buffers[0] != 0) {
        const auto cb = m_buffers.find(constant_buffers[0]);
        if (cb != m_buffers.end() && cb->second.shadow.size() >= 32) {
            note += " cb0=";
            const usize words = std::min<usize>(16, cb->second.shadow.size() / 4);
            for (usize i = 0; i < words; ++i) {
                u32 word = 0;
                std::memcpy(&word, cb->second.shadow.data() + i * 4, 4);
                std::snprintf(buf, sizeof(buf), "%s%08x", i ? "," : "", word);
                note += buf;
            }
        }
    }
    return note;
}

void D3D11Manager::bind_compute_stage(
    std::span<const GuestAddr> constant_buffers,
    std::span<const u32> constant_first,
    std::span<const GuestAddr> shader_resources,
    std::span<const GuestAddr> samplers,
    std::span<const GuestAddr> unordered_access_views,
    const d3d11::ShaderBindingLayout& layout,
    graphics::BackendDispatchCall& call) {
    constexpr usize kComputeConstantBufferBase =
        graphics::BACKEND_CONSTANT_BUFFER_BASE;
    // A table-mode shader reads b#/t#/s# from its argument buffers, which occupy
    // these same Metal buffer indices. Binding both would have the flat writes
    // land on top of the tables.
    for (usize slot = 0;
         !layout.uses_descriptor_tables &&
         slot < constant_buffers.size() &&
         kComputeConstantBufferBase + slot < call.buffers.size(); ++slot) {
        auto buffer = m_buffers.find(constant_buffers[slot]);
        if (buffer != m_buffers.end()) {
            call.buffers[kComputeConstantBufferBase + slot] =
                buffer->second.backend.get();
            // CSSetConstantBuffers1's window, in constants of 16 bytes.
            call.buffer_offsets[kComputeConstantBufferBase + slot] =
                constant_buffer_bind_offset(constant_buffers[slot],
                                            constant_first[slot]);
        }
    }

    for (usize slot = 0;
         !layout.uses_descriptor_tables &&
         slot < shader_resources.size() &&
         slot < graphics::BACKEND_MAX_TEXTURE_SLOTS; ++slot) {
        const auto srv = m_shader_resource_views.find(
            shader_resources[slot]);
        if (srv != m_shader_resource_views.end()) {
            // A Buffer<T> SRV: the shader reads a texture_buffer at t#, in the
            // texture namespace, exactly as the draw stages bind it.
            if (srv->second.texture_buffer != nullptr &&
                slot < call.texture_buffers.size()) {
                call.texture_buffers[slot] = srv->second.texture_buffer.get();
            }
            if (srv->second.buffer != 0 &&
                srv->second.texture_buffer == nullptr) {
                constexpr usize kComputeSrvBufferBase =
                    graphics::BACKEND_SRV_BUFFER_BASE;
                const usize backend_slot = kComputeSrvBufferBase + slot;
                auto buffer = m_buffers.find(srv->second.buffer);
                if (backend_slot < call.buffers.size() &&
                    buffer != m_buffers.end()) {
                    call.buffers[backend_slot] = buffer->second.backend.get();
                    call.buffer_offsets[backend_slot] =
                        srv->second.first_element *
                        srv->second.element_bytes;
                }
            }
            // Same resolver as the draw stages: a volume lives in backend_3d
            // and binds through textures_3d, a 2D/cube/array texture through
            // textures + its view. This block used to bind only `backend`,
            // so every compute shader reading a Texture3D SRV read zeros
            // (measured: the replay audit, UE's volumetric passes).
            resolve_shader_resource(shader_resources[slot], call.textures[slot],
                                    call.texture_views[slot],
                                    call.textures_3d[slot]);
            if (call.textures[slot] != nullptr &&
                !d3d11::fit_view_to_declaration(
                    layout, false, static_cast<u32>(slot),
                    call.texture_views[slot])) {
                call.textures[slot] = nullptr;
            }
        }

        // s# is independent of t# (resolve_sampler).
        if (slot < samplers.size() &&
            resolve_sampler(samplers[slot], call.samplers[slot], nullptr)) {
            call.sampler_bound[slot] = true;
        }
    }

    for (usize slot = 0;
         !layout.uses_descriptor_tables &&
         slot < unordered_access_views.size() &&
         slot < call.textures.size(); ++slot) {
        auto view = m_unordered_access_views.find(
            unordered_access_views[slot]);
        if (view == m_unordered_access_views.end()) continue;
        if (view->second.texture_buffer != nullptr) {
            // RWBuffer<T>: a read_write texture_buffer in the u# texture
            // window, never the raw buffer window.
            const usize backend_slot = graphics::BACKEND_UAV_TEXTURE_BASE + slot;
            if (backend_slot < call.texture_buffers.size()) {
                call.texture_buffers[backend_slot] =
                    view->second.texture_buffer.get();
            }
            continue;
        }
        if (view->second.buffer != 0) {
            constexpr usize kComputeUavBufferBase =
                graphics::BACKEND_UAV_BUFFER_BASE;
            const usize backend_slot = kComputeUavBufferBase + slot;
            auto buffer = m_buffers.find(view->second.buffer);
            if (backend_slot < call.buffers.size() &&
                buffer != m_buffers.end()) {
                call.buffers[backend_slot] = buffer->second.backend.get();
                call.buffer_offsets[backend_slot] =
                    view->second.first_element *
                    view->second.element_bytes;
            }
            continue;
        }
        auto texture = m_textures.find(view->second.texture);
        if (texture != m_textures.end()) {
            constexpr usize kComputeUavTextureBase =
                graphics::BACKEND_UAV_TEXTURE_BASE;
            if (kComputeUavTextureBase + slot < call.textures.size()) {
                const usize backend_slot = kComputeUavTextureBase + slot;
                if (texture->second.volume) {
                    // One mip under the view's format -- an R32 cast
                    // included (volume_uav_binding).
                    graphics::BackendTexture3D* volume =
                        volume_uav_binding(view->second, texture->second,
                                           static_cast<u32>(slot));
                    if (volume == nullptr) continue;
                    ++texture->second.diag_uav_binds;
                    texture->second.diag_uav_writer = m_diag_binding_shader;
                    texture->second.diag_uav_writer_compute = true;
                    call.textures_3d[backend_slot] = volume;
                    continue;
                }
                ++texture->second.diag_uav_binds;
                texture->second.diag_uav_writer = m_diag_binding_shader;
                texture->second.diag_uav_writer_compute = true;
                texture->second.diag_uav_note =
                    describe_dispatch_inputs(constant_buffers, shader_resources);
                // Rarely written targets are one-shot pipelines (UE's bloom
                // kernel transform runs once per kernel change); log each of
                // their first few dispatches with inputs, since the
                // first-dispatch line above only covers the very first run.
                if (texture->second.diag_uav_binds <= 6) {
                    pipeline_diag("Dispatch rare cs=0x%llx -> uav tex 0x%llx "
                                  "(%ux%u fmt=%u, write #%u)%s",
                                  static_cast<unsigned long long>(
                                      m_diag_binding_shader),
                                  static_cast<unsigned long long>(
                                      texture->first),
                                  texture->second.desc.width,
                                  texture->second.desc.height,
                                  texture->second.desc.format,
                                  texture->second.diag_uav_binds,
                                  texture->second.diag_uav_note.c_str());
                }
                graphics::BackendTextureViewDesc uav_view = uav_texture_view(
                    view->second.view_dimension, view->second.mip_slice,
                    view->second.first_array_slice, view->second.array_size,
                    backend_format(view->second.format),
                    texture->second.backend->desc().pixel_format);
                if (!d3d11::fit_view_to_declaration(
                        layout, true, static_cast<u32>(slot), uav_view)) {
                    continue;
                }
                call.textures[backend_slot] = texture->second.backend.get();
                call.texture_views[backend_slot] = uav_view;
            }
        }
    }
}

void D3D11Manager::note_immediate_gpu_work(GuestAddr context) {
    // VITREO_D3D11_EARLY_SUBMIT=<n>: the minimum draws/dispatches recorded
    // before an early submit (default 256; 0 turns early submission off).
    // 64 split one menu frame into ~60 command buffers for no
    // measurable gain over 256 or 512 (6+6 interleaved runs, 2026-10-07).
    // Without it the whole frame reached the GPU at Present, and the
    // engine's busy-wait on the previous frame's occlusion queries (~5 ms
    // a frame at that menu) waited out that frame's GPU tail.
    static const u32 min_chunk = [] {
        const char* v = std::getenv("VITREO_D3D11_EARLY_SUBMIT");
        return v == nullptr ? 256u
                            : static_cast<u32>(std::strtoul(v, nullptr, 10));
    }();
    if (min_chunk == 0 || is_deferred_context(context)) return;
    if (++m_work_since_submit < min_chunk) return;
    if (!m_backend.wants_early_submit()) return;
    m_work_since_submit = 0;
    m_backend.flush();
}

void D3D11Manager::execute_dispatch(
    GuestAddr context, u32 group_count_x, u32 group_count_y,
    u32 group_count_z, GuestAddr indirect_args, u32 indirect_offset) {
    auto state_it = m_context_states.find(context);
    if (state_it == m_context_states.end()) return;
    // As with an indirect draw, the counts of an indirect dispatch were written
    // by the GPU and zero is a legal value for it to have written.
    const graphics::BackendBuffer* indirect_buffer = nullptr;
    if (indirect_args != 0) {
        // ThreadGroupCountX, Y and Z: 3 UINTs.
        indirect_buffer = resolve_indirect_args(
            indirect_args, indirect_offset, "DispatchIndirect",
            3u * static_cast<u32>(sizeof(u32)));
        if (indirect_buffer == nullptr) return;
    } else if (group_count_x == 0 || group_count_y == 0 ||
               group_count_z == 0) {
        return;
    }
    auto& state = state_it->second;
    auto shader = m_compute_shaders.find(state.compute_shader);
    if (shader == m_compute_shaders.end()) {
        pipeline_diag("Dispatch skipped: no compute shader bound");
        return;
    }
    // Border colours Metal cannot express: the variant sampling through
    // border twins (border_twin_mask), compiled once per mask.
    const u32 cs_twins = border_twin_mask(state.compute_samplers);
    // R32G32B32 typed buffer views: the variant reading three R32 texels
    // per element (rgb32_buffer_srv_mask).
    bool rgb32_unservable = false;
    const u32 cs_rgb32 = rgb32_buffer_srv_mask(state.compute_shader_resources,
                                               rgb32_unservable);
    if (rgb32_unservable) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED d3d11!Dispatch: an "
                         "R32G32B32 typed buffer view at t32 or above\n");
        }
        return;
    }
    // Multisample views of one-sample textures: the variant declaring
    // them 2D (single_sample_ms_srv_mask).
    bool ms1_unservable = false;
    const u32 cs_ms1 = single_sample_ms_srv_mask(
        state.compute_shader_resources, ms1_unservable);
    if (ms1_unservable) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED d3d11!Dispatch: a "
                         "TEXTURE2DMS view of a one-sample texture at t32 or "
                         "above\n");
        }
        return;
    }
    // MINIMUM_ / MAXIMUM_ samplers: the variant reducing in the shader.
    const u32 cs_minmax = minmax_sampler_mask(state.compute_samplers);
    const std::pair<u64, u64> variant_key{
        static_cast<u64>(cs_twins) | (static_cast<u64>(cs_rgb32) << 32),
        static_cast<u64>(cs_ms1) | (static_cast<u64>(cs_minmax) << 32)};
    auto cached = shader->second.pipelines.find(variant_key);
    if (cached == shader->second.pipelines.end()) {
        const std::string* msl = &shader->second.msl_source;
        if (variant_key != std::pair<u64, u64>{}) {
            auto found = shader->second.border_variants.find(variant_key);
            if (found == shader->second.border_variants.end()) {
                std::string variant;
                u32 x = 0, y = 0, z = 0;
                if (!compile_compute_shader_to_msl(
                        shader->second.bytecode.data(),
                        shader->second.bytecode.size(), variant, x, y, z,
                        nullptr, cs_twins, cs_rgb32, cs_ms1, cs_minmax)) {
                    pipeline_diag("Compute shader variant translation failed");
                    variant.clear();
                }
                found = shader->second.border_variants
                            .emplace(variant_key, std::move(variant)).first;
            }
            msl = &found->second;
        }
        std::shared_ptr<graphics::BackendComputePipeline> built;
        if (!msl->empty()) {
            graphics::BackendComputePipelineDesc desc{};
            desc.compute_shader_msl = *msl;
            desc.math_mode = math_mode_for_dxbc(shader->second.bytecode);
            // SPIRV-Cross renames the DXBC entry point `main` to `main0`.
            desc.compute_entry_point = "main0";
            desc.cache_key = std::hash<std::string_view>{}(std::string_view(
                reinterpret_cast<const char*>(shader->second.bytecode.data()),
                shader->second.bytecode.size())) ^
                (variant_key.first * 0x9E3779B97F4A7C15ull) ^
                (variant_key.second * 0xC2B2AE3D27D4EB4Full);
            built = m_backend.create_compute_pipeline(desc);
            if (built && !built->valid()) built.reset();
            if (!built) {
                pipeline_diag("Dispatch skipped: compute pipeline unavailable");
            }
        }
        cached = shader->second.pipelines.emplace(variant_key, std::move(built))
                     .first;
    }
    if (!cached->second) return;
    graphics::BackendDispatchCall call{};
    call.pipeline = cached->second.get();
    call.group_count_x = group_count_x;
    call.group_count_y = group_count_y;
    call.group_count_z = group_count_z;
    call.threads_per_group_x = shader->second.workgroup_size_x;
    call.threads_per_group_y = shader->second.workgroup_size_y;
    call.threads_per_group_z = shader->second.workgroup_size_z;
    const d3d11::ShaderBindingLayout cs_layout = shader->second.binding_layout;
    if (!sync_descriptor_tables(state, d3d11::StageBindings::Compute, cs_layout,
                                call.descriptor_tables, cs_twins)) {
        pipeline_diag("Dispatch skipped: compute descriptor tables "
                      "unavailable");
        return;
    }
    if (indirect_buffer != nullptr) {
        call.indirect.buffer = indirect_buffer;
        call.indirect.offset = indirect_offset;
    }
    // One line per compute shader on its first dispatch (d3d11 diag only):
    // what it reads and its cb0 words, so a zero or NaN UAV target can be
    // traced upstream even when the intermediate textures are transient.
    {
        static std::unordered_set<GuestAddr> logged;
        if (logged.insert(state.compute_shader).second) {
            pipeline_diag("Dispatch first cs=0x%llx groups=%ux%ux%u threads=%ux%ux%u%s",
                       static_cast<unsigned long long>(state.compute_shader),
                       group_count_x, group_count_y, group_count_z,
                       shader->second.workgroup_size_x,
                       shader->second.workgroup_size_y,
                       shader->second.workgroup_size_z,
                       describe_dispatch_inputs(state.compute_constant_buffers,
                                                state.compute_shader_resources)
                           .c_str());
            // The bytecode beside the diag log, so the dispatch can be replayed
            // on Windows with tools/oracle_probe/d3d11_dxbc_compute_probe.c.
            if (::access("/tmp/vitreo-d3d11-diag", F_OK) == 0 &&
                !shader->second.bytecode.empty()) {
                char path[128];
                std::snprintf(path, sizeof(path),
                              "/tmp/vitreo-d3d11-diag-%d-cs-%llx.dxbc",
                              static_cast<int>(::getpid()),
                              static_cast<unsigned long long>(state.compute_shader));
                if (std::FILE* f = std::fopen(path, "wb")) {
                    std::fwrite(shader->second.bytecode.data(), 1,
                                shader->second.bytecode.size(), f);
                    std::fclose(f);
                }
            }
        }
    }
    m_diag_binding_shader = state.compute_shader;
    bind_compute_stage(state.compute_constant_buffers,
                       state.compute_constant_first,
                       state.compute_shader_resources, state.compute_samplers,
                       state.compute_unordered_access_views, cs_layout, call);
    m_diag_binding_shader = 0;
    if (!bind_uav_counters(
            state.compute_unordered_access_views, cs_layout,
            std::span(call.buffers)
                .subspan<graphics::BACKEND_UAV_COUNTER_BUFFER_BASE,
                         graphics::BACKEND_MAX_UAV_COUNTER_BINDINGS>())) {
        pipeline_diag("Dispatch skipped: a UAV counter is unbound");
        return;
    }
    // The push block the twin variant reads its border colours from: the
    // same layout as the draw stages (member 0: the s# LOD biases). Member 2
    // carries the t#/u# buffer view sizes for the translator's out-of-bounds
    // guard, so the block is bound on every dispatch.
    constexpr u32 kSizeTableWord = graphics::kGuardSizeTableOffset / 4;
    std::array<u32, graphics::kPushBlockBytes / 4> compute_push{};
    if (cs_twins != 0) {
        for (u32 slot = 0; slot < kD3D11SamplerSlotCount; ++slot) {
            if ((cs_twins & (1u << slot)) == 0) continue;
            // s0..s7 in push member 1 (byte 64), s8..s15 in member 11.
            const u32 word = slot < 8
                ? 16u + slot * 4u
                : graphics::kBorderHighOffset / 4u + (slot - 8u) * 4u;
            const auto colour =
                border_twin_push_colour(state.compute_samplers[slot]);
            std::memcpy(compute_push.data() + word, colour.data(), 16);
        }
        // Flat-model twins bind at Metal samplers 8..15; a descriptor-table
        // shader's are in its sampler table.
        call.border_twin_mask =
            shader->second.binding_layout.uses_descriptor_tables
                ? 0u : (cs_twins & 0xFFu);
    }
    for (u32 slot = 0; slot < kD3D11SamplerSlotCount; ++slot) {
        const float bias = sampler_mip_lod_bias(state.compute_samplers[slot]);
        std::memcpy(compute_push.data() + slot, &bias, sizeof(bias));
    }
    compute_push[graphics::kPointMipMaskOffset / 4] =
        point_mip_sampler_mask(state.compute_samplers);
    compute_push[graphics::kPointMipNudgeMaskOffset / 4] =
        point_mip_nudge_mask(state.compute_samplers);
    fill_minmax_samplers(compute_push, state.compute_samplers);
    for (u32 slot = 0; slot < graphics::kGuardSizeSrvSlots; ++slot) {
        const GuestAddr view = state.compute_shader_resources[slot];
        compute_push[kSizeTableWord + slot] =
            view != 0 ? srv_buffer_size_words(view) : 0u;
    }
    for (u32 slot = 0; slot < graphics::kGuardSizeUavSlots &&
                       slot < state.compute_unordered_access_views.size();
         ++slot) {
        compute_push[kSizeTableWord + graphics::kGuardSizeSrvSlots + slot] =
            uav_buffer_size_words(state.compute_unordered_access_views[slot]);
    }
    fill_constant_buffer_limits(compute_push, state.compute_constant_buffers,
                                state.compute_constant_first,
                                state.compute_constant_count);
    call.constants = compute_push.data();
    call.constants_size = sizeof(compute_push);
    // PIPELINE_STATISTICS CSInvocations, counted where the dispatch is
    // recorded (an indirect one's argument block is copied just before it).
    count_pipeline_dispatch(
        context,
        static_cast<u64>(group_count_x) * group_count_y * group_count_z,
        static_cast<u64>(call.threads_per_group_x) * call.threads_per_group_y *
            call.threads_per_group_z,
        indirect_buffer, indirect_offset);
    if (!m_backend.dispatch(call)) {
        pipeline_diag("Dispatch backend submission failed");
    } else {
        note_immediate_gpu_work(context);
    }
    // Race bisection knobs (diagnostic): submit after every dispatch, or
    // drain the GPU after every dispatch. A defect that the first knob
    // cures is host-side (record-time vs encode-time state); one only the
    // second cures is a GPU ordering hazard.
    static const int per_dispatch_sync = [] {
        const char* v = std::getenv("VITREO_D3D11_DISPATCH_SYNC");
        return v == nullptr ? 0 : std::atoi(v);
    }();
    if (per_dispatch_sync == 1) {
        m_backend.flush();
    } else if (per_dispatch_sync == 2) {
        (void)m_backend.wait_idle();
    }
}

} // namespace vitreo::winapi
