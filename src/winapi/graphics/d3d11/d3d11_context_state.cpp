// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_context_state.cpp
/// @brief Resetting a context's binding state (ClearState).
///
/// clear_context_state, shared by ClearState, FinishCommandList and
/// ExecuteCommandList, and apply_initial_count, a UAV setter's
/// pUAVInitialCounts entry.
///
/// Split out of d3d11_context.cpp.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/core/com_direct.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_texel_encode.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/gpu_resource.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <string>
#include <mutex>
#include <set>
#include <vector>

namespace vitreo::winapi {

/// Release every reference a context's binding state holds and return it to its
/// default. This is ClearState's work, shared with FinishCommandList and
/// ExecuteCommandList, which Learn also specifies as resetting the state.
///
/// Assigning a fresh ContextState without this would drop every bound object's
/// reference on the floor -- a leak of every texture, buffer and state object the
/// context held.
///
/// The output-merger views go with everything else.
void D3D11Manager::clear_context_state(GuestAddr context) {
    {
        auto& state = m_context_states[context];
        for (usize slot = 0; slot < state.render_targets.size(); ++slot) {
            const GuestAddr previous = state.render_targets[slot];
            state.render_targets.set(slot, 0);
            if (previous != 0) (void)release_render_target_view(previous);
        }
        const GuestAddr depth = state.depth_stencil_view;
        state.depth_stencil_view.set(0);
        if (depth != 0) (void)release_depth_stencil_view(depth);
    }
    // Learn (ClearState): the context returns to its default state, and the
    // default is no predication. The value goes with it -- GetPredication after
    // ClearState must not report a predicate value from before.
    const GuestAddr predicate = m_context_states[context].predicate;
    if (predicate != 0) {
        (void)release_query(predicate);
        m_context_states[context].predicate = 0;
    }
    m_context_states[context].predicate_value = 0;
    const GuestAddr blend_state = m_context_states[context].blend_state;
    if (blend_state != 0) {
        (void)release_blend_state(blend_state);
        m_context_states[context].blend_state = 0;
    }
    for (const GuestAddr view :
         m_context_states[context].pixel_shader_resources) {
        if (view != 0) (void)release_shader_resource_view(view);
    }
    for (const GuestAddr view :
         m_context_states[context].vertex_shader_resources) {
        if (view != 0) (void)release_shader_resource_view(view);
    }
    for (const GuestAddr sampler :
         m_context_states[context].pixel_samplers) {
        if (sampler != 0) (void)release_sampler_state(sampler);
    }
    for (const GuestAddr sampler :
         m_context_states[context].vertex_samplers) {
        if (sampler != 0) (void)release_sampler_state(sampler);
    }
    for (const GuestAddr view :
         m_context_states[context].compute_shader_resources) {
        if (view != 0) (void)release_shader_resource_view(view);
    }
    for (const GuestAddr view :
         m_context_states[context].compute_unordered_access_views) {
        if (view != 0) (void)release_unordered_access_view(view);
    }
    for (const GuestAddr view :
         m_context_states[context].output_merger_unordered_access_views) {
        if (view != 0) (void)release_unordered_access_view(view);
    }
    for (const GuestAddr sampler :
         m_context_states[context].compute_samplers) {
        if (sampler != 0) (void)release_sampler_state(sampler);
    }
    for (const LinkedStage stage :
         {LinkedStage::Vertex, LinkedStage::Pixel, LinkedStage::Compute,
          LinkedStage::Geometry, LinkedStage::Hull, LinkedStage::Domain}) {
        unbind_stage_shader(m_context_states[context], stage);
    }
    for (const GuestAddr buffer :
         m_context_states[context].geometry_constant_buffers) {
        if (buffer != 0) (void)release_buffer(buffer);
    }
    for (const GuestAddr view :
         m_context_states[context].geometry_shader_resources) {
        if (view != 0) (void)release_shader_resource_view(view);
    }
    for (const GuestAddr sampler : m_context_states[context].geometry_samplers) {
        if (sampler != 0) (void)release_sampler_state(sampler);
    }
    {
        auto& state = m_context_states[context];
        for (const auto* buffers :
             {&state.hull_constant_buffers, &state.domain_constant_buffers}) {
            for (const GuestAddr buffer : *buffers) {
                if (buffer != 0) (void)release_buffer(buffer);
            }
        }
        for (const auto* views :
             {&state.hull_shader_resources, &state.domain_shader_resources}) {
            for (const GuestAddr view : *views) {
                if (view != 0) (void)release_shader_resource_view(view);
            }
        }
        for (const auto* samplers : {&state.hull_samplers, &state.domain_samplers}) {
            for (const GuestAddr sampler : *samplers) {
                if (sampler != 0) (void)release_sampler_state(sampler);
            }
        }
    }
    // Learn (ClearState): stream-output targets are unbound too, measured.
    for (const GuestAddr buffer :
         m_context_states[context].stream_out_targets) {
        if (buffer != 0) (void)release_buffer(buffer);
    }
    const GuestAddr input_layout =
        m_context_states[context].input_layout;
    if (input_layout != 0) (void)release_input_layout(input_layout);
    for (const GuestAddr buffer : m_context_states[context].vertex_buffers) {
        if (buffer != 0) (void)release_buffer(buffer);
    }
    const GuestAddr index_buffer =
        m_context_states[context].index_buffer;
    if (index_buffer != 0) (void)release_buffer(index_buffer);
    for (const GuestAddr buffer :
         m_context_states[context].vertex_constant_buffers) {
        if (buffer != 0) (void)release_buffer(buffer);
    }
    for (const GuestAddr buffer :
         m_context_states[context].pixel_constant_buffers) {
        if (buffer != 0) (void)release_buffer(buffer);
    }
    for (const GuestAddr buffer :
         m_context_states[context].compute_constant_buffers) {
        if (buffer != 0) (void)release_buffer(buffer);
    }
    const GuestAddr rasterizer_state =
        m_context_states[context].rasterizer_state;
    if (rasterizer_state != 0)
        (void)release_rasterizer_state(rasterizer_state);
    const GuestAddr depth_stencil_state =
        m_context_states[context].depth_stencil_state;
    if (depth_stencil_state != 0)
        (void)release_depth_stencil_state(depth_stencil_state);
    // The pipeline cache survives a state reset: it is a translation cache
    // keyed by bound state rather than binding state itself -- discarding it
    // would re-translate every shader combination after each ClearState.
    auto fresh = ContextState{};
    fresh.pipelines = std::move(m_context_states[context].pipelines);
    // Measured quirk (d3d11_a05b_bind_rules_probe): ClearState resets the
    // stride and offset of IA slots 0..N only, N being the highest slot that
    // holds a buffer. Past it -- and in every slot when no buffer is bound --
    // a stride/offset recorded with a NULL buffer survives: {NULL,vbA,NULL}
    // with strides {5,7,9} reads back {0,0,9} after ClearState.
    {
        const auto& old = m_context_states[context];
        usize kept_from = 0;
        for (usize slot = 0; slot < old.vertex_buffers.size(); ++slot) {
            if (old.vertex_buffers[slot] != 0) kept_from = slot + 1;
        }
        for (usize slot = kept_from; slot < old.vertex_buffers.size(); ++slot) {
            fresh.vertex_buffer_strides[slot] = old.vertex_buffer_strides[slot];
            fresh.vertex_buffer_offsets[slot] = old.vertex_buffer_offsets[slot];
        }
    }
    m_context_states[context] = std::move(fresh);
}

void D3D11Manager::apply_initial_count(GuestAddr counts_array, u32 index,
                                       GuestAddr view_address) {
    // Learn (OMSetRenderTargetsAndUnorderedAccessViews / CSSetUnorderedAccess-
    // Views): pUAVInitialCounts may be NULL, and -1 keeps the view's current
    // offset. Only APPEND/COUNTER views have a counter to set at all.
    if (counts_array == 0 || view_address == 0) return;
    const auto value = m_memory.read_u32(counts_array + index * sizeof(u32));
    if (!value.is_ok()) return;
    if (!apply_uav_initial_count(view_address, value.value())) {
        d3d11_diag("pUAVInitialCounts[%u]=%u could not be written to the "
                   "hidden counter of view 0x%llx", index, value.value(),
                   static_cast<unsigned long long>(view_address));
    }
}

} // namespace vitreo::winapi
