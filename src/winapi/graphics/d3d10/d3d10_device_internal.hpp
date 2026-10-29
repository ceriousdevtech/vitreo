// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d10_device_internal.hpp
/// @brief File-local constants and helpers shared by the files split out of
///        d3d10_device.cpp (the ID3D10Device1 dispatcher, device creation,
///        the Create* methods, forwarding, the facets, blobs and state
///        blocks). They stay in an anonymous namespace: each including TU
///        gets its own internal-linkage copy, as the one TU had before.
///        Include from those d3d10_*.cpp files only.

#include "winapi/graphics/d3d11/d3d11.hpp"

namespace vitreo::winapi {
namespace {

// Not every including file calls every function below; [[maybe_unused]]
// keeps those files free of -Wunused-function without changing codegen.

constexpr u32 kSOk = 0x00000000u;
constexpr u32 kEFail = 0x80004005u;
constexpr u32 kEInvalidArg = 0x80070057u;
constexpr u32 kEOutOfMemory = 0x8007000Eu;
constexpr u32 kENoInterface = 0x80004002u;
constexpr u32 kEPointer = 0x80004003u;
constexpr u32 kDxgiErrorUnsupported = 0x887A0004u;

/// D3D10_RESOURCE_MISC_FLAG (d3d10.h) and the D3D11 bits they become.
/// SHARED_KEYEDMUTEX (0x10) is refused by the D3D10 runtime (measured:
/// E_INVALIDARG for every format and bind, where a D3D11 device of the same
/// feature level accepts D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX), and so is
/// every bit D3D10 does not define.
constexpr u32 kD3D10MiscGenerateMips = 0x1u;
constexpr u32 kD3D10MiscShared = 0x2u;
constexpr u32 kD3D10MiscTextureCube = 0x4u;
constexpr u32 kD3D10MiscSharedKeyedMutex = 0x10u;
constexpr u32 kD3D10MiscGdiCompatible = 0x20u;
constexpr u32 kD3D11MiscSharedKeyedMutex = 0x100u;
constexpr u32 kD3D11MiscGdiCompatible = 0x200u;
/// D3D10_BIND_FLAG: VERTEX_BUFFER (0x1) .. DEPTH_STENCIL (0x40). D3D11's
/// UNORDERED_ACCESS and the video binds are E_INVALIDARG (measured).
constexpr u32 kD3D10BindMask = 0x7Fu;

[[maybe_unused]]
[[nodiscard]] bool misc_to_d3d11(u32 d3d10, u32& d3d11) {
    constexpr u32 accepted = kD3D10MiscGenerateMips | kD3D10MiscShared |
                             kD3D10MiscTextureCube | kD3D10MiscGdiCompatible;
    if ((d3d10 & ~accepted) != 0) return false;
    d3d11 = (d3d10 & (kD3D10MiscGenerateMips | kD3D10MiscShared |
                      kD3D10MiscTextureCube)) |
            ((d3d10 & kD3D10MiscGdiCompatible) != 0 ? kD3D11MiscGdiCompatible
                                                    : 0u);
    return true;
}

[[maybe_unused]]
[[nodiscard]] u32 misc_to_d3d10(u32 d3d11) {
    return (d3d11 & (kD3D10MiscGenerateMips | kD3D10MiscShared |
                     kD3D10MiscTextureCube)) |
           ((d3d11 & kD3D11MiscSharedKeyedMutex) != 0
                ? kD3D10MiscSharedKeyedMutex
                : 0u) |
           ((d3d11 & kD3D11MiscGdiCompatible) != 0 ? kD3D10MiscGdiCompatible
                                                   : 0u);
}

} // namespace
} // namespace vitreo::winapi
