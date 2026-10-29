// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d12_layout.hpp
/// @brief Direct3D 12 resource layout: D3D12_RESOURCE_DESC validation,
/// ID3D12Device::GetCopyableFootprints / ID3D12Device8::GetCopyableFootprints1,
/// GetResourceAllocationInfo / 1 / 2 / 3 and GetResourceTiling.
///
/// Everything here is a pure function of a description and a LayoutCaps (the
/// device facts the rules depend on), plus guest-struct readers and writers
/// for the SDK layouts. There is no Metal dependency: the one backend fact a
/// texture's allocation size needs -- what a placement heap needs to hold it
/// -- comes in through LayoutCaps::texture_heap_requirement.
///
/// Sources: Learn GetCopyableFootprints, GetResourceAllocationInfo,
/// D3D12_RESOURCE_DESC (the alignment, dimension and flag rules),
/// D3D12_RESOURCE_FLAGS, D3D12_TEXTURE_LAYOUT, GetResourceTiling,
/// D3D12_SUBRESOURCE_TILING; SDK 10.0.26100.0 d3d12.h for every constant and
/// layout. Everything Learn leaves unstated is measured on Windows (WARP12)
/// by tools/oracle_probe/d3d12_layout_probe.c, whose .expected files the
/// tests replay line by line.

#include <vitreo/common.hpp>

#include <functional>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {
class Marshaler;
}

namespace vitreo::winapi::d3d12 {

// d3d12.h (SDK 10.0.26100.0) constants.
inline constexpr u32 TEXTURE_DATA_PITCH_ALIGNMENT = 256;
inline constexpr u32 TEXTURE_DATA_PLACEMENT_ALIGNMENT = 512;
inline constexpr u64 SMALL_RESOURCE_PLACEMENT_ALIGNMENT = 4096;
inline constexpr u64 DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT = 65536;
inline constexpr u64 SMALL_MSAA_RESOURCE_PLACEMENT_ALIGNMENT = 65536;
inline constexpr u64 DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT = 4194304;
inline constexpr u32 REQ_TEXTURE1D_U_DIMENSION = 16384;
inline constexpr u32 REQ_TEXTURE1D_ARRAY_AXIS_DIMENSION = 2048;
inline constexpr u32 REQ_TEXTURE2D_U_OR_V_DIMENSION = 16384;
inline constexpr u32 REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION = 2048;
inline constexpr u32 REQ_TEXTURE3D_U_V_OR_W_DIMENSION = 2048;
inline constexpr u32 TILED_RESOURCE_TILE_SIZE_IN_BYTES = 65536;
/// D3D12_PACKED_TILE: StartTileIndexInOverallResource of a packed mip.
inline constexpr u32 PACKED_TILE = 0xFFFFFFFFu;
/// D3D12_STANDARD_MULTISAMPLE_PATTERN / D3D12_CENTER_MULTISAMPLE_PATTERN.
inline constexpr u32 STANDARD_MULTISAMPLE_PATTERN = 0xFFFFFFFFu;
inline constexpr u32 CENTER_MULTISAMPLE_PATTERN = 0xFFFFFFFEu;

/// D3D12_RESOURCE_DIMENSION.
enum : u32 {
    RESOURCE_DIMENSION_UNKNOWN = 0,
    RESOURCE_DIMENSION_BUFFER = 1,
    RESOURCE_DIMENSION_TEXTURE1D = 2,
    RESOURCE_DIMENSION_TEXTURE2D = 3,
    RESOURCE_DIMENSION_TEXTURE3D = 4,
};

/// D3D12_TEXTURE_LAYOUT.
enum : u32 {
    TEXTURE_LAYOUT_UNKNOWN = 0,
    TEXTURE_LAYOUT_ROW_MAJOR = 1,
    TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE = 2,
    TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE = 3,
};

/// D3D12_RESOURCE_FLAGS.
enum : u32 {
    RESOURCE_FLAG_ALLOW_RENDER_TARGET = 0x1u,
    RESOURCE_FLAG_ALLOW_DEPTH_STENCIL = 0x2u,
    RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS = 0x4u,
    RESOURCE_FLAG_DENY_SHADER_RESOURCE = 0x8u,
    RESOURCE_FLAG_ALLOW_CROSS_ADAPTER = 0x10u,
    RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS = 0x20u,
    RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY = 0x40u,
    RESOURCE_FLAG_VIDEO_ENCODE_REFERENCE_ONLY = 0x80u,
    RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE = 0x100u,
    /// Every flag d3d12.h (26100) defines.
    RESOURCE_FLAGS_DEFINED = 0x1FFu,
};

/// D3D12_FORMAT_SUPPORT1 bits the layout rules read.
enum : u32 {
    FORMAT_SUPPORT1_TEXTURE1D = 0x10u,
    FORMAT_SUPPORT1_TEXTURE2D = 0x20u,
    FORMAT_SUPPORT1_TEXTURE3D = 0x40u,
    FORMAT_SUPPORT1_MIP = 0x1000u,
    FORMAT_SUPPORT1_RENDER_TARGET = 0x4000u,
    FORMAT_SUPPORT1_DEPTH_STENCIL = 0x10000u,
    FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW = 0x2000000u,
};

/// D3D12_RESOURCE_DESC1 (the D3D12_RESOURCE_DESC fields plus
/// SamplerFeedbackMipRegion, which is zero for a plain DESC).
struct ResourceDesc {
    u32 dimension{0};
    u64 alignment{0};
    u64 width{0};
    u32 height{0};
    u16 depth_or_array_size{0};
    u16 mip_levels{0};
    u32 format{0};
    u32 sample_count{0};
    u32 sample_quality{0};
    u32 layout{0};
    u32 flags{0};
    u32 mip_region_width{0};
    u32 mip_region_height{0};
    u32 mip_region_depth{0};
};

/// Guest sizes (identical on x86 and x64: UINT64 aligns to 8 on both).
inline constexpr usize RESOURCE_DESC_SIZE = 56;
inline constexpr usize RESOURCE_DESC1_SIZE = 64;
inline constexpr usize PLACED_SUBRESOURCE_FOOTPRINT_SIZE = 32;
inline constexpr usize RESOURCE_ALLOCATION_INFO_SIZE = 16;
inline constexpr usize RESOURCE_ALLOCATION_INFO1_SIZE = 24;
inline constexpr usize PACKED_MIP_INFO_SIZE = 12;
inline constexpr usize TILE_SHAPE_SIZE = 12;
inline constexpr usize SUBRESOURCE_TILING_SIZE = 12;

/// Decode a D3D12_RESOURCE_DESC (desc1 = false) or D3D12_RESOURCE_DESC1 from
/// its guest bytes.
[[nodiscard]] ResourceDesc decode_resource_desc(const u8* bytes, bool desc1) noexcept;
/// Encode for ID3D12Resource::GetDesc / GetDesc1.
void encode_resource_desc(const ResourceDesc& desc, bool desc1, u8* bytes) noexcept;
[[nodiscard]] bool read_resource_desc(const GuestMemory& memory, GuestAddr address,
                                      bool desc1, ResourceDesc& desc);

/// What a Metal placement heap needs to hold a texture (MTLDevice
/// heapTextureSizeAndAlignWithDescriptor:). `supported` false when the
/// backend cannot make the texture at all.
struct TextureHeapRequirement {
    bool supported{false};
    u64 size{0};
    u64 alignment{0};
};

/// The device facts the layout rules depend on.
struct LayoutCaps {
    /// D3D12_FORMAT_SUPPORT1 of this device for a DXGI format; 0 when the
    /// device cannot use the format at all.
    std::function<u32(u32 dxgi_format)> format_support1;
    /// NumQualityLevels this device reports for (format, sample count),
    /// flags NONE; 0 for an unsupported count.
    std::function<u32(u32 dxgi_format, u32 sample_count)> quality_levels;
    /// The backend requirement for a texture of @p desc whose mip count is
    /// @p mip_levels (MipLevels 0 already resolved). SizeInBytes is this
    /// size rounded up to the D3D12 alignment, so a placed resource at any
    /// legal offset of a heap sized from it fits the Metal placement heap;
    /// a backend alignment above the D3D12 one fails the description.
    /// Empty (tests, no backend): the tight data size of every subresource
    /// times the sample count.
    std::function<TextureHeapRequirement(const ResourceDesc& desc, u32 mip_levels)>
        texture_heap_requirement;
    /// The largest buffer Width GetResourceAllocationInfo sizes; a wider one
    /// reports the measured "too large" SizeInBytes (see
    /// BUFFER_TOO_LARGE_SIZE). WARP x64's limit is 2^32 - 4.
    u64 max_buffer_width{0xFFFFFFFCull};
    /// The nodes that exist (visibleMask must be 0 or a subset).
    u32 node_mask{1};
    /// D3D12_OPTIONS12.RelaxedFormatCastingSupported: castable formats in
    /// GetResourceAllocationInfo3.
    bool relaxed_format_casting{false};
    /// D3D12_OPTIONS8.UnalignedBlockTexturesSupported: a block-compressed
    /// texture's top level may be any size (WARP: TRUE, and 37x19 BC1 is
    /// accepted); without it both dimensions are multiples of the block.
    bool unaligned_block_textures{false};
};

/// SizeInBytes WARP returns for a buffer wider than the device's limit
/// (measured on both bitnesses: UINT64_MAX rounded down to 64 KiB, with the
/// usual 64 KiB alignment), which differs from the UINT64_MAX of an invalid
/// description.
inline constexpr u64 BUFFER_TOO_LARGE_SIZE = 0xFFFFFFFFFFFF0000ull;

/// The D3D12_RESOURCE_DESC rules GetCopyableFootprints and
/// GetResourceAllocationInfo apply (measured: MipLevels beyond the full chain
/// and a 2D Width up to 2^23 are accepted, SampleDesc.Quality is checked only
/// for MSAA, a usage flag needs the usage in the format's cast family).
/// nullptr when valid, else the rule broken (diagnostics only).
[[nodiscard]] const char* resource_desc_violation(const ResourceDesc& desc,
                                                  const LayoutCaps& caps) noexcept;

/// MipLevels with 0 resolved to the full chain of the dimension.
[[nodiscard]] u32 resolved_mip_levels(const ResourceDesc& desc) noexcept;

/// FORMAT_INFO PlaneCount: 0 for a format the runtime does not know
/// (CheckFeatureSupport(FORMAT_INFO) fails with E_INVALIDARG).
[[nodiscard]] u32 format_plane_count(u32 dxgi_format) noexcept;

/// Subresources of a valid description: mips x array x planes.
[[nodiscard]] u64 subresource_count(const ResourceDesc& desc) noexcept;

/// Whether two formats share one cast family (the same TYPELESS parent in
/// dxgiformat.h).
[[nodiscard]] bool same_cast_family(u32 a, u32 b) noexcept;

/// Bytes of one element (texel, or block of a block-compressed / packed
/// format) of a single-plane format; 0 for planar and unknown formats.
[[nodiscard]] u32 format_element_bytes(u32 dxgi_format) noexcept;

// ---- GetCopyableFootprints --------------------------------------------------

struct Footprint {
    u64 offset{0};
    u32 format{0};
    u32 width{0};
    u32 height{0};
    u32 depth{0};
    u32 row_pitch{0};
    u32 num_rows{0};
    u64 row_size{0};
};

/// GetCopyableFootprints for subresources [first, first + num). Returns false
/// for an invalid description or range (the caller then fills every output
/// with all-ones and pTotalBytes with UINT64_MAX, as Windows does). On
/// success calls @p each (when set) once per subresource in order and stores
/// the total. Offsets and the total follow the runtime's 32-bit running
/// arithmetic (measured: a 4 GiB subresource wraps them).
bool copyable_footprints(const ResourceDesc& desc, u32 first, u32 num, u64 base_offset,
                         const LayoutCaps& caps,
                         const std::function<void(u32 index, const Footprint&)>& each,
                         u64& total_bytes);

/// ID3D12Device::GetCopyableFootprints (desc1 false) /
/// ID3D12Device8::GetCopyableFootprints1 (desc1 true) on guest memory: every
/// output pointer optional. A NULL pResourceDesc leaves the outputs alone.
void guest_get_copyable_footprints(GuestMemory& memory, const LayoutCaps& caps, bool desc1,
                                   GuestAddr desc_address, u32 first, u32 num,
                                   u64 base_offset, GuestAddr layouts, GuestAddr num_rows,
                                   GuestAddr row_sizes, GuestAddr total_bytes);

// ---- GetResourceAllocationInfo ----------------------------------------------

struct AllocationInfo {
    u64 size{0};
    u64 alignment{0};
};

struct AllocationInfo1 {
    u64 offset{0};
    u64 alignment{0};
    u64 size{0};
};

/// Castable formats of one description (GetResourceAllocationInfo3).
struct CastableFormats {
    std::vector<u32> formats;
};

/// GetResourceAllocationInfo / 1 / 2 / 3 over @p descs. @p castable is empty
/// or one entry per description. On success @p per_resource (when set) gets
/// one entry per description; on failure it is untouched (measured) and the
/// result is {UINT64_MAX, 64 KiB}. No descriptions: {0, 1}.
[[nodiscard]] AllocationInfo resource_allocation_info(
    u32 visible_mask, const std::vector<ResourceDesc>& descs,
    const std::vector<CastableFormats>& castable, const LayoutCaps& caps,
    std::vector<AllocationInfo1>* per_resource);

/// The guest methods. `variant` 0 = GetResourceAllocationInfo (DESC, no
/// INFO1), 1 = GetResourceAllocationInfo1 (DESC), 2 = ...2 (DESC1), 3 = ...3
/// (DESC1 + castable formats). The struct return goes to @p ret_address (the
/// hidden return pointer); the caller returns that pointer.
void guest_get_resource_allocation_info(GuestMemory& memory, const LayoutCaps& caps,
                                        bool is_64bit, u32 variant, GuestAddr ret_address,
                                        u32 visible_mask, u32 num_descs,
                                        GuestAddr descs_address,
                                        GuestAddr num_castable_address,
                                        GuestAddr castable_address,
                                        GuestAddr info1_address);

// ---- GetResourceTiling ------------------------------------------------------

/// How a reserved resource is tiled: the tile shape of its non-packed mips,
/// how many mips are standard (whole tiles), and the tiles its packed mips
/// take per array slice.
struct TiledLayout {
    u32 tile_width{0};
    u32 tile_height{0};
    u32 tile_depth{0};
    u32 standard_mips{0};
    u32 packed_tiles{0};
};

/// The D3D12 standard tile shape (D3D11.3 functional spec 5.9.3.1 table, the
/// shapes GetResourceTiling reports; measured WARP in d3d12_layout_probe
/// [tiling]) for a buffer, a Texture2D of any sample count 1/2/4/8/16, or a
/// Texture3D. False for a format without a single element size (planar,
/// 96-bit, unknown) or another sample count.
[[nodiscard]] bool standard_tile_shape(const ResourceDesc& desc, u32& width, u32& height,
                                       u32& depth) noexcept;

/// The layout WARP uses (measured): standard shapes, no packed mips.
[[nodiscard]] bool standard_tiled_layout(const ResourceDesc& desc, TiledLayout& layout) noexcept;

struct SubresourceTiling {
    u32 width_in_tiles{0};
    u16 height_in_tiles{0};
    u16 depth_in_tiles{0};
    u32 start_tile_index{0};
};

struct ResourceTiling {
    u32 total_tiles{0};
    u8 num_standard_mips{0};
    u8 num_packed_mips{0};
    u32 packed_tiles{0};
    u32 packed_start_tile{0};
    u32 shape_width{0};
    u32 shape_height{0};
    u32 shape_depth{0};
};

/// GetResourceTiling for a reserved resource: the whole-resource answer, and
/// the subresource tiling of subresource @p index (which may lie past the
/// last array slice: Windows extrapolates, measured).
[[nodiscard]] ResourceTiling resource_tiling(const ResourceDesc& desc, const TiledLayout& layout);
[[nodiscard]] SubresourceTiling subresource_tiling(const ResourceDesc& desc,
                                                   const TiledLayout& layout, u32 index);

/// ID3D12Device::GetResourceTiling on guest memory for a resource whose
/// description is @p desc. @p reserved false (a committed or placed resource)
/// writes the measured all-zero answer. Every output pointer optional;
/// *pNumSubresourceTilings is in/out.
void guest_get_resource_tiling(GuestMemory& memory, const ResourceDesc& desc, bool reserved,
                               const TiledLayout& layout, GuestAddr num_tiles,
                               GuestAddr packed_mip_desc, GuestAddr tile_shape,
                               GuestAddr num_subresource_tilings, u32 first_subresource,
                               GuestAddr subresource_tilings);

// ---- Vtable entry points -------------------------------------------------
//
// The whole guest call, arguments read from the Marshaler as the SDK
// declares them (argument 0 is `this`; a UINT64 takes two x86 slots), for
// the device dispatch to call from its slot:
//   ID3D12Device slot 25 GetResourceAllocationInfo       -> variant 0
//   ID3D12Device4 GetResourceAllocationInfo1              -> variant 1
//   ID3D12Device8 GetResourceAllocationInfo2              -> variant 2
//   ID3D12Device12 GetResourceAllocationInfo3             -> variant 3
//   ID3D12Device slot 38 GetCopyableFootprints            -> desc1 false
//   ID3D12Device8 GetCopyableFootprints1                  -> desc1 true
//   ID3D12Device slot 42 GetResourceTiling (the caller resolves
//     pTiledResource to its description and tiled layout first)

/// (This, RetVal, visibleMask, numResourceDescs, pResourceDescs[,
/// pResourceAllocationInfo1 | pNumCastableFormats, ppCastableFormats,
/// pResourceAllocationInfo1]); returns RetVal (the SDK's C binding of the
/// struct return on both bitnesses).
void marshal_get_resource_allocation_info(Marshaler& m, GuestMemory& memory,
                                          const LayoutCaps& caps, bool is_64bit, u32 variant);

/// (This, pResourceDesc, FirstSubresource, NumSubresources, UINT64
/// BaseOffset, pLayouts, pNumRows, pRowSizeInBytes, pTotalBytes); void.
void marshal_get_copyable_footprints(Marshaler& m, GuestMemory& memory, const LayoutCaps& caps,
                                     bool is_64bit, bool desc1);

/// (This, pTiledResource, pNumTilesForEntireResource, pPackedMipDesc,
/// pStandardTileShapeForNonPackedMips, pNumSubresourceTilings,
/// FirstSubresourceTilingToGet, pSubresourceTilingsForNonPackedMips); void.
void marshal_get_resource_tiling(Marshaler& m, GuestMemory& memory, const ResourceDesc& desc,
                                 bool reserved, const TiledLayout& layout);

}  // namespace vitreo::winapi::d3d12
