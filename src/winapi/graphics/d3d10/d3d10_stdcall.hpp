// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d10_stdcall.hpp
/// @brief The D3D10 vtables: slot counts, x86 stdcall cleanup and method names.
///
/// Generated from the mingw-w64 SDK headers (the same IDL as the Windows SDK)
/// by the rule in tools/audit_d3d11_vtables.py, which checks every array here
/// in CI. Never hand-edit a number: a wrong entry corrupts the guest stack on
/// every x86 call through that slot without ever failing visibly.

#include <vitreo/common.hpp>

#include <array>

namespace vitreo::winapi::d3d10 {

/// ID3D10Device1: ID3D10Device is its first 98 slots, and one table serves both.
constexpr usize DEVICE_METHOD_COUNT = 101;
/// ID3D10Buffer.
constexpr usize BUFFER_METHOD_COUNT = 13;
/// ID3D10Texture1D / 2D / 3D: the same slots and the same stdcall cleanup (the
/// audit checks all three), so one table serves every dimension.
constexpr usize TEXTURE_METHOD_COUNT = 13;
/// ID3D10ShaderResourceView1 (the newest interface the view answers).
constexpr usize SHADER_RESOURCE_VIEW_METHOD_COUNT = 10;
/// ID3D10RenderTargetView.
constexpr usize RENDER_TARGET_VIEW_METHOD_COUNT = 9;
/// ID3D10DepthStencilView.
constexpr usize DEPTH_STENCIL_VIEW_METHOD_COUNT = 9;
/// ID3D10BlendState1 (every D3D10 blend state answers it, measured).
constexpr usize BLEND_STATE_METHOD_COUNT = 9;
/// ID3D10DepthStencilState.
constexpr usize DEPTH_STENCIL_STATE_METHOD_COUNT = 8;
/// ID3D10RasterizerState.
constexpr usize RASTERIZER_STATE_METHOD_COUNT = 8;
/// ID3D10SamplerState.
constexpr usize SAMPLER_STATE_METHOD_COUNT = 8;
/// ID3D10InputLayout / VertexShader / GeometryShader / PixelShader: no methods
/// beyond ID3D10DeviceChild, so one table serves all four.
constexpr usize DEVICE_CHILD_METHOD_COUNT = 7;
/// ID3D10Query / ID3D10Predicate.
constexpr usize QUERY_METHOD_COUNT = 12;
/// ID3D10Blob (= ID3DBlob).
constexpr usize BLOB_METHOD_COUNT = 5;
/// ID3D10StateBlock.
constexpr usize STATE_BLOCK_METHOD_COUNT = 7;

// ID3D10Device1Vtbl (d3d10_1.h), 101 slots.
inline constexpr std::array<u16, DEVICE_METHOD_COUNT>
    DEVICE_STDCALL_BYTES{{
    12, 4, 4, 16, 16, 8, 16, 8, 16, 12, 16, 8, 24, 16, 24, 20, 16, 8, 8,
    16, 16, 12, 16, 16, 16, 16, 12, 16, 4, 8, 12, 12, 36, 12, 28, 12, 20, 8,
    24, 16, 16, 8, 16, 8, 16, 8, 24, 16, 16, 8, 8, 16, 16, 12, 16, 16, 16,
    16, 12, 16, 8, 12, 12, 4, 8, 4, 16, 16, 12, 4, 4, 16, 16, 16, 16, 16,
    16, 16, 24, 16, 16, 28, 16, 12, 12, 12, 12, 12, 12, 12, 12, 16, 8, 40, 4,
    16, 12, 12, 16, 12, 4
}};
inline constexpr std::array<const char*, DEVICE_METHOD_COUNT>
    DEVICE_METHOD_NAMES{{
    "QueryInterface", "AddRef", "Release", "VSSetConstantBuffers",
    "PSSetShaderResources", "PSSetShader", "PSSetSamplers", "VSSetShader",
    "DrawIndexed", "Draw", "PSSetConstantBuffers", "IASetInputLayout",
    "IASetVertexBuffers", "IASetIndexBuffer", "DrawIndexedInstanced",
    "DrawInstanced", "GSSetConstantBuffers", "GSSetShader",
    "IASetPrimitiveTopology", "VSSetShaderResources", "VSSetSamplers",
    "SetPredication", "GSSetShaderResources", "GSSetSamplers",
    "OMSetRenderTargets", "OMSetBlendState", "OMSetDepthStencilState",
    "SOSetTargets", "DrawAuto", "RSSetState", "RSSetViewports",
    "RSSetScissorRects", "CopySubresourceRegion", "CopyResource",
    "UpdateSubresource", "ClearRenderTargetView", "ClearDepthStencilView",
    "GenerateMips", "ResolveSubresource", "VSGetConstantBuffers",
    "PSGetShaderResources", "PSGetShader", "PSGetSamplers", "VSGetShader",
    "PSGetConstantBuffers", "IAGetInputLayout", "IAGetVertexBuffers",
    "IAGetIndexBuffer", "GSGetConstantBuffers", "GSGetShader",
    "IAGetPrimitiveTopology", "VSGetShaderResources", "VSGetSamplers",
    "GetPredication", "GSGetShaderResources", "GSGetSamplers",
    "OMGetRenderTargets", "OMGetBlendState", "OMGetDepthStencilState",
    "SOGetTargets", "RSGetState", "RSGetViewports", "RSGetScissorRects",
    "GetDeviceRemovedReason", "SetExceptionMode", "GetExceptionMode",
    "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface",
    "ClearState", "Flush", "CreateBuffer", "CreateTexture1D",
    "CreateTexture2D", "CreateTexture3D", "CreateShaderResourceView",
    "CreateRenderTargetView", "CreateDepthStencilView", "CreateInputLayout",
    "CreateVertexShader", "CreateGeometryShader",
    "CreateGeometryShaderWithStreamOutput", "CreatePixelShader",
    "CreateBlendState", "CreateDepthStencilState", "CreateRasterizerState",
    "CreateSamplerState", "CreateQuery", "CreatePredicate", "CreateCounter",
    "CheckFormatSupport", "CheckMultisampleQualityLevels",
    "CheckCounterInfo", "CheckCounter", "GetCreationFlags",
    "OpenSharedResource", "SetTextFilterSize", "GetTextFilterSize",
    "CreateShaderResourceView1", "CreateBlendState1", "GetFeatureLevel"
}};

// ID3D10BufferVtbl (d3d10.h), 13 slots.
inline constexpr std::array<u16, BUFFER_METHOD_COUNT>
    BUFFER_STDCALL_BYTES{{
    12, 4, 4, 8, 16, 16, 12, 8, 8, 4, 16, 4, 8
}};
inline constexpr std::array<const char*, BUFFER_METHOD_COUNT>
    BUFFER_METHOD_NAMES{{
    "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData",
    "SetPrivateData", "SetPrivateDataInterface", "GetType",
    "SetEvictionPriority", "GetEvictionPriority", "Map", "Unmap", "GetDesc"
}};

// ID3D10Texture2DVtbl (d3d10.h), 13 slots.
inline constexpr std::array<u16, TEXTURE_METHOD_COUNT>
    TEXTURE_STDCALL_BYTES{{
    12, 4, 4, 8, 16, 16, 12, 8, 8, 4, 20, 8, 8
}};
inline constexpr std::array<const char*, TEXTURE_METHOD_COUNT>
    TEXTURE_METHOD_NAMES{{
    "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData",
    "SetPrivateData", "SetPrivateDataInterface", "GetType",
    "SetEvictionPriority", "GetEvictionPriority", "Map", "Unmap", "GetDesc"
}};

// ID3D10ShaderResourceView1Vtbl (d3d10_1.h), 10 slots.
inline constexpr std::array<u16, SHADER_RESOURCE_VIEW_METHOD_COUNT>
    SHADER_RESOURCE_VIEW_STDCALL_BYTES{{
    12, 4, 4, 8, 16, 16, 12, 8, 8, 8
}};
inline constexpr std::array<const char*, SHADER_RESOURCE_VIEW_METHOD_COUNT>
    SHADER_RESOURCE_VIEW_METHOD_NAMES{{
    "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData",
    "SetPrivateData", "SetPrivateDataInterface", "GetResource", "GetDesc",
    "GetDesc1"
}};

// ID3D10RenderTargetViewVtbl (d3d10.h), 9 slots.
inline constexpr std::array<u16, RENDER_TARGET_VIEW_METHOD_COUNT>
    RENDER_TARGET_VIEW_STDCALL_BYTES{{
    12, 4, 4, 8, 16, 16, 12, 8, 8
}};
inline constexpr std::array<const char*, RENDER_TARGET_VIEW_METHOD_COUNT>
    RENDER_TARGET_VIEW_METHOD_NAMES{{
    "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData",
    "SetPrivateData", "SetPrivateDataInterface", "GetResource", "GetDesc"
}};

// ID3D10DepthStencilViewVtbl (d3d10.h), 9 slots.
inline constexpr std::array<u16, DEPTH_STENCIL_VIEW_METHOD_COUNT>
    DEPTH_STENCIL_VIEW_STDCALL_BYTES{{
    12, 4, 4, 8, 16, 16, 12, 8, 8
}};
inline constexpr std::array<const char*, DEPTH_STENCIL_VIEW_METHOD_COUNT>
    DEPTH_STENCIL_VIEW_METHOD_NAMES{{
    "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData",
    "SetPrivateData", "SetPrivateDataInterface", "GetResource", "GetDesc"
}};

// ID3D10BlendState1Vtbl (d3d10_1.h), 9 slots.
inline constexpr std::array<u16, BLEND_STATE_METHOD_COUNT>
    BLEND_STATE_STDCALL_BYTES{{
    12, 4, 4, 8, 16, 16, 12, 8, 8
}};
inline constexpr std::array<const char*, BLEND_STATE_METHOD_COUNT>
    BLEND_STATE_METHOD_NAMES{{
    "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData",
    "SetPrivateData", "SetPrivateDataInterface", "GetDesc", "GetDesc1"
}};

// ID3D10DepthStencilStateVtbl (d3d10.h), 8 slots.
inline constexpr std::array<u16, DEPTH_STENCIL_STATE_METHOD_COUNT>
    DEPTH_STENCIL_STATE_STDCALL_BYTES{{
    12, 4, 4, 8, 16, 16, 12, 8
}};
inline constexpr std::array<const char*, DEPTH_STENCIL_STATE_METHOD_COUNT>
    DEPTH_STENCIL_STATE_METHOD_NAMES{{
    "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData",
    "SetPrivateData", "SetPrivateDataInterface", "GetDesc"
}};

// ID3D10RasterizerStateVtbl (d3d10.h), 8 slots.
inline constexpr std::array<u16, RASTERIZER_STATE_METHOD_COUNT>
    RASTERIZER_STATE_STDCALL_BYTES{{
    12, 4, 4, 8, 16, 16, 12, 8
}};
inline constexpr std::array<const char*, RASTERIZER_STATE_METHOD_COUNT>
    RASTERIZER_STATE_METHOD_NAMES{{
    "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData",
    "SetPrivateData", "SetPrivateDataInterface", "GetDesc"
}};

// ID3D10SamplerStateVtbl (d3d10.h), 8 slots.
inline constexpr std::array<u16, SAMPLER_STATE_METHOD_COUNT>
    SAMPLER_STATE_STDCALL_BYTES{{
    12, 4, 4, 8, 16, 16, 12, 8
}};
inline constexpr std::array<const char*, SAMPLER_STATE_METHOD_COUNT>
    SAMPLER_STATE_METHOD_NAMES{{
    "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData",
    "SetPrivateData", "SetPrivateDataInterface", "GetDesc"
}};

// ID3D10InputLayoutVtbl (d3d10.h), 7 slots.
inline constexpr std::array<u16, DEVICE_CHILD_METHOD_COUNT>
    DEVICE_CHILD_STDCALL_BYTES{{
    12, 4, 4, 8, 16, 16, 12
}};
inline constexpr std::array<const char*, DEVICE_CHILD_METHOD_COUNT>
    DEVICE_CHILD_METHOD_NAMES{{
    "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData",
    "SetPrivateData", "SetPrivateDataInterface"
}};

// ID3D10QueryVtbl (d3d10.h), 12 slots.
inline constexpr std::array<u16, QUERY_METHOD_COUNT>
    QUERY_STDCALL_BYTES{{
    12, 4, 4, 8, 16, 16, 12, 4, 4, 16, 4, 8
}};
inline constexpr std::array<const char*, QUERY_METHOD_COUNT>
    QUERY_METHOD_NAMES{{
    "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData",
    "SetPrivateData", "SetPrivateDataInterface", "Begin", "End", "GetData",
    "GetDataSize", "GetDesc"
}};

// ID3D10BlobVtbl (d3dcommon.h), 5 slots.
inline constexpr std::array<u16, BLOB_METHOD_COUNT>
    BLOB_STDCALL_BYTES{{
    12, 4, 4, 4, 4
}};
inline constexpr std::array<const char*, BLOB_METHOD_COUNT>
    BLOB_METHOD_NAMES{{
    "QueryInterface", "AddRef", "Release", "GetBufferPointer",
    "GetBufferSize"
}};

// ID3D10StateBlockVtbl (d3d10effect.h), 7 slots.
inline constexpr std::array<u16, STATE_BLOCK_METHOD_COUNT>
    STATE_BLOCK_STDCALL_BYTES{{
    12, 4, 4, 4, 4, 4, 8
}};
inline constexpr std::array<const char*, STATE_BLOCK_METHOD_COUNT>
    STATE_BLOCK_METHOD_NAMES{{
    "QueryInterface", "AddRef", "Release", "Capture", "Apply",
    "ReleaseAllDeviceObjects", "GetDevice"
}};

} // namespace vitreo::winapi::d3d10
