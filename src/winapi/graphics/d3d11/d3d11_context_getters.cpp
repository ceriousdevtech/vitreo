// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_context_getters.cpp
/// @brief The ID3D11DeviceContext Get* state read-back family.
///
/// Slots 72-85 and 87-109.
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

void D3D11Manager::context_getter_method(Marshaler& m, usize slot,
                                         GuestAddr self) {
    switch (slot) {
    case 91: {
        // OMGetBlendState. Learn: every output is optional; a returned state
        // receives an additional reference.
        auto& state = m_context_states[self];
        const GuestAddr state_out = m.arg_ptr(1);
        const GuestAddr factor_out = m.arg_ptr(2);
        const GuestAddr mask_out = m.arg_ptr(3);
        if (state_out != 0) {
            GuestAddr value = 0;
            if (m_blend_states.contains(state.blend_state)) {
                (void)retain_object(state.blend_state);
                value = state.blend_state;
            }
            (void)m.write_ptr(state_out, value);
        }
        if (factor_out != 0) {
            (void)m_memory.write_bytes(
                factor_out,
                reinterpret_cast<const Byte*>(state.blend_factor.data()),
                sizeof(state.blend_factor));
        }
        if (mask_out != 0) {
            (void)m_memory.write_u32(mask_out, state.sample_mask);
        }
        m.set_return(0);
        return;
    }
    case 107:
        // CSGetShader(ppShader, ppClassInstances, pNumClassInstances)
        get_stage_shader(m, m_context_states[self], LinkedStage::Compute);
        m.set_return(0);
        return;
    // ---------------------------------------------------------------------
    // The Get* state read-back family.
    //
    // Learn (ID3D11DeviceContext Get* methods): each returns the objects
    // currently bound to that stage, AddRef'ing every non-NULL interface it
    // hands back, and writing NULL into slots past what is bound. Callers use
    // them to save and restore pipeline state around their own rendering —
    // an injected overlay does exactly this — so leaving them unimplemented
    // hands back whatever the caller's array happened to contain, which is a
    // stale interface pointer it will then Release.
    // ---------------------------------------------------------------------
    case 72:   // VSGetConstantBuffers
    case 73:   // PSGetShaderResources
    case 74:   // PSGetShader
    case 75:   // PSGetSamplers
    case 76:   // VSGetShader
    case 77:   // PSGetConstantBuffers
    case 78:   // IAGetInputLayout
    case 80:   // IAGetIndexBuffer
    case 81:   // GSGetConstantBuffers
    case 82:   // GSGetShader
    case 83:   // IAGetPrimitiveTopology
    case 84:   // VSGetShaderResources
    case 85:   // VSGetSamplers
    case 87:   // GSGetShaderResources
    case 88:   // GSGetSamplers
    case 92:   // OMGetDepthStencilState
    case 94:   // RSGetState
    case 97:   // HSGetShaderResources
    case 98:   // HSGetShader
    case 99:   // HSGetSamplers
    case 100:  // HSGetConstantBuffers
    case 101:  // DSGetShaderResources
    case 102:  // DSGetShader
    case 103:  // DSGetSamplers
    case 104:  // DSGetConstantBuffers
    case 105:  // CSGetShaderResources
    case 106:  // CSGetUnorderedAccessViews
    case 108:  // CSGetSamplers
    // Slot 109, from the ID3D11DeviceContext1 vtable in the Windows SDK header
    // (d3d11_1.h): ... 105 CSGetShaderResources, 106 CSGetUnorderedAccessViews,
    // 107 CSGetShader, 108 CSGetSamplers, 109 CSGetConstantBuffers, 110
    // ClearState ... 114 FinishCommandList, and 115 begins
    // ID3D11DeviceContext1 with CopySubresourceRegion1.
    //
    // This arm used to be numbered 115, which meant two defects at once: a guest
    // calling CSGetConstantBuffers got a stub that returned nothing, and a guest
    // calling CopySubresourceRegion1 got this readback -- writing interface
    // pointers to an address taken from the copy's DstX coordinate and never
    // performing the copy.
    case 109: {  // CSGetConstantBuffers
        auto& state = m_context_states[self];
        const usize ptr_size = m_is_64bit ? 8u : 4u;

        // Writes `count` interface pointers from `bound` starting at `start`,
        // NULLing anything past the array, and AddRefs each one returned.
        const auto write_slots =
            [&](std::span<const GuestAddr> bound, u32 start, u32 count,
                GuestAddr out) {
                if (out == 0) return;
                for (u32 i = 0; i < count; ++i) {
                    const usize index = static_cast<usize>(start) + i;
                    const GuestAddr value =
                        index < bound.size() ? bound[index] : 0;
                    // Getters hand out OWNING references. add_ref() only knows
                    // the device-level interfaces, so every view, state,
                    // shader and buffer returned here used to come back
                    // without one -- and a guest that saved state with a Get
                    // and Released it afterwards freed objects still bound.
                    if (value != 0) (void)retain_object(value);
                    (void)m.write_ptr(
                        out + static_cast<GuestAddr>(i) * ptr_size, value);
                }
            };
        const auto write_one = [&](GuestAddr value, GuestAddr out) {
            if (out == 0) return;
            if (value != 0) (void)retain_object(value);
            (void)m.write_ptr(out, value);
        };

        switch (slot) {
        // (StartSlot, NumBuffers, ppOut)
        case 72: write_slots(state.vertex_constant_buffers,
                             static_cast<u32>(m.arg(1)),
                             static_cast<u32>(m.arg(2)), m.arg_ptr(3)); break;
        case 77: write_slots(state.pixel_constant_buffers,
                             static_cast<u32>(m.arg(1)),
                             static_cast<u32>(m.arg(2)), m.arg_ptr(3)); break;
        case 109: write_slots(state.compute_constant_buffers,
                              static_cast<u32>(m.arg(1)),
                              static_cast<u32>(m.arg(2)), m.arg_ptr(3)); break;
        case 73: write_slots(state.pixel_shader_resources,
                             static_cast<u32>(m.arg(1)),
                             static_cast<u32>(m.arg(2)), m.arg_ptr(3)); break;
        case 84: write_slots(state.vertex_shader_resources,
                             static_cast<u32>(m.arg(1)),
                             static_cast<u32>(m.arg(2)), m.arg_ptr(3)); break;
        case 105: write_slots(state.compute_shader_resources,
                              static_cast<u32>(m.arg(1)),
                              static_cast<u32>(m.arg(2)), m.arg_ptr(3)); break;
        case 106: write_slots(state.compute_unordered_access_views,
                              static_cast<u32>(m.arg(1)),
                              static_cast<u32>(m.arg(2)), m.arg_ptr(3)); break;
        case 75: write_slots(state.pixel_samplers,
                             static_cast<u32>(m.arg(1)),
                             static_cast<u32>(m.arg(2)), m.arg_ptr(3)); break;
        case 85: write_slots(state.vertex_samplers,
                             static_cast<u32>(m.arg(1)),
                             static_cast<u32>(m.arg(2)), m.arg_ptr(3)); break;
        case 108: write_slots(state.compute_samplers,
                              static_cast<u32>(m.arg(1)),
                              static_cast<u32>(m.arg(2)), m.arg_ptr(3)); break;
        case 81: write_slots(state.geometry_constant_buffers,
                             static_cast<u32>(m.arg(1)),
                             static_cast<u32>(m.arg(2)), m.arg_ptr(3)); break;
        case 87: write_slots(state.geometry_shader_resources,
                             static_cast<u32>(m.arg(1)),
                             static_cast<u32>(m.arg(2)), m.arg_ptr(3)); break;
        case 88: write_slots(state.geometry_samplers,
                             static_cast<u32>(m.arg(1)),
                             static_cast<u32>(m.arg(2)), m.arg_ptr(3)); break;
        case 97: write_slots(state.hull_shader_resources,
                             static_cast<u32>(m.arg(1)),
                             static_cast<u32>(m.arg(2)), m.arg_ptr(3)); break;
        case 99: write_slots(state.hull_samplers,
                             static_cast<u32>(m.arg(1)),
                             static_cast<u32>(m.arg(2)), m.arg_ptr(3)); break;
        case 100: write_slots(state.hull_constant_buffers,
                              static_cast<u32>(m.arg(1)),
                              static_cast<u32>(m.arg(2)), m.arg_ptr(3)); break;
        case 101: write_slots(state.domain_shader_resources,
                              static_cast<u32>(m.arg(1)),
                              static_cast<u32>(m.arg(2)), m.arg_ptr(3)); break;
        case 103: write_slots(state.domain_samplers,
                              static_cast<u32>(m.arg(1)),
                              static_cast<u32>(m.arg(2)), m.arg_ptr(3)); break;
        case 104: write_slots(state.domain_constant_buffers,
                              static_cast<u32>(m.arg(1)),
                              static_cast<u32>(m.arg(2)), m.arg_ptr(3)); break;
        // (ppShader, ppClassInstances, pNumClassInstances)
        case 74: get_stage_shader(m, state, LinkedStage::Pixel); break;
        case 76: get_stage_shader(m, state, LinkedStage::Vertex); break;
        case 82: get_stage_shader(m, state, LinkedStage::Geometry); break;
        case 98: get_stage_shader(m, state, LinkedStage::Hull); break;
        case 102: get_stage_shader(m, state, LinkedStage::Domain); break;
        case 78: write_one(state.input_layout, m.arg_ptr(1)); break;
        case 94: write_one(state.rasterizer_state, m.arg_ptr(1)); break;
        // OMGetDepthStencilState(ppState, pStencilRef)
        case 92: write_one(state.depth_stencil_state, m.arg_ptr(1));
                 if (m.arg_ptr(2) != 0) {
                     (void)m.write_u32(m.arg_ptr(2), state.stencil_ref);
                 }
                 break;
        // IAGetIndexBuffer(ppIndexBuffer, pFormat, pOffset)
        case 80: write_one(state.index_buffer, m.arg_ptr(1));
                 if (m.arg_ptr(2) != 0) {
                     (void)m.write_u32(m.arg_ptr(2), state.index_buffer_format);
                 }
                 if (m.arg_ptr(3) != 0) {
                     (void)m.write_u32(m.arg_ptr(3), state.index_buffer_offset);
                 }
                 break;
        case 83: if (m.arg_ptr(1) != 0) {
                     (void)m.write_u32(m.arg_ptr(1), state.primitive_topology);
                 }
                 break;
        default: break;
        }
        m.set_return(0);
        return;
    }
    case 93:
        // SOGetTargets(NumBuffers, ppSOTargets): owning references.
        context_so_get_targets(m, self);
        return;
    // OMGetRenderTargetsAndUnorderedAccessViews: the render-target half is
    // OMGetRenderTargets, and the UAV half reads the u# array that
    // OMSetRenderTargetsAndUnorderedAccessViews fills.
    case 90: {
        auto& state = m_context_states[self];
        const usize ptr_size = m_is_64bit ? 8u : 4u;
        const u32 rtv_count = static_cast<u32>(m.arg(1));
        const GuestAddr rtvs_out = m.arg_ptr(2);
        const GuestAddr dsv_out = m.arg_ptr(3);
        const u32 uav_start = static_cast<u32>(m.arg(4));
        const u32 uav_count = static_cast<u32>(m.arg(5));
        const GuestAddr uavs_out = m.arg_ptr(6);
        if (rtvs_out != 0) {
            for (u32 i = 0; i < rtv_count; ++i) {
                const GuestAddr view =
                    i < state.render_targets.size() ? state.render_targets[i] : 0;
                if (view != 0) (void)retain_object(view);
                (void)m.write_ptr(
                    rtvs_out + static_cast<GuestAddr>(i) * ptr_size, view);
            }
        }
        if (dsv_out != 0) {
            const GuestAddr dsv = state.depth_stencil_view;
            if (dsv != 0) (void)retain_object(dsv);
            (void)m.write_ptr(dsv_out, dsv);
        }
        if (uavs_out != 0) {
            auto& uavs = state.output_merger_unordered_access_views;
            for (u32 i = 0; i < uav_count; ++i) {
                const usize index = static_cast<usize>(uav_start) + i;
                const GuestAddr view = index < uavs.size() ? uavs[index] : 0;
                if (view != 0) (void)retain_object(view);
                (void)m.write_ptr(
                    uavs_out + static_cast<GuestAddr>(i) * ptr_size, view);
            }
        }
        m.set_return(0);
        return;
    }
    // IAGetVertexBuffers(StartSlot, NumBuffers, ppBuffers, pStrides, pOffsets)
    case 79: {
        auto& state = m_context_states[self];
        const usize ptr_size = m_is_64bit ? 8u : 4u;
        const u32 start = static_cast<u32>(m.arg(1));
        const u32 count = static_cast<u32>(m.arg(2));
        const GuestAddr buffers = m.arg_ptr(3);
        const GuestAddr strides = m.arg_ptr(4);
        const GuestAddr offsets = m.arg_ptr(5);
        for (u32 i = 0; i < count; ++i) {
            const usize index = static_cast<usize>(start) + i;
            const bool in_range = index < state.vertex_buffers.size();
            const GuestAddr buffer = in_range ? state.vertex_buffers[index] : 0;
            if (buffers != 0) {
                if (buffer != 0) (void)retain_object(buffer);
                (void)m.write_ptr(
                    buffers + static_cast<GuestAddr>(i) * ptr_size, buffer);
            }
            if (strides != 0) {
                (void)m.write_u32(
                    strides + static_cast<GuestAddr>(i) * sizeof(u32),
                    in_range ? state.vertex_buffer_strides[index] : 0);
            }
            if (offsets != 0) {
                (void)m.write_u32(
                    offsets + static_cast<GuestAddr>(i) * sizeof(u32),
                    in_range ? state.vertex_buffer_offsets[index] : 0);
            }
        }
        m.set_return(0);
        return;
    }
    // RSGetViewports / RSGetScissorRects: Learn -- when the array is NULL the
    // bound count is returned. Otherwise min(requested, bound) entries are
    // written, the requested entries past the bound ones are ZEROED, and the
    // count is updated to how many were bound (all measured).
    case 95:
    case 96: {
        auto& state = m_context_states[self];
        const GuestAddr count_ptr = m.arg_ptr(1);
        const GuestAddr out = m.arg_ptr(2);
        m.set_return(0);
        if (count_ptr == 0) return;
        const bool viewports = slot == 95;
        const u32 bound =
            viewports ? state.viewport_count : state.scissor_rect_count;
        if (out == 0) {
            (void)m.write_u32(count_ptr, bound);
            return;
        }
        const u32 requested =
            std::min(m.read_u32(count_ptr).value_or(0), 16u);
        const u32 written = std::min(requested, bound);
        const usize entry = viewports ? sizeof(state.viewports[0])
                                      : sizeof(state.scissor_rects[0]);
        const Byte* source = viewports
            ? reinterpret_cast<const Byte*>(state.viewports.data())
            : reinterpret_cast<const Byte*>(state.scissor_rects.data());
        std::array<Byte, sizeof(state.viewports)> bytes{};
        std::memcpy(bytes.data(), source, entry * written);
        (void)m_memory.write_bytes(out, bytes.data(), entry * requested);
        (void)m.write_u32(count_ptr, written);
        return;
    }
    case 89: {
        // OMGetRenderTargets(NumViews, ppRenderTargetViews, ppDepthStencilView)
        // Learn: every returned interface is AddRef'd, and slots beyond the
        // bound count are set to NULL. UE saves and restores render-target
        // state around passes with this, so leaving the out-parameters
        // untouched hands it whatever the caller's array happened to hold.
        const u32 count = static_cast<u32>(m.arg(1));
        const GuestAddr views_out = m.arg_ptr(2);
        const GuestAddr depth_out = m.arg_ptr(3);
        auto& state = m_context_states[self];
        const usize ptr_size = m_is_64bit ? 8u : 4u;
        if (views_out != 0) {
            for (u32 i = 0; i < count && i < state.render_targets.size(); ++i) {
                const GuestAddr view = state.render_targets[i];
                if (view != 0) (void)retain_object(view);
                (void)m.write_ptr(
                    views_out + static_cast<GuestAddr>(i) * ptr_size, view);
            }
            for (u32 i = static_cast<u32>(state.render_targets.size());
                 i < count; ++i) {
                (void)m.write_ptr(
                    views_out + static_cast<GuestAddr>(i) * ptr_size, 0);
            }
        }
        if (depth_out != 0) {
            const GuestAddr depth = state.depth_stencil_view;
            if (depth != 0) (void)retain_object(depth);
            (void)m.write_ptr(depth_out, depth);
        }
        d3d11_diag(
            "ID3D11DeviceContext::OMGetRenderTargets count=%u -> rt0=0x%llx "
            "dsv=0x%llx",
            count,
            static_cast<unsigned long long>(
                state.render_targets.empty() ? 0 : state.render_targets[0]),
            static_cast<unsigned long long>(state.depth_stencil_view));
        m.set_return(0);
        return;
    }
    }
}

} // namespace vitreo::winapi
