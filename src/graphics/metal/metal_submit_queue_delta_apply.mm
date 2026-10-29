// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_submit_queue_delta_apply.mm
/// @brief delta-record consumer (apply_delta) and the draw state hash.

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

void reset_draw(CsDraw& draw) {
    static_cast<CsDrawState&>(draw) = CsDrawState{};
    draw.vs_const.clear();
    draw.ps_const.clear();
    draw.viewports.clear();
    draw.scissors.clear();
    draw.resident.clear();
}

/// Apply one DeltaDraw at @p record to @p state. False when it is not a
/// keyframe and @p valid is not set (nothing to apply it to).
bool apply_delta(const u8* record, CsDraw& state, bool& valid) {
    const auto* head = reinterpret_cast<const DeltaHead*>(record);
    if ((head->flags & kDeltaKeyframe) != 0) {
        reset_draw(state);
        valid = true;
    } else if (!valid) {
        return false;
    }
    const auto* params =
        reinterpret_cast<const DeltaParams*>(record + sizeof(DeltaHead));
    state.vertex_count = params->vertex_count;
    state.index_count = params->index_count;
    state.instance_count = params->instance_count;
    state.first_vertex = params->first_vertex;
    state.first_instance = params->first_instance;
    state.base_vertex = params->base_vertex;
    state.indirect_args = params->indirect_args;
    state.indirect_offset = static_cast<usize>(params->indirect_offset);
    state.tess_factors = params->tess_factors;
    state.tess_factors_offset = static_cast<usize>(params->tess_factors_offset);
    state.patch_count = params->patch_count;
    state.patch_control_points = params->patch_control_points;
    const u8* p = record + sizeof(DeltaHead) + sizeof(DeltaParams);
    struct HandleOffset { void* h; u64 off; };
    for (u16 n = 0; n < head->entries; ++n) {
        DeltaEntry e{};
        std::memcpy(&e, p, sizeof(e));
        const u8* payload = p + sizeof(e);
        p = payload + e.bytes;
        const auto ho = [payload] {
            HandleOffset v{};
            std::memcpy(&v, payload, sizeof(v));
            return v;
        };
        const auto h = [payload] {
            void* v = nullptr;
            std::memcpy(&v, payload, sizeof(v));
            return v;
        };
        const u16 f = e.field;
        if (f == kFPipeline) {
            state.pipeline = h();
        } else if (f == kFDepth) {
            struct { void* h; u32 ref; u32 writes; } v{};
            std::memcpy(&v, payload, sizeof(v));
            state.depth_stencil = v.h;
            state.stencil_reference = v.ref;
            state.writes_depth_stencil = v.writes != 0;
        } else if (f == kFStreamCount) {
            state.vertex_stream_count = e.a;
        } else if (f >= kFStream && f < kFStream + 16) {
            const HandleOffset v = ho();
            state.vertex_buffers[f - kFStream] = v.h;
            state.vertex_offsets[f - kFStream] = static_cast<usize>(v.off);
            state.vertex_slots[f - kFStream] = e.a;
        } else if (f == kFIndex) {
            const HandleOffset v = ho();
            state.index_buffer = v.h;
            state.index_offset = static_cast<usize>(v.off);
            state.index_type = e.a & 0xFFFFu;
            state.indexed = (e.a & 0x10000u) != 0;
        } else if (f >= kFVsCb && f < kFVsCb + 16) {
            const HandleOffset v = ho();
            state.vs_cbuffer[f - kFVsCb] = v.h;
            state.vs_cbuffer_off[f - kFVsCb] = static_cast<usize>(v.off);
        } else if (f >= kFPsCb && f < kFPsCb + 16) {
            const HandleOffset v = ho();
            state.ps_cbuffer[f - kFPsCb] = v.h;
            state.ps_cbuffer_off[f - kFPsCb] = static_cast<usize>(v.off);
        } else if (f >= kFTex && f < kFTex + 16) {
            state.textures[f - kFTex] = h();
        } else if (f >= kFSamp && f < kFSamp + 16) {
            state.samplers[f - kFSamp] = h();
        } else if (f >= kFVTex && f < kFVTex + 16) {
            state.vertex_textures[f - kFVTex] = h();
        } else if (f >= kFVSamp && f < kFVSamp + 16) {
            state.vertex_samplers[f - kFVSamp] = h();
        } else if (f == kFViewport) {
            std::memcpy(state.viewport.data(), payload, sizeof(state.viewport));
        } else if (f == kFBlend) {
            std::memcpy(state.blend_color.data(), payload,
                        sizeof(state.blend_color));
        } else if (f == kFScissor) {
            std::array<u32, 5> v{};
            std::memcpy(v.data(), payload, sizeof(v));
            state.scissor_x = v[0];
            state.scissor_y = v[1];
            state.scissor_w = v[2];
            state.scissor_h = v[3];
            state.scissor = v[4] != 0;
        } else if (f == kFRaster) {
            struct {
                u32 cull, fill, clip, winding, primitive;
                float bias, slope, clamp;
            } v{};
            std::memcpy(&v, payload, sizeof(v));
            state.cull_mode = v.cull;
            state.fill_mode = v.fill;
            state.depth_clip_mode = v.clip;
            state.winding = v.winding;
            state.primitive = v.primitive;
            state.depth_bias = v.bias;
            state.depth_bias_slope_scale = v.slope;
            state.depth_bias_clamp = v.clamp;
        } else if (f >= kFUav && f < kFUav + 8) {
            const HandleOffset v = ho();
            state.ps_uav_buffer[f - kFUav] = v.h;
            state.ps_uav_buffer_off[f - kFUav] = static_cast<usize>(v.off);
        } else if (f >= kFCounter && f < kFStats) {
            const HandleOffset v = ho();
            state.ps_uav_counter[f - kFCounter] = v.h;
            state.ps_uav_counter_off[f - kFCounter] = static_cast<usize>(v.off);
        } else if (f == kFStats) {
            const HandleOffset v = ho();
            state.ps_statistics = v.h;
            state.ps_statistics_off = static_cast<usize>(v.off);
        } else if (f >= kFVsTable && f < kFVsTable + 4) {
            state.vs_descriptor_tables[f - kFVsTable] = h();
        } else if (f >= kFPsTable && f < kFPsTable + 4) {
            state.ps_descriptor_tables[f - kFPsTable] = h();
        } else if (f == kFResident) {
            state.resident.resize(e.a);
            if (e.a != 0) {
                std::memcpy(state.resident.data(), payload, e.a * sizeof(void*));
            }
        } else if (f == kFViewportArray) {
            const u32 n = e.a;
            state.viewports.resize(n);
            state.scissors.resize(n);
            if (n != 0) {
                std::memcpy(state.viewports.data(), payload,
                            n * sizeof(state.viewports[0]));
                std::memcpy(state.scissors.data(),
                            payload + n * sizeof(state.viewports[0]),
                            n * sizeof(state.scissors[0]));
            }
        } else if (f == kFVsPush || f == kFPsPush) {
            struct { const u8* p; u64 size; } v{};
            std::memcpy(&v, payload, sizeof(v));
            if (f == kFVsPush) {
                state.vs_push = v.p;
                state.vs_push_size = static_cast<u32>(v.size);
            } else {
                state.ps_push = v.p;
                state.ps_push_size = static_cast<u32>(v.size);
            }
        }
    }
    return true;
}

u64 cs_draw_state_hash(const CsDraw& d) {
    u64 h = 0xcbf29ce484222325ull;
    const auto mix = [&h](const void* data, usize size) {
        const auto* bytes = static_cast<const u8*>(data);
        for (usize i = 0; i < size; ++i) h = (h ^ bytes[i]) * 0x100000001b3ull;
    };
    const auto v = [&mix](const auto& value) { mix(&value, sizeof(value)); };
    const auto flag = [&mix](bool b) {
        const u8 x = b ? 1u : 0u;
        mix(&x, 1);
    };
    v(d.pipeline);
    v(d.depth_stencil);
    v(d.stencil_reference);
    flag(d.writes_depth_stencil);
    v(d.vertex_buffers);
    v(d.vertex_offsets);
    v(d.vertex_slots);
    v(d.vertex_stream_count);
    v(d.index_buffer);
    v(d.index_offset);
    v(d.index_type);
    flag(d.indexed);
    // The push blocks by their bytes, wherever they are kept.
    const auto push = [&](const u8* span, u32 span_size,
                          const CsDraw::PushBytes& vec) {
        const u8* bytes = span != nullptr ? span : vec.data();
        const usize size = span != nullptr ? span_size : vec.size();
        v(size);
        if (size != 0 && bytes != nullptr) mix(bytes, size);
    };
    push(d.vs_push, d.vs_push_size, d.vs_const);
    push(d.ps_push, d.ps_push_size, d.ps_const);
    v(d.vs_cbuffer);
    v(d.vs_cbuffer_off);
    v(d.ps_cbuffer);
    v(d.ps_cbuffer_off);
    v(d.textures);
    v(d.samplers);
    v(d.vertex_textures);
    v(d.vertex_samplers);
    v(d.viewport);
    v(d.blend_color);
    v(d.scissor_x);
    v(d.scissor_y);
    v(d.scissor_w);
    v(d.scissor_h);
    flag(d.scissor);
    v(d.viewports.size());
    if (!d.viewports.empty()) {
        mix(d.viewports.data(), d.viewports.size() * sizeof(d.viewports[0]));
    }
    v(d.scissors.size());
    if (!d.scissors.empty()) {
        mix(d.scissors.data(), d.scissors.size() * sizeof(d.scissors[0]));
    }
    v(d.cull_mode);
    v(d.fill_mode);
    v(d.depth_clip_mode);
    v(d.depth_bias);
    v(d.depth_bias_slope_scale);
    v(d.depth_bias_clamp);
    v(d.winding);
    v(d.primitive);
    v(d.vertex_count);
    v(d.index_count);
    v(d.instance_count);
    v(d.first_vertex);
    v(d.first_instance);
    v(d.base_vertex);
    v(d.indirect_args);
    v(d.indirect_offset);
    v(d.tess_factors);
    v(d.tess_factors_offset);
    v(d.patch_count);
    v(d.patch_control_points);
    v(d.ps_uav_buffer);
    v(d.ps_uav_buffer_off);
    v(d.ps_uav_counter);
    v(d.ps_uav_counter_off);
    v(d.ps_statistics);
    v(d.ps_statistics_off);
    v(d.vs_descriptor_tables);
    v(d.ps_descriptor_tables);
    v(d.resident.size());
    if (!d.resident.empty()) {
        mix(d.resident.data(), d.resident.size() * sizeof(void*));
    }
    return h;
}

namespace {
// ---- end delta-record format
}  // namespace

} // namespace vitreo::graphics
