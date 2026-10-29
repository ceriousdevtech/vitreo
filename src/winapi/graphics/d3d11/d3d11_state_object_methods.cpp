// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_state_object_methods.cpp
/// @brief The COM vtable methods of ID3D11BlendState(1),
///        ID3D11RasterizerState(1/2), ID3D11DepthStencilState and
///        ID3D11SamplerState, and their export names.
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

namespace {

constexpr GuidBytes IID_ID3D11BLENDSTATE{
    0xAA, 0x8F, 0xB6, 0x75, 0x7D, 0x34, 0x59, 0x41,
    0x8F, 0x45, 0xA0, 0x64, 0x0F, 0x01, 0xCD, 0x9A};
constexpr GuidBytes IID_ID3D11RASTERIZERSTATE{
    0x81, 0xAB, 0xB4, 0x9B, 0x1A, 0xAB, 0x8F, 0x4D,
    0xB5, 0x06, 0xFC, 0x04, 0x20, 0x0B, 0x6E, 0xE7};
constexpr GuidBytes IID_ID3D11BLENDSTATE1{
    0xBE, 0xFA, 0x86, 0xCC, 0x55, 0xDA, 0x1D, 0x40,
    0x85, 0xE7, 0xE3, 0xC9, 0xDE, 0x28, 0x77, 0xE9};
constexpr GuidBytes IID_ID3D11RASTERIZERSTATE1{
    0xA6, 0xD7, 0x17, 0x12, 0x39, 0x50, 0x8C, 0x41,
    0xB0, 0x42, 0x9C, 0xBE, 0x25, 0x6A, 0xFD, 0x6E};
constexpr GuidBytes IID_ID3D11RASTERIZERSTATE2{
    0xFB, 0x02, 0xBD, 0x6F, 0x9F, 0x20, 0xC4, 0x46,
    0xB0, 0x59, 0x2E, 0xD1, 0x55, 0x86, 0xA6, 0xAC};
constexpr GuidBytes IID_ID3D11DEPTHSTENCILSTATE{
    0xFB, 0x3E, 0x82, 0x03, 0x8F, 0x8D, 0x1C, 0x4E,
    0x9A, 0xA2, 0xF6, 0x4B, 0xB2, 0xCB, 0xFD, 0xF1};
constexpr GuidBytes IID_ID3D11SAMPLERSTATE{
    0x51, 0xEA, 0x6F, 0xDA, 0x4C, 0x56, 0x87, 0x44,
    0x98, 0x10, 0xF0, 0xD0, 0xF9, 0xB4, 0xE3, 0xA5};

} // namespace

void D3D11Manager::impl_BlendStateMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto it = m_blend_states.find(self);
    if (it == m_blend_states.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0: {
        const GuestAddr object_out = m.arg_ptr(2);
        if (object_out == 0) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        (void)m.write_ptr(object_out, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_eq(iid, IID_IUNKNOWN) &&
             !guid_eq(iid, IID_ID3D11DEVICECHILD) &&
             !guid_eq(iid, IID_ID3D11BLENDSTATE) &&
             !guid_eq(iid, IID_ID3D11BLENDSTATE1))) {
            // The object's D3D10 interfaces (d3d10_device.cpp).
            if (const GuestAddr facet = d3d10_interface_for(self, iid)) {
                (void)retain_object(self);
                (void)m.write_ptr(object_out, facet);
                m.set_return(d3d11::S_OK);
                return;
            }
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        (void)retain_object(self);
        if (m.write_ptr(object_out, self).is_err()) {
            (void)release_object(self);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 1:
        m.set_return(retain_object(self));
        return;
    case 2:
        m.set_return(release_object(self));
        return;
    case 3: {
        const GuestAddr device_out = m.arg_ptr(1);
        auto device = m_devices.find(it->second.device_owner);
        if (device_out != 0 && device != m_devices.end()) {
            (void)add_ref(device->second.device_interface);
            if (m.write_ptr(device_out,
                            device->second.device_interface).is_err()) {
                (void)release(device->second.device_interface);
            }
        }
        m.set_return(0);
        return;
    }
    case 4:
        m.set_return(get_private_data(m, it->second.private_data));
        return;
    case 5:
        m.set_return(set_private_data(m, it->second.private_data));
        return;
    case 6:
        m.set_return(set_private_data_interface(m, it->second.private_data));
        return;
    case 7: {
        // GetDesc — write AlphaToCoverage / IndependentBlend / RT[0..7].
        const GuestAddr desc_out = m.arg_ptr(1);
        if (desc_out == 0) {
            m.set_return(0);
            return;
        }
        const auto& d = it->second.desc;
        (void)m_memory.write_u32(desc_out, d.alpha_to_coverage_enable);
        (void)m_memory.write_u32(desc_out + 4, d.independent_blend_enable);
        for (u32 target = 0; target < 8; ++target) {
            const GuestAddr base = desc_out + 8 + target * 32;
            (void)m_memory.write_u32(base, d.blend_enable[target]);
            (void)m_memory.write_u32(base + 4, d.src_blend[target]);
            (void)m_memory.write_u32(base + 8, d.dest_blend[target]);
            (void)m_memory.write_u32(base + 12, d.blend_op[target]);
            (void)m_memory.write_u32(base + 16, d.src_blend_alpha[target]);
            (void)m_memory.write_u32(base + 20, d.dest_blend_alpha[target]);
            (void)m_memory.write_u32(base + 24, d.blend_op_alpha[target]);
            (void)m_memory.write_u32(
                base + 28, d.render_target_write_mask[target]);
        }
        m.set_return(0);
        return;
    }
    case 8: {
        // ID3D11BlendState1::GetDesc1. Per target: BlendEnable,
        // LogicOpEnable, the six blend fields, LogicOp (NOOP unless enabled
        // -- measured), and a UINT8 write mask padded to 4 bytes; the
        // canonical description (canonical_blend_desc).
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(0);
            return;
        }
        const auto& d = it->second.desc;
        (void)m_memory.write_u32(out, d.alpha_to_coverage_enable);
        (void)m_memory.write_u32(out + 4, d.independent_blend_enable);
        for (u32 target = 0; target < 8; ++target) {
            const GuestAddr base = out + 8 + target * 40;
            const std::array<u32, 10> words{
                d.blend_enable[target], d.logic_op_enable[target],
                d.src_blend[target],
                d.dest_blend[target], d.blend_op[target],
                d.src_blend_alpha[target], d.dest_blend_alpha[target],
                d.blend_op_alpha[target], d.logic_op[target],
                d.render_target_write_mask[target] & 0xFFu};
            (void)m_memory.write_bytes(
                base, reinterpret_cast<const Byte*>(words.data()),
                sizeof(words));
        }
        m.set_return(0);
        return;
    }
    default:
        m.set_return(d3d11::E_NOTIMPL);
        return;
    }
}

void D3D11Manager::impl_RasterizerStateMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto it = m_rasterizer_states.find(self);
    if (it == m_rasterizer_states.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    if (slot == 2) {
        m.set_return(release_object(self));
        return;
    }
    if (slot == 1) {
        m.set_return(retain_object(self));
        return;
    }
    if (slot == 0) {
        const GuestAddr object_out = m.arg_ptr(2);
        if (object_out == 0) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        (void)m.write_ptr(object_out, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid)) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        // The runtime's rasterizer state is always the latest version,
        // whichever creator made it: a CreateRasterizerState object answers
        // ID3D11RasterizerState1 and 2 too (measured,
        // d3d11_a04_state_objects_probe "rs base-object qi
        // RasterizerState1/2"), and deduplication hands one object to every
        // creation path.
        if (!guid_eq(iid, IID_IUNKNOWN) &&
            !guid_eq(iid, IID_ID3D11DEVICECHILD) &&
            !guid_eq(iid, IID_ID3D11RASTERIZERSTATE) &&
            !guid_eq(iid, IID_ID3D11RASTERIZERSTATE1) &&
            !guid_eq(iid, IID_ID3D11RASTERIZERSTATE2)) {
            // The object's D3D10 interfaces (d3d10_device.cpp).
            if (const GuestAddr facet = d3d10_interface_for(self, iid)) {
                (void)retain_object(self);
                (void)m.write_ptr(object_out, facet);
                m.set_return(d3d11::S_OK);
                return;
            }
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        (void)retain_object(self);
        if (m.write_ptr(object_out, self).is_err()) {
            (void)release_object(self);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    if (slot == 3) {
        const GuestAddr device_out = m.arg_ptr(1);
        auto device = m_devices.find(it->second.device_owner);
        if (device_out != 0 && device != m_devices.end()) {
            (void)add_ref(device->second.device_interface);
            if (m.write_ptr(device_out,
                            device->second.device_interface).is_err()) {
                (void)release(device->second.device_interface);
            }
        }
        m.set_return(0);
        return;
    }
    if (slot == 4) {
        m.set_return(get_private_data(m, it->second.private_data));
        return;
    }
    if (slot == 5) {
        m.set_return(set_private_data(m, it->second.private_data));
        return;
    }
    if (slot == 6) {
        // SetPrivateDataInterface, as on every other device child (measured,
        // "rs setprivatedatainterface").
        m.set_return(set_private_data_interface(m, it->second.private_data));
        return;
    }
    if (slot >= 7 && slot <= 9) {
        const GuestAddr desc_out = m.arg_ptr(1);
        if (desc_out == 0) {
            m.set_return(0);
            return;
        }
        const auto& d = it->second.desc;
        (void)m_memory.write_u32(desc_out, d.fill_mode);
        (void)m_memory.write_u32(desc_out + 4, d.cull_mode);
        (void)m_memory.write_u32(desc_out + 8, d.front_counter_clockwise);
        (void)m_memory.write_u32(desc_out + 12,
                                 static_cast<u32>(d.depth_bias));
        (void)m_memory.write_bytes(
            desc_out + 16,
            reinterpret_cast<const u8*>(&d.depth_bias_clamp), 4);
        (void)m_memory.write_bytes(
            desc_out + 20,
            reinterpret_cast<const u8*>(&d.slope_scaled_depth_bias), 4);
        (void)m_memory.write_u32(desc_out + 24, d.depth_clip_enable);
        (void)m_memory.write_u32(desc_out + 28, d.scissor_enable);
        (void)m_memory.write_u32(desc_out + 32, d.multisample_enable);
        (void)m_memory.write_u32(desc_out + 36, d.antialiased_line_enable);
        if (slot >= 8) {
            (void)m_memory.write_u32(desc_out + 40, d.forced_sample_count);
        }
        if (slot >= 9) {
            (void)m_memory.write_u32(desc_out + 44, d.conservative_raster);
        }
        m.set_return(0);
        return;
    }
    m.set_return(d3d11::E_NOTIMPL);
}

void D3D11Manager::impl_DepthStencilStateMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto it = m_depth_stencil_states.find(self);
    if (it == m_depth_stencil_states.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    if (slot == 2) {
        m.set_return(release_object(self));
        return;
    }
    if (slot == 1) {
        m.set_return(retain_object(self));
        return;
    }
    if (slot == 0) {
        const GuestAddr object_out = m.arg_ptr(2);
        if (object_out == 0) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        (void)m.write_ptr(object_out, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_eq(iid, IID_IUNKNOWN) &&
             !guid_eq(iid, IID_ID3D11DEVICECHILD) &&
             !guid_eq(iid, IID_ID3D11DEPTHSTENCILSTATE))) {
            // The object's D3D10 interfaces (d3d10_device.cpp).
            if (const GuestAddr facet = d3d10_interface_for(self, iid)) {
                (void)retain_object(self);
                (void)m.write_ptr(object_out, facet);
                m.set_return(d3d11::S_OK);
                return;
            }
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        (void)retain_object(self);
        if (m.write_ptr(object_out, self).is_err()) {
            (void)release_object(self);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    if (slot == 3) {
        const GuestAddr device_out = m.arg_ptr(1);
        auto device = m_devices.find(it->second.device_owner);
        if (device_out != 0 && device != m_devices.end()) {
            (void)add_ref(device->second.device_interface);
            if (m.write_ptr(device_out,
                            device->second.device_interface).is_err()) {
                (void)release(device->second.device_interface);
            }
        }
        m.set_return(0);
        return;
    }
    if (slot == 4) {
        m.set_return(get_private_data(m, it->second.private_data));
        return;
    }
    if (slot == 6) {
        // SetPrivateDataInterface, as on every other device child.
        m.set_return(set_private_data_interface(m, it->second.private_data));
        return;
    }
    if (slot == 5) {
        m.set_return(set_private_data(m, it->second.private_data));
        return;
    }
    if (slot == 7) {
        const GuestAddr out = m.arg_ptr(1);
        const auto& d = it->second.desc;
        bool ok = out != 0 &&
            m_memory.write_u32(out, d.depth_enable).is_ok() &&
            m_memory.write_u32(out + 4, d.depth_write_mask).is_ok() &&
            m_memory.write_u32(out + 8, d.depth_func).is_ok() &&
            m_memory.write_u32(out + 12, d.stencil_enable).is_ok() &&
            m_memory.write_u8(out + 16, d.stencil_read_mask).is_ok() &&
            m_memory.write_u8(out + 17, d.stencil_write_mask).is_ok();
        const std::array<u32, 8> fields{
            d.front_stencil_fail_op, d.front_stencil_depth_fail_op,
            d.front_stencil_pass_op, d.front_stencil_func,
            d.back_stencil_fail_op, d.back_stencil_depth_fail_op,
            d.back_stencil_pass_op, d.back_stencil_func,
        };
        for (usize i = 0; ok && i < fields.size(); ++i) {
            ok = m_memory.write_u32(out + 20 + i * 4, fields[i]).is_ok();
        }
        if (!ok) pipeline_diag("DepthStencilState::GetDesc invalid output");
        m.set_return(0);
        return;
    }
    m.set_return(d3d11::E_NOTIMPL);
}

void D3D11Manager::impl_SamplerStateMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto it = m_sampler_states.find(self);
    if (it == m_sampler_states.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    if (slot == 2) {
        m.set_return(release_object(self));
        return;
    }
    if (slot == 1) {
        m.set_return(retain_object(self));
        return;
    }
    if (slot == 0) {
        const GuestAddr object_out = m.arg_ptr(2);
        if (object_out == 0) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        (void)m.write_ptr(object_out, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_eq(iid, IID_IUNKNOWN) &&
             !guid_eq(iid, IID_ID3D11DEVICECHILD) &&
             !guid_eq(iid, IID_ID3D11SAMPLERSTATE))) {
            // The object's D3D10 interfaces (d3d10_device.cpp).
            if (const GuestAddr facet = d3d10_interface_for(self, iid)) {
                (void)retain_object(self);
                (void)m.write_ptr(object_out, facet);
                m.set_return(d3d11::S_OK);
                return;
            }
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        (void)retain_object(self);
        if (m.write_ptr(object_out, self).is_err()) {
            (void)release_object(self);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    if (slot == 3) {
        const GuestAddr device_out = m.arg_ptr(1);
        auto device = m_devices.find(it->second.device_owner);
        if (device_out != 0 && device != m_devices.end()) {
            (void)add_ref(device->second.device_interface);
            if (m.write_ptr(device_out,
                            device->second.device_interface).is_err()) {
                (void)release(device->second.device_interface);
            }
        }
        m.set_return(0);
        return;
    }
    if (slot == 4) {
        m.set_return(get_private_data(m, it->second.private_data));
        return;
    }
    if (slot == 6) {
        // SetPrivateDataInterface, as on every other device child.
        m.set_return(set_private_data_interface(m, it->second.private_data));
        return;
    }
    if (slot == 5) {
        m.set_return(set_private_data(m, it->second.private_data));
        return;
    }
    if (slot == 7) {
        const GuestAddr out = m.arg_ptr(1);
        const auto& d = it->second.desc;
        bool ok = out != 0 &&
            m_memory.write_u32(out, d.filter).is_ok() &&
            m_memory.write_u32(out + 4, d.address_u).is_ok() &&
            m_memory.write_u32(out + 8, d.address_v).is_ok() &&
            m_memory.write_u32(out + 12, d.address_w).is_ok() &&
            m_memory.write_bytes(out + 16,
                reinterpret_cast<const u8*>(&d.mip_lod_bias), 4).is_ok() &&
            m_memory.write_u32(out + 20, d.max_anisotropy).is_ok() &&
            m_memory.write_u32(out + 24, d.comparison_func).is_ok() &&
            m_memory.write_bytes(out + 28,
                reinterpret_cast<const u8*>(d.border_color), 16).is_ok() &&
            m_memory.write_bytes(out + 44,
                reinterpret_cast<const u8*>(&d.min_lod), 4).is_ok() &&
            m_memory.write_bytes(out + 48,
                reinterpret_cast<const u8*>(&d.max_lod), 4).is_ok();
        if (!ok) pipeline_diag("SamplerState::GetDesc invalid output");
        m.set_return(0);
        return;
    }
    m.set_return(d3d11::E_NOTIMPL);
}

std::string d3d11_blend_state_method_export_name(usize slot) {
    char name[64];
    std::snprintf(name, sizeof(name), "_D3D11_BlendState_%03zu", slot);
    return name;
}
std::string d3d11_rasterizer_state_method_export_name(usize slot) {
    char name[64];
    std::snprintf(name, sizeof(name), "_D3D11_RasterizerState_%03zu", slot);
    return name;
}
std::string d3d11_depth_stencil_state_method_export_name(usize slot) {
    char name[64];
    std::snprintf(name, sizeof(name), "_D3D11_DepthStencilState_%03zu", slot);
    return name;
}
std::string d3d11_sampler_state_method_export_name(usize slot) {
    char name[64];
    std::snprintf(name, sizeof(name), "_D3D11_SamplerState_%03zu", slot);
    return name;
}

} // namespace vitreo::winapi
