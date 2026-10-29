// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_context_shader_stages.cpp
/// @brief ID3D11DeviceContext shader, SRV, sampler and CS-UAV setters.
///
/// Slots 8-11, 23, 25, 26, 31, 32, 59-61, 63-65 and 67-70.
/// impl_ContextMethod (d3d11_context.cpp) routes exactly these slots here.
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

void D3D11Manager::context_shader_stage_method(Marshaler& m, usize slot,
                                               GuestAddr self) {
    switch (slot) {
    case 8: {
        // PSSetShaderResources
        const u32 start = static_cast<u32>(m.arg(1));
        const u32 count = static_cast<u32>(m.arg(2));
        const GuestAddr views = m.arg_ptr(3);
        auto& state = m_context_states[self];
        const usize ptr_size = m_is_64bit ? 8u : 4u;
        for (u32 i = 0; i < count && start + i < state.pixel_shader_resources.size();
             ++i) {
            GuestAddr view = 0;
            if (views != 0) {
                (void)read_pointer(views + i * ptr_size, view);
            }
            // A subresource bound for output (RTV, writable DSV plane, CS or
            // OM UAV, SO target) cannot also be read: the slot is bound NULL
            // instead, on every stage (measured, hazard rows b/d/f/h/m/n2).
            if (view != 0) {
                auto next = m_shader_resource_views.find(view);
                if (next == m_shader_resource_views.end() ||
                    bound_output_overlaps(state, srv_subresources(view))) {
                    view = 0;
                } else {
                    ++next->second.ref_count;
                }
            }
            const GuestAddr previous = state.pixel_shader_resources[start + i];
            state.pixel_shader_resources.set(start + i, view);
            if (previous != 0)
                (void)release_shader_resource_view(previous);
        }
        m.set_return(0);
        return;
    }
    case 9:
        // PSSetShader(pShader, ppClassInstances, NumClassInstances)
        set_stage_shader(self, LinkedStage::Pixel, m.arg_ptr(1), m.arg_ptr(2),
                         static_cast<u32>(m.arg(3)));
        m.set_return(0);
        return;
    case 10: {
        // PSSetSamplers
        const u32 start = static_cast<u32>(m.arg(1));
        const u32 count = static_cast<u32>(m.arg(2));
        const GuestAddr samplers = m.arg_ptr(3);
        auto& state = m_context_states[self];
        const usize ptr_size = m_is_64bit ? 8u : 4u;
        for (u32 i = 0; i < count && start + i < state.pixel_samplers.size();
             ++i) {
            GuestAddr samp = 0;
            if (samplers != 0) {
                (void)read_pointer(samplers + i * ptr_size, samp);
            }
            if (samp != 0) {
                auto next = m_sampler_states.find(samp);
                if (next == m_sampler_states.end()) samp = 0;
                else ++next->second.ref_count;
            }
            const GuestAddr previous = state.pixel_samplers[start + i];
            state.pixel_samplers.set(start + i, samp);
            if (previous != 0) (void)release_sampler_state(previous);
        }
        m.set_return(0);
        return;
    }
    case 11:
        // VSSetShader(pShader, ppClassInstances, NumClassInstances)
        set_stage_shader(self, LinkedStage::Vertex, m.arg_ptr(1), m.arg_ptr(2),
                         static_cast<u32>(m.arg(3)));
        m.set_return(0);
        return;
    case 23:
        // GSSetShader. The only geometry shaders this device creates are the
        // stream-output ones CreateGeometryShaderWithStreamOutput builds over
        // a vertex shader; see d3d11_stream_out.cpp.
        context_gs_set_shader(m, self);
        return;
    case 25:    // VSSetShaderResources
    case 31:    // GSSetShaderResources
    case 59:    // HSSetShaderResources
    case 63: {  // DSSetShaderResources
        const u32 start = static_cast<u32>(m.arg(1));
        const u32 count = static_cast<u32>(m.arg(2));
        const GuestAddr views = m.arg_ptr(3);
        auto& state = m_context_states[self];
        auto& stage_views = slot == 31   ? state.geometry_shader_resources
                            : slot == 59 ? state.hull_shader_resources
                            : slot == 63 ? state.domain_shader_resources
                                         : state.vertex_shader_resources;
        const usize ptr_size = m_is_64bit ? 8u : 4u;
        for (u32 i = 0;
             i < count && start + i < stage_views.size();
             ++i) {
            GuestAddr view = 0;
            if (views != 0) {
                (void)read_pointer(views + i * ptr_size, view);
            }
            // A subresource bound for output (RTV, writable DSV plane, CS or
            // OM UAV, SO target) cannot also be read: the slot is bound NULL
            // instead, on every stage (measured, hazard rows b/d/f/h/m/n2).
            if (view != 0) {
                auto next = m_shader_resource_views.find(view);
                if (next == m_shader_resource_views.end() ||
                    bound_output_overlaps(state, srv_subresources(view))) {
                    view = 0;
                } else {
                    ++next->second.ref_count;
                }
            }
            const GuestAddr previous = stage_views[start + i];
            stage_views.set(start + i, view);
            if (previous != 0)
                (void)release_shader_resource_view(previous);
        }
        m.set_return(0);
        return;
    }
    case 26:    // VSSetSamplers
    case 32:    // GSSetSamplers
    case 61:    // HSSetSamplers
    case 65: {  // DSSetSamplers
        const u32 start = static_cast<u32>(m.arg(1));
        const u32 count = static_cast<u32>(m.arg(2));
        const GuestAddr samplers = m.arg_ptr(3);
        auto& state = m_context_states[self];
        auto& stage_samplers = slot == 32   ? state.geometry_samplers
                               : slot == 61 ? state.hull_samplers
                               : slot == 65 ? state.domain_samplers
                                            : state.vertex_samplers;
        const usize ptr_size = m_is_64bit ? 8u : 4u;
        for (u32 i = 0; i < count && start + i < stage_samplers.size();
             ++i) {
            GuestAddr sampler = 0;
            if (samplers != 0) {
                (void)read_pointer(samplers + i * ptr_size, sampler);
            }
            if (sampler != 0) {
                auto next = m_sampler_states.find(sampler);
                if (next == m_sampler_states.end()) sampler = 0;
                else ++next->second.ref_count;
            }
            const GuestAddr previous = stage_samplers[start + i];
            stage_samplers.set(start + i, sampler);
            if (previous != 0) (void)release_sampler_state(previous);
        }
        m.set_return(0);
        return;
    }
    case 60:   // HSSetShader
    case 64:   // DSSetShader
        context_tessellation_method(m, self, slot);
        return;
    case 67: {
        const u32 start = static_cast<u32>(m.arg(1));
        const u32 count = static_cast<u32>(m.arg(2));
        const GuestAddr views = m.arg_ptr(3);
        auto& state = m_context_states[self];
        const usize ptr_size = m_is_64bit ? 8u : 4u;
        for (u32 i = 0; i < count && start + i < state.compute_shader_resources.size(); ++i) {
            GuestAddr next_view = 0;
            if (views != 0) (void)read_pointer(views + i * ptr_size, next_view);
            // Refused while bound for output, the DSV's writable planes
            // included (measured: CS accepted a depth SRV under a writable DSV).
            if (next_view != 0) {
                auto next = m_shader_resource_views.find(next_view);
                if (next == m_shader_resource_views.end() ||
                    bound_output_overlaps(state,
                                          srv_subresources(next_view))) {
                    next_view = 0;
                } else {
                    ++next->second.ref_count;
                }
            }
            const GuestAddr previous = state.compute_shader_resources[start + i];
            state.compute_shader_resources.set(start + i, next_view);
            if (previous != 0) (void)release_shader_resource_view(previous);
        }
        m.set_return(0);
        return;
    }
    case 68: {
        // CSSetUnorderedAccessViews(StartSlot, NumUAVs, ppUAVs,
        //   pUAVInitialCounts).
        const u32 start = static_cast<u32>(m.arg(1));
        const u32 count = static_cast<u32>(m.arg(2));
        const GuestAddr views = m.arg_ptr(3);
        const GuestAddr initial_counts = m.arg_ptr(4);
        auto& state = m_context_states[self];
        auto& slots = state.compute_unordered_access_views;
        const usize ptr_size = m_is_64bit ? 8u : 4u;
        std::array<GuestAddr, d3d11::UAV_SLOTS> next{};
        u32 in_range = 0;
        for (u32 i = 0;
             i < count && static_cast<u64>(start) + i < slots.size(); ++i) {
            ++in_range;
            GuestAddr view = 0;
            if (views != 0) (void)read_pointer(views + i * ptr_size, view);
            if (view != 0 && m_unordered_access_views.find(view) ==
                                 m_unordered_access_views.end()) {
                view = 0;
            }
            next[i] = view;
        }
        // One call naming a subresource twice -- the same view, or two views
        // of it -- is dropped whole (measured, d3d11_a05b_bind_rules_probe:
        // the previously bound UAVs stay).
        std::array<BoundSubresources, d3d11::UAV_SLOTS> written{};
        usize written_count = 0;
        for (u32 i = 0; i < in_range; ++i) {
            if (next[i] == 0) continue;
            const BoundSubresources set = uav_subresources(next[i]);
            for (usize j = 0; j < written_count; ++j) {
                if (subresources_overlap(written[j], set)) {
                    d3d11_diag("ID3D11DeviceContext::CSSetUnorderedAccessViews:"
                               " two UAVs share a subresource -> call dropped");
                    m.set_return(0);
                    return;
                }
            }
            written[written_count++] = set;
        }
        for (u32 i = 0; i < in_range; ++i) {
            if (next[i] != 0) ++m_unordered_access_views.at(next[i]).ref_count;
        }
        for (u32 i = 0; i < in_range; ++i) {
            const GuestAddr previous = slots[start + i];
            slots[start + i] = next[i];
            if (previous != 0) (void)release_unordered_access_view(previous);
        }
        // The new UAV displaces other outputs of the same subresource: RTVs
        // and OM UAVs (measured both ways), SO targets, and a CS UAV in
        // another slot (measured: u0 then u1 of one texture leaves u0 NULL).
        for (usize j = 0; j < written_count; ++j) {
            unbind_outputs_overlapping(state, written[j],
                                       kOutputRenderTargets |
                                           kOutputMergerUavs |
                                           kOutputStreamTargets);
            for (u32 slot = 0; slot < slots.size(); ++slot) {
                if (slot >= start && slot - start < in_range) continue;
                if (slots[slot] == 0 ||
                    !subresources_overlap(uav_subresources(slots[slot]),
                                          written[j])) {
                    continue;
                }
                const GuestAddr previous = slots[slot];
                slots[slot] = 0;
                (void)release_unordered_access_view(previous);
            }
        }
        unbind_inputs_overlapping(
            state, std::span<const BoundSubresources>(written.data(),
                                                      written_count));
        // Deferred: the counters are set when the list executes.
        for (u32 i = 0; i < in_range && !is_deferred_context(self); ++i) {
            apply_initial_count(initial_counts, i, next[i]);
        }
        m.set_return(0);
        return;
    }
    case 69:
        // CSSetShader retains the bound shader until replacement/ClearState.
        set_stage_shader(self, LinkedStage::Compute, m.arg_ptr(1),
                         m.arg_ptr(2), static_cast<u32>(m.arg(3)));
        m.set_return(0);
        return;
    case 70: {
        const u32 start = static_cast<u32>(m.arg(1));
        const u32 count = static_cast<u32>(m.arg(2));
        const GuestAddr samplers = m.arg_ptr(3);
        auto& state = m_context_states[self];
        const usize ptr_size = m_is_64bit ? 8u : 4u;
        for (u32 i = 0; i < count && start + i < state.compute_samplers.size(); ++i) {
            GuestAddr next_sampler = 0;
            if (samplers != 0) (void)read_pointer(samplers + i * ptr_size, next_sampler);
            if (next_sampler != 0) {
                auto next = m_sampler_states.find(next_sampler);
                if (next == m_sampler_states.end()) next_sampler = 0;
                else ++next->second.ref_count;
            }
            const GuestAddr previous = state.compute_samplers[start + i];
            state.compute_samplers.set(start + i, next_sampler);
            if (previous != 0) (void)release_sampler_state(previous);
        }
        m.set_return(0);
        return;
    }
    }
}

} // namespace vitreo::winapi
