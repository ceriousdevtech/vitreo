// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d12_replay_feedback.cpp
/// @brief Sampler feedback transcodes: ResolveSubresourceRegion with
/// D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK / ENCODE_SAMPLER_FEEDBACK
/// between a feedback map's opaque storage (graphics/shaders/
/// sampler_feedback_layout.hpp) and an R8_UINT texture or buffer, and
/// CopyResource of feedback maps.
///
/// Contract: DirectX-Specs "Sampler Feedback", "Transcoding" -- decoded
/// MIN_MIP is one byte per mip-0 region, the most detailed requested mip or
/// 0xFF; decoded MIP_REGION_USED is one byte per region of each mip, 255
/// requested or 0; encoding 0xFF (MIN_MIP) / 0 (MIP_REGION_USED) requests
/// nothing. MIN_MIP is "not transitive": an encoded value v marks mip v's
/// region, so a decode reports v (or a more detailed mip requested nearby)
/// over that whole region -- "You get the most amount of precision for mip
/// zero, but lower levels of precision for mips thereafter"; values past
/// the last mip mark the last mip (the spec's own example: index 4 of a
/// 5-mip map decodes to <= 4 everywhere). Subresource rules: a MIN_MIP map
/// is SrcSubresource (decode) / DstSubresource (encode) UINT_MAX and the
/// other index names the array slice (UINT_MAX: all); a MIP_REGION_USED
/// transcode is the whole resource, both indices UINT_MAX (measured on the
/// RTX 4080 SUPER: per-subresource decodes make Close fail with
/// E_INVALIDARG). Source rectangles are not permitted for MIN_MIP.

#include "winapi/graphics/d3d12/d3d12_replay_internal.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace vitreo::winapi::d3d12 {

namespace fb = graphics::sampler_feedback;

namespace {

constexpr u32 kAll = 0xFFFFFFFFu;

[[nodiscard]] bool r8_transcode_format(u32 f) {
    return f == DXGI_FORMAT_R8_UINT || f == DXGI_FORMAT_R8_TYPELESS;
}

/// One slice's bits: mip m, region (x, y).
struct SliceBits {
    const fb::Geometry& g;
    u32* words;
    [[nodiscard]] bool get(u32 m, u32 x, u32 y) const {
        if (m >= g.mips || x >= g.grid_width[m] || y >= g.grid_height[m]) return false;
        const u32 b = y * g.grid_width[m] + x;
        return (words[g.mip_offset[m] + b / 32u] >> (b % 32u)) & 1u;
    }
    void set(u32 m, u32 x, u32 y) const {
        if (m >= g.mips || x >= g.grid_width[m] || y >= g.grid_height[m]) return;
        const u32 b = y * g.grid_width[m] + x;
        words[g.mip_offset[m] + b / 32u] |= 1u << (b % 32u);
    }
};

/// The decoded MIN_MIP value of mip-0 region (x, y).
[[nodiscard]] u8 min_mip_at(const SliceBits& s, u32 x, u32 y) {
    for (u32 m = 0; m < s.g.mips; ++m) {
        if (s.get(m, x >> m, y >> m)) return static_cast<u8>(m);
    }
    return 0xFF;
}

void wait_for_gpu(graphics::GraphicsBackendDevice& backend) {
    graphics::frame_profile::WaitScope profile_wait("replay:feedback");
    if (auto completion = backend.flush_completion()) {
        (void)completion->wait();
    } else {
        (void)backend.wait_idle();
    }
}

/// Reads @p length bytes of @p buffer at @p offset as the GPU left them: a
/// copy into a CPU-visible buffer (heap memory is GPU-private), then a wait.
[[nodiscard]] bool read_range(graphics::GraphicsBackendDevice& backend,
                              graphics::BackendBuffer& buffer, u64 offset, void* data,
                              usize length) {
    if (length == 0) return true;
    graphics::BackendBufferDesc desc{};
    desc.byte_width = length;
    auto staging = backend.create_buffer(desc, nullptr);
    if (!staging || !staging->valid() ||
        !backend.copy_buffer_region(staging.get(), 0, &buffer, offset, length)) {
        return false;
    }
    wait_for_gpu(backend);
    return staging->read(0, data, length);
}

/// Writes @p length bytes into @p buffer at @p offset in order with the
/// recorded work (a copy from a buffer holding them).
[[nodiscard]] bool write_range(graphics::GraphicsBackendDevice& backend,
                               graphics::BackendBuffer& buffer, u64 offset, const void* data,
                               usize length) {
    if (length == 0) return true;
    graphics::BackendBufferDesc desc{};
    desc.byte_width = length;
    auto source = backend.create_buffer(desc, data);
    return source && source->valid() &&
           backend.copy_buffer_region(&buffer, offset, source.get(), 0, length);
}

}  // namespace

void Frontend::record_feedback_transcode(CommandList& list, Recording& rec, Marshaler& m,
                                         Resource& d, u64 dst_serial, Resource& s,
                                         u64 src_serial, u32 mode) {
    const char* name = "ResolveSubresourceRegion";
    const bool decode = mode == D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK;
    Resource& map = decode ? s : d;
    Resource& other = decode ? d : s;
    if (!map.feedback_map || other.feedback_map) {
        record_error(list, E_INVALIDARG, name, "a feedback transcode needs one feedback map");
        return;
    }
    const u32 dst_sub = m.arg32(2);
    const u32 src_sub = m.arg32(6);
    const GuestAddr rect = m.arg_ptr(7);
    const u32 format = m.arg32(8);
    const u32 map_sub = decode ? src_sub : dst_sub;
    const u32 other_sub = decode ? dst_sub : src_sub;
    const bool min_mip = map.desc.format == kFeedbackMinMipFormat;
    const fb::Geometry& g = map.feedback;
    if (format != DXGI_FORMAT_R8_UINT) {
        record_error(list, E_INVALIDARG, name, "a feedback transcode format other than R8_UINT");
        return;
    }
    if (map_sub != kAll) {
        record_error(list, E_INVALIDARG, name, "a feedback map subresource other than UINT_MAX");
        return;
    }
    const u32 dst_x = decode ? m.arg32(3) : 0u;
    const u32 dst_y = decode ? m.arg32(4) : 0u;
    if (min_mip) {
        if (rect != 0) {
            record_error(list, E_INVALIDARG, name, "a source rectangle on a MIN_MIP transcode");
            return;
        }
    } else {
        if (other_sub != kAll) {
            record_error(list, E_INVALIDARG, name,
                         "a per-subresource MIP_REGION_USED transcode");
            return;
        }
        if (rect != 0 || m.arg32(3) != 0 || m.arg32(4) != 0) {
            unimplemented("ID3D12GraphicsCommandList1::ResolveSubresourceRegion",
                          "a MIP_REGION_USED transcode with a rectangle or offset");
            record_error(list, E_INVALIDARG, name, "MIP_REGION_USED rectangle");
            return;
        }
    }
    // The transcode resource: at least the region grid, R8_UINT (a texture,
    // or for MIN_MIP a buffer of at least width x height bytes per slice).
    if (other.is_buffer()) {
        const u64 per_slice = static_cast<u64>(g.grid_width[0]) * g.grid_height[0];
        const u64 slices = other_sub == kAll ? g.slices : 1u;
        if (!min_mip || other.desc.width < per_slice * slices || dst_x != 0 || dst_y != 0) {
            record_error(list, E_INVALIDARG, name, "a feedback transcode buffer too small");
            return;
        }
    } else {
        const u32 mips = std::max(1u, other.desc.mip_levels);
        const u32 slices = other.array_size();
        if (other.desc.dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            other.desc.sample_count > 1 || !r8_transcode_format(other.desc.format) ||
            other.desc.width < static_cast<u64>(g.grid_width[0]) + dst_x ||
            other.desc.height < g.grid_height[0] + dst_y ||
            (!min_mip && (mips < g.mips || slices < g.slices)) ||
            (min_mip && other_sub == kAll && slices < g.slices) ||
            (min_mip && other_sub != kAll && other_sub >= mips * slices)) {
            record_error(list, E_INVALIDARG, name, "an invalid feedback transcode resource");
            return;
        }
    }
    cmd::ResolveSubresource c{d.address, dst_serial, dst_sub, s.address, src_serial, src_sub,
                              format};
    c.dst_x = dst_x;
    c.dst_y = dst_y;
    c.mode = mode;
    rec.commands.emplace_back(c);
}

void Replayer::transcode_feedback(Resource& dst, Resource& src, const cmd::ResolveSubresource& c) {
    const bool decode = c.mode == D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK;
    Resource& map = decode ? src : dst;
    Resource& other = decode ? dst : src;
    const u32 other_sub = decode ? c.dst_sub : c.src_sub;
    const fb::Geometry& g = map.feedback;
    const bool min_mip = map.desc.format == kFeedbackMinMipFormat;
    graphics::BackendBuffer* storage = map.buffer();
    if (storage == nullptr) return;
    const char* method = "ResolveSubresourceRegion";
    // The bits are read and written on the CPU once the GPU work before
    // the transcode has finished (as the depth-stencil plane copies do).
    wait_for_gpu(f.m_backend);
    const usize data_offset = static_cast<usize>(map.offset) + fb::kHeaderWords * 4u;
    std::vector<u32> words(static_cast<usize>(g.words_per_slice) * g.slices, 0u);
    if (!read_range(f.m_backend, *storage, data_offset, words.data(), words.size() * 4u)) {
        unimplemented(method, "a feedback map the backend cannot read back");
        return;
    }
    // Slices this transcode covers, and where each lands in `other`.
    u32 first = 0, count = g.slices, other_mip = 0;
    if (min_mip && other_sub != kAll && !other.is_buffer()) {
        const u32 mips = std::max(1u, other.desc.mip_levels);
        other_mip = other_sub % mips;
        first = other_sub / mips;
        count = 1;
    } else if (min_mip && other_sub != kAll) {
        count = 1;
    }
    if (first >= g.slices) return;
    count = std::min(count, g.slices - first);
    const u32 gw = g.grid_width[0], gh = g.grid_height[0];
    for (u32 k = 0; k < count; ++k) {
        const u32 slice = first + k;
        SliceBits bits{g, words.data() + static_cast<usize>(g.words_per_slice) * slice};
        // other's slice: the same index (a whole-resource MIN_MIP transcode
        // of an array maps slice to slice).
        const u32 other_slice = slice;
        if (min_mip) {
            if (other.is_buffer()) {
                graphics::BackendBuffer* buffer = other.buffer();
                if (buffer == nullptr) return;
                const usize at = static_cast<usize>(other.offset) +
                                 static_cast<usize>(gw) * gh * (other_sub == kAll ? slice : 0u);
                std::vector<u8> grid(static_cast<usize>(gw) * gh);
                if (decode) {
                    for (u32 y = 0; y < gh; ++y)
                        for (u32 x = 0; x < gw; ++x) grid[y * gw + x] = min_mip_at(bits, x, y);
                    if (!write_range(f.m_backend, *buffer, at, grid.data(), grid.size())) {
                        unimplemented(method, "a decode into a buffer the backend refused");
                    }
                } else {
                    if (!read_range(f.m_backend, *buffer, at, grid.data(), grid.size())) {
                        unimplemented(method, "an encode from a buffer the backend cannot read");
                        return;
                    }
                    std::fill(bits.words, bits.words + g.words_per_slice, 0u);
                    for (u32 y = 0; y < gh; ++y)
                        for (u32 x = 0; x < gw; ++x) {
                            const u8 v = grid[y * gw + x];
                            if (v == 0xFF) continue;
                            const u32 mip = std::min<u32>(v, g.mips - 1u);
                            bits.set(mip, x >> mip, y >> mip);
                        }
                }
                continue;
            }
            graphics::BackendTexture2D* tex = other.texture.get();
            if (tex == nullptr) return;
            const u32 w = std::max(1u, tex->desc().width >> other_mip);
            const u32 h = std::max(1u, tex->desc().height >> other_mip);
            std::vector<u8> level(static_cast<usize>(w) * h, 0);
            if (!tex->readback_slice(other_mip, other_slice, level.data(), w)) {
                unimplemented(method, "an R8_UINT texture the backend cannot read back");
                return;
            }
            if (decode) {
                for (u32 y = 0; y < gh && y + c.dst_y < h; ++y)
                    for (u32 x = 0; x < gw && x + c.dst_x < w; ++x)
                        level[(y + c.dst_y) * w + x + c.dst_x] = min_mip_at(bits, x, y);
                if (!tex->update_slice(other_mip, other_slice, level.data(), w)) {
                    unimplemented(method, "a decode into a texture the backend refused");
                }
            } else {
                std::fill(bits.words, bits.words + g.words_per_slice, 0u);
                for (u32 y = 0; y < gh && y < h; ++y)
                    for (u32 x = 0; x < gw && x < w; ++x) {
                        const u8 v = level[y * w + x];
                        if (v == 0xFF) continue;
                        const u32 mip = std::min<u32>(v, g.mips - 1u);
                        bits.set(mip, x >> mip, y >> mip);
                    }
            }
            continue;
        }
        // MIP_REGION_USED: mip m of the map <-> mip m of the texture.
        graphics::BackendTexture2D* tex = other.texture.get();
        if (tex == nullptr) return;
        if (!decode) std::fill(bits.words, bits.words + g.words_per_slice, 0u);
        for (u32 mip = 0; mip < g.mips; ++mip) {
            const u32 w = std::max(1u, tex->desc().width >> mip);
            const u32 h = std::max(1u, tex->desc().height >> mip);
            std::vector<u8> level(static_cast<usize>(w) * h, 0);
            if (!tex->readback_slice(mip, other_slice, level.data(), w)) {
                unimplemented(method, "an R8_UINT texture the backend cannot read back");
                return;
            }
            const u32 rw = std::min(w, g.grid_width[mip]);
            const u32 rh = std::min(h, g.grid_height[mip]);
            if (decode) {
                for (u32 y = 0; y < rh; ++y)
                    for (u32 x = 0; x < rw; ++x) level[y * w + x] = bits.get(mip, x, y) ? 0xFF : 0x00;
                if (!tex->update_slice(mip, other_slice, level.data(), w)) {
                    unimplemented(method, "a decode into a texture the backend refused");
                }
            } else {
                for (u32 y = 0; y < rh; ++y)
                    for (u32 x = 0; x < rw; ++x)
                        if (level[y * w + x] != 0) bits.set(mip, x, y);
            }
        }
    }
    if (!decode && !write_range(f.m_backend, *storage, data_offset, words.data(),
                                words.size() * 4u)) {
        unimplemented(method, "an encode into a feedback map the backend refused");
    }
}

}  // namespace vitreo::winapi::d3d12
