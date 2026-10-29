// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d11_swapchain_internal.hpp
/// @brief File-local DXGI swap-chain constants (DXGI_SWAP_EFFECT,
///        DXGI_SWAP_CHAIN_FLAG, DXGI_PRESENT, DXGI_MODE_ROTATION,
///        DXGI_COLOR_SPACE_TYPE, DXGI_HDR_METADATA_TYPE, ...), the
///        swap-effect and back-buffer-format predicates and the
///        DXGI_UNIMPLEMENTED_ONCE macro, shared by the d3d11_swapchain*.cpp
///        files. Everything sits in an anonymous namespace: each including
///        TU gets its own internal-linkage copy. Include from those files
///        only. Split out of d3d11_swapchain.cpp as verbatim code motion.

#include "winapi/graphics/d3d11/d3d11.hpp"

#include <cstdio>
#include <mutex>

namespace vitreo::winapi {

namespace {

// One stderr line per call site, the first time it is reached.
#define DXGI_UNIMPLEMENTED_ONCE(text)                                        \
    do {                                                                     \
        static std::once_flag dxgi_unimplemented_once_;                      \
        std::call_once(dxgi_unimplemented_once_, [] {                        \
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED " text "\n");       \
        });                                                                  \
    } while (0)

/// Offset of a swap chain's IDXGISwapChainMedia facet inside its object.
constexpr GuestAddr kSwapChainMediaFacet = 0x20;
// DXGI_SWAP_EFFECT (dxgi.h).
constexpr u32 kSwapEffectDiscard = 0;
constexpr u32 kSwapEffectSequential = 1;
constexpr u32 kSwapEffectFlipSequential = 3;
constexpr u32 kSwapEffectFlipDiscard = 4;
// DXGI_MAX_SWAP_CHAIN_BUFFERS (dxgi.h).
constexpr u32 kDxgiMaxSwapChainBuffers = 16;
// The Metal texture limit, which is also D3D11's
// D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION.
constexpr u32 kSwapChainMaxDimension = 16384;
// IDXGISwapChain2::SetMaximumFrameLatency: 17 is accepted and 100 refused
// on Windows 11 (measured); DXGI's own ceiling sits in between.
constexpr u32 kDxgiMaxFrameLatency = 17;
// D3D11_STANDARD_MULTISAMPLE_PATTERN / D3D11_CENTER_MULTISAMPLE_PATTERN.
constexpr u32 kStandardMultisamplePattern = 0xFFFFFFFFu;
constexpr u32 kCenterMultisamplePattern = 0xFFFFFFFEu;
// DXGI_USAGE_* / DXGI_CPU_ACCESS_FIELD (dxgi.h).
constexpr u32 kDxgiCpuAccessField = 0xFu;
constexpr u32 kDxgiUsageShared = 0x80u;
constexpr u32 kDxgiUsageReadOnly = 0x100u;
constexpr u32 kDxgiUsageDiscardOnPresent = 0x200u;
constexpr u32 kDxgiUsageUnorderedAccess = 0x400u;
// DXGI_SWAP_CHAIN_FLAG (dxgi.h).
constexpr u32 kSwapChainFlagAllowModeSwitch = 0x2u;
constexpr u32 kSwapChainFlagGdiCompatible = 0x4u;
constexpr u32 kSwapChainFlagRestrictedContent = 0x8u;
constexpr u32 kSwapChainFlagRestrictSharedResourceDriver = 0x10u;
constexpr u32 kSwapChainFlagFrameLatencyWaitableObject = 0x40u;
constexpr u32 kSwapChainFlagForegroundLayer = 0x80u;
constexpr u32 kSwapChainFlagAllowTearing = 0x800u;
constexpr u32 kSwapChainFlagRestrictedToAllHolographicDisplays = 0x1000u;
// The flags a chain of each model accepts at creation (measured;
// NONPREROTATED, DISPLAY_ONLY, FOREGROUND_LAYER, FULLSCREEN_VIDEO, YUV_VIDEO
// and HW_PROTECTED are refused by both, WAITABLE and ALLOW_TEARING by the
// bitblt model). RESTRICT_SHARED_RESOURCE_DRIVER is handled separately.
constexpr u32 kSwapChainFlagsBitblt =
    kSwapChainFlagAllowModeSwitch | kSwapChainFlagGdiCompatible |
    kSwapChainFlagRestrictedContent |
    kSwapChainFlagRestrictedToAllHolographicDisplays;
constexpr u32 kSwapChainFlagsFlip =
    kSwapChainFlagsBitblt | kSwapChainFlagFrameLatencyWaitableObject |
    kSwapChainFlagAllowTearing;
// DXGI_PRESENT (dxgi.h; Learn's DXGI_PRESENT page has the three stereo /
// restrict values permuted, the header is authoritative).
constexpr u32 kPresentTest = 0x1u;
constexpr u32 kPresentDoNotSequence = 0x2u;
constexpr u32 kPresentRestart = 0x4u;
constexpr u32 kPresentDoNotWait = 0x8u;
constexpr u32 kPresentStereoPreferRight = 0x10u;
constexpr u32 kPresentStereoTemporaryMono = 0x20u;
constexpr u32 kPresentRestrictToOutput = 0x40u;
constexpr u32 kPresentUseDuration = 0x100u;
constexpr u32 kPresentAllowTearing = 0x200u;
constexpr u32 kPresentFlagsDefined =
    kPresentTest | kPresentDoNotSequence | kPresentRestart |
    kPresentDoNotWait | kPresentStereoPreferRight |
    kPresentStereoTemporaryMono | kPresentRestrictToOutput |
    kPresentUseDuration | kPresentAllowTearing;
// DXGI_MODE_ROTATION (dxgitype.h).
constexpr u32 kDxgiRotationIdentity = 1;
constexpr u32 kDxgiRotationRotate270 = 4;
// DXGI_COLOR_SPACE_TYPE (dxgicommon.h).
constexpr u32 kDxgiColorSpaceRgbFullG22NoneP709 = 0;
constexpr u32 kDxgiColorSpaceRgbFullG10NoneP709 = 1;
constexpr u32 kDxgiColorSpaceRgbFullG2084NoneP2020 = 12;
constexpr u32 kDxgiColorSpaceRgbFullG22NoneP2020 = 17;
// DXGI_HDR_METADATA_TYPE and the structure sizes (dxgi1_5.h):
// DXGI_HDR_METADATA_HDR10 is 4x2 UINT16 chromaticities, two UINT
// luminances and two UINT16 light levels; HDR10PLUS is BYTE Data[72].
constexpr u32 kDxgiHdrMetadataNone = 0;
constexpr u32 kDxgiHdrMetadataHdr10 = 1;
constexpr u32 kDxgiHdrMetadataHdr10Plus = 2;
constexpr u32 kDxgiHdrMetadataHdr10Size = 28;
constexpr u32 kDxgiHdrMetadataHdr10PlusSize = 72;
constexpr u32 kDxgiFormatR16G16B16A16Float = 10;

[[nodiscard]] constexpr bool dxgi_flip_effect(u32 effect) {
    return effect == kSwapEffectFlipSequential ||
           effect == kSwapEffectFlipDiscard;
}
/// The effects whose buffers are presented in order and preserved.
[[nodiscard]] constexpr bool dxgi_sequential_effect(u32 effect) {
    return effect == kSwapEffectSequential ||
           effect == kSwapEffectFlipSequential;
}
/// The effects whose BufferCount is a ring the chain rotates.
[[nodiscard]] constexpr bool dxgi_ring_effect(u32 effect) {
    return effect == kSwapEffectSequential || dxgi_flip_effect(effect);
}
/// Back-buffer formats of a bitblt chain: R8G8B8A8_UNORM(_SRGB),
/// B8G8R8A8_UNORM(_SRGB), R10G10B10A2_UNORM, R16G16B16A16_FLOAT and
/// R10G10B10_XR_BIAS_A2_UNORM (measured; anything else E_INVALIDARG).
[[nodiscard]] constexpr bool dxgi_bitblt_swap_chain_format(u32 format) {
    return format == 28 || format == 29 || format == 87 || format == 91 ||
           format == 24 || format == 10 || format == 89;
}
/// Back-buffer formats of a flip chain (Learn DXGI_SWAP_EFFECT: no sRGB;
/// measured, anything else INVALID_CALL).
[[nodiscard]] constexpr bool dxgi_flip_swap_chain_format(u32 format) {
    return format == 28 || format == 87 || format == 24 || format == 10;
}
/// Learn DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE / D3D11_RESOURCE_MISC_GDI_
/// COMPATIBLE: a B8G8R8A8 surface (measured, RGBA is E_INVALIDARG).
[[nodiscard]] constexpr bool dxgi_gdi_compatible_format(u32 format) {
    return format == 87 || format == 91;
}

} // namespace

} // namespace vitreo::winapi
