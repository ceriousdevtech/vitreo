// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_context_fixed_stages.cpp
/// @brief ID3D11DeviceContext input-assembler, stream-output, rasterizer
/// and output-merger setters.
///
/// Slots 17-19 and 24 (IA), 33-36 (OM), 37 (SO) and 43-45 (RS).
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

void D3D11Manager::context_fixed_stage_method(Marshaler& m, usize slot,
                                              GuestAddr self) {
    switch (slot) {
    case 17: {
        // IASetInputLayout
        auto& state = m_context_states[self];
        GuestAddr layout = m.arg_ptr(1);
        if (layout != 0) {
            auto next = m_input_layouts.find(layout);
            if (next == m_input_layouts.end()) layout = 0;
            else ++next->second.ref_count;
        }
        const GuestAddr previous = state.input_layout;
        state.input_layout = layout;
        if (previous != 0) (void)release_input_layout(previous);
        m.set_return(0);
        return;
    }
    case 18: {
        // IASetVertexBuffers
        const u32 start = static_cast<u32>(m.arg(1));
        const u32 count = static_cast<u32>(m.arg(2));
        const GuestAddr buffers = m.arg_ptr(3);
        const GuestAddr strides = m.arg_ptr(4);
        const GuestAddr offsets = m.arg_ptr(5);
        auto& state = m_context_states[self];
        const usize ptr_size = m_is_64bit ? 8u : 4u;
        for (u32 i = 0; i < count &&
                        static_cast<u64>(start) + i < state.vertex_buffers.size();
             ++i) {
            GuestAddr buf = 0;
            if (buffers != 0) {
                (void)read_pointer(buffers + i * ptr_size, buf);
            }
            // A buffer bound for output (a UAV or SO target) binds NULL
            // here; the stride and offset are recorded anyway (measured:
            // "m vb set while cs uavB bound: vb1: - strides 4").
            if (buf != 0) {
                auto next = m_buffers.find(buf);
                if (next == m_buffers.end() ||
                    bound_output_overlaps(state, buffer_subresources(buf))) {
                    buf = 0;
                } else {
                    ++next->second.ref_count;
                }
            }
            const GuestAddr previous = state.vertex_buffers[start + i];
            state.vertex_buffers.set(start + i, buf);
            if (previous != 0) (void)release_buffer(previous);
            if (strides != 0) {
                const auto s = m_memory.read_u32(strides + i * 4);
                if (s.is_ok()) state.vertex_buffer_strides[start + i] = s.value();
            }
            if (offsets != 0) {
                const auto o = m_memory.read_u32(offsets + i * 4);
                if (o.is_ok()) state.vertex_buffer_offsets[start + i] = o.value();
            }
        }
        m.set_return(0);
        return;
    }
    case 19: {
        // IASetIndexBuffer. Refused (NULL, format and offset still stored)
        // while the buffer is bound for output (measured, hazard rows m/n3).
        auto& state = m_context_states[self];
        GuestAddr buffer = m.arg_ptr(1);
        if (buffer != 0) {
            auto next = m_buffers.find(buffer);
            if (next == m_buffers.end() ||
                bound_output_overlaps(state, buffer_subresources(buffer))) {
                buffer = 0;
            } else {
                ++next->second.ref_count;
            }
        }
        const GuestAddr previous = state.index_buffer;
        state.index_buffer = buffer;
        state.index_buffer_format = static_cast<u32>(m.arg(2));
        state.index_buffer_offset = static_cast<u32>(m.arg(3));
        if (previous != 0) (void)release_buffer(previous);
        m.set_return(0);
        return;
    }
    case 24: {
        // IASetPrimitiveTopology
        m_context_states[self].primitive_topology =
            static_cast<u32>(m.arg(1));
        m.set_return(0);
        return;
    }
    case 33: {
        // OMSetRenderTargets(NumViews, ppRenderTargetViews, pDepthStencilView).
        // It is the NumUAVs == 0 form of the call below: every UAV bound
        // through the output merger is unbound (measured: UAVs at u1-2, u0-1
        // and u3-4 all read back NULL after OMSetRenderTargets).
        set_output_merger(self, static_cast<u32>(m.arg(1)), m.arg_ptr(2),
                          m.arg_ptr(3), 0, 0, 0, 0);
        m.set_return(0);
        return;
    }
    case 34: {
        // OMSetRenderTargetsAndUnorderedAccessViews(NumRTVs, ppRTVs, pDSV,
        //   UAVStartSlot, NumUAVs, ppUAVs, pUAVInitialCounts) — Learn slot 34.
        //
        // Either half can be told to keep what is already bound:
        // D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL and
        // D3D11_KEEP_UNORDERED_ACCESS_VIEWS are both 0xFFFFFFFF. For the
        // graphics pipeline the UAVs share the u# register space with the
        // render targets, so UAVStartSlot is the absolute u# index, and a new
        // UAV range replaces EVERY OM UAV slot, not only the ones it names.
        const u32 count = static_cast<u32>(m.arg(1));
        const u32 uav_start = static_cast<u32>(m.arg(4));
        const u32 uav_count = static_cast<u32>(m.arg(5));
        set_output_merger(self, count, m.arg_ptr(2), m.arg_ptr(3), uav_start,
                          uav_count, m.arg_ptr(6), m.arg_ptr(7));
        if (m_diag_transfer_log != nullptr) [[unlikely]] {
            diag_log_om_uavs(count, m.arg_ptr(3), uav_start, uav_count,
                             m.arg_ptr(6), m.arg_ptr(7));
        }
        d3d11_diag(
            "ID3D11DeviceContext::OMSetRenderTargetsAndUnorderedAccessViews "
            "rtvs=%u uav_start=%u uav_count=%u",
            count, uav_start, uav_count);
        m.set_return(0);
        return;
    }
    case 35: {
        // OMSetBlendState
        auto& state = m_context_states[self];
        const GuestAddr next_blend_state = m.arg_ptr(1);
        if (next_blend_state != state.blend_state) {
            if (next_blend_state != 0) {
                auto next = m_blend_states.find(next_blend_state);
                if (next != m_blend_states.end()) {
                    ++next->second.ref_count;
                }
            }
            const GuestAddr previous = state.blend_state;
            state.blend_state = next_blend_state;
            if (previous != 0) {
                (void)release_blend_state(previous);
            }
        }
        const GuestAddr factor = m.arg_ptr(2);
        if (factor != 0) {
            (void)m_memory.read_bytes(
                factor, reinterpret_cast<Byte*>(state.blend_factor.data()),
                sizeof(state.blend_factor));
        } else {
            state.blend_factor.fill(1.0f);
        }
        state.sample_mask = static_cast<u32>(m.arg(3));
        m.set_return(0);
        return;
    }
    case 36: {
        // OMSetDepthStencilState
        auto& state = m_context_states[self];
        GuestAddr next_state = m.arg_ptr(1);
        if (next_state != 0) {
            auto next = m_depth_stencil_states.find(next_state);
            if (next == m_depth_stencil_states.end()) next_state = 0;
            else ++next->second.ref_count;
        }
        const GuestAddr previous = state.depth_stencil_state;
        state.depth_stencil_state = next_state;
        // The stencil reference is 8 bits (D3D11_DEFAULT_STENCIL_REFERENCE
        // range); Windows stores and returns StencilRef & 0xFF (measured:
        // 0x1FF and 0xFFFFFFFF both read back 0xff).
        state.stencil_ref = static_cast<u32>(m.arg(2)) & 0xFFu;
        if (previous != 0) (void)release_depth_stencil_state(previous);
        m.set_return(0);
        return;
    }
    case 37:
        // SOSetTargets(NumBuffers, ppSOTargets, pOffsets).
        context_so_set_targets(m, self);
        return;
    case 43: {
        // RSSetState
        auto& state = m_context_states[self];
        GuestAddr next_state = m.arg_ptr(1);
        if (next_state != 0) {
            auto next = m_rasterizer_states.find(next_state);
            if (next == m_rasterizer_states.end()) next_state = 0;
            else ++next->second.ref_count;
        }
        const GuestAddr previous = state.rasterizer_state;
        state.rasterizer_state = next_state;
        if (previous != 0) (void)release_rasterizer_state(previous);
        m.set_return(0);
        return;
    }
    // RSSetViewports / RSSetScissorRects: the call replaces the whole bound
    // set, so a shorter list unbinds the rest and a count of 0 unbinds all
    // (measured). More than the pipeline's 16
    // (D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) drops the
    // call: the previous set stays bound (measured, "rs set 17 after 2: vp 2
    // sc 2").
    case 44: {
        const u32 count = static_cast<u32>(m.arg(1));
        const GuestAddr viewports = m.arg_ptr(2);
        if (count > 16u) {
            d3d11_diag("ID3D11DeviceContext::RSSetViewports(%u) > 16 -> "
                       "call dropped", count);
            m.set_return(0);
            return;
        }
        auto& state = m_context_states[self];
        const u32 viewport_count = viewports != 0 ? count : 0u;
        decltype(state.viewports) incoming{};
        incoming[0] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        if (viewport_count != 0) {
            (void)m_memory.read_bytes(
                viewports, reinterpret_cast<Byte*>(incoming.data()),
                sizeof(incoming[0]) * viewport_count);
        }
        // D3D11.3 15.6.1: MinDepth and MaxDepth in [0, 1] with MinDepth <=
        // MaxDepth, and TopLeftX/Y and TopLeft + Width/Height within
        // [-32768, 32767]. "The runtime validates the parameters to be in
        // valid range, skipping the call if there is an error" -- the bound
        // viewports stay as they were (measured: MinDepth 0.8 > MaxDepth 0.2
        // keeps the previous viewport, d3d11_a07_rs_probe `R2`). A NaN is in
        // no range.
        const auto in_range = [](float value, float lo, float hi) {
            return value >= lo && value <= hi;
        };
        for (u32 i = 0; i < viewport_count; ++i) {
            const auto& vp = incoming[i];
            if (!in_range(vp[4], 0.0f, 1.0f) || !in_range(vp[5], 0.0f, 1.0f) ||
                !(vp[4] <= vp[5]) ||
                !in_range(vp[0], -32768.0f, 32767.0f) ||
                !in_range(vp[1], -32768.0f, 32767.0f) ||
                !in_range(vp[0] + vp[2], -32768.0f, 32767.0f) ||
                !in_range(vp[1] + vp[3], -32768.0f, 32767.0f)) {
                m.set_return(0);
                return;
            }
        }
        state.viewport_count = viewport_count;
        state.viewports = incoming;
        m.set_return(0);
        return;
    }
    case 45: {
        const u32 count = static_cast<u32>(m.arg(1));
        const GuestAddr rects = m.arg_ptr(2);
        if (count > 16u) {
            d3d11_diag("ID3D11DeviceContext::RSSetScissorRects(%u) > 16 -> "
                       "call dropped", count);
            m.set_return(0);
            return;
        }
        auto& state = m_context_states[self];
        state.scissor_rect_count = rects != 0 ? count : 0u;
        state.scissor_rects = {};
        if (state.scissor_rect_count != 0) {
            (void)m_memory.read_bytes(
                rects, reinterpret_cast<Byte*>(state.scissor_rects.data()),
                sizeof(state.scissor_rects[0]) * state.scissor_rect_count);
        }
        m.set_return(0);
        return;
    }
    }
}

} // namespace vitreo::winapi
