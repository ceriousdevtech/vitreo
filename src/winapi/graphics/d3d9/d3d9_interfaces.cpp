// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_interfaces.cpp
/// @brief D3D9 COM vtable layouts and x86 stdcall cleanup sizes.
///
/// One table per interface, in exact vtable order. The `stdcall_bytes` value
/// is `4 * (1 + argument_count)` for a 32-bit guest: every COM method is
/// stdcall and the callee pops the implicit `this` along with its arguments.
/// A wrong value here silently corrupts the guest stack on every call, so the
/// argument count of each method is spelled out in the entry above it.

#include "winapi/graphics/d3d9/d3d9.hpp"

#include <cstring>

namespace vitreo::winapi {
namespace {

/// 4 * (1 + args): `this` plus the declared arguments.
constexpr u16 pop(unsigned args) {
    return static_cast<u16>(4u * (1u + args));
}

// -----------------------------------------------------------------------------
// IUnknown, shared by every interface below at slots 0..2.
// -----------------------------------------------------------------------------
#define D3D9_IUNKNOWN_METHODS                                                 \
    {"QueryInterface", pop(2)}, /* REFIID, void** */                          \
    {"AddRef", pop(0)},                                                       \
    {"Release", pop(0)}

// -----------------------------------------------------------------------------
// IDirect3DResource9, shared by every resource interface at slots 3..10.
// -----------------------------------------------------------------------------
#define D3D9_IRESOURCE_METHODS                                                \
    {"GetDevice", pop(1)},        /* IDirect3DDevice9** */                    \
    {"SetPrivateData", pop(4)},   /* REFGUID, const void*, DWORD, DWORD */    \
    {"GetPrivateData", pop(3)},   /* REFGUID, void*, DWORD* */                \
    {"FreePrivateData", pop(1)},  /* REFGUID */                               \
    {"SetPriority", pop(1)},      /* DWORD */                                 \
    {"GetPriority", pop(0)},                                                  \
    {"PreLoad", pop(0)},                                                      \
    {"GetType", pop(0)}

constexpr D3D9MethodDesc DIRECT3D9_METHODS[] = {
    D3D9_IUNKNOWN_METHODS,
    {"RegisterSoftwareDevice", pop(1)},
    {"GetAdapterCount", pop(0)},
    {"GetAdapterIdentifier", pop(3)},
    {"GetAdapterModeCount", pop(2)},
    {"EnumAdapterModes", pop(4)},
    {"GetAdapterDisplayMode", pop(2)},
    {"CheckDeviceType", pop(5)},
    {"CheckDeviceFormat", pop(6)},
    {"CheckDeviceMultiSampleType", pop(6)},
    {"CheckDepthStencilMatch", pop(5)},
    {"CheckDeviceFormatConversion", pop(4)},
    {"GetDeviceCaps", pop(3)},
    {"GetAdapterMonitor", pop(1)},
    {"CreateDevice", pop(6)},
};

constexpr D3D9MethodDesc DEVICE_METHODS[] = {
    D3D9_IUNKNOWN_METHODS,
    {"TestCooperativeLevel", pop(0)},
    {"GetAvailableTextureMem", pop(0)},
    {"EvictManagedResources", pop(0)},
    {"GetDirect3D", pop(1)},
    {"GetDeviceCaps", pop(1)},
    {"GetDisplayMode", pop(2)},
    {"GetCreationParameters", pop(1)},
    {"SetCursorProperties", pop(3)},
    {"SetCursorPosition", pop(3)},
    {"ShowCursor", pop(1)},
    {"CreateAdditionalSwapChain", pop(2)},
    {"GetSwapChain", pop(2)},
    {"GetNumberOfSwapChains", pop(0)},
    {"Reset", pop(1)},
    {"Present", pop(4)},
    {"GetBackBuffer", pop(4)},
    {"GetRasterStatus", pop(2)},
    {"SetDialogBoxMode", pop(1)},
    {"SetGammaRamp", pop(3)},
    {"GetGammaRamp", pop(2)},
    {"CreateTexture", pop(8)},
    {"CreateVolumeTexture", pop(9)},
    {"CreateCubeTexture", pop(7)},
    {"CreateVertexBuffer", pop(6)},
    {"CreateIndexBuffer", pop(6)},
    {"CreateRenderTarget", pop(8)},
    {"CreateDepthStencilSurface", pop(8)},
    {"UpdateSurface", pop(4)},
    {"UpdateTexture", pop(2)},
    {"GetRenderTargetData", pop(2)},
    {"GetFrontBufferData", pop(2)},
    {"StretchRect", pop(5)},
    {"ColorFill", pop(3)},
    {"CreateOffscreenPlainSurface", pop(6)},
    {"SetRenderTarget", pop(2)},
    {"GetRenderTarget", pop(2)},
    {"SetDepthStencilSurface", pop(1)},
    {"GetDepthStencilSurface", pop(1)},
    {"BeginScene", pop(0)},
    {"EndScene", pop(0)},
    {"Clear", pop(6)},
    {"SetTransform", pop(2)},
    {"GetTransform", pop(2)},
    {"MultiplyTransform", pop(2)},
    {"SetViewport", pop(1)},
    {"GetViewport", pop(1)},
    {"SetMaterial", pop(1)},
    {"GetMaterial", pop(1)},
    {"SetLight", pop(2)},
    {"GetLight", pop(2)},
    {"LightEnable", pop(2)},
    {"GetLightEnable", pop(2)},
    {"SetClipPlane", pop(2)},
    {"GetClipPlane", pop(2)},
    {"SetRenderState", pop(2)},
    {"GetRenderState", pop(2)},
    {"CreateStateBlock", pop(2)},
    {"BeginStateBlock", pop(0)},
    {"EndStateBlock", pop(1)},
    {"SetClipStatus", pop(1)},
    {"GetClipStatus", pop(1)},
    {"GetTexture", pop(2)},
    {"SetTexture", pop(2)},
    {"GetTextureStageState", pop(3)},
    {"SetTextureStageState", pop(3)},
    {"GetSamplerState", pop(3)},
    {"SetSamplerState", pop(3)},
    {"ValidateDevice", pop(1)},
    {"SetPaletteEntries", pop(2)},
    {"GetPaletteEntries", pop(2)},
    {"SetCurrentTexturePalette", pop(1)},
    {"GetCurrentTexturePalette", pop(1)},
    {"SetScissorRect", pop(1)},
    {"GetScissorRect", pop(1)},
    {"SetSoftwareVertexProcessing", pop(1)},
    {"GetSoftwareVertexProcessing", pop(0)},
    {"SetNPatchMode", pop(1)},
    {"GetNPatchMode", pop(0)},
    {"DrawPrimitive", pop(3)},
    {"DrawIndexedPrimitive", pop(6)},
    {"DrawPrimitiveUP", pop(4)},
    {"DrawIndexedPrimitiveUP", pop(8)},
    {"ProcessVertices", pop(6)},
    {"CreateVertexDeclaration", pop(2)},
    {"SetVertexDeclaration", pop(1)},
    {"GetVertexDeclaration", pop(1)},
    {"SetFVF", pop(1)},
    {"GetFVF", pop(1)},
    {"CreateVertexShader", pop(2)},
    {"SetVertexShader", pop(1)},
    {"GetVertexShader", pop(1)},
    {"SetVertexShaderConstantF", pop(3)},
    {"GetVertexShaderConstantF", pop(3)},
    {"SetVertexShaderConstantI", pop(3)},
    {"GetVertexShaderConstantI", pop(3)},
    {"SetVertexShaderConstantB", pop(3)},
    {"GetVertexShaderConstantB", pop(3)},
    {"SetStreamSource", pop(4)},
    {"GetStreamSource", pop(4)},
    {"SetStreamSourceFreq", pop(2)},
    {"GetStreamSourceFreq", pop(2)},
    {"SetIndices", pop(1)},
    {"GetIndices", pop(1)},
    {"CreatePixelShader", pop(2)},
    {"SetPixelShader", pop(1)},
    {"GetPixelShader", pop(1)},
    {"SetPixelShaderConstantF", pop(3)},
    {"GetPixelShaderConstantF", pop(3)},
    {"SetPixelShaderConstantI", pop(3)},
    {"GetPixelShaderConstantI", pop(3)},
    {"SetPixelShaderConstantB", pop(3)},
    {"GetPixelShaderConstantB", pop(3)},
    {"DrawRectPatch", pop(3)},
    {"DrawTriPatch", pop(3)},
    {"DeletePatch", pop(1)},
    {"CreateQuery", pop(2)},
};

constexpr D3D9MethodDesc SWAP_CHAIN_METHODS[] = {
    D3D9_IUNKNOWN_METHODS,
    {"Present", pop(5)},
    {"GetFrontBufferData", pop(1)},
    {"GetBackBuffer", pop(3)},
    {"GetRasterStatus", pop(1)},
    {"GetDisplayMode", pop(1)},
    {"GetDevice", pop(1)},
    {"GetPresentParameters", pop(1)},
};

constexpr D3D9MethodDesc SURFACE_METHODS[] = {
    D3D9_IUNKNOWN_METHODS,
    D3D9_IRESOURCE_METHODS,
    {"GetContainer", pop(2)},
    {"GetDesc", pop(1)},
    {"LockRect", pop(3)},
    {"UnlockRect", pop(0)},
    {"GetDC", pop(1)},
    {"ReleaseDC", pop(1)},
};

constexpr D3D9MethodDesc TEXTURE_METHODS[] = {
    D3D9_IUNKNOWN_METHODS,
    D3D9_IRESOURCE_METHODS,
    // IDirect3DBaseTexture9
    {"SetLOD", pop(1)},
    {"GetLOD", pop(0)},
    {"GetLevelCount", pop(0)},
    {"SetAutoGenFilterType", pop(1)},
    {"GetAutoGenFilterType", pop(0)},
    {"GenerateMipSubLevels", pop(0)},
    // IDirect3DTexture9
    {"GetLevelDesc", pop(2)},
    {"GetSurfaceLevel", pop(2)},
    {"LockRect", pop(4)},
    {"UnlockRect", pop(1)},
    {"AddDirtyRect", pop(1)},
};

constexpr D3D9MethodDesc CUBE_TEXTURE_METHODS[] = {
    D3D9_IUNKNOWN_METHODS,
    D3D9_IRESOURCE_METHODS,
    // IDirect3DBaseTexture9
    {"SetLOD", pop(1)},
    {"GetLOD", pop(0)},
    {"GetLevelCount", pop(0)},
    {"SetAutoGenFilterType", pop(1)},
    {"GetAutoGenFilterType", pop(0)},
    {"GenerateMipSubLevels", pop(0)},
    // IDirect3DCubeTexture9
    {"GetLevelDesc", pop(2)},
    {"GetCubeMapSurface", pop(3)},
    {"LockRect", pop(5)},
    {"UnlockRect", pop(2)},
    {"AddDirtyRect", pop(2)},
};

constexpr D3D9MethodDesc VOLUME_TEXTURE_METHODS[] = {
    D3D9_IUNKNOWN_METHODS,
    D3D9_IRESOURCE_METHODS,
    // IDirect3DBaseTexture9
    {"SetLOD", pop(1)},
    {"GetLOD", pop(0)},
    {"GetLevelCount", pop(0)},
    {"SetAutoGenFilterType", pop(1)},
    {"GetAutoGenFilterType", pop(0)},
    {"GenerateMipSubLevels", pop(0)},
    // IDirect3DVolumeTexture9
    {"GetLevelDesc", pop(2)},
    {"GetVolumeLevel", pop(2)},
    {"LockBox", pop(4)},
    {"UnlockBox", pop(1)},
    {"AddDirtyBox", pop(1)},
};

// IDirect3DVolume9 is not an IDirect3DResource9: no pool/priority/type slots.
constexpr D3D9MethodDesc VOLUME_METHODS[] = {
    D3D9_IUNKNOWN_METHODS,
    {"GetDevice", pop(1)},
    {"SetPrivateData", pop(4)},
    {"GetPrivateData", pop(3)},
    {"FreePrivateData", pop(1)},
    {"GetContainer", pop(2)},
    {"GetDesc", pop(1)},
    {"LockBox", pop(3)},
    {"UnlockBox", pop(0)},
};

constexpr D3D9MethodDesc VERTEX_BUFFER_METHODS[] = {
    D3D9_IUNKNOWN_METHODS,
    D3D9_IRESOURCE_METHODS,
    {"Lock", pop(4)},
    {"Unlock", pop(0)},
    {"GetDesc", pop(1)},
};

constexpr D3D9MethodDesc INDEX_BUFFER_METHODS[] = {
    D3D9_IUNKNOWN_METHODS,
    D3D9_IRESOURCE_METHODS,
    {"Lock", pop(4)},
    {"Unlock", pop(0)},
    {"GetDesc", pop(1)},
};

constexpr D3D9MethodDesc VERTEX_SHADER_METHODS[] = {
    D3D9_IUNKNOWN_METHODS,
    {"GetDevice", pop(1)},
    {"GetFunction", pop(2)}, /* void*, UINT* */
};

constexpr D3D9MethodDesc PIXEL_SHADER_METHODS[] = {
    D3D9_IUNKNOWN_METHODS,
    {"GetDevice", pop(1)},
    {"GetFunction", pop(2)},
};

constexpr D3D9MethodDesc STATE_BLOCK_METHODS[] = {
    D3D9_IUNKNOWN_METHODS,
    {"GetDevice", pop(1)},
    {"Capture", pop(0)},
    {"Apply", pop(0)},
};

constexpr D3D9MethodDesc VERTEX_DECLARATION_METHODS[] = {
    D3D9_IUNKNOWN_METHODS,
    {"GetDevice", pop(1)},
    {"GetDeclaration", pop(2)},
};

constexpr D3D9MethodDesc QUERY_METHODS[] = {
    D3D9_IUNKNOWN_METHODS,
    {"GetDevice", pop(1)},
    {"GetType", pop(0)},
    {"GetDataSize", pop(0)},
    {"Issue", pop(1)},
    {"GetData", pop(3)},
};

#undef D3D9_IUNKNOWN_METHODS
#undef D3D9_IRESOURCE_METHODS

struct InterfaceDesc {
    const char* name;
    const D3D9MethodDesc* methods;
    usize count;
};

constexpr InterfaceDesc INTERFACES[D3D9_INTERFACE_COUNT] = {
    {"Direct3D9", DIRECT3D9_METHODS, std::size(DIRECT3D9_METHODS)},
    {"Device", DEVICE_METHODS, std::size(DEVICE_METHODS)},
    {"SwapChain", SWAP_CHAIN_METHODS, std::size(SWAP_CHAIN_METHODS)},
    {"Surface", SURFACE_METHODS, std::size(SURFACE_METHODS)},
    {"Texture", TEXTURE_METHODS, std::size(TEXTURE_METHODS)},
    {"VertexBuffer", VERTEX_BUFFER_METHODS, std::size(VERTEX_BUFFER_METHODS)},
    {"IndexBuffer", INDEX_BUFFER_METHODS, std::size(INDEX_BUFFER_METHODS)},
    {"VertexShader", VERTEX_SHADER_METHODS,
     std::size(VERTEX_SHADER_METHODS)},
    {"PixelShader", PIXEL_SHADER_METHODS, std::size(PIXEL_SHADER_METHODS)},
    {"StateBlock", STATE_BLOCK_METHODS, std::size(STATE_BLOCK_METHODS)},
    {"VertexDeclaration", VERTEX_DECLARATION_METHODS,
     std::size(VERTEX_DECLARATION_METHODS)},
    {"CubeTexture", CUBE_TEXTURE_METHODS, std::size(CUBE_TEXTURE_METHODS)},
    {"Query", QUERY_METHODS, std::size(QUERY_METHODS)},
    {"VolumeTexture", VOLUME_TEXTURE_METHODS,
     std::size(VOLUME_TEXTURE_METHODS)},
    {"Volume", VOLUME_METHODS, std::size(VOLUME_METHODS)},
};

// Vtable sizes are part of the ABI: a guest indexes the table by slot, so a
// missing or extra entry misdirects every later call.
static_assert(std::size(DIRECT3D9_METHODS) == 17,
              "IDirect3D9 has 17 vtable slots");
static_assert(std::size(DEVICE_METHODS) == 119,
              "IDirect3DDevice9 has 119 vtable slots");
static_assert(std::size(SWAP_CHAIN_METHODS) == 10,
              "IDirect3DSwapChain9 has 10 vtable slots");
static_assert(std::size(SURFACE_METHODS) == 17,
              "IDirect3DSurface9 has 17 vtable slots");
static_assert(std::size(TEXTURE_METHODS) == 22,
              "IDirect3DTexture9 has 22 vtable slots");
static_assert(std::size(VERTEX_BUFFER_METHODS) == 14,
              "IDirect3DVertexBuffer9 has 14 vtable slots");
static_assert(std::size(INDEX_BUFFER_METHODS) == 14,
              "IDirect3DIndexBuffer9 has 14 vtable slots");
static_assert(std::size(VERTEX_SHADER_METHODS) == 5,
              "IDirect3DVertexShader9 has 5 vtable slots");
static_assert(std::size(PIXEL_SHADER_METHODS) == 5,
              "IDirect3DPixelShader9 has 5 vtable slots");
static_assert(std::size(STATE_BLOCK_METHODS) == 6,
              "IDirect3DStateBlock9 has 6 vtable slots");
static_assert(std::size(VERTEX_DECLARATION_METHODS) == 5,
              "IDirect3DVertexDeclaration9 has 5 vtable slots");
static_assert(std::size(CUBE_TEXTURE_METHODS) == 22,
              "IDirect3DCubeTexture9 has 22 vtable slots");
static_assert(std::size(QUERY_METHODS) == 8,
              "IDirect3DQuery9 has 8 vtable slots");
static_assert(std::size(VOLUME_TEXTURE_METHODS) == 22,
              "IDirect3DVolumeTexture9 has 22 vtable slots");
static_assert(std::size(VOLUME_METHODS) == 11,
              "IDirect3DVolume9 has 11 vtable slots");
static_assert(std::size(DIRECT3D9_METHODS) == D3D9_METHOD_COUNT,
              "D3D9_METHOD_COUNT must match the IDirect3D9 table");

} // namespace

const D3D9MethodDesc* d3d9_interface_methods(D3D9InterfaceId iface,
                                             usize& count) noexcept {
    const auto index = static_cast<usize>(iface);
    if (index >= D3D9_INTERFACE_COUNT) {
        count = 0;
        return nullptr;
    }
    count = INTERFACES[index].count;
    return INTERFACES[index].methods;
}

usize d3d9_interface_method_count(D3D9InterfaceId iface) noexcept {
    usize count = 0;
    (void)d3d9_interface_methods(iface, count);
    return count;
}

const char* d3d9_interface_name(D3D9InterfaceId iface) noexcept {
    const auto index = static_cast<usize>(iface);
    return index < D3D9_INTERFACE_COUNT ? INTERFACES[index].name : "Invalid";
}

std::string d3d9_method_export_name(D3D9InterfaceId iface, usize slot) {
    usize count = 0;
    const D3D9MethodDesc* methods = d3d9_interface_methods(iface, count);
    if (methods == nullptr || slot >= count) return "_D3D9_Invalid";
    return std::string("_D3D9_") + d3d9_interface_name(iface) + "_" +
           methods[slot].name;
}

} // namespace vitreo::winapi
