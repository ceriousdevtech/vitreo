// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d10_facets.cpp
/// @brief The D3D10 vtable specifications, the facets' QueryInterface
///        graph and identity, and the view, state, device-child and
///        query facets. Split out of d3d10_device.cpp as verbatim code
///        motion.

#include "winapi/graphics/d3d10/d3d10.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d10/d3d10_device_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace vitreo::winapi {
namespace {

/// D3D10_QUERY_PIPELINE_STATISTICS: 8 UINT64s, the first 8 of D3D11's 11.
constexpr u32 kQueryPipelineStatistics = 4u;
constexpr u32 kD3D10PipelineStatisticsBytes = 64u;
constexpr u32 kD3D11PipelineStatisticsBytes = 88u;

} // namespace

// -----------------------------------------------------------------------------
// The vtable specifications (create_d3d11_thunks registers them)
// -----------------------------------------------------------------------------

std::string d3d10_method_export_name(const char* interface_name, usize slot) {
    char name[96];
    std::snprintf(name, sizeof(name), "_D3D10_%s_%03zu", interface_name, slot);
    return name;
}

std::span<const D3D10VtableSpec> d3d10_vtable_specs() {
    using T = D3D11Manager::MethodThunks;
    static const std::array<D3D10VtableSpec, 14> specs{{
        {"Device", d3d10::DEVICE_METHOD_COUNT,
         d3d10::DEVICE_STDCALL_BYTES.data(),
         &D3D11Manager::impl_D3D10DeviceMethod,
         [](T& t) { return t.d3d10_device.data(); }},
        {"Buffer", d3d10::BUFFER_METHOD_COUNT,
         d3d10::BUFFER_STDCALL_BYTES.data(),
         &D3D11Manager::impl_D3D10BufferMethod,
         [](T& t) { return t.d3d10_buffer.data(); }},
        {"Texture", d3d10::TEXTURE_METHOD_COUNT,
         d3d10::TEXTURE_STDCALL_BYTES.data(),
         &D3D11Manager::impl_D3D10TextureMethod,
         [](T& t) { return t.d3d10_texture.data(); }},
        {"ShaderResourceView", d3d10::SHADER_RESOURCE_VIEW_METHOD_COUNT,
         d3d10::SHADER_RESOURCE_VIEW_STDCALL_BYTES.data(),
         &D3D11Manager::impl_D3D10ShaderResourceViewMethod,
         [](T& t) { return t.d3d10_shader_resource_view.data(); }},
        {"RenderTargetView", d3d10::RENDER_TARGET_VIEW_METHOD_COUNT,
         d3d10::RENDER_TARGET_VIEW_STDCALL_BYTES.data(),
         &D3D11Manager::impl_D3D10RenderTargetViewMethod,
         [](T& t) { return t.d3d10_render_target_view.data(); }},
        {"DepthStencilView", d3d10::DEPTH_STENCIL_VIEW_METHOD_COUNT,
         d3d10::DEPTH_STENCIL_VIEW_STDCALL_BYTES.data(),
         &D3D11Manager::impl_D3D10DepthStencilViewMethod,
         [](T& t) { return t.d3d10_depth_stencil_view.data(); }},
        {"BlendState", d3d10::BLEND_STATE_METHOD_COUNT,
         d3d10::BLEND_STATE_STDCALL_BYTES.data(),
         &D3D11Manager::impl_D3D10BlendStateMethod,
         [](T& t) { return t.d3d10_blend_state.data(); }},
        {"DepthStencilState", d3d10::DEPTH_STENCIL_STATE_METHOD_COUNT,
         d3d10::DEPTH_STENCIL_STATE_STDCALL_BYTES.data(),
         &D3D11Manager::impl_D3D10DepthStencilStateMethod,
         [](T& t) { return t.d3d10_depth_stencil_state.data(); }},
        {"RasterizerState", d3d10::RASTERIZER_STATE_METHOD_COUNT,
         d3d10::RASTERIZER_STATE_STDCALL_BYTES.data(),
         &D3D11Manager::impl_D3D10RasterizerStateMethod,
         [](T& t) { return t.d3d10_rasterizer_state.data(); }},
        {"SamplerState", d3d10::SAMPLER_STATE_METHOD_COUNT,
         d3d10::SAMPLER_STATE_STDCALL_BYTES.data(),
         &D3D11Manager::impl_D3D10SamplerStateMethod,
         [](T& t) { return t.d3d10_sampler_state.data(); }},
        {"DeviceChild", d3d10::DEVICE_CHILD_METHOD_COUNT,
         d3d10::DEVICE_CHILD_STDCALL_BYTES.data(),
         &D3D11Manager::impl_D3D10DeviceChildMethod,
         [](T& t) { return t.d3d10_device_child.data(); }},
        {"Query", d3d10::QUERY_METHOD_COUNT,
         d3d10::QUERY_STDCALL_BYTES.data(),
         &D3D11Manager::impl_D3D10QueryMethod,
         [](T& t) { return t.d3d10_query.data(); }},
        {"Blob", d3d10::BLOB_METHOD_COUNT, d3d10::BLOB_STDCALL_BYTES.data(),
         &D3D11Manager::impl_D3D10BlobMethod,
         [](T& t) { return t.d3d10_blob.data(); }},
        {"StateBlock", d3d10::STATE_BLOCK_METHOD_COUNT,
         d3d10::STATE_BLOCK_STDCALL_BYTES.data(),
         &D3D11Manager::impl_D3D10StateBlockMethod,
         [](T& t) { return t.d3d10_state_block.data(); }},
    }};
    return specs;
}

// -----------------------------------------------------------------------------
// Facets: identity
// -----------------------------------------------------------------------------

void D3D11Manager::d3d10_forget_facet(GuestAddr address) {
    const auto it = m_d3d10_facets.find(address + kD3D10FacetOffset);
    if (it != m_d3d10_facets.end() && it->second == address) {
        m_d3d10_facets.erase(it);
    }
}

GuestAddr D3D11Manager::d3d10_device_facet(GuestAddr device_owner) const {
    const auto device = m_devices.find(device_owner);
    if (device == m_devices.end() || !device->second.d3d10_interfaces) {
        return 0;
    }
    return device->second.d3d10_device_interface;
}

GuestAddr D3D11Manager::d3d10_interface_for(GuestAddr owner,
                                            const GuidBytes& iid) {
    // The measured graphs (d3d10_contract_probe [qi], the same from a D3D10
    // and a D3D11 device): ID3D10DeviceChild on every kind, plus
    //  buffer: ID3D10Resource, ID3D10Buffer; texture: ID3D10Resource and its
    //  dimension's ID3D10Texture1D/2D/3D; SRV: ID3D10View,
    //  ID3D10ShaderResourceView, ID3D10ShaderResourceView1; RTV / DSV:
    //  ID3D10View and their own; blend: ID3D10BlendState and
    //  ID3D10BlendState1 (from either creator); the other states, input
    //  layouts and shaders: their own; query: ID3D10Asynchronous,
    //  ID3D10Query, and ID3D10Predicate for a predicate type.
    D3D10Kind kind{};
    if (!m_d3d10_ready || !d3d10_kind_of(owner, kind)) return 0;
    bool answers = guid_equal(iid, IID_ID3D10DEVICECHILD);
    switch (kind) {
    case D3D10Kind::Buffer:
        answers = answers || guid_equal(iid, IID_ID3D10RESOURCE) ||
                  guid_equal(iid, IID_ID3D10BUFFER);
        break;
    case D3D10Kind::Texture: {
        const Texture2D& texture = m_textures.at(owner);
        const GuidBytes& own = texture.linear   ? IID_ID3D10TEXTURE1D
                               : texture.volume ? IID_ID3D10TEXTURE3D
                                                : IID_ID3D10TEXTURE2D;
        answers = answers || guid_equal(iid, IID_ID3D10RESOURCE) ||
                  guid_equal(iid, own);
        break;
    }
    case D3D10Kind::ShaderResourceView:
        answers = answers || guid_equal(iid, IID_ID3D10VIEW) ||
                  guid_equal(iid, IID_ID3D10SHADERRESOURCEVIEW) ||
                  guid_equal(iid, IID_ID3D10SHADERRESOURCEVIEW1);
        break;
    case D3D10Kind::RenderTargetView:
        answers = answers || guid_equal(iid, IID_ID3D10VIEW) ||
                  guid_equal(iid, IID_ID3D10RENDERTARGETVIEW);
        break;
    case D3D10Kind::DepthStencilView:
        answers = answers || guid_equal(iid, IID_ID3D10VIEW) ||
                  guid_equal(iid, IID_ID3D10DEPTHSTENCILVIEW);
        break;
    case D3D10Kind::BlendState:
        answers = answers || guid_equal(iid, IID_ID3D10BLENDSTATE) ||
                  guid_equal(iid, IID_ID3D10BLENDSTATE1);
        break;
    case D3D10Kind::DepthStencilState:
        answers = answers || guid_equal(iid, IID_ID3D10DEPTHSTENCILSTATE);
        break;
    case D3D10Kind::RasterizerState:
        answers = answers || guid_equal(iid, IID_ID3D10RASTERIZERSTATE);
        break;
    case D3D10Kind::SamplerState:
        answers = answers || guid_equal(iid, IID_ID3D10SAMPLERSTATE);
        break;
    case D3D10Kind::DeviceChild:
        answers =
            answers ||
            (m_input_layouts.contains(owner) &&
             guid_equal(iid, IID_ID3D10INPUTLAYOUT)) ||
            (m_vertex_shaders.contains(owner) &&
             guid_equal(iid, IID_ID3D10VERTEXSHADER)) ||
            (m_geometry_shaders.contains(owner) &&
             guid_equal(iid, IID_ID3D10GEOMETRYSHADER)) ||
            (m_pixel_shaders.contains(owner) &&
             guid_equal(iid, IID_ID3D10PIXELSHADER));
        break;
    case D3D10Kind::Query:
        answers = answers || guid_equal(iid, IID_ID3D10ASYNCHRONOUS) ||
                  guid_equal(iid, IID_ID3D10QUERY) ||
                  (m_queries.at(owner).predicate &&
                   guid_equal(iid, IID_ID3D10PREDICATE));
        break;
    }
    return answers ? d3d10_facet_of(owner) : 0;
}

// -----------------------------------------------------------------------------
// The device-child facets: views, states, shaders and queries
// -----------------------------------------------------------------------------

void D3D11Manager::impl_D3D10ShaderResourceViewMethod(Marshaler& m,
                                                      usize slot) {
    const GuestAddr owner = d3d10_owner_of(m.arg_ptr(0));
    if (!m_shader_resource_views.contains(owner) || owner == m.arg_ptr(0)) {
        m.set_return(slot == 1 || slot == 2 ? 0 : kEInvalidArg);
        return;
    }
    const MethodDispatch method = &D3D11Manager::impl_ShaderResourceViewMethod;
    switch (slot) {
    case 7:  // GetResource(ID3D10Resource**)
        (void)d3d10_forward(method, 7, owner, {m.arg(1)});
        d3d10_objects_out(m.arg_ptr(1), 1);
        m.set_return(0);
        return;
    case 8:  // GetDesc: D3D11's layout (the D3D10 dimensions are its first).
    case 9:  // GetDesc1: D3D10_SHADER_RESOURCE_VIEW_DESC1 is the same.
        (void)d3d10_forward(method, 8, owner, {m.arg(1)});
        m.set_return(0);
        return;
    default:
        d3d10_child_method(m, slot, owner, method);
        return;
    }
}

void D3D11Manager::impl_D3D10RenderTargetViewMethod(Marshaler& m,
                                                    usize slot) {
    const GuestAddr owner = d3d10_owner_of(m.arg_ptr(0));
    if (!m_render_target_views.contains(owner) || owner == m.arg_ptr(0)) {
        m.set_return(slot == 1 || slot == 2 ? 0 : kEInvalidArg);
        return;
    }
    const MethodDispatch method = &D3D11Manager::impl_RenderTargetViewMethod;
    switch (slot) {
    case 7:
        (void)d3d10_forward(method, 7, owner, {m.arg(1)});
        d3d10_objects_out(m.arg_ptr(1), 1);
        m.set_return(0);
        return;
    case 8:  // GetDesc: D3D11's layout.
        (void)d3d10_forward(method, 8, owner, {m.arg(1)});
        m.set_return(0);
        return;
    default:
        d3d10_child_method(m, slot, owner, method);
        return;
    }
}

void D3D11Manager::impl_D3D10DepthStencilViewMethod(Marshaler& m,
                                                    usize slot) {
    const GuestAddr owner = d3d10_owner_of(m.arg_ptr(0));
    if (!m_depth_stencil_views.contains(owner) || owner == m.arg_ptr(0)) {
        m.set_return(slot == 1 || slot == 2 ? 0 : kEInvalidArg);
        return;
    }
    const MethodDispatch method = &D3D11Manager::impl_DepthStencilViewMethod;
    switch (slot) {
    case 7:
        (void)d3d10_forward(method, 7, owner, {m.arg(1)});
        d3d10_objects_out(m.arg_ptr(1), 1);
        m.set_return(0);
        return;
    case 8: {
        // GetDesc: D3D11's description without its Flags word.
        const GuestAddr out = m.arg_ptr(1);
        if (out != 0) {
            const usize mark = d3d10_scratch_mark();
            const GuestAddr desc = d3d10_scratch(24);
            if (desc != 0) {
                (void)d3d10_forward(method, 8, owner, {desc});
                std::array<u32, 6> d3d11{};
                if (m_memory.read_bytes(desc,
                                        reinterpret_cast<u8*>(d3d11.data()),
                                        sizeof(d3d11)).is_ok()) {
                    const std::array<u32, 5> d3d10{d3d11[0], d3d11[1],
                                                   d3d11[3], d3d11[4],
                                                   d3d11[5]};
                    (void)m_memory.write_bytes(
                        out, reinterpret_cast<const u8*>(d3d10.data()),
                        sizeof(d3d10));
                }
            }
            d3d10_scratch_release(mark);
        }
        m.set_return(0);
        return;
    }
    default:
        d3d10_child_method(m, slot, owner, method);
        return;
    }
}

void D3D11Manager::impl_D3D10BlendStateMethod(Marshaler& m, usize slot) {
    const GuestAddr owner = d3d10_owner_of(m.arg_ptr(0));
    if (!m_blend_states.contains(owner) || owner == m.arg_ptr(0)) {
        m.set_return(slot == 1 || slot == 2 ? 0 : kEInvalidArg);
        return;
    }
    const MethodDispatch method = &D3D11Manager::impl_BlendStateMethod;
    switch (slot) {
    case 7: {
        // GetDesc(D3D10_BLEND_DESC*): from the D3D11 description -- each
        // target's enable and mask, and the function of the first enabled
        // target (measured: a state with none enabled reports the defaults
        // ONE / ZERO, whatever it was created with).
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(0);
            return;
        }
        const usize mark = d3d10_scratch_mark();
        const GuestAddr desc = d3d10_scratch(8 + 8 * 32);
        if (desc != 0) {
            (void)d3d10_forward(method, 7, owner, {desc});
            std::array<u8, 8 + 8 * 32> d3d11{};
            if (m_memory.read_bytes(desc, d3d11.data(), d3d11.size())
                    .is_ok()) {
                std::array<u8, 68> d3d10{};
                std::memcpy(d3d10.data(), d3d11.data(), 4);
                usize function_from = 8;
                for (usize rt = 0; rt < 8; ++rt) {
                    u32 enable = 0;
                    std::memcpy(&enable, d3d11.data() + 8 + rt * 32, 4);
                    if (enable != 0 && function_from == 8) function_from = rt;
                    std::memcpy(d3d10.data() + 4 + rt * 4, &enable, 4);
                    d3d10[60 + rt] = d3d11[8 + rt * 32 + 28];
                }
                if (function_from == 8) function_from = 0;
                std::memcpy(d3d10.data() + 36,
                            d3d11.data() + 8 + function_from * 32 + 4, 24);
                (void)m_memory.write_bytes(out, d3d10.data(), d3d10.size());
            }
        }
        d3d10_scratch_release(mark);
        m.set_return(0);
        return;
    }
    case 8:
        // GetDesc1(D3D10_BLEND_DESC1*): D3D11's D3D11_BLEND_DESC.
        (void)d3d10_forward(method, 7, owner, {m.arg(1)});
        m.set_return(0);
        return;
    default:
        d3d10_child_method(m, slot, owner, method);
        return;
    }
}

void D3D11Manager::impl_D3D10DepthStencilStateMethod(Marshaler& m,
                                                     usize slot) {
    const GuestAddr owner = d3d10_owner_of(m.arg_ptr(0));
    if (!m_depth_stencil_states.contains(owner) || owner == m.arg_ptr(0)) {
        m.set_return(slot == 1 || slot == 2 ? 0 : kEInvalidArg);
        return;
    }
    const MethodDispatch method = &D3D11Manager::impl_DepthStencilStateMethod;
    if (slot == 7) {  // GetDesc: D3D11's description.
        (void)d3d10_forward(method, 7, owner, {m.arg(1)});
        m.set_return(0);
        return;
    }
    d3d10_child_method(m, slot, owner, method);
}

void D3D11Manager::impl_D3D10RasterizerStateMethod(Marshaler& m,
                                                   usize slot) {
    const GuestAddr owner = d3d10_owner_of(m.arg_ptr(0));
    if (!m_rasterizer_states.contains(owner) || owner == m.arg_ptr(0)) {
        m.set_return(slot == 1 || slot == 2 ? 0 : kEInvalidArg);
        return;
    }
    const MethodDispatch method = &D3D11Manager::impl_RasterizerStateMethod;
    if (slot == 7) {  // GetDesc: D3D11's (base) description.
        (void)d3d10_forward(method, 7, owner, {m.arg(1)});
        m.set_return(0);
        return;
    }
    d3d10_child_method(m, slot, owner, method);
}

void D3D11Manager::impl_D3D10SamplerStateMethod(Marshaler& m, usize slot) {
    const GuestAddr owner = d3d10_owner_of(m.arg_ptr(0));
    if (!m_sampler_states.contains(owner) || owner == m.arg_ptr(0)) {
        m.set_return(slot == 1 || slot == 2 ? 0 : kEInvalidArg);
        return;
    }
    const MethodDispatch method = &D3D11Manager::impl_SamplerStateMethod;
    if (slot == 7) {  // GetDesc: D3D11's description.
        (void)d3d10_forward(method, 7, owner, {m.arg(1)});
        m.set_return(0);
        return;
    }
    d3d10_child_method(m, slot, owner, method);
}

void D3D11Manager::impl_D3D10DeviceChildMethod(Marshaler& m, usize slot) {
    // Input layouts and vertex / geometry / pixel shaders.
    const GuestAddr owner = d3d10_owner_of(m.arg_ptr(0));
    MethodDispatch method = nullptr;
    if (m_input_layouts.contains(owner)) {
        method = &D3D11Manager::impl_InputLayoutMethod;
    } else if (m_vertex_shaders.contains(owner)) {
        method = &D3D11Manager::impl_VertexShaderMethod;
    } else if (m_geometry_shaders.contains(owner)) {
        method = &D3D11Manager::impl_GeometryShaderMethod;
    } else if (m_pixel_shaders.contains(owner)) {
        method = &D3D11Manager::impl_PixelShaderMethod;
    }
    if (method == nullptr || owner == m.arg_ptr(0)) {
        m.set_return(slot == 1 || slot == 2 ? 0 : kEInvalidArg);
        return;
    }
    d3d10_child_method(m, slot, owner, method);
}

void D3D11Manager::impl_D3D10QueryMethod(Marshaler& m, usize slot) {
    const GuestAddr owner = d3d10_owner_of(m.arg_ptr(0));
    const auto query = m_queries.find(owner);
    if (query == m_queries.end() || owner == m.arg_ptr(0)) {
        m.set_return(slot == 1 || slot == 2 ? 0 : kEInvalidArg);
        return;
    }
    const MethodDispatch method = &D3D11Manager::impl_QueryMethod;
    const auto device = m_devices.find(query->second.device_owner);
    const GuestAddr context =
        device != m_devices.end() ? device->second.context_interface : 0;
    const bool statistics =
        query->second.query_type == kQueryPipelineStatistics;
    switch (slot) {
    case 7:  // Begin: the immediate context's Begin of the query.
    case 8:  // End
        if (context != 0) {
            (void)d3d10_forward(&D3D11Manager::impl_ContextMethod,
                                slot == 7 ? 27 : 28, context, {owner});
        }
        m.set_return(0);
        return;
    case 9: {
        // GetData(pData, DataSize, GetDataFlags). D3D10's pipeline
        // statistics are the first 64 bytes of D3D11's 88, and 64 is the
        // only size it takes (measured: 88 is E_INVALIDARG).
        if (context == 0) {
            m.set_return(kEInvalidArg);
            return;
        }
        const GuestAddr data = m.arg_ptr(1);
        const u32 size = m.arg32(2);
        if (!statistics || (data == 0 && size == 0)) {
            m.set_return(d3d10_forward(&D3D11Manager::impl_ContextMethod, 29,
                                       context,
                                       {owner, m.arg(1), m.arg(2), m.arg(3)}));
            return;
        }
        if (size != kD3D10PipelineStatisticsBytes || data == 0) {
            m.set_return(kEInvalidArg);
            return;
        }
        const usize mark = d3d10_scratch_mark();
        const GuestAddr full = d3d10_scratch(kD3D11PipelineStatisticsBytes);
        u32 hr = kEOutOfMemory;
        if (full != 0) {
            hr = static_cast<u32>(d3d10_forward(
                &D3D11Manager::impl_ContextMethod, 29, context,
                {owner, full, kD3D11PipelineStatisticsBytes, m.arg(3)}));
            if (hr == kSOk) {
                std::array<u8, kD3D10PipelineStatisticsBytes> bytes{};
                if (m_memory.read_bytes(full, bytes.data(), bytes.size())
                        .is_ok()) {
                    (void)m_memory.write_bytes(data, bytes.data(),
                                               bytes.size());
                }
            }
        }
        d3d10_scratch_release(mark);
        m.set_return(hr);
        return;
    }
    case 10:  // GetDataSize
        m.set_return(statistics ? kD3D10PipelineStatisticsBytes
                                : d3d10_forward(method, 7, owner, {}));
        return;
    case 11:  // GetDesc(D3D10_QUERY_DESC*): D3D11's layout.
        (void)d3d10_forward(method, 8, owner, {m.arg(1)});
        m.set_return(0);
        return;
    default:
        d3d10_child_method(m, slot, owner, method);
        return;
    }
}

} // namespace vitreo::winapi
