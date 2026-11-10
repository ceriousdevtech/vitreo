// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_submit_queue_encode_draw.mm
/// @brief Encoding one draw into the open render encoder.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreFoundation/CoreFoundation.h>
#import <pthread.h>

#include "graphics/shaders/mesh_stage.hpp"
#include <cmath>
#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/metal/metal_submit_queue_internal.hpp"
#include "graphics/metal/metal_tiled.hpp"
#include "graphics/metal/metal_shared_resource.hpp"
#include "graphics/metal/metal_command.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_swapchain.hpp"
#include "graphics/metal/metal_texture.hpp"

#include <vitreo/present_cpu.hpp>
#include "graphics/contracts/delta_record.hpp"
#include "graphics/contracts/slim_record.hpp"

#include <algorithm>
#include <string>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <utility>

namespace vitreo::graphics {

namespace {
std::atomic<std::vector<u64>*> g_encode_hash_sink{nullptr};

/// UAV buffers and textures this draw writes. The statistics record is not
/// included: the CPU reads it after the encoder ends. False only when a write
/// exists but did not fit in @p out (the caller uses the scope barrier).
bool collect_uav_writes(const CsDraw& draw, void** out, u32 cap, u32& count) {
    count = 0;
    bool overflow = false;
    const auto add = [&](void* resource) {
        if (resource == nullptr) return;
        for (u32 i = 0; i < count; ++i) {
            if (out[i] == resource) return;
        }
        if (count >= cap) {
            overflow = true;
            return;
        }
        out[count++] = resource;
    };
    for (void* buffer : draw.ps_uav_buffer) add(buffer);
    for (void* counter : draw.ps_uav_counter) add(counter);
    for (usize i = graphics::BACKEND_UAV_TEXTURE_BASE; i < draw.textures.size();
         ++i) {
        add(draw.textures[i]);
    }
    if (draw.resident_read_count < draw.resident.size()) {
        for (usize i = draw.resident_read_count; i < draw.resident.size(); ++i) {
            add(draw.resident[i]);
        }
    }
    // A UAV descriptor table with no resources wrote nothing. Treating that
    // pointer as an unnameable write armed a fragment scope barrier on the
    // next draw, and that barrier stores every tile. Overflow — more writes
    // than the name list can hold — is the only unnameable case.
    return !overflow;
}
}  // namespace

void MetalSubmitQueue::set_encode_hash_sink_for_test(
    std::vector<u64>* sink) noexcept {
    g_encode_hash_sink.store(sink, std::memory_order_release);
}

void MetalSubmitQueue::encode_draw_in_pass(const CsDraw& draw, u64 pass_id) {
    if (pass_id != 0 && pass_id != m_open_pass && draw.pipeline != nullptr &&
        m_recorder) [[unlikely]] {
        // The pass's encoder is not open: its BeginPass went into an earlier
        // list (another thread submitted between it and this draw), or an op
        // another thread recorded in between closed it. The draw is part of
        // the pass all the same (D3D11.3: every command issued on the
        // immediate context executes, in order): reopen the pass, loading
        // what its earlier encoder stored, with its occlusion slot.
        if (reopen_pass(pass_id)) {
            m_draws_reopened.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (pass_id != 0 && pass_id == m_open_pass) {
        const bool restart =
            m_open_pass_merged && draw_needs_own_encoder(draw);
        if (restart) {
            // An encoder boundary orders this draw after the earlier passes;
            // re-begin this pass loading what they stored. (An ended pass
            // kept open for merging is no longer live: its descriptor is
            // still the open one.)
            m_passes_restarted.fetch_add(1, std::memory_order_relaxed);
            (void)reopen_pass(pass_id, m_open_begin);
        } else if (m_open_pass_wrote && draw_uav_orders(draw) &&
                   m_recorder != nullptr && m_recorder->has_render_encoder()) {
            // This draw can read a UAV a previous draw wrote. Order those
            // resources only. A scope barrier also waits for every attachment,
            // which stores the tile and serialises the draws that follow.
            if (m_open_uav_count == 0) {
                m_recorder->memory_barrier_fragment_writes();
            } else {
                m_recorder->memory_barrier_fragment_resources(
                    m_open_uavs.data(), m_open_uav_count);
            }
        }
        if (draw_uav_orders(draw)) {
            void* named[kOpenUavSlots];
            u32 named_count = 0;
            if (!collect_uav_writes(draw, named, kOpenUavSlots, named_count)) {
                // More UAV writes than the name list holds. The next reader
                // cannot order those resources individually.
                m_open_pass_wrote = true;
                m_open_uav_count = 0;
            } else if (named_count > 0) {
                m_open_pass_wrote = true;
                m_open_uav_count = named_count;
                for (u32 i = 0; i < named_count; ++i) m_open_uavs[i] = named[i];
            }
            // No UAV resources: this draw wrote nothing. Leave the previous
            // writer's names in place. Clearing them makes the next reader
            // take the scope barrier above, which stores the tile.
        }
        // writes_depth_stencil gates both: a producer that only states the
        // combined flag leaves the per-plane ones at their conservative true.
        if (draw.writes_depth_stencil) {
            // VITREO_METAL_DS_PLANES=0 (bisection): one write dirties both
            // planes, as before the planes were tracked separately.
            static const bool planes =
                environment_flag_enabled("VITREO_METAL_DS_PLANES", true);
            if (draw.writes_depth || !planes) m_open_depth_dirty = true;
            if (draw.writes_stencil || !planes) m_open_stencil_dirty = true;
        }
        if (draw.writes_color) {
            m_open_color_dirty = true;
        }
    }
    if (std::vector<u64>* sink =
            g_encode_hash_sink.load(std::memory_order_acquire)) {
        sink->push_back(cs_draw_state_hash(draw));
    }
    encode_draw(draw, pass_id);
}

void MetalSubmitQueue::encode_draw(const CsDraw& draw, u64 pass_id) {
    if (!m_recorder || draw.pipeline == nullptr) {
        // No frontend records one (fill_backend_draw refuses a draw without
        // a pipeline state); counted all the same, never dropped unseen.
        m_draws_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // A draw is only meaningful inside the pass it was recorded against.
    // Encoder addresses are recycled, so a non-null `has_render_encoder()`
    // is not enough: a Draw recorded for pass N must not message pass N+1's
    // encoder, and must not message an encoder that EndPass already released.
    if (pass_id == 0 || pass_id != m_open_pass ||
        !m_recorder->has_render_encoder()) {
        m_draws_dropped.fetch_add(1, std::memory_order_relaxed);
        VITREO_TRACE("[submit] draw dropped: pass=%llu open=%llu encoder=%d\n",
                     static_cast<unsigned long long>(pass_id),
                     static_cast<unsigned long long>(m_open_pass),
                     m_recorder->has_render_encoder() ? 1 : 0);
        return;
    }
    m_recorder->set_render_pipeline(draw.pipeline);
    m_recorder->set_depth_stencil(draw.depth_stencil);
    m_recorder->set_stencil_reference(draw.stencil_reference);
    // ---- WS5 begin ----
    // A mesh draw: what the call bound on the vertex stage goes to the mesh
    // stage (the vertex binding caches stay valid: Metal keeps the tables
    // apart), the object stage gets the mesh-group total, and a barrier
    // orders an earlier in-pass capture's writes before the mesh reads.
    const bool d3d12_mesh = draw.d3d12_mesh();
    const bool mesh = draw.mesh_object_groups != 0 || d3d12_mesh;
    if (mesh) {
        for (u32 i = 0; i < draw.vertex_stream_count; ++i) {
            if (draw.vertex_buffers[i] == nullptr) continue;
            m_recorder->set_mesh_buffer(draw.vertex_buffers[i],
                                        draw.vertex_offsets[i],
                                        draw.vertex_slots[i]);
        }
        if (draw.vs_push != nullptr) {
            m_recorder->set_mesh_bytes(draw.vs_push, draw.vs_push_size, 0);
        } else if (!draw.vs_const.empty()) {
            m_recorder->set_mesh_bytes(draw.vs_const.data(),
                                       draw.vs_const.size(), 0);
        }
        for (u32 i = 0; i < 16; ++i) {
            if (draw.vs_cbuffer[i] != nullptr) {
                m_recorder->set_mesh_buffer(draw.vs_cbuffer[i],
                                            draw.vs_cbuffer_off[i], 2 + i);
            }
            m_recorder->set_mesh_texture(draw.vertex_textures[i], i);
            m_recorder->set_mesh_sampler(draw.vertex_samplers[i], i);
        }
        if (d3d12_mesh) {
            // The D3D12 stages' descriptor tables (argument buffers): the
            // mesh shader's are the call's vertex-stage ones.
            for (unsigned set = 0; set < BACKEND_MAX_DESCRIPTOR_SETS; ++set) {
                const u32 index = BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set;
                if (draw.vs_descriptor_tables[set] != nullptr) {
                    m_recorder->set_mesh_buffer(draw.vs_descriptor_tables[set], 0, index);
                }
                if (draw.os_descriptor_tables[set] != nullptr) {
                    m_recorder->set_object_buffer(draw.os_descriptor_tables[set], 0, index);
                }
            }
            if (!draw.os_const.empty()) {
                m_recorder->set_object_bytes(draw.os_const.data(), draw.os_const.size(), 0);
            }
            if (draw.mesh_statistics != nullptr) {
                m_recorder->set_mesh_buffer(draw.mesh_statistics, draw.mesh_statistics_offset,
                                            kMeshStatisticsBufferIndex);
                m_recorder->set_object_buffer(draw.mesh_statistics, draw.mesh_statistics_offset,
                                              kMeshStatisticsBufferIndex);
            }
            if (draw.mesh_scratch != nullptr) {
                m_recorder->set_mesh_buffer(draw.mesh_scratch, draw.mesh_scratch_offset,
                                            kMeshScratchBufferIndex);
                if (draw.mesh_origin_buffer != nullptr) {
                    m_recorder->set_mesh_buffer(draw.mesh_origin_buffer, draw.mesh_origin_buffer_offset,
                                                kMeshOriginBufferIndex);
                    m_recorder->set_object_buffer(draw.mesh_origin_buffer, draw.mesh_origin_buffer_offset,
                                                  kMeshOriginBufferIndex);
                } else {
                    m_recorder->set_mesh_bytes(draw.mesh_origin.data(), sizeof(draw.mesh_origin),
                                               kMeshOriginBufferIndex);
                    m_recorder->set_object_bytes(draw.mesh_origin.data(), sizeof(draw.mesh_origin),
                                                 kMeshOriginBufferIndex);
                }
            }
            if (draw.mesh_capture != nullptr) {
                m_recorder->set_mesh_buffer(draw.mesh_capture, draw.mesh_capture_offset,
                                            kMeshCaptureBufferIndex);
            }
            if (draw.object_view_capture != nullptr) {
                m_recorder->set_object_buffer(draw.object_view_capture, draw.object_view_capture_offset,
                                              kMeshViewCaptureBufferIndex);
            }
            if (draw.mesh_view_capture != nullptr) {
                m_recorder->set_mesh_buffer(draw.mesh_view_capture, draw.mesh_view_capture_offset,
                                            kMeshViewCaptureBufferIndex);
            }
        } else {
            m_recorder->set_object_bytes(&draw.mesh_groups,
                                         sizeof(draw.mesh_groups), 0);
        }
    }
    for (u32 i = 0; i < (mesh ? 0u : draw.vertex_stream_count); ++i) {
    // ---- WS5 end ----
        if (draw.vertex_buffers[i] == nullptr) {
            continue;
        }
        // Bits 8..: a dynamic-stride pipeline's stride (metal_backend_draw).
        m_recorder->set_vertex_buffer(draw.vertex_buffers[i],
                                      draw.vertex_offsets[i],
                                      draw.vertex_slots[i] & 0xFFu,
                                      draw.vertex_slots[i] >> 8u);
    }
    // push-intern: the recording arena's copy when the draw has one, else
    // the draw's own vectors. Either is bound in every encoder it lands in.
    if (mesh) {
        // WS5: the mesh stage has its push block (above).
    } else if (draw.vs_push != nullptr) {
        m_recorder->set_vertex_bytes(draw.vs_push, draw.vs_push_size, 0);
    } else {
        m_recorder->set_vertex_bytes(
            draw.vs_const.empty() ? nullptr : draw.vs_const.data(),
            draw.vs_const.size(), 0);
    }
    if (draw.ps_push != nullptr) {
        m_recorder->set_fragment_bytes(draw.ps_push, draw.ps_push_size, 0);
    } else {
        m_recorder->set_fragment_bytes(
            draw.ps_const.empty() ? nullptr : draw.ps_const.data(),
            draw.ps_const.size(), 0);
    }
    for (u32 i = 0; i < 16; ++i) {
        if (draw.vs_cbuffer[i] != nullptr && !mesh) {  // WS5
            m_recorder->set_vertex_buffer(draw.vs_cbuffer[i],
                                          draw.vs_cbuffer_off[i], 2 + i);
        }
        if (draw.ps_cbuffer[i] != nullptr) {
            m_recorder->set_fragment_buffer(draw.ps_cbuffer[i],
                                            draw.ps_cbuffer_off[i], 2 + i);
        }
        m_recorder->set_fragment_texture(draw.textures[i], i);
        m_recorder->set_fragment_sampler(draw.samplers[i], i);
        if (!mesh) {  // WS5
            m_recorder->set_vertex_texture(draw.vertex_textures[i], i);
            m_recorder->set_vertex_sampler(draw.vertex_samplers[i], i);
        }
    }
    if (!draw.viewports.empty()) {
        m_recorder->set_viewports(draw.viewports.data(), draw.scissors.data(),
                                  static_cast<u32>(draw.viewports.size()));
    } else {
        m_recorder->set_viewport(draw.viewport[0], draw.viewport[1],
                                 draw.viewport[2], draw.viewport[3],
                                 draw.viewport[4], draw.viewport[5]);
        if (draw.scissor) {
            m_recorder->set_scissor(draw.scissor_x, draw.scissor_y,
                                    draw.scissor_w, draw.scissor_h);
        } else {
            const u32 x = draw.viewport[0] > 0.0f
                              ? static_cast<u32>(draw.viewport[0])
                              : 0u;
            const u32 y = draw.viewport[1] > 0.0f
                              ? static_cast<u32>(draw.viewport[1])
                              : 0u;
            const u32 w = draw.viewport[2] > 1.0f
                              ? static_cast<u32>(draw.viewport[2])
                              : 1u;
            const u32 h = draw.viewport[3] > 1.0f
                              ? static_cast<u32>(draw.viewport[3])
                              : 1u;
            m_recorder->set_scissor(x, y, w, h);
        }
    }
    m_recorder->set_cull_mode(draw.cull_mode);
    m_recorder->set_triangle_fill_mode(draw.fill_mode);
    m_recorder->set_depth_clip_mode(draw.depth_clip_mode);
    m_recorder->set_depth_bias(draw.depth_bias, draw.depth_bias_slope_scale,
                               draw.depth_bias_clamp);
    m_recorder->set_front_facing(draw.winding);
    m_recorder->set_blend_color(draw.blend_color[0], draw.blend_color[1],
                                draw.blend_color[2], draw.blend_color[3]);
    for (unsigned slot = 0; slot < draw.ps_uav_buffer.size(); ++slot) {
        if (draw.ps_uav_buffer[slot] == nullptr) continue;
        m_recorder->set_fragment_buffer(draw.ps_uav_buffer[slot],
                                        draw.ps_uav_buffer_off[slot],
                                        BACKEND_UAV_BUFFER_BASE + slot);
    }
    for (unsigned k = 0; k < draw.ps_uav_counter.size(); ++k) {
        if (draw.ps_uav_counter[k] == nullptr) continue;
        m_recorder->set_fragment_buffer(draw.ps_uav_counter[k],
                                        draw.ps_uav_counter_off[k],
                                        BACKEND_UAV_COUNTER_BUFFER_BASE + k);
    }
    if (draw.ps_statistics != nullptr) {
        m_recorder->set_fragment_buffer(
            draw.ps_statistics, draw.ps_statistics_off,
            BACKEND_FRAGMENT_STATISTICS_BUFFER_INDEX);
    }
    for (unsigned set = 0; set < BACKEND_MAX_DESCRIPTOR_SETS; ++set) {
        const NSUInteger index = BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set;
        if (draw.vs_descriptor_tables[set] != nullptr && !mesh) {  // WS5
            m_recorder->set_vertex_buffer(draw.vs_descriptor_tables[set], 0,
                                          static_cast<u32>(index));
        }
        if (draw.ps_descriptor_tables[set] != nullptr) {
            m_recorder->set_fragment_buffer(draw.ps_descriptor_tables[set], 0,
                                            static_cast<u32>(index));
        }
    }
    // Shader-resource and constant-buffer views are read-only. Only the
    // unordered-access suffix is writable. A zero count is a draw that
    // did not split the list; the certificate covers that whole list.
    const u32 resident_n = static_cast<u32>(draw.resident.size());
    const u32 read_n = std::min(draw.resident_read_count, resident_n);
    void* const* const handles = draw.resident.data();
    if (read_n == 0 || read_n == resident_n) {
        // A zero count did not split the list; the certificate covers it.
        // A count that covers a non-empty list is the shader-resource
        // prefix with no unordered-access suffix, which D3D defines as
        // read-only.
        const bool read_only =
            read_n == resident_n && resident_n != 0
                ? true
                : draw.indirect_resources_read_only;
        m_recorder->use_render_resources(handles, resident_n, read_only);
    } else {
        m_recorder->use_render_resources(handles, read_n, true);
        m_recorder->use_render_resources(handles + read_n, resident_n - read_n,
                                         false);
    }
    // Heap windows: only what this encoder has not declared from each log.
    for (const MetalResidentToken& token : draw.resident_logs) {
        m_recorder->use_render_resident_log(token);
    }
    // ---- WS5 begin ----
    if (mesh) {
        if (draw.barrier_before_mesh) {
            m_recorder->memory_barrier_vertex_to_mesh();
        }
        // Device-staging chunks share a scratch; a view-instancing replay
        // reads records an earlier draw of this encoder (pass-fold) wrote.
        if (draw.mesh_serialize || draw.object_view_capture != nullptr ||
            draw.mesh_view_capture != nullptr) {
            m_recorder->memory_barrier_mesh_to_mesh();
        }
        if (d3d12_mesh) {
            m_recorder->draw_mesh_grid(draw.mesh_grid, draw.mesh_object_shape,
                                       draw.mesh_shape, draw.mesh_indirect,
                                       draw.mesh_indirect_offset);
            return;
        }
        m_recorder->draw_mesh(draw.mesh_object_groups,
                              draw.mesh_object_threads, draw.mesh_threads);
        return;
    }
    // ---- WS5 end ----
    if (draw.tess_factors != nullptr) {
        m_recorder->draw_patches(draw.tess_factors, draw.tess_factors_offset,
                                 draw.patch_control_points, draw.patch_count,
                                 draw.instance_count, draw.first_instance);
    } else if (draw.indirect_args != nullptr) {
        if (draw.indexed) {
            m_recorder->draw_indexed_instanced_indirect(
                draw.primitive, draw.index_type, draw.index_buffer,
                draw.index_offset, draw.indirect_args, draw.indirect_offset);
        } else {
            m_recorder->draw_instanced_indirect(
                draw.primitive, draw.indirect_args, draw.indirect_offset);
        }
    } else if (draw.indexed) {
        m_recorder->draw_indexed_instanced(
            draw.primitive, draw.index_count, draw.index_type, draw.index_buffer,
            draw.index_offset, draw.instance_count, draw.base_vertex,
            draw.first_instance);
    } else {
        m_recorder->draw_instanced(draw.primitive, draw.vertex_count,
                                   draw.instance_count, draw.first_vertex,
                                   draw.first_instance);
    }
}

} // namespace vitreo::graphics
