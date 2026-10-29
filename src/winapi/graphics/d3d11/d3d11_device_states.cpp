// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_device_states.cpp
/// @brief ID3D11Device state-object creators: CreateBlendState,
///        CreateDepthStencilState, CreateRasterizerState(2) and
///        CreateSamplerState (vtable slots 20-23 and 56).
///
/// Split out of d3d11_device_methods.cpp; impl_DeviceMethod forwards the
/// slots here once it has found the device. Validation, the canonical
/// description and deduplication are in d3d11_device_state_pool.cpp.

#include "core/memory/host_memory_floor.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_caps_log.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/gpu_resource.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <array>
#include <cstring>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi {

void D3D11Manager::device_state_method(Marshaler& m, usize slot, GuestAddr self,
                                       Device* device) {
    switch (slot) {
    case 20: {
        // CreateBlendState(pBlendStateDesc, ppBlendState). The description is
        // validated before a NULL ppBlendState answers S_FALSE (measured,
        // d3d11_a04_state_objects_probe "blend null-out invalid-desc").
        const GuestAddr desc_address = m.arg_ptr(1);
        const GuestAddr state_out = m.arg_ptr(2);
        if (state_out != 0) (void)m.write_ptr(state_out, 0);
        BlendStateDesc desc{};
        if (desc_address == 0 ||
            !read_blend_desc(desc_address, /*desc1=*/false, desc) ||
            !blend_desc_valid(desc, /*logic_ops=*/false)) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        d3d11_diag(
            "ID3D11Device::CreateBlendState enable=%u src=%u dst=%u op=%u "
            "src-alpha=%u dst-alpha=%u op-alpha=%u mask=0x%x",
            desc.blend_enable[0], desc.src_blend[0],
            desc.dest_blend[0], desc.blend_op[0],
            desc.src_blend_alpha[0], desc.dest_blend_alpha[0],
            desc.blend_op_alpha[0], desc.render_target_write_mask[0]);
        if (state_out == 0) {
            m.set_return(1u);
            return;
        }
        GuestAddr state = 0;
        const u32 hr = acquire_blend_state(
            m_interfaces.find(self)->second.owner, desc, state);
        if (hr != d3d11::S_OK) {
            m.set_return(hr);
            return;
        }
        if (m.write_ptr(state_out, state).is_err()) {
            (void)release_object(state);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 21: {
        // CreateDepthStencilState
        const GuestAddr desc_address = m.arg_ptr(1);
        const GuestAddr state_out = m.arg_ptr(2);
        if (state_out != 0) (void)m.write_ptr(state_out, 0);
        if (desc_address == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        DepthStencilStateDesc desc{};
        const auto depth_enable = m_memory.read_u32(desc_address);
        const auto write_mask = m_memory.read_u32(desc_address + 4);
        const auto depth_func = m_memory.read_u32(desc_address + 8);
        const auto stencil_enable = m_memory.read_u32(desc_address + 12);
        const auto stencil_read_mask = m_memory.read_u8(desc_address + 16);
        const auto stencil_write_mask = m_memory.read_u8(desc_address + 17);
        std::array<u32*, 8> stencil_fields{
            &desc.front_stencil_fail_op,
            &desc.front_stencil_depth_fail_op,
            &desc.front_stencil_pass_op,
            &desc.front_stencil_func,
            &desc.back_stencil_fail_op,
            &desc.back_stencil_depth_fail_op,
            &desc.back_stencil_pass_op,
            &desc.back_stencil_func,
        };
        bool valid = depth_enable.is_ok() && write_mask.is_ok() &&
                     depth_func.is_ok() && stencil_enable.is_ok() &&
                     stencil_read_mask.is_ok() && stencil_write_mask.is_ok();
        desc.depth_enable = depth_enable.value_or(0);
        desc.depth_write_mask = write_mask.value_or(0);
        desc.depth_func = depth_func.value_or(0);
        desc.stencil_enable = stencil_enable.value_or(0);
        desc.stencil_read_mask = stencil_read_mask.value_or(0);
        desc.stencil_write_mask = stencil_write_mask.value_or(0);
        for (usize i = 0; i < stencil_fields.size(); ++i) {
            const auto value = m_memory.read_u32(desc_address + 20 + i * 4);
            valid = valid && value.is_ok();
            *stencil_fields[i] = value.value_or(0);
        }
        const auto valid_compare = [](u32 value) {
            return value >= 1 && value <= 8;
        };
        const auto valid_op = [](u32 value) {
            return value >= 1 && value <= 8;
        };
        // D3D11.3 functional spec §17.9: with DepthEnable FALSE no depth
        // test runs, and Windows validates DepthFunc only when it does -- a
        // zero-filled description is a valid "depth off" state -- while
        // DepthWriteMask must be ZERO or ALL either way (measured,
        // d3d11_a04_state_objects_probe "ds zero-filled", "ds depth=0
        // func=0|9", "ds depth=0 writemask=2"). The stencil fields likewise
        // only when StencilEnable is set.
        valid = valid && desc.depth_write_mask <= 1 &&
                (desc.depth_enable == 0 || valid_compare(desc.depth_func));
        if (desc.stencil_enable != 0) {
            valid = valid && valid_op(desc.front_stencil_fail_op) &&
                    valid_op(desc.front_stencil_depth_fail_op) &&
                    valid_op(desc.front_stencil_pass_op) &&
                    valid_compare(desc.front_stencil_func) &&
                    valid_op(desc.back_stencil_fail_op) &&
                    valid_op(desc.back_stencil_depth_fail_op) &&
                    valid_op(desc.back_stencil_pass_op) &&
                    valid_compare(desc.back_stencil_func);
        }
        if (!valid) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        if (state_out == 0) {
            m.set_return(1u);
            return;
        }
        GuestAddr state = 0;
        const u32 hr = acquire_depth_stencil_state(
            m_interfaces.find(self)->second.owner, desc, state);
        if (hr != d3d11::S_OK) {
            m.set_return(hr);
            return;
        }
        if (m.write_ptr(state_out, state).is_err()) {
            (void)release_object(state);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 22:
    case 56: {
        // CreateRasterizerState(pDesc, ppState) and
        // ID3D11Device3::CreateRasterizerState2(pDesc2, ppState). DESC2
        // keeps the 40-byte D3D11_RASTERIZER_DESC and appends
        // ForcedSampleCount and ConservativeRaster; the base description is
        // DESC2 with both 0. One pool serves every version (measured,
        // d3d11_a04_state_objects_probe "rs2 dedup vs CreateRasterizerState").
        const GuestAddr desc_address = m.arg_ptr(1);
        const GuestAddr state_out = m.arg_ptr(2);
        if (state_out != 0) (void)m.write_ptr(state_out, 0);
        RasterizerStateDesc desc{};
        if (desc_address == 0 ||
            m_memory.read_bytes(desc_address, reinterpret_cast<Byte*>(&desc),
                                slot == 56 ? 48u : 40u).is_err() ||
            !rasterizer_desc_valid(desc, device->feature_level)) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        if (state_out == 0) {
            m.set_return(1u); // S_FALSE: validation-only success.
            return;
        }
        GuestAddr state = 0;
        const u32 hr = acquire_rasterizer_state(
            m_interfaces.find(self)->second.owner, desc, state);
        if (hr != d3d11::S_OK) {
            m.set_return(hr);
            return;
        }
        if (m.write_ptr(state_out, state).is_err()) {
            (void)release_object(state);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        d3d11_diag(
            "ID3D11Device::CreateRasterizerState%s -> 0x%llx fill=%u "
            "cull=%u scissor=%u forced=%u",
            slot == 56 ? "2" : "", static_cast<unsigned long long>(state),
            desc.fill_mode, desc.cull_mode, desc.scissor_enable,
            desc.forced_sample_count);
        m.set_return(d3d11::S_OK);
        return;
    }
    case 23: {
        // CreateSamplerState
        const GuestAddr desc_address = m.arg_ptr(1);
        const GuestAddr state_out = m.arg_ptr(2);
        if (state_out != 0) (void)m.write_ptr(state_out, 0);
        if (desc_address == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        SamplerStateDesc desc{};
        const auto read_float = [&](GuestAddr address, float& value) {
            return m_memory.read_bytes(
                address, reinterpret_cast<u8*>(&value), sizeof(value)).is_ok();
        };
        bool valid = m_memory.read_u32(desc_address).is_ok() &&
            m_memory.read_u32(desc_address + 4).is_ok() &&
            m_memory.read_u32(desc_address + 8).is_ok() &&
            m_memory.read_u32(desc_address + 12).is_ok() &&
            read_float(desc_address + 16, desc.mip_lod_bias) &&
            m_memory.read_u32(desc_address + 20).is_ok() &&
            m_memory.read_u32(desc_address + 24).is_ok();
        if (valid) {
            desc.filter = m_memory.read_u32(desc_address).value();
            desc.address_u = m_memory.read_u32(desc_address + 4).value();
            desc.address_v = m_memory.read_u32(desc_address + 8).value();
            desc.address_w = m_memory.read_u32(desc_address + 12).value();
            desc.max_anisotropy =
                m_memory.read_u32(desc_address + 20).value();
            desc.comparison_func =
                m_memory.read_u32(desc_address + 24).value();
            for (usize i = 0; valid && i < 4; ++i) {
                valid = read_float(desc_address + 28 + i * 4,
                                   desc.border_color[i]);
            }
            valid = valid && read_float(desc_address + 44, desc.min_lod) &&
                    read_float(desc_address + 48, desc.max_lod);
        }
        const u32 filter_base = desc.filter & 0x7fu;
        const bool filter_valid = (desc.filter & ~0x1d5u) == 0 &&
            ((filter_base & ~0x15u) == 0 || filter_base == 0x55u);
        const bool anisotropic = filter_base == 0x55u;
        const bool comparison = (desc.filter & 0x180u) == 0x80u;
        const auto address_valid = [](u32 address) {
            return address >= 1u && address <= 5u;
        };
        // Learn D3D11_SAMPLER_DESC and d3d11.h, as measured
        // (d3d11_a04_state_objects_probe "smp *"):
        //  - MaxAnisotropy lies in [D3D11_MIN_MAXANISOTROPY (0),
        //    D3D11_MAX_MAXANISOTROPY] for every filter -- 0 is legal even for
        //    an anisotropic one (Learn's "between 1 and 16" is contradicted
        //    by the header and the runtime), and 17 is refused even for a
        //    point filter;
        //  - ComparisonFunc is judged only for a COMPARISON filter;
        //  - MipLODBias lies in [D3D11_MIP_LOD_BIAS_MIN, _MAX], so NaN and
        //    the infinities are refused;
        //  - MinLOD and MaxLOD may be any non-NaN value, and MinLOD > MaxLOD
        //    is legal: D3D11.3 7.18.11, "MinLOD takes precedence" (measured:
        //    MinLOD 3, MaxLOD 1 samples mip 3, d3d11_a07_sampler_probe `M6`).
        valid = valid && filter_valid && address_valid(desc.address_u) &&
                address_valid(desc.address_v) &&
                address_valid(desc.address_w) &&
                desc.max_anisotropy <= D3D11_MAX_MAXANISOTROPY &&
                (!comparison || (desc.comparison_func >= 1u &&
                                 desc.comparison_func <= 8u)) &&
                desc.mip_lod_bias >= D3D11_MIP_LOD_BIAS_MIN &&
                desc.mip_lod_bias <= D3D11_MIP_LOD_BIAS_MAX &&
                !std::isnan(desc.min_lod) && !std::isnan(desc.max_lod);
        if (!valid) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        // The MINIMUM_ / MAXIMUM_ filters (all 18, measured accepted on WARP,
        // d3d11_minmax_probe [caps]) reduce in the translated shader
        // (DxbcTranslationOptions::minmax_samplers), so every one of them is
        // served: D3D11_OPTIONS1.MinMaxFiltering is TRUE.
        // BORDER addressing takes any float4 (D3D11.3 7.18.9.1). Metal offers
        // transparent black, opaque black and opaque white; every other
        // colour -- and for a comparison filter every border red but 0 and
        // 1, which is compared rather than filtered -- is completed in the
        // shader through the sampler's border twin (border_twin_mask).
        if (state_out == 0) {
            m.set_return(1u);
            return;
        }
        GuestAddr state = 0;
        const u32 hr = acquire_sampler_state(
            m_interfaces.find(self)->second.owner, desc, state);
        if (hr != d3d11::S_OK) {
            m.set_return(hr);
            return;
        }
        if (m.write_ptr(state_out, state).is_err()) {
            (void)release_object(state);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        d3d11_diag(
            "ID3D11Device::CreateSamplerState -> 0x%llx filter=0x%x "
            "address=(%u,%u,%u) lod=%g..%g bias=%g aniso=%u",
            static_cast<unsigned long long>(state), desc.filter,
            desc.address_u, desc.address_v, desc.address_w,
            static_cast<double>(desc.min_lod),
            static_cast<double>(desc.max_lod),
            static_cast<double>(desc.mip_lod_bias), desc.max_anisotropy);
        m.set_return(d3d11::S_OK);
        return;
    }
    }
}

} // namespace vitreo::winapi
