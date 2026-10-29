// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_submit_queue_encode_draw.mm
/// @brief Encoding one draw into the open render encoder.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreFoundation/CoreFoundation.h>
#import <pthread.h>

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
        if (m_open_pass_merged && draw_needs_own_encoder(draw)) {
            // An encoder boundary orders this draw after the earlier passes;
            // re-begin this pass loading what they stored. (An ended pass
            // kept open for merging is no longer live: its descriptor is
            // still the open one.)
            m_passes_restarted.fetch_add(1, std::memory_order_relaxed);
            (void)reopen_pass(pass_id, m_open_begin);
        }
        if (draw_writes(draw)) m_open_pass_wrote = true;
        if (draw.writes_depth_stencil) {
            m_open_depth_dirty = true;
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
    for (u32 i = 0; i < draw.vertex_stream_count; ++i) {
        if (draw.vertex_buffers[i] == nullptr) {
            continue;
        }
        m_recorder->set_vertex_buffer(draw.vertex_buffers[i],
                                      draw.vertex_offsets[i],
                                      draw.vertex_slots[i]);
    }
    // push-intern: the recording arena's copy when the draw has one, else
    // the draw's own vectors. Either is bound in every encoder it lands in.
    if (draw.vs_push != nullptr) {
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
        if (draw.vs_cbuffer[i] != nullptr) {
            m_recorder->set_vertex_buffer(draw.vs_cbuffer[i],
                                          draw.vs_cbuffer_off[i], 2 + i);
        }
        if (draw.ps_cbuffer[i] != nullptr) {
            m_recorder->set_fragment_buffer(draw.ps_cbuffer[i],
                                            draw.ps_cbuffer_off[i], 2 + i);
        }
        m_recorder->set_fragment_texture(draw.textures[i], i);
        m_recorder->set_fragment_sampler(draw.samplers[i], i);
        m_recorder->set_vertex_texture(draw.vertex_textures[i], i);
        m_recorder->set_vertex_sampler(draw.vertex_samplers[i], i);
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
    for (unsigned set = 0; set < 4; ++set) {
        const NSUInteger index = BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set;
        if (draw.vs_descriptor_tables[set] != nullptr) {
            m_recorder->set_vertex_buffer(draw.vs_descriptor_tables[set], 0,
                                          static_cast<u32>(index));
        }
        if (draw.ps_descriptor_tables[set] != nullptr) {
            m_recorder->set_fragment_buffer(draw.ps_descriptor_tables[set], 0,
                                            static_cast<u32>(index));
        }
    }
    m_recorder->use_render_resources(draw.resident);
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
