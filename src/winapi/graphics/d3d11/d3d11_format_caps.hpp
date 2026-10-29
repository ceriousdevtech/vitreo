// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d11_format_caps.hpp
/// @brief The one per-DXGI_FORMAT capability table of the D3D11 frontend.
///
/// CheckFormatSupport, CheckFeatureSupport(FORMAT_SUPPORT / FORMAT_SUPPORT2),
/// CheckMultisampleQualityLevels and resource creation all answer from here,
/// so a device never reports a capability its creation path refuses or the
/// Metal backend cannot honour, nor refuses what it reports.
///
/// Every answer is the Windows ceiling -- the Windows (WARP) masks of
/// tools/oracle_probe/d3d11_a01_device_caps_probe64.expected, which are the
/// Learn "Format support for Direct3D Feature Level 11.0 hardware" required
/// bits plus WARP's optional ones, generated into d3d11_format_oracle.inc --
/// intersected with what this implementation can do on the running GPU.

#include <vitreo/common.hpp>

#include "graphics/contracts/format_converter.hpp"

#include <array>

namespace vitreo::winapi::d3d11 {

/// D3D11_FORMAT_SUPPORT (d3d11.h).
enum : u32 {
    FORMAT_SUPPORT_BUFFER = 0x1u,
    FORMAT_SUPPORT_IA_VERTEX_BUFFER = 0x2u,
    FORMAT_SUPPORT_IA_INDEX_BUFFER = 0x4u,
    FORMAT_SUPPORT_SO_BUFFER = 0x8u,
    FORMAT_SUPPORT_TEXTURE1D = 0x10u,
    FORMAT_SUPPORT_TEXTURE2D = 0x20u,
    FORMAT_SUPPORT_TEXTURE3D = 0x40u,
    FORMAT_SUPPORT_TEXTURECUBE = 0x80u,
    FORMAT_SUPPORT_SHADER_LOAD = 0x100u,
    FORMAT_SUPPORT_SHADER_SAMPLE = 0x200u,
    FORMAT_SUPPORT_SHADER_SAMPLE_COMPARISON = 0x400u,
    FORMAT_SUPPORT_SHADER_SAMPLE_MONO_TEXT = 0x800u,
    FORMAT_SUPPORT_MIP = 0x1000u,
    FORMAT_SUPPORT_MIP_AUTOGEN = 0x2000u,
    FORMAT_SUPPORT_RENDER_TARGET = 0x4000u,
    FORMAT_SUPPORT_BLENDABLE = 0x8000u,
    FORMAT_SUPPORT_DEPTH_STENCIL = 0x10000u,
    FORMAT_SUPPORT_CPU_LOCKABLE = 0x20000u,
    FORMAT_SUPPORT_MULTISAMPLE_RESOLVE = 0x40000u,
    FORMAT_SUPPORT_DISPLAY = 0x80000u,
    FORMAT_SUPPORT_CAST_WITHIN_BIT_LAYOUT = 0x100000u,
    FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET = 0x200000u,
    FORMAT_SUPPORT_MULTISAMPLE_LOAD = 0x400000u,
    FORMAT_SUPPORT_SHADER_GATHER = 0x800000u,
    FORMAT_SUPPORT_BACK_BUFFER_CAST = 0x1000000u,
    FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW = 0x2000000u,
    FORMAT_SUPPORT_SHADER_GATHER_COMPARISON = 0x4000000u,
    FORMAT_SUPPORT_DECODER_OUTPUT = 0x8000000u,
    FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT = 0x10000000u,
    FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT = 0x20000000u,
    FORMAT_SUPPORT_VIDEO_ENCODER = 0x40000000u,
};

/// D3D11_FORMAT_SUPPORT2 (d3d11.h).
enum : u32 {
    FORMAT_SUPPORT2_UAV_ATOMIC_ADD = 0x1u,
    FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS = 0x2u,
    FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE = 0x4u,
    FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE = 0x8u,
    FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX = 0x10u,
    FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX = 0x20u,
    FORMAT_SUPPORT2_UAV_TYPED_LOAD = 0x40u,
    FORMAT_SUPPORT2_UAV_TYPED_STORE = 0x80u,
    FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP = 0x100u,
    FORMAT_SUPPORT2_TILED = 0x200u,
    FORMAT_SUPPORT2_SHAREABLE = 0x400u,
    FORMAT_SUPPORT2_MULTIPLANE_OVERLAY = 0x4000u,
    FORMAT_SUPPORT2_DISPLAYABLE = 0x10000u,
};

/// The host-GPU facts the format answers depend on.
struct FormatCapsContext {
    u32 feature_level{0xB000u};
    /// MTLDevice supportsTextureSampleCount: for 2, 4, 8 and 16.
    bool sample_count_2{false};
    bool sample_count_4{false};
    bool sample_count_8{false};
    bool sample_count_16{false};
    /// MTLDevice supports32BitFloatFiltering.
    bool float32_filtering{false};
    /// Metal Feature Set Tables: an MSAA resolve of R32Float / RG32Float /
    /// RGBA32Float is Apple9+ only.
    bool float32_msaa_resolve{false};
    /// GraphicsBackendDevice::supports_shared_texture, per storage format:
    /// the formats whose textures have storage another device or process
    /// can open (FORMAT_SUPPORT2_SHAREABLE).
    std::array<bool, static_cast<usize>(graphics::PixelFormat::Count)>
        shared_storage{};
    /// GraphicsBackendDevice::supports_extended_sharing: every shape of a
    /// shareable format shares (D3D11_OPTIONS.ExtendedResourceSharing), so
    /// NTHANDLE takes mipmapped and array Texture2Ds.
    bool extended_sharing{false};
    /// D3D11_OPTIONS.OutputMergerLogicOp: the output merger's logic ops run
    /// (GraphicsBackendDevice::supports_framebuffer_fetch), on every
    /// renderable UINT format (D3D11.3 17.7): FORMAT_SUPPORT2
    /// OUTPUT_MERGER_LOGIC_OP.
    bool logic_ops{false};
    /// A device of the Direct3D 10.0 API (d3d10.dll's D3D10CreateDevice):
    /// the DXGI 1.0 formats and the D3D10 support bits only (measured; see
    /// windows_support in d3d11_format_caps.cpp).
    bool dxgi_1_0_formats_only{false};
    /// Tiled resources (D3D11_TILED_RESOURCES_TIER_1 from feature level
    /// 11_0): per DXGI format, whether a tiled 2D texture of it can be made
    /// with the standard tile shape (standard_tile_shape) on this GPU, single
    /// sample and 4x. FORMAT_SUPPORT2_TILED, tiled creation and
    /// CheckMultisampleQualityLevels1(TILED) answer from these.
    bool tiled_resources{false};
    std::array<bool, 192> tiled_format{};
    std::array<bool, 192> tiled_format_4x{};
};

/// The standard tile shape of a 64 KiB tile of a Texture2D of @p dxgi_format
/// with @p samples samples (1 or 4, the counts Tier 1 has), in texels
/// (D3D11.3 functional spec 5.9.3.1, "Texture2D and Texture2DArray
/// Subresources": 256x256 at 8 bits per element down to 64x64 at 128, BC
/// formats by their 4x4 blocks; measured WARP GetResourceTiling in
/// d3d11_tiled_probe64.expected [tiling]). False for any other count or a
/// format without a single element size.
[[nodiscard]] bool standard_tile_shape(u32 dxgi_format, u32 samples,
                                       u32& width, u32& height) noexcept;

struct FormatSupportAnswer {
    u32 hr{0x80004005u};
    u32 mask{0};
};

/// dxgiformat.h assigns 0..115, 130..132 and 189..191.
[[nodiscard]] bool dxgi_format_assigned(u32 dxgi_format) noexcept;

/// The Metal storage of a texture created with @p dxgi_format, or Unknown
/// when this implementation cannot create one. Unlike the view mapping
/// (pixel_format_from_dxgi) this covers formats that exist only as storage.
[[nodiscard]] graphics::PixelFormat texture_storage_format(
    u32 dxgi_format) noexcept;

/// ID3D11Device::CheckFormatSupport, and identically
/// CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT) (Learn: "same as
/// CheckFormatSupport"). E_FAIL for a format the device does not know.
[[nodiscard]] FormatSupportAnswer check_format_support(
    u32 dxgi_format, const FormatCapsContext& context) noexcept;

/// The CheckFormatSupport bits the Windows answer (the Windows mask) has
/// for @p dxgi_format that this implementation does not honour: a use a
/// Windows device supports and this one refuses, which callers report as an
/// implementation gap.
[[nodiscard]] u32 unhonoured_format_support(
    u32 dxgi_format, const FormatCapsContext& context) noexcept;

/// The FORMAT_SUPPORT2 mask Windows (Windows WARP, feature level 11_0)
/// reports for @p dxgi_format, before this implementation's limits.
[[nodiscard]] u32 windows_support2(u32 dxgi_format) noexcept;

/// CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2). E_FAIL with 0 when
/// the format has no SUPPORT2 capability (measured).
[[nodiscard]] FormatSupportAnswer check_format_support2(
    u32 dxgi_format, const FormatCapsContext& context) noexcept;

/// CheckMultisampleQualityLevels(1). Returns the HRESULT and writes the
/// level count; E_INVALIDARG leaves @p levels untouched.
[[nodiscard]] u32 check_multisample_quality_levels(
    u32 dxgi_format, u32 sample_count, const FormatCapsContext& context,
    u32& levels) noexcept;

/// Whether a shader can load from / store to a typed UAV of this host format
/// (Metal Tier-2 read/write set and the Apple7+ Write set).
[[nodiscard]] bool supports_typed_uav_load(
    graphics::PixelFormat pixel_format) noexcept;
[[nodiscard]] bool supports_typed_uav_store(
    graphics::PixelFormat pixel_format) noexcept;

/// Bytes in one element of a typed buffer view, 0 when the format cannot be
/// a typed buffer view.
[[nodiscard]] u32 typed_buffer_element_bytes(u32 dxgi_format) noexcept;

/// Whether @p dxgi_format has an alpha component: one whose dxgiformat.h
/// name carries an A field (A8, A2, A1, A4, A16, A32). A format without one
/// reads alpha 1 (D3D11.3 19.1.3.3), also for a load past a view.
[[nodiscard]] bool dxgi_format_has_alpha(u32 dxgi_format) noexcept;

/// The component mapping a shader view of @p view_format needs on top of
/// its Metal storage. B8G8R8X8 has no alpha (reads 1); B4G4R4A4 is stored
/// as ABGR4; a depth plane reads (d, 0, 0, 1) (D3D11.3 functional spec
/// §19.1: components a format lacks read 0, alpha 1).
enum class ViewSwizzle : u8 {
    Identity,
    OpaqueAlpha,
    Bgra4,
    DepthRed,
};
[[nodiscard]] ViewSwizzle view_swizzle_for(u32 view_format,
                                           bool depth_plane) noexcept;

/// Whether a render target of @p rtv_format stores no alpha: the X8 formats,
/// whose fourth byte is never written and whose destination alpha reads 1
/// when blending.
[[nodiscard]] bool render_target_has_no_alpha(u32 rtv_format) noexcept;

/// A texture descriptor as the creation rules see it: 1D and 3D fill the 2D
/// shape (height 1 / array 1, one sample).
struct TextureRuleDesc {
    u32 dimension{2};  // 1, 2 or 3
    u32 width{0};
    u32 height{1};
    u32 depth{1};
    u32 mip_levels{0};  // 0 = the full chain
    u32 array_size{1};
    u32 format{0};
    u32 sample_count{1};
    u32 sample_quality{0};
    u32 usage{0};
    u32 bind_flags{0};
    u32 cpu_access_flags{0};
    u32 misc_flags{0};
    bool initial_data{false};
    /// D3D11_TEXTURE_LAYOUT of CreateTexture2D1 / CreateTexture3D1 (0 for
    /// the base creators): any value, as WARP stores what it is given.
    u32 layout{0};
};

/// The CreateTexture1D/2D/3D descriptor rules (Learn D3D11_TEXTURE*_DESC,
/// D3D11_USAGE, D3D11_BIND_FLAG, D3D11_RESOURCE_MISC_FLAG,
/// DXGI_SAMPLE_DESC) as measured on Windows
/// (d3d11_a02_create_matrix_probe / d3d11_a02_create_contracts_probe), with
/// every format decision read from check_format_support. Returns nullptr when
/// the descriptor is legal, else the rule it breaks (for diagnostics); every
/// failure is E_INVALIDARG.
[[nodiscard]] const char* texture_desc_violation(
    const TextureRuleDesc& desc, const FormatCapsContext& context) noexcept;

struct BufferRuleDesc {
    u32 byte_width{0};
    u32 usage{0};
    u32 bind_flags{0};
    u32 cpu_access_flags{0};
    u32 misc_flags{0};
    u32 structure_byte_stride{0};
    bool initial_data{false};
    /// D3D11_FEATURE_DATA_D3D11_OPTIONS1.MapOnDefaultBuffers of the device
    /// (feature level 11_0 and up): DEFAULT buffers may take CPU access.
    bool map_on_default_buffers{false};
    /// The device has tiled resources (TILED and TILE_POOL are legal).
    bool tiled_resources{false};
};

/// The CreateBuffer descriptor rules (Learn D3D11_BUFFER_DESC, CreateBuffer,
/// measured on Windows). Returns S_OK, E_INVALIDARG, or E_OUTOFMEMORY for a
/// buffer larger than the 2 GiB D3D11 resource limit
/// (D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM = 2048).
[[nodiscard]] u32 buffer_desc_check(const BufferRuleDesc& desc,
                                    const char** violation) noexcept;

}  // namespace vitreo::winapi::d3d11
