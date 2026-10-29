// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_shader_object_methods.cpp
/// @brief The COM vtable methods of ID3D11VertexShader, ID3D11PixelShader,
///        ID3D11ComputeShader and ID3D11InputLayout, and their export names.
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

constexpr GuidBytes IID_ID3D11VERTEXSHADER{
    0x64, 0x1D, 0x30, 0x3B, 0x78, 0xD6, 0x89, 0x42,
    0x88, 0x97, 0x22, 0xF8, 0x92, 0x8B, 0x72, 0xF3};
constexpr GuidBytes IID_ID3D11PIXELSHADER{
    0x0D, 0xE4, 0x82, 0xEA, 0xDC, 0x51, 0x33, 0x4F,
    0x93, 0xD4, 0xDB, 0x7C, 0x91, 0x25, 0xAE, 0x8C};
constexpr GuidBytes IID_ID3D11COMPUTESHADER{
    0x6E, 0x19, 0x5B, 0x4F, 0xBD, 0xC2, 0x5E, 0x49,
    0xBD, 0x01, 0x1F, 0xDE, 0xD3, 0x8E, 0x49, 0x69};
constexpr GuidBytes IID_ID3D11INPUTLAYOUT{
    0xDC, 0x9D, 0x81, 0xE4, 0xF0, 0x4C, 0x25, 0x40,
    0xBD, 0x26, 0x5D, 0xE8, 0x2A, 0x3E, 0x07, 0xB7};

} // namespace

void D3D11Manager::impl_VertexShaderMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto it = m_vertex_shaders.find(self);
    if (it == m_vertex_shaders.end()) {
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
             !guid_eq(iid, IID_ID3D11VERTEXSHADER))) {
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
    default:
        m.set_return(d3d11::E_NOTIMPL);
        return;
    }
}

void D3D11Manager::impl_PixelShaderMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto it = m_pixel_shaders.find(self);
    if (it == m_pixel_shaders.end()) {
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
             !guid_eq(iid, IID_ID3D11PIXELSHADER))) {
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
    default:
        m.set_return(d3d11::E_NOTIMPL);
        return;
    }
}

void D3D11Manager::impl_ComputeShaderMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto it = m_compute_shaders.find(self);
    if (it == m_compute_shaders.end()) {
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
             !guid_eq(iid, IID_ID3D11COMPUTESHADER))) {
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
            if (m.write_ptr(device_out, device->second.device_interface).is_err()) {
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
    default:
        m.set_return(d3d11::E_NOTIMPL);
        return;
    }
}

void D3D11Manager::impl_InputLayoutMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto it = m_input_layouts.find(self);
    if (it == m_input_layouts.end()) {
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
             !guid_eq(iid, IID_ID3D11INPUTLAYOUT))) {
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
    default:
        m.set_return(d3d11::E_NOTIMPL);
        return;
    }
}

std::string d3d11_vertex_shader_method_export_name(usize slot) {
    char name[64];
    std::snprintf(name, sizeof(name), "_D3D11_VertexShader_%03zu", slot);
    return name;
}
std::string d3d11_pixel_shader_method_export_name(usize slot) {
    char name[64];
    std::snprintf(name, sizeof(name), "_D3D11_PixelShader_%03zu", slot);
    return name;
}
std::string d3d11_compute_shader_method_export_name(usize slot) {
    char name[64];
    std::snprintf(name, sizeof(name), "_D3D11_ComputeShader_%03zu", slot);
    return name;
}
std::string d3d11_input_layout_method_export_name(usize slot) {
    char name[64];
    std::snprintf(name, sizeof(name), "_D3D11_InputLayout_%03zu", slot);
    return name;
}

} // namespace vitreo::winapi
