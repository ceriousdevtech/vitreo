// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/graphics/d3d12/d3d12_layout.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

namespace vitreo::winapi::d3d12 {

namespace {

// ---- DXGI_FORMAT layout -----------------------------------------------------
//
// The element of every plane of every DXGI_FORMAT the D3D12 runtime knows:
// Learn DXGI_FORMAT (bit sizes, the packed 2x1 formats, block compression),
// "Planar formats" / "Recommended 8-bit YUV formats" (plane shapes), and the
// measured D3D12 plane formats and PlaneCounts of d3d12_layout_probe [fmt] /
// d3d12_caps_probe [format] (a depth-stencil format's planes are R32_TYPELESS
// and R8_TYPELESS; NV12's R8_TYPELESS and R8G8_TYPELESS). test_d3d12_layout
// replays every footprint of the probe through this table.

struct PlaneLayout {
    u8 format;     ///< footprint DXGI_FORMAT (the resource format for 1 plane)
    u8 bytes;      ///< bytes per element (texel or block)
    u8 block_w;    ///< texels per element horizontally
    u8 block_h;    ///< texels per element vertically
    u8 sub_x;      ///< plane width = texture width / sub_x
    u8 sub_y;      ///< plane height = texture height / sub_y
};

struct FormatLayout {
    u8 planes{0};  ///< 0: the runtime does not know the format
    u8 width_multiple{1};
    u8 height_multiple{1};
    PlaneLayout plane[2]{};
};

constexpr FormatLayout single(u32 format, u8 bytes, u8 block_w = 1, u8 block_h = 1,
                              u8 width_multiple = 1) {
    FormatLayout f{};
    f.planes = 1;
    f.width_multiple = width_multiple;
    f.plane[0] = {static_cast<u8>(format), bytes, block_w, block_h, 1, 1};
    return f;
}

constexpr FormatLayout two_planes(PlaneLayout p0, PlaneLayout p1, u8 width_multiple,
                                  u8 height_multiple) {
    FormatLayout f{};
    f.planes = 2;
    f.width_multiple = width_multiple;
    f.height_multiple = height_multiple;
    f.plane[0] = p0;
    f.plane[1] = p1;
    return f;
}

constexpr u8 R32_TYPELESS = 39;
constexpr u8 R8_TYPELESS = 60;
constexpr u8 R8G8_TYPELESS = 48;
constexpr u8 R16_TYPELESS = 53;
constexpr u8 R16G16_TYPELESS = 33;

constexpr FormatLayout format_layout_of(u32 f) {
    if (f == 0) return single(0, 0);  // UNKNOWN: FORMAT_INFO answers 1 plane
    if (f >= 1 && f <= 4) return single(f, 16);
    if (f >= 5 && f <= 8) return single(f, 12);
    if (f >= 9 && f <= 18) return single(f, 8);
    if (f >= 19 && f <= 22) {
        // DXGI_FORMAT_R32G8X24 family: depth plane, then stencil plane.
        return two_planes({R32_TYPELESS, 4, 1, 1, 1, 1}, {R8_TYPELESS, 1, 1, 1, 1, 1}, 1, 1);
    }
    if (f >= 23 && f <= 43) return single(f, 4);
    if (f >= 44 && f <= 47) {
        // DXGI_FORMAT_R24G8 family.
        return two_planes({R32_TYPELESS, 4, 1, 1, 1, 1}, {R8_TYPELESS, 1, 1, 1, 1, 1}, 1, 1);
    }
    if (f >= 48 && f <= 59) return single(f, 2);
    if (f >= 60 && f <= 65) return single(f, 1);
    if (f == 66) return FormatLayout{};  // R1_UNORM: unknown to D3D12
    if (f == 67) return single(f, 4);
    if (f == 68 || f == 69) return single(f, 4, 2, 1, 2);  // R8G8_B8G8 / G8R8_G8B8
    if ((f >= 70 && f <= 72) || (f >= 79 && f <= 81)) return single(f, 8, 4, 4);   // BC1, BC4
    if ((f >= 73 && f <= 78) || (f >= 82 && f <= 84)) return single(f, 16, 4, 4);  // BC2/3/5
    if (f == 85 || f == 86) return single(f, 2);
    if (f >= 87 && f <= 93) return single(f, 4);
    if (f >= 94 && f <= 99) return single(f, 16, 4, 4);  // BC6H, BC7
    if (f == 100 || f == 101) return single(f, 4);         // AYUV, Y410
    if (f == 102) return single(f, 8);                     // Y416
    if (f == 103) {                                         // NV12
        return two_planes({R8_TYPELESS, 1, 1, 1, 1, 1}, {R8G8_TYPELESS, 2, 1, 1, 2, 2}, 2, 2);
    }
    if (f == 104 || f == 105) {  // P010, P016
        return two_planes({R16_TYPELESS, 2, 1, 1, 1, 1}, {R16G16_TYPELESS, 4, 1, 1, 2, 2}, 2,
                          2);
    }
    if (f == 106) {
        // 420_OPAQUE: FORMAT_INFO answers one plane (measured) and its
        // layout is opaque; its dimensions are 4:2:0.
        FormatLayout o = single(f, 1);
        o.width_multiple = 2;
        o.height_multiple = 2;
        return o;
    }
    if (f == 107) return single(f, 4, 2, 1, 2);  // YUY2
    if (f == 108 || f == 109) return single(f, 8, 2, 1, 2);  // Y210, Y216
    if (f == 110) {  // NV11: 4:1:1, chroma a quarter wide, full height
        return two_planes({R8_TYPELESS, 1, 1, 1, 1, 1}, {R8G8_TYPELESS, 2, 1, 1, 4, 1}, 4, 1);
    }
    if (f == 111 || f == 112 || f == 113) return single(f, 1);  // AI44, IA44, P8
    if (f == 114 || f == 115) return single(f, 2);              // A8P8, B4G4R4A4
    // 116..188 (including P208 / V208 / V408, which FORMAT_INFO rejects on
    // this runtime) are unknown.
    if (f == 189 || f == 190) return single(f, 1);  // sampler feedback (opaque)
    if (f == 191) return single(f, 2);              // A4B4G4R4
    return FormatLayout{};
}

constexpr std::array<FormatLayout, 192> make_format_table() {
    std::array<FormatLayout, 192> t{};
    for (u32 f = 0; f < 192; ++f) t[f] = format_layout_of(f);
    return t;
}

constexpr std::array<FormatLayout, 192> kFormats = make_format_table();

const FormatLayout* format_layout(u32 format) {
    if (format >= kFormats.size() || kFormats[format].planes == 0) return nullptr;
    return &kFormats[format];
}

bool is_sampler_feedback(u32 format) { return format == 189 || format == 190; }

u64 align_up(u64 value, u64 alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

/// A mip's extent. MipLevels may exceed the full chain (measured: 65535 on a
/// 64x32 texture is accepted), and the runtime's shifts then wrap the way
/// the hardware shift does: the UINT64 Width by the count mod 64, the UINT
/// Height and depth by the count mod 32 (d3d12_layout_probe "mipw64all":
/// 100 mips total 104196, 65535 mips 60816388).
u32 mip_width(u64 width, u32 mip) {
    return static_cast<u32>(std::max<u64>(1, width >> (mip & 63u)));
}

u32 mip_extent(u32 extent, u32 mip) { return std::max(1u, extent >> (mip & 31u)); }

u32 full_chain(u64 w, u64 h, u64 d) {
    u64 extent = std::max(w, std::max(h, d));
    u32 levels = 1;
    while (extent > 1) {
        extent >>= 1;
        ++levels;
    }
    return levels;
}

u32 support_of(const LayoutCaps& caps, u32 format) {
    return caps.format_support1 ? caps.format_support1(format) : 0u;
}

/// The support of a format and every member of its cast family: a usage
/// flag is legal when some format a view could take has the usage
/// (measured: ALLOW_RENDER_TARGET on D32_FLOAT and ALLOW_DEPTH_STENCIL on
/// R32_FLOAT / R16_UNORM / R24_UNORM_X8_TYPELESS are accepted, on
/// R8G8B8A8 / R8_UNORM / BC1 / D24_UNORM_S8_UINT refused).
u32 family_support(const LayoutCaps& caps, u32 format) {
    u32 mask = support_of(caps, format);
    const auto* family = dxgi_typeless_family(format);
    if (family == nullptr) return mask;
    mask |= support_of(caps, family->typeless);
    for (u8 i = 0; i < family->typed_count; ++i) mask |= support_of(caps, family->typed[i]);
    return mask;
}

/// The runtime's small-resource estimate (Learn D3D12_RESOURCE_DESC,
/// "Alignment"): near-equilateral 4 KiB tiles over the most detailed mip,
/// at most 16 of them. The tile of an element of B bytes holds 4096 / B
/// elements (B rounded up to a power of two: R32G32B32 estimates as 16
/// bytes), split width-first -- 2D 64x64 at 1 byte down to 16x16 at 16; 3D
/// 16x16x16 down to 8x8x4; 1D 4096 / B (measured, d3d12_layout_probe
/// [alloc] "small" rows).
bool fits_small_alignment(const ResourceDesc& desc, const FormatLayout& fl) {
    if (fl.planes != 1) return false;  // planar formats never (measured)
    const PlaneLayout& p = fl.plane[0];
    u32 bytes = 1;
    while (bytes < p.bytes) bytes <<= 1;
    u32 bits = 0;
    while ((1u << bits) < 4096u / bytes) ++bits;  // log2 of elements per tile
    const u64 ew = (desc.width + p.block_w - 1) / p.block_w;
    const u64 eh = (static_cast<u64>(desc.height) + p.block_h - 1) / p.block_h;
    u64 tiles = 0;
    if (desc.dimension == RESOURCE_DIMENSION_TEXTURE1D) {
        tiles = (ew + (1ull << bits) - 1) >> bits;
    } else if (desc.dimension == RESOURCE_DIMENSION_TEXTURE2D) {
        const u32 bw = (bits + 1) / 2;
        const u32 bh = bits - bw;
        tiles = ((ew + (1ull << bw) - 1) >> bw) * ((eh + (1ull << bh) - 1) >> bh);
    } else {
        const u32 bx = (bits + 2) / 3;
        const u32 by = (bits - bx + 1) / 2;
        const u32 bz = bits - bx - by;
        const u64 ed = desc.depth_or_array_size;
        tiles = ((ew + (1ull << bx) - 1) >> bx) * ((eh + (1ull << by) - 1) >> by) *
                ((ed + (1ull << bz) - 1) >> bz);
    }
    return tiles <= 16;
}

bool quality_ok(const ResourceDesc& desc, const LayoutCaps& caps) {
    const u32 levels = caps.quality_levels ? caps.quality_levels(desc.format, desc.sample_count)
                                           : 0u;
    if (levels == 0) return false;
    return desc.sample_quality < levels || desc.sample_quality == STANDARD_MULTISAMPLE_PATTERN ||
           desc.sample_quality == CENTER_MULTISAMPLE_PATTERN;
}

const char* buffer_violation(const ResourceDesc& desc) {
    if (desc.width == 0) return "buffer Width 0";
    if (desc.height != 1 || desc.depth_or_array_size != 1 || desc.mip_levels != 1) {
        return "buffer Height/DepthOrArraySize/MipLevels must be 1";
    }
    if (desc.format != 0) return "buffer Format must be UNKNOWN";
    if (desc.sample_count != 1 || desc.sample_quality != 0) {
        return "buffer SampleDesc must be {1, 0}";
    }
    if (desc.layout != TEXTURE_LAYOUT_ROW_MAJOR) return "buffer Layout must be ROW_MAJOR";
    if (desc.alignment != 0 && desc.alignment != DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) {
        return "buffer Alignment must be 0 or 64 KiB";
    }
    // Learn D3D12_RESOURCE_FLAGS: a buffer is never a depth-stencil target
    // and is always simultaneous-access; measured both refused.
    if ((desc.flags & (RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)) !=
        0) {
        return "buffer with ALLOW_DEPTH_STENCIL or ALLOW_SIMULTANEOUS_ACCESS";
    }
    return nullptr;
}

const char* texture_violation(const ResourceDesc& desc, const LayoutCaps& caps) {
    const FormatLayout* fl = format_layout(desc.format);
    const u32 own = support_of(caps, desc.format);
    if (fl == nullptr || own == 0) return "format unknown or unsupported";
    if (desc.width == 0 || desc.height == 0 || desc.depth_or_array_size == 0) {
        return "zero dimension";
    }
    const u32 dim = desc.dimension;
    const bool msaa = desc.sample_count > 1;
    const u32 mips = resolved_mip_levels(desc);
    switch (dim) {
    case RESOURCE_DIMENSION_TEXTURE1D:
        if ((own & FORMAT_SUPPORT1_TEXTURE1D) == 0) return "format has no TEXTURE1D";
        if (desc.height != 1) return "Texture1D Height must be 1";
        if (desc.width > REQ_TEXTURE1D_U_DIMENSION) return "Texture1D too wide";
        if (desc.depth_or_array_size > REQ_TEXTURE1D_ARRAY_AXIS_DIMENSION) {
            return "Texture1D array too large";
        }
        break;
    case RESOURCE_DIMENSION_TEXTURE2D:
        if ((own & FORMAT_SUPPORT1_TEXTURE2D) == 0) return "format has no TEXTURE2D";
        // Measured: the runtime checks 2D sizes against 2^23 and applies
        // D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION only to an explicit mip chain.
        if (desc.width > (1ull << 23) || desc.height > (1u << 23)) return "Texture2D too large";
        if (desc.mip_levels > 1 && (desc.width > REQ_TEXTURE2D_U_OR_V_DIMENSION ||
                                    desc.height > REQ_TEXTURE2D_U_OR_V_DIMENSION)) {
            return "mipped Texture2D above 16384";
        }
        if (desc.depth_or_array_size > REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) {
            return "Texture2D array too large";
        }
        break;
    case RESOURCE_DIMENSION_TEXTURE3D:
        if ((own & FORMAT_SUPPORT1_TEXTURE3D) == 0) return "format has no TEXTURE3D";
        if (desc.width > REQ_TEXTURE3D_U_V_OR_W_DIMENSION ||
            desc.height > REQ_TEXTURE3D_U_V_OR_W_DIMENSION ||
            desc.depth_or_array_size > REQ_TEXTURE3D_U_V_OR_W_DIMENSION) {
            return "Texture3D too large";
        }
        break;
    default:
        return "bad Dimension";
    }
    if (desc.width % fl->width_multiple != 0 || desc.height % fl->height_multiple != 0) {
        return "dimensions not a multiple of the format's subsampling";
    }
    if (!caps.unaligned_block_textures && fl->plane[0].block_h == 4 &&
        (desc.width % 4 != 0 || desc.height % 4 != 0)) {
        return "block-compressed texture not a multiple of 4 (OPTIONS8)";
    }
    if (mips > 1 && (own & FORMAT_SUPPORT1_MIP) == 0) return "format has no MIP";

    // SampleDesc (Quality is not looked at for one sample: measured).
    if (desc.sample_count == 0) return "SampleDesc.Count 0";
    if (msaa) {
        if (dim != RESOURCE_DIMENSION_TEXTURE2D) return "MSAA on a non-2D texture";
        if ((desc.flags & (RESOURCE_FLAG_ALLOW_RENDER_TARGET | RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) ==
            0) {
            return "MSAA without ALLOW_RENDER_TARGET or ALLOW_DEPTH_STENCIL";
        }
        if (mips != 1) return "MSAA with mips";
        if ((desc.flags & RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0) {
            return "MSAA with ALLOW_SIMULTANEOUS_ACCESS";
        }
        if (!quality_ok(desc, caps)) return "sample count / quality unsupported";
    }

    // Layout.
    switch (desc.layout) {
    case TEXTURE_LAYOUT_UNKNOWN:
        break;
    case TEXTURE_LAYOUT_ROW_MAJOR:
        if (dim != RESOURCE_DIMENSION_TEXTURE2D || mips != 1 || desc.depth_or_array_size != 1 ||
            msaa || fl->planes != 1 ||
            (desc.flags & RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) {
            return "ROW_MAJOR needs a single-plane, single-sample 2D texture without mips or array";
        }
        break;
    case TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE:
        if (dim == RESOURCE_DIMENSION_TEXTURE1D) return "64KB_UNDEFINED_SWIZZLE on a Texture1D";
        break;
    case TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE:
        if (dim == RESOURCE_DIMENSION_TEXTURE1D || msaa) {
            return "64KB_STANDARD_SWIZZLE on a Texture1D or MSAA texture";
        }
        if (fl->planes == 1 && fl->plane[0].bytes == 12) return "64KB_STANDARD_SWIZZLE on 96-bit";
        break;
    default:
        return "bad Layout";
    }

    // Flags.
    const u32 flags = desc.flags;
    if ((flags & RESOURCE_FLAG_ALLOW_CROSS_ADAPTER) != 0 && mips != 1) {
        return "ALLOW_CROSS_ADAPTER with mips";  // Learn D3D12_RESOURCE_DESC, MipLevels
    }
    const bool rt = (flags & RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    const bool ds = (flags & RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    const bool uav = (flags & RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    if (rt && ds) return "ALLOW_RENDER_TARGET with ALLOW_DEPTH_STENCIL";
    if (ds && (uav || (flags & RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0)) {
        return "ALLOW_DEPTH_STENCIL with UAV or simultaneous access";
    }
    if (ds && dim == RESOURCE_DIMENSION_TEXTURE3D) return "ALLOW_DEPTH_STENCIL on a Texture3D";
    if ((flags & RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0 && !ds) {
        return "DENY_SHADER_RESOURCE without ALLOW_DEPTH_STENCIL";
    }
    const u32 family = family_support(caps, desc.format);
    if (rt && (family & FORMAT_SUPPORT1_RENDER_TARGET) == 0) return "format family cannot render";
    if (ds && (family & FORMAT_SUPPORT1_DEPTH_STENCIL) == 0) return "format family has no depth";
    // A sampler feedback map is written through its own UAV kind, not a
    // typed one (measured: feedback formats take ALLOW_UNORDERED_ACCESS).
    if (uav && !is_sampler_feedback(desc.format) &&
        (family & FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) == 0) {
        return "format family has no typed UAV";
    }

    // SamplerFeedbackMipRegion (D3D12_RESOURCE_DESC1): only the feedback
    // formats have one, and they need it (measured).
    const bool region = desc.mip_region_width != 0 || desc.mip_region_height != 0 ||
                        desc.mip_region_depth != 0;
    if (is_sampler_feedback(desc.format)) {
        const auto pow2_at_least_4 = [](u32 v) { return v >= 4 && (v & (v - 1)) == 0; };
        if (!pow2_at_least_4(desc.mip_region_width) || !pow2_at_least_4(desc.mip_region_height) ||
            desc.mip_region_depth > 1) {
            return "sampler feedback format without a valid SamplerFeedbackMipRegion";
        }
    } else if (region) {
        return "SamplerFeedbackMipRegion on a non-feedback format";
    }

    // Alignment (Learn D3D12_RESOURCE_DESC, "Alignment"; measured: 64 KiB is
    // always accepted, MSAA too; 4 MiB only for MSAA; 4 KiB only for small
    // single-sample UNKNOWN-layout textures that are neither render target
    // nor depth stencil).
    switch (desc.alignment) {
    case 0:
    case DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT:
        break;
    case DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT:
        if (!msaa) return "4 MiB alignment on a single-sample texture";
        break;
    case SMALL_RESOURCE_PLACEMENT_ALIGNMENT:
        if (msaa || rt || ds || desc.layout != TEXTURE_LAYOUT_UNKNOWN ||
            !fits_small_alignment(desc, *fl)) {
            return "4 KiB alignment on a texture that is not small";
        }
        break;
    default:
        return "bad Alignment";
    }
    return nullptr;
}

u64 all_ones64() { return std::numeric_limits<u64>::max(); }

}  // namespace

// ---- Guest structs ----------------------------------------------------------

ResourceDesc decode_resource_desc(const u8* b, bool desc1) noexcept {
    const auto u32_at = [b](usize o) {
        u32 v;
        std::memcpy(&v, b + o, 4);
        return v;
    };
    const auto u64_at = [b](usize o) {
        u64 v;
        std::memcpy(&v, b + o, 8);
        return v;
    };
    const auto u16_at = [b](usize o) {
        u16 v;
        std::memcpy(&v, b + o, 2);
        return v;
    };
    ResourceDesc d;
    d.dimension = u32_at(0);
    d.alignment = u64_at(8);
    d.width = u64_at(16);
    d.height = u32_at(24);
    d.depth_or_array_size = u16_at(28);
    d.mip_levels = u16_at(30);
    d.format = u32_at(32);
    d.sample_count = u32_at(36);
    d.sample_quality = u32_at(40);
    d.layout = u32_at(44);
    d.flags = u32_at(48);
    if (desc1) {
        d.mip_region_width = u32_at(52);
        d.mip_region_height = u32_at(56);
        d.mip_region_depth = u32_at(60);
    }
    return d;
}

void encode_resource_desc(const ResourceDesc& d, bool desc1, u8* b) noexcept {
    std::memset(b, 0, desc1 ? RESOURCE_DESC1_SIZE : RESOURCE_DESC_SIZE);
    const auto put32 = [b](usize o, u32 v) { std::memcpy(b + o, &v, 4); };
    const auto put64 = [b](usize o, u64 v) { std::memcpy(b + o, &v, 8); };
    const auto put16 = [b](usize o, u16 v) { std::memcpy(b + o, &v, 2); };
    put32(0, d.dimension);
    put64(8, d.alignment);
    put64(16, d.width);
    put32(24, d.height);
    put16(28, d.depth_or_array_size);
    put16(30, d.mip_levels);
    put32(32, d.format);
    put32(36, d.sample_count);
    put32(40, d.sample_quality);
    put32(44, d.layout);
    put32(48, d.flags);
    if (desc1) {
        put32(52, d.mip_region_width);
        put32(56, d.mip_region_height);
        put32(60, d.mip_region_depth);
    }
}

bool read_resource_desc(const GuestMemory& memory, GuestAddr address, bool desc1,
                        ResourceDesc& desc) {
    u8 bytes[RESOURCE_DESC1_SIZE] = {};
    if (address == 0 ||
        memory.read_bytes(address, bytes, desc1 ? RESOURCE_DESC1_SIZE : RESOURCE_DESC_SIZE)
            .is_err()) {
        return false;
    }
    desc = decode_resource_desc(bytes, desc1);
    return true;
}

// ---- Rules ------------------------------------------------------------------

u32 resolved_mip_levels(const ResourceDesc& desc) noexcept {
    if (desc.mip_levels != 0) return desc.mip_levels;
    switch (desc.dimension) {
    case RESOURCE_DIMENSION_TEXTURE1D:
        return full_chain(desc.width, 1, 1);
    case RESOURCE_DIMENSION_TEXTURE2D:
        return full_chain(desc.width, desc.height, 1);
    case RESOURCE_DIMENSION_TEXTURE3D:
        return full_chain(desc.width, desc.height, desc.depth_or_array_size);
    default:
        return 1;
    }
}

u32 format_plane_count(u32 dxgi_format) noexcept {
    const FormatLayout* fl = format_layout(dxgi_format);
    return fl == nullptr ? 0u : fl->planes;
}

u64 subresource_count(const ResourceDesc& desc) noexcept {
    if (desc.dimension == RESOURCE_DIMENSION_BUFFER) return 1;
    const u64 array =
        desc.dimension == RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.depth_or_array_size;
    return static_cast<u64>(resolved_mip_levels(desc)) * array *
           std::max(1u, format_plane_count(desc.format));
}

bool same_cast_family(u32 a, u32 b) noexcept {
    if (a == b) return true;
    const auto* fa = dxgi_typeless_family(a);
    return fa != nullptr && fa == dxgi_typeless_family(b);
}

u32 format_element_bytes(u32 dxgi_format) noexcept {
    const FormatLayout* fl = format_layout(dxgi_format);
    if (fl == nullptr || fl->planes != 1) return 0;
    return fl->plane[0].bytes;
}

const char* resource_desc_violation(const ResourceDesc& desc, const LayoutCaps& caps) noexcept {
    if ((desc.flags & ~RESOURCE_FLAGS_DEFINED) != 0) return "undefined D3D12_RESOURCE_FLAGS bit";
    // No video decode / encode on this device (D3D12_FEATURE_DATA_D3D12_OPTIONS
    // and the video queries report none); WARP refuses them too.
    if ((desc.flags & (RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY |
                       RESOURCE_FLAG_VIDEO_ENCODE_REFERENCE_ONLY)) != 0) {
        return "video reference-only resource";
    }
    if (desc.dimension == RESOURCE_DIMENSION_BUFFER) return buffer_violation(desc);
    if (desc.dimension < RESOURCE_DIMENSION_TEXTURE1D || desc.dimension > RESOURCE_DIMENSION_TEXTURE3D) {
        return "bad Dimension";
    }
    return texture_violation(desc, caps);
}

// ---- GetCopyableFootprints --------------------------------------------------

bool copyable_footprints(const ResourceDesc& desc, u32 first, u32 num, u64 base_offset,
                         const LayoutCaps& caps,
                         const std::function<void(u32, const Footprint&)>& each,
                         u64& total_bytes) {
    if (resource_desc_violation(desc, caps) != nullptr) return false;
    if (desc.dimension == RESOURCE_DIMENSION_BUFFER) {
        // Footprint.Width is a UINT; the runtime refuses a Width it cannot
        // hold (0xFFFFFFFF and up: measured) and truncates the aligned pitch.
        if (desc.width > 0xFFFFFFFEull) return false;
        if (first > 1 || num > 1 - first) return false;
        if (num == 1) {
            Footprint f;
            f.offset = base_offset;
            f.format = 0;
            f.width = static_cast<u32>(desc.width);
            f.height = 1;
            f.depth = 1;
            f.row_pitch = static_cast<u32>(align_up(desc.width, TEXTURE_DATA_PITCH_ALIGNMENT));
            f.num_rows = 1;
            f.row_size = desc.width;
            if (each) each(0, f);
            total_bytes = desc.width;
        } else {
            total_bytes = 0;
        }
        return true;
    }
    // No subresources asked for: a total of 0 whatever FirstSubresource is
    // (measured: first 30720 of 14, num 0).
    if (num == 0) {
        total_bytes = 0;
        return true;
    }
    const u64 count = subresource_count(desc);
    if (first > count || num > count - first) return false;
    const FormatLayout& fl = *format_layout(desc.format);
    const u32 mips = resolved_mip_levels(desc);
    const u32 array =
        desc.dimension == RESOURCE_DIMENSION_TEXTURE3D ? 1u : desc.depth_or_array_size;
    const u64 per_plane = static_cast<u64>(mips) * array;
    // Two-plane formats align rows to 512 (measured: D24_UNORM_S8_UINT's
    // 1200-byte depth rows pitch at 1536, NV12's 300-byte rows at 512).
    const u32 pitch_alignment =
        fl.planes > 1 ? TEXTURE_DATA_PLACEMENT_ALIGNMENT : TEXTURE_DATA_PITCH_ALIGNMENT;
    u32 relative = 0;  // the runtime's running offset is 32-bit (measured)
    u32 last_end = 0;
    for (u32 i = 0; i < num; ++i) {
        const u64 sub = static_cast<u64>(first) + i;
        const u32 plane = static_cast<u32>(sub / per_plane);
        const u32 mip = static_cast<u32>(sub % mips);
        const PlaneLayout& p = fl.plane[plane];
        const u32 w = mip_width(desc.width, mip) / p.sub_x;
        const u32 h = mip_extent(desc.height, mip) / p.sub_y;
        const u32 d = desc.dimension == RESOURCE_DIMENSION_TEXTURE3D
                          ? mip_extent(desc.depth_or_array_size, mip)
                          : 1u;
        Footprint f;
        f.format = p.format;
        f.width = static_cast<u32>(align_up(std::max(1u, w), p.block_w));
        f.height = static_cast<u32>(align_up(std::max(1u, h), p.block_h));
        f.depth = d;
        f.row_size = static_cast<u64>(f.width / p.block_w) * p.bytes;
        f.row_pitch = static_cast<u32>(align_up(f.row_size, pitch_alignment));
        f.num_rows = f.height / p.block_h;
        if (i != 0) relative = static_cast<u32>(align_up(last_end, TEXTURE_DATA_PLACEMENT_ALIGNMENT));
        f.offset = base_offset + relative;
        const u64 size =
            (static_cast<u64>(f.num_rows) * f.depth - 1) * f.row_pitch + f.row_size;
        last_end = static_cast<u32>(relative + size);
        if (each) each(i, f);
    }
    total_bytes = num == 0 ? 0 : last_end;
    return true;
}

void guest_get_copyable_footprints(GuestMemory& memory, const LayoutCaps& caps, bool desc1,
                                   GuestAddr desc_address, u32 first, u32 num,
                                   u64 base_offset, GuestAddr layouts, GuestAddr num_rows,
                                   GuestAddr row_sizes, GuestAddr total_bytes) {
    ResourceDesc desc;
    if (!read_resource_desc(memory, desc_address, desc1, desc)) return;
    const auto write_entry = [&](u32 i, const Footprint& f) {
        if (layouts != 0) {
            u8 b[PLACED_SUBRESOURCE_FOOTPRINT_SIZE] = {};
            std::memcpy(b + 0, &f.offset, 8);
            std::memcpy(b + 8, &f.format, 4);
            std::memcpy(b + 12, &f.width, 4);
            std::memcpy(b + 16, &f.height, 4);
            std::memcpy(b + 20, &f.depth, 4);
            std::memcpy(b + 24, &f.row_pitch, 4);
            // Bytes 28..31 are the structure's tail padding, not a field.
            (void)memory.write_bytes(layouts + static_cast<u64>(i) * PLACED_SUBRESOURCE_FOOTPRINT_SIZE,
                                     b, 28);
        }
        if (num_rows != 0) (void)memory.write_u32(num_rows + static_cast<u64>(i) * 4, f.num_rows);
        if (row_sizes != 0) (void)memory.write_u64(row_sizes + static_cast<u64>(i) * 8, f.row_size);
    };
    u64 total = 0;
    if (copyable_footprints(desc, first, num, base_offset, caps, write_entry, total)) {
        if (total_bytes != 0) (void)memory.write_u64(total_bytes, total);
        return;
    }
    // Invalid description or range: every requested entry reads all-ones and
    // the total UINT64_MAX (Learn; measured for all three arrays).
    Footprint ones;
    ones.offset = all_ones64();
    ones.format = ones.width = ones.height = ones.depth = ones.row_pitch = ones.num_rows =
        0xFFFFFFFFu;
    ones.row_size = all_ones64();
    if (layouts != 0 || num_rows != 0 || row_sizes != 0) {
        for (u32 i = 0; i < num; ++i) {
            const GuestAddr probe = layouts != 0 ? layouts + static_cast<u64>(i) * PLACED_SUBRESOURCE_FOOTPRINT_SIZE
                                                 : (num_rows != 0 ? num_rows + static_cast<u64>(i) * 4
                                                                  : row_sizes + static_cast<u64>(i) * 8);
            if (memory.read_u8(probe).is_err()) break;  // Windows faults here
            write_entry(i, ones);
        }
    }
    if (total_bytes != 0) (void)memory.write_u64(total_bytes, all_ones64());
}

// ---- GetResourceAllocationInfo ----------------------------------------------

namespace {

bool castable_ok(const ResourceDesc& desc, const CastableFormats& castable, const LayoutCaps& caps) {
    if (castable.formats.empty()) return true;
    // Castable formats are the Relaxed Format Casting contract
    // (D3D12_OPTIONS12.RelaxedFormatCastingSupported); without it none is
    // honoured, so the description cannot be sized.
    if (!caps.relaxed_format_casting) return false;
    if (desc.dimension == RESOURCE_DIMENSION_BUFFER) return false;
    const u32 own = format_element_bytes(desc.format);
    if (own == 0) return false;
    for (u32 f : castable.formats) {
        // Measured: UNKNOWN and a format of another element size (BC1 on
        // R8G8B8A8) fail; same-size formats of other families pass.
        if (f == 0 || format_element_bytes(f) != own || support_of(caps, f) == 0) return false;
    }
    return true;
}

bool texture_allocation(const ResourceDesc& desc, const LayoutCaps& caps, u64& size,
                        u64& alignment) {
    const bool msaa = desc.sample_count > 1;
    alignment = desc.alignment != 0 ? desc.alignment
                                    : (msaa ? DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                                            : DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
    const u32 mips = resolved_mip_levels(desc);
    if (caps.texture_heap_requirement) {
        const TextureHeapRequirement req = caps.texture_heap_requirement(desc, mips);
        if (!req.supported) return false;
        if (req.alignment > alignment) {
            // The backend cannot place the texture at the alignment the
            // description asks for. For the 4 KiB small alignment that is
            // the "not small" answer apps expect and retry from (Learn,
            // Small resources sample); nothing above 64 KiB is expected of a
            // Metal texture, so report that case.
            if (alignment > SMALL_RESOURCE_PLACEMENT_ALIGNMENT) {
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED d3d12!GetResourceAllocationInfo: texture "
                             "needs %llu-byte placement alignment\n",
                             static_cast<unsigned long long>(req.alignment));
            }
            return false;
        }
        size = align_up(std::max<u64>(req.size, 1), alignment);
        return true;
    }
    // No backend: the tight data size, every subresource once per sample.
    u64 total = 0;
    ResourceDesc linear = desc;
    linear.alignment = 0;
    linear.sample_count = 1;
    linear.sample_quality = 0;
    linear.flags &= ~(RESOURCE_FLAG_ALLOW_RENDER_TARGET | RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                      RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    u64 sum = 0;
    const u64 count = subresource_count(desc);
    const bool ok = copyable_footprints(
        linear, 0, static_cast<u32>(std::min<u64>(count, 0xFFFFFFFFull)), 0, caps,
        [&sum](u32, const Footprint& f) {
            sum += static_cast<u64>(f.num_rows) * f.depth * f.row_size;
        },
        total);
    if (!ok) sum = 0;
    size = align_up(std::max<u64>(sum * std::max(1u, desc.sample_count), 1), alignment);
    return true;
}

}  // namespace

AllocationInfo resource_allocation_info(u32 visible_mask, const std::vector<ResourceDesc>& descs,
                                        const std::vector<CastableFormats>& castable,
                                        const LayoutCaps& caps,
                                        std::vector<AllocationInfo1>* per_resource) {
    const AllocationInfo invalid{all_ones64(), DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT};
    // visibleMask: 0 or nodes that exist (measured: 2, 3 and 0x80000000 fail
    // on one node).
    if ((visible_mask & ~caps.node_mask) != 0) return invalid;
    if (descs.empty()) return {0, 1};
    std::vector<AllocationInfo1> entries(descs.size());
    u64 running = 0;
    u64 max_alignment = 1;
    for (usize i = 0; i < descs.size(); ++i) {
        const ResourceDesc& desc = descs[i];
        if (resource_desc_violation(desc, caps) != nullptr) return invalid;
        if (i < castable.size() && !castable_ok(desc, castable[i], caps)) return invalid;
        u64 size = 0;
        u64 alignment = DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        if (desc.dimension == RESOURCE_DIMENSION_BUFFER) {
            // Learn: a buffer is the smallest multiple of 64 KiB >= Width on
            // every adapter.
            size = desc.width > caps.max_buffer_width
                       ? BUFFER_TOO_LARGE_SIZE
                       : align_up(desc.width, DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
        } else if (!texture_allocation(desc, caps, size, alignment)) {
            return invalid;
        }
        // Learn: "the C++ algorithm for calculating a structure size and
        // alignment" -- each member at its alignment, the whole padded to the
        // largest.
        const u64 offset = align_up(running, alignment);
        entries[i] = {offset, alignment, size};
        running = offset + size;
        max_alignment = std::max(max_alignment, alignment);
    }
    if (per_resource != nullptr) *per_resource = std::move(entries);
    return {align_up(running, max_alignment), max_alignment};
}

void guest_get_resource_allocation_info(GuestMemory& memory, const LayoutCaps& caps,
                                        bool is_64bit, u32 variant, GuestAddr ret_address,
                                        u32 visible_mask, u32 num_descs,
                                        GuestAddr descs_address,
                                        GuestAddr num_castable_address,
                                        GuestAddr castable_address,
                                        GuestAddr info1_address) {
    const bool desc1 = variant >= 2;
    const usize stride = desc1 ? RESOURCE_DESC1_SIZE : RESOURCE_DESC_SIZE;
    std::vector<ResourceDesc> descs;
    std::vector<CastableFormats> castable;
    AllocationInfo result{all_ones64(), DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT};
    bool readable = true;
    descs.reserve(std::min<u32>(num_descs, 4096));
    for (u32 i = 0; i < num_descs && readable; ++i) {
        ResourceDesc d;
        readable = read_resource_desc(memory, descs_address + static_cast<u64>(i) * stride, desc1, d);
        descs.push_back(d);
    }
    if (readable && variant == 3 && num_castable_address != 0 && castable_address != 0) {
        // pNumCastableFormats[i] formats at ppCastableFormats[i]; a NULL
        // list pointer is no list (measured).
        const usize ptr = is_64bit ? 8 : 4;
        castable.resize(num_descs);
        for (u32 i = 0; i < num_descs && readable; ++i) {
            const auto count = memory.read_u32(num_castable_address + static_cast<u64>(i) * 4);
            GuestAddr list = 0;
            if (is_64bit) {
                const auto p = memory.read_u64(castable_address + static_cast<u64>(i) * ptr);
                readable = count.is_ok() && p.is_ok();
                if (readable) list = p.value();
            } else {
                const auto p = memory.read_u32(castable_address + static_cast<u64>(i) * ptr);
                readable = count.is_ok() && p.is_ok();
                if (readable) list = p.value();
            }
            if (!readable || list == 0) continue;
            for (u32 j = 0; j < count.value() && readable; ++j) {
                const auto f = memory.read_u32(list + static_cast<u64>(j) * 4);
                readable = f.is_ok();
                if (readable) castable[i].formats.push_back(f.value());
            }
        }
    }
    std::vector<AllocationInfo1> per;
    if (readable) {
        result = resource_allocation_info(visible_mask, descs, castable, caps,
                                          variant != 0 && info1_address != 0 ? &per : nullptr);
    }
    if (variant != 0 && info1_address != 0) {
        for (usize i = 0; i < per.size(); ++i) {
            u8 b[RESOURCE_ALLOCATION_INFO1_SIZE];
            std::memcpy(b + 0, &per[i].offset, 8);
            std::memcpy(b + 8, &per[i].alignment, 8);
            std::memcpy(b + 16, &per[i].size, 8);
            (void)memory.write_bytes(info1_address + i * RESOURCE_ALLOCATION_INFO1_SIZE, b,
                                     sizeof(b));
        }
    }
    if (ret_address != 0) {
        u8 b[RESOURCE_ALLOCATION_INFO_SIZE];
        std::memcpy(b + 0, &result.size, 8);
        std::memcpy(b + 8, &result.alignment, 8);
        (void)memory.write_bytes(ret_address, b, sizeof(b));
    }
}

// ---- GetResourceTiling ------------------------------------------------------

bool standard_tile_shape(const ResourceDesc& desc, u32& width, u32& height, u32& depth) noexcept {
    if (desc.dimension == RESOURCE_DIMENSION_BUFFER) {
        width = TILED_RESOURCE_TILE_SIZE_IN_BYTES;
        height = depth = 1;
        return true;
    }
    const FormatLayout* fl = format_layout(desc.format);
    if (fl == nullptr || fl->planes != 1) return false;
    const PlaneLayout& p = fl->plane[0];
    u32 log2_bytes = 0;
    switch (p.bytes) {
    case 1: log2_bytes = 0; break;
    case 2: log2_bytes = 1; break;
    case 4: log2_bytes = 2; break;
    case 8: log2_bytes = 3; break;
    case 16: log2_bytes = 4; break;
    default: return false;
    }
    // D3D11.3 functional spec 5.9.3.1: 64 KiB of elements, 256x256 at one
    // byte halving width then height per doubling of element size; 3D
    // 64x32x32 at one byte down to 16x16x16 at sixteen. Measured WARP
    // (d3d12_layout_probe [tiling]): R8 256x256, R16 256x128, RGBA8 128x128,
    // RGBA16 128x64, RGBA32 64x64, BC1 512x256, BC7 256x256, 4x RGBA8 64x64;
    // 3D R8 64x32x32, RGBA8 32x32x16, RGBA32 16x16x16.
    static constexpr u32 k2d[5][2] = {{256, 256}, {256, 128}, {128, 128}, {128, 64}, {64, 64}};
    static constexpr u32 k3d[5][3] = {
        {64, 32, 32}, {32, 32, 32}, {32, 32, 16}, {32, 16, 16}, {16, 16, 16}};
    if (desc.dimension == RESOURCE_DIMENSION_TEXTURE3D) {
        if (desc.sample_count > 1) return false;
        width = k3d[log2_bytes][0] * p.block_w;
        height = k3d[log2_bytes][1] * p.block_h;
        depth = k3d[log2_bytes][2];
        return true;
    }
    if (desc.dimension != RESOURCE_DIMENSION_TEXTURE2D) return false;
    u32 w = k2d[log2_bytes][0];
    u32 h = k2d[log2_bytes][1];
    // Multisampled: the same 64 KiB holds fewer texels (spec table: 2x
    // halves width, 4x both, 8x width twice, 16x both twice).
    switch (desc.sample_count) {
    case 0:
    case 1: break;
    case 2: w /= 2; break;
    case 4: w /= 2; h /= 2; break;
    case 8: w /= 4; h /= 2; break;
    case 16: w /= 4; h /= 4; break;
    default: return false;
    }
    width = w * p.block_w;
    height = h * p.block_h;
    depth = 1;
    return true;
}

bool standard_tiled_layout(const ResourceDesc& desc, TiledLayout& layout) noexcept {
    u32 w = 0, h = 0, d = 0;
    if (!standard_tile_shape(desc, w, h, d)) return false;
    layout.tile_width = w;
    layout.tile_height = h;
    layout.tile_depth = d;
    layout.standard_mips = resolved_mip_levels(desc);
    layout.packed_tiles = 0;
    return true;
}

namespace {

struct MipTiles {
    u32 x, y, z;
};

MipTiles mip_tiles(const ResourceDesc& desc, const TiledLayout& layout, u32 mip) {
    if (desc.dimension == RESOURCE_DIMENSION_BUFFER) {
        return {static_cast<u32>((desc.width + layout.tile_width - 1) / layout.tile_width), 1, 1};
    }
    const u64 w = mip_width(desc.width, mip);
    const u64 h = mip_extent(desc.height, mip);
    const u64 d = desc.dimension == RESOURCE_DIMENSION_TEXTURE3D
                      ? mip_extent(desc.depth_or_array_size, mip)
                      : 1;
    return {static_cast<u32>((w + layout.tile_width - 1) / layout.tile_width),
            static_cast<u32>((h + layout.tile_height - 1) / layout.tile_height),
            static_cast<u32>((d + layout.tile_depth - 1) / layout.tile_depth)};
}

u32 tiles_per_slice(const ResourceDesc& desc, const TiledLayout& layout) {
    u64 tiles = 0;
    for (u32 mip = 0; mip < layout.standard_mips; ++mip) {
        const MipTiles t = mip_tiles(desc, layout, mip);
        tiles += static_cast<u64>(t.x) * t.y * t.z;
    }
    return static_cast<u32>(tiles + layout.packed_tiles);
}

}  // namespace

ResourceTiling resource_tiling(const ResourceDesc& desc, const TiledLayout& layout) {
    ResourceTiling r;
    const u32 mips = desc.dimension == RESOURCE_DIMENSION_BUFFER ? 1u : resolved_mip_levels(desc);
    const u32 array = desc.dimension == RESOURCE_DIMENSION_TEXTURE2D ||
                              desc.dimension == RESOURCE_DIMENSION_TEXTURE1D
                          ? desc.depth_or_array_size
                          : 1u;
    const u32 per_slice = tiles_per_slice(desc, layout);
    r.total_tiles = per_slice * array;
    if (desc.dimension == RESOURCE_DIMENSION_BUFFER) {
        // Measured: a buffer reports no standard or packed mips.
        r.num_standard_mips = 0;
        r.num_packed_mips = 0;
    } else {
        r.num_standard_mips = static_cast<u8>(std::min(layout.standard_mips, mips));
        r.num_packed_mips = static_cast<u8>(mips - std::min(layout.standard_mips, mips));
    }
    if (r.num_packed_mips != 0) {
        r.packed_tiles = layout.packed_tiles;
        r.packed_start_tile = per_slice - layout.packed_tiles;
    }
    r.shape_width = layout.tile_width;
    r.shape_height = layout.tile_height;
    r.shape_depth = layout.tile_depth;
    return r;
}

SubresourceTiling subresource_tiling(const ResourceDesc& desc, const TiledLayout& layout,
                                     u32 index) {
    const u32 mips = desc.dimension == RESOURCE_DIMENSION_BUFFER ? 1u : resolved_mip_levels(desc);
    const u32 mip = index % mips;
    const u32 slice = index / mips;
    SubresourceTiling s;
    if (mip >= layout.standard_mips && desc.dimension != RESOURCE_DIMENSION_BUFFER) {
        // Learn D3D12_SUBRESOURCE_TILING: a packed mip reads D3D12_PACKED_TILE
        // with zero extents.
        s.start_tile_index = PACKED_TILE;
        return s;
    }
    u64 start = static_cast<u64>(slice) * tiles_per_slice(desc, layout);
    for (u32 m = 0; m < mip; ++m) {
        const MipTiles t = mip_tiles(desc, layout, m);
        start += static_cast<u64>(t.x) * t.y * t.z;
    }
    const MipTiles t = mip_tiles(desc, layout, mip);
    s.width_in_tiles = t.x;
    s.height_in_tiles = static_cast<u16>(t.y);
    s.depth_in_tiles = static_cast<u16>(t.z);
    s.start_tile_index = static_cast<u32>(start);
    return s;
}

void guest_get_resource_tiling(GuestMemory& memory, const ResourceDesc& desc, bool reserved,
                               const TiledLayout& layout, GuestAddr num_tiles,
                               GuestAddr packed_mip_desc, GuestAddr tile_shape,
                               GuestAddr num_subresource_tilings, u32 first_subresource,
                               GuestAddr subresource_tilings) {
    ResourceTiling r;
    u32 subresources = 0;
    if (reserved) {
        r = resource_tiling(desc, layout);
        subresources = desc.dimension == RESOURCE_DIMENSION_BUFFER
                           ? 1u
                           : static_cast<u32>(subresource_count(desc));
    }
    // A committed or placed resource answers all zero (measured).
    if (num_tiles != 0) (void)memory.write_u32(num_tiles, r.total_tiles);
    if (packed_mip_desc != 0) {
        u8 b[PACKED_MIP_INFO_SIZE] = {};
        b[0] = r.num_standard_mips;
        b[1] = r.num_packed_mips;
        std::memcpy(b + 4, &r.packed_tiles, 4);
        std::memcpy(b + 8, &r.packed_start_tile, 4);
        // Bytes 2..3 are padding, not a field.
        (void)memory.write_bytes(packed_mip_desc, b, 2);
        (void)memory.write_bytes(packed_mip_desc + 4, b + 4, 8);
    }
    if (tile_shape != 0) {
        (void)memory.write_u32(tile_shape, r.shape_width);
        (void)memory.write_u32(tile_shape + 4, r.shape_height);
        (void)memory.write_u32(tile_shape + 8, r.shape_depth);
    }
    if (num_subresource_tilings == 0) return;
    const auto in = memory.read_u32(num_subresource_tilings);
    if (in.is_err()) return;
    // Measured: the count is min(requested, subresources - first) in UINT
    // arithmetic, so a first past the end wraps and Windows extrapolates
    // slices that do not exist.
    const u32 n = reserved ? std::min(in.value(), subresources - first_subresource) : 0u;
    (void)memory.write_u32(num_subresource_tilings, n);
    if (subresource_tilings == 0) return;  // Windows faults for n != 0
    for (u32 i = 0; i < n; ++i) {
        const SubresourceTiling s = subresource_tiling(desc, layout, first_subresource + i);
        u8 b[SUBRESOURCE_TILING_SIZE];
        std::memcpy(b + 0, &s.width_in_tiles, 4);
        std::memcpy(b + 4, &s.height_in_tiles, 2);
        std::memcpy(b + 6, &s.depth_in_tiles, 2);
        std::memcpy(b + 8, &s.start_tile_index, 4);
        if (memory.write_bytes(subresource_tilings + static_cast<u64>(i) * SUBRESOURCE_TILING_SIZE, b,
                               sizeof(b))
                .is_err()) {
            break;
        }
    }
}

// ---- Vtable entry points ----------------------------------------------------

void marshal_get_resource_allocation_info(Marshaler& m, GuestMemory& memory,
                                          const LayoutCaps& caps, bool is_64bit, u32 variant) {
    const GuestAddr ret = m.arg_ptr(1);
    const u32 visible_mask = m.arg32(2);
    const u32 num_descs = m.arg32(3);
    const GuestAddr descs = m.arg_ptr(4);
    GuestAddr num_castable = 0, castable = 0, info1 = 0;
    if (variant == 1 || variant == 2) {
        info1 = m.arg_ptr(5);
    } else if (variant == 3) {
        num_castable = m.arg_ptr(5);
        castable = m.arg_ptr(6);
        info1 = m.arg_ptr(7);
    }
    guest_get_resource_allocation_info(memory, caps, is_64bit, variant, ret, visible_mask,
                                       num_descs, descs, num_castable, castable, info1);
    m.set_return(ret);
}

void marshal_get_copyable_footprints(Marshaler& m, GuestMemory& memory, const LayoutCaps& caps,
                                     bool is_64bit, bool desc1) {
    const GuestAddr desc = m.arg_ptr(1);
    const u32 first = m.arg32(2);
    const u32 num = m.arg32(3);
    u64 base = 0;
    usize next = 5;
    if (is_64bit) {
        base = m.arg(4);
    } else {
        base = static_cast<u64>(m.arg32(4)) | (static_cast<u64>(m.arg32(5)) << 32);
        next = 6;
    }
    guest_get_copyable_footprints(memory, caps, desc1, desc, first, num, base, m.arg_ptr(next),
                                  m.arg_ptr(next + 1), m.arg_ptr(next + 2), m.arg_ptr(next + 3));
}

void marshal_get_resource_tiling(Marshaler& m, GuestMemory& memory, const ResourceDesc& desc,
                                 bool reserved, const TiledLayout& layout) {
    guest_get_resource_tiling(memory, desc, reserved, layout, m.arg_ptr(2), m.arg_ptr(3),
                              m.arg_ptr(4), m.arg_ptr(5), m.arg32(6), m.arg_ptr(7));
}

}  // namespace vitreo::winapi::d3d12
