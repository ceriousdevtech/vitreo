// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_tiled.cpp
/// @brief D3D11.2 tiled resources, TiledResourcesTier 1: tile pools, tiled
///        buffers and 2D textures, ID3D11Device2::GetResourceTiling and the
///        ID3D11DeviceContext2 mapping, copy and barrier methods.
///
/// Sources: Learn "Tiled resources" (creation parameters, tile pool creation,
/// Tier 1, duplicate-mapping limits), the ID3D11DeviceContext2 method pages
/// (UpdateTileMappings, CopyTileMappings, CopyTiles, UpdateTiles,
/// ResizeTilePool, TiledResourceBarrier), ID3D11Device2::GetResourceTiling,
/// D3D11_PACKED_MIP_DESC, D3D11_TILE_REGION_SIZE,
/// D3D11_TILED_RESOURCE_COORDINATE; everything they leave open measured on
/// Windows (tools/oracle_probe/d3d11_tiled_probe.c, both bitnesses).
///
/// The frontend owns the contract: a resource's tile numbering, its one pool,
/// the pool tile each tile shows, every validation rule and the D3D linear
/// layout of CopyTiles / UpdateTiles. The backend (metal_tiled.hpp) turns a
/// list of final per-tile mappings into sparse-page updates ordered in the
/// command stream, and pool tiles into memory as they are first mapped.
///
/// Tier 1 leaves NULL-mapped reads and writes undefined; this backend reads
/// zeros and drops writes there (Metal sparse tier 1), one of the allowed
/// outcomes. Packed mips are the backend's own (a Metal mip tail), which the
/// tier allows: GetResourceTiling reports them as they are.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_format_caps.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/format_converter.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <map>
#include <vector>

namespace vitreo::winapi {
namespace {

constexpr u64 kTileBytes = graphics::BACKEND_TILE_BYTES;
static_assert(kTileBytes == D3D11_2_TILED_RESOURCE_TILE_SIZE_IN_BYTES);

/// D3D11_TILED_RESOURCE_COORDINATE (d3d11_2.h): four UINTs.
struct Coordinate {
    u32 x{0};
    u32 y{0};
    u32 z{0};
    u32 subresource{0};
};

/// D3D11_TILE_REGION_SIZE (d3d11_2.h): UINT NumTiles, BOOL bUseBox,
/// UINT Width, UINT16 Height, UINT16 Depth -- 16 bytes.
struct RegionSize {
    u32 num_tiles{0};
    bool use_box{false};
    u32 width{0};
    u32 height{0};
    u32 depth{0};
};

constexpr u32 kRegionBytes = 16;
constexpr u32 kCoordinateBytes = 16;
/// A tile a box region names outside its subresource: it takes its place in
/// the range walk but maps nothing (measured WARP: such boxes answer S_OK).
constexpr u32 kOutsideTile = 0xFFFFFFFFu;

bool read_coordinate(GuestMemory& memory, GuestAddr address, Coordinate& out) {
    std::array<u32, 4> words{};
    for (usize i = 0; i < words.size(); ++i) {
        const auto value = memory.read_u32(address + i * 4u);
        if (value.is_err()) return false;
        words[i] = value.value();
    }
    out = {words[0], words[1], words[2], words[3]};
    return true;
}

bool read_region(GuestMemory& memory, GuestAddr address, RegionSize& out) {
    const auto tiles = memory.read_u32(address);
    const auto box = memory.read_u32(address + 4);
    const auto width = memory.read_u32(address + 8);
    const auto extents = memory.read_u32(address + 12);
    if (tiles.is_err() || box.is_err() || width.is_err() ||
        extents.is_err()) {
        return false;
    }
    out.num_tiles = tiles.value();
    out.use_box = box.value() != 0;
    out.width = width.value();
    out.height = extents.value() & 0xFFFFu;
    out.depth = extents.value() >> 16;
    return true;
}

/// A region's tile count as the range walk counts it (D3D11_TILE_REGION_SIZE:
/// "NumTiles should be equal to Width * Height * Depth" with bUseBox).
bool region_count(const RegionSize& region, u64& count) {
    if (region.use_box) {
        if (region.width == 0 || region.height == 0 || region.depth == 0) {
            return false;
        }
        const u64 box = static_cast<u64>(region.width) * region.height *
                        region.depth;
        if (box != region.num_tiles) return false;
        count = box;
        return true;
    }
    if (region.num_tiles == 0) return false;
    count = region.num_tiles;
    return true;
}

}  // namespace

D3D11Manager::TiledState::~TiledState() {
    if (!pool || !pool->backend) return;
    for (const auto& [tile, pool_tile] : mapped) {
        pool->backend->unreference(pool_tile);
    }
}

u32 D3D11Manager::tiled_resources_tier(u32 feature_level) const {
    return feature_level >= d3d11::FEATURE_LEVEL_11_0 &&
                   m_backend.supports_tiled_resources()
               ? 1u
               : 0u;
}

u64 D3D11Manager::arg_u64(Marshaler& m, usize index) const {
    if (m_is_64bit) return m.arg(index);
    return (m.arg(index) & 0xFFFFFFFFull) | (m.arg(index + 1) << 32);
}

std::unique_ptr<D3D11Manager::TiledState> D3D11Manager::tiled_buffer_state(
    u32 byte_width) const {
    auto state = std::make_unique<TiledState>();
    state->buffer = true;
    state->tile_width = static_cast<u32>(kTileBytes);
    state->tile_height = 1;
    const u32 tiles = static_cast<u32>(
        (static_cast<u64>(byte_width) + kTileBytes - 1) / kTileBytes);
    state->standard_mips = 1;
    state->tiles_per_slice = tiles;
    state->total_tiles = tiles;
    state->mip_first = {0};
    state->mip_tiles_x = {tiles};
    state->mip_tiles_y = {1};
    return state;
}

std::unique_ptr<D3D11Manager::TiledState> D3D11Manager::tiled_texture_state(
    const Texture2DDesc& desc, graphics::BackendTexture2D* backend) {
    graphics::BackendTiledLayout layout{};
    if (backend == nullptr || !m_backend.tiled_texture_layout(backend, layout) ||
        layout.tile_width == 0 || layout.tile_height == 0) {
        return nullptr;
    }
    auto state = std::make_unique<TiledState>();
    state->tile_width = layout.tile_width;
    state->tile_height = layout.tile_height;
    state->mip_levels = std::max(1u, desc.mip_levels);
    state->array_size = std::max(1u, desc.array_size);
    state->standard_mips =
        std::min(layout.first_packed_level, state->mip_levels);
    state->packed_tiles =
        state->standard_mips < state->mip_levels ? layout.packed_tiles : 0u;
    // Tier 1: an array has no packed mips (texture_desc_violation keeps every
    // mip of an array whole tiles, which the backend never packs).
    if (state->array_size > 1 && state->packed_tiles != 0) {
        d3d11_unimplemented_once("CreateTexture2D",
                                 "tiled array whose backend packs mips");
        return nullptr;
    }
    u64 first = 0;
    for (u32 mip = 0; mip < state->standard_mips; ++mip) {
        const u32 w = std::max(1u, desc.width >> mip);
        const u32 h = std::max(1u, desc.height >> mip);
        const u32 tx = (w + state->tile_width - 1) / state->tile_width;
        const u32 ty = (h + state->tile_height - 1) / state->tile_height;
        state->mip_first.push_back(static_cast<u32>(first));
        state->mip_tiles_x.push_back(tx);
        state->mip_tiles_y.push_back(ty);
        first += static_cast<u64>(tx) * ty;
    }
    first += state->packed_tiles;
    const u64 total = first * state->array_size;
    if (total > 0xFFFFFFFEull) return nullptr;
    state->tiles_per_slice = static_cast<u32>(first);
    state->total_tiles = static_cast<u32>(total);
    return state;
}

// -- GetResourceTiling --------------------------------------------------------

void D3D11Manager::get_resource_tiling(Marshaler& m) {
    // GetResourceTiling(pTiledResource, pNumTilesForEntireResource,
    // pPackedMipDesc, pStandardTileShapeForNonPackedMips,
    // pNumSubresourceTilings, FirstSubresourceTilingToGet,
    // pSubresourceTilingsForNonPackedMips). void.
    m.set_return(0);
    const GuestAddr resource = m.arg_ptr(1);
    const GuestAddr total_out = m.arg_ptr(2);
    const GuestAddr packed_out = m.arg_ptr(3);
    const GuestAddr shape_out = m.arg_ptr(4);
    const GuestAddr count_io = m.arg_ptr(5);
    const u32 first = static_cast<u32>(m.arg(6));
    const GuestAddr tilings_out = m.arg_ptr(7);

    const TiledState* state = nullptr;
    if (auto buffer = m_buffers.find(resource); buffer != m_buffers.end()) {
        state = buffer->second.tiled.get();
    } else if (auto texture = m_textures.find(resource);
               texture != m_textures.end()) {
        state = texture->second.tiled.get();
    }
    if (state == nullptr) {
        // Not a tiled resource: every output zero, no subresource tiling
        // written (measured: buffer, texture and tile pool alike).
        if (total_out != 0) (void)m_memory.write_u32(total_out, 0);
        for (u32 i = 0; packed_out != 0 && i < 12; i += 4) {
            (void)m_memory.write_u32(packed_out + i, 0);
        }
        for (u32 i = 0; shape_out != 0 && i < 12; i += 4) {
            (void)m_memory.write_u32(shape_out + i, 0);
        }
        if (count_io != 0) (void)m_memory.write_u32(count_io, 0);
        return;
    }

    const u32 packed_mips = state->mip_levels - state->standard_mips;
    if (total_out != 0) (void)m_memory.write_u32(total_out, state->total_tiles);
    if (packed_out != 0) {
        // D3D11_PACKED_MIP_DESC: UINT8 NumStandardMips, UINT8 NumPackedMips,
        // UINT NumTilesForPackedMips, UINT StartTileIndexInOverallResource
        // ("the offset for the tiles that contain the packed mipmaps for the
        // first array slice"; 0 and 0 when nothing is packed).
        (void)m_memory.write_u32(packed_out,
                                 (state->standard_mips & 0xFFu) |
                                     ((packed_mips & 0xFFu) << 8));
        (void)m_memory.write_u32(packed_out + 4,
                                 packed_mips != 0 ? state->packed_tiles : 0u);
        (void)m_memory.write_u32(
            packed_out + 8, packed_mips != 0
                                ? state->tiles_per_slice - state->packed_tiles
                                : 0u);
    }
    if (shape_out != 0) {
        // D3D11_TILE_SHAPE: zeros when the whole resource is packed ("the
        // tiled resource has no defined layout for packed mipmaps"). A
        // buffer's tile is 65536 x 1 x 1 (measured).
        const bool any_standard = state->standard_mips != 0;
        (void)m_memory.write_u32(shape_out,
                                 any_standard ? state->tile_width : 0u);
        (void)m_memory.write_u32(shape_out + 4,
                                 any_standard ? state->tile_height : 0u);
        (void)m_memory.write_u32(shape_out + 8, any_standard ? 1u : 0u);
    }
    if (count_io == 0) return;
    const auto requested = m_memory.read_u32(count_io);
    if (requested.is_err()) return;
    const u32 subresources = state->mip_levels * state->array_size;
    // "clamped to what's available" -- measured: FirstSubresourceTilingToGet
    // 5 with 100 requested of 9 answers 4; a first past the end keeps the
    // requested count and describes those subresources by the same
    // arithmetic (slice = index / MipLevels), as WARP does.
    u32 count = requested.value();
    if (first < subresources) count = std::min(count, subresources - first);
    (void)m_memory.write_u32(count_io, count);
    if (tilings_out == 0) return;
    for (u32 i = 0; i < count; ++i) {
        const u32 subresource = first + i;
        const u32 mip = subresource % state->mip_levels;
        const u32 slice = subresource / state->mip_levels;
        // D3D11_SUBRESOURCE_TILING: UINT WidthInTiles, UINT16 HeightInTiles,
        // UINT16 DepthInTiles, UINT StartTileIndexInOverallResource. A packed
        // subresource is zeros and D3D11_PACKED_TILE.
        const GuestAddr entry = tilings_out + static_cast<GuestAddr>(i) * 12u;
        if (mip < state->standard_mips) {
            (void)m_memory.write_u32(entry, state->mip_tiles_x[mip]);
            (void)m_memory.write_u32(entry + 4,
                                     (state->mip_tiles_y[mip] & 0xFFFFu) |
                                         (1u << 16));
            (void)m_memory.write_u32(
                entry + 8, slice * state->tiles_per_slice +
                               state->mip_first[mip]);
        } else {
            (void)m_memory.write_u32(entry, 0);
            (void)m_memory.write_u32(entry + 4, 0);
            (void)m_memory.write_u32(entry + 8, D3D11_PACKED_TILE);
        }
    }
}

// -- Tile walks ---------------------------------------------------------------

namespace {

/// Where tile @p index of a tiled resource is, for the backend.
graphics::BackendTileCoord tile_coord(const D3D11Manager::TiledState& state,
                                      u32 index) {
    graphics::BackendTileCoord coord{};
    if (state.buffer) {
        coord.x = index;
        return coord;
    }
    coord.slice = index / state.tiles_per_slice;
    const u32 within = index % state.tiles_per_slice;
    const u32 packed_first = state.tiles_per_slice - state.packed_tiles;
    if (within >= packed_first) {
        coord.packed = true;
        coord.x = within - packed_first;
        return coord;
    }
    for (u32 mip = 0; mip < state.standard_mips; ++mip) {
        const u32 tiles = state.mip_tiles_x[mip] * state.mip_tiles_y[mip];
        if (within < state.mip_first[mip] + tiles) {
            const u32 local = within - state.mip_first[mip];
            coord.level = mip;
            coord.x = local % state.mip_tiles_x[mip];
            coord.y = local / state.mip_tiles_x[mip];
            return coord;
        }
    }
    return coord;
}

/// The tiles a region names, in the order the range walk visits them (x,
/// then y, then z for a box; tile order, spilling across subresources, for
/// a count). A box tile outside its subresource is kOutsideTile.
/// @p strict refuses such boxes (CopyTileMappings, CopyTiles) instead.
/// False: the region is invalid (E_INVALIDARG).
bool region_tiles(const D3D11Manager::TiledState& state,
                  const Coordinate& at, const RegionSize& region, bool strict,
                  std::vector<u32>& tiles) {
    u64 count = 0;
    if (!region_count(region, count)) return false;
    if (state.buffer) {
        // A buffer is one row of tiles; Y and Z are not looked at, and a box
        // is its Width (measured: a 2x2 box at (1,1) maps tiles 1 and 2).
        if (at.subresource != 0) return false;
        const u64 run = region.use_box ? region.width : count;
        if (static_cast<u64>(at.x) + run > state.total_tiles) return false;
        for (u64 i = 0; i < count; ++i) {
            tiles.push_back(i < run ? at.x + static_cast<u32>(i)
                                    : kOutsideTile);
        }
        return true;
    }
    const u32 subresources = state.mip_levels * state.array_size;
    if (at.subresource >= subresources) return false;
    const u32 mip = at.subresource % state.mip_levels;
    const u32 slice = at.subresource / state.mip_levels;
    if (mip >= state.standard_mips) {
        // Packed: a count only, X an offset into the slice's packed tiles,
        // Y and Z zero (D3D11_TILE_REGION_SIZE); every packed subresource
        // names the same tiles (D3D11_TILED_RESOURCE_COORDINATE).
        if (region.use_box || at.y != 0 || at.z != 0 ||
            at.x >= state.packed_tiles) {
            return false;
        }
        const u64 start = static_cast<u64>(slice) * state.tiles_per_slice +
                          (state.tiles_per_slice - state.packed_tiles) + at.x;
        if (start + count > state.total_tiles) return false;
        for (u64 i = 0; i < count; ++i) {
            tiles.push_back(static_cast<u32>(start + i));
        }
        return true;
    }
    const u32 tx = state.mip_tiles_x[mip];
    const u32 ty = state.mip_tiles_y[mip];
    if (!region.use_box) {
        // Tile order from (X, Y) of the subresource, spilling into the
        // following subresources; past the last tile of the resource is out
        // of bounds (WARP removes the device; refused here).
        const u64 start = static_cast<u64>(slice) * state.tiles_per_slice +
                          state.mip_first[mip] + static_cast<u64>(at.y) * tx +
                          at.x;
        if (start + count > state.total_tiles) return false;
        for (u64 i = 0; i < count; ++i) {
            tiles.push_back(static_cast<u32>(start + i));
        }
        return true;
    }
    // A box: Width x Height tiles of the mip, Depth consecutive array slices
    // of it (D3D11_TILE_REGION_SIZE: "For arrays, used for advancing in
    // depth jumps to next slice of same mipmap size"). Z addresses a 3D
    // texture's depth; a Texture2D has one tile of depth.
    for (u32 z = 0; z < region.depth; ++z) {
        for (u32 y = 0; y < region.height; ++y) {
            for (u32 x = 0; x < region.width; ++x) {
                const u64 px = static_cast<u64>(at.x) + x;
                const u64 py = static_cast<u64>(at.y) + y;
                const u64 ps = static_cast<u64>(slice) + z;
                if (at.z != 0 || px >= tx || py >= ty ||
                    ps >= state.array_size) {
                    if (strict) return false;
                    tiles.push_back(kOutsideTile);
                    continue;
                }
                tiles.push_back(static_cast<u32>(
                    ps * state.tiles_per_slice + state.mip_first[mip] +
                    py * tx + px));
            }
        }
    }
    return true;
}

}  // namespace

// -- Applying mappings ----------------------------------------------------------

namespace {

/// A tile's new mapping in one call: a pool tile or unmapped.
using FinalMappings = std::map<u32, u64>;

}  // namespace

/// Point @p state's tiles at @p changes (pool tiles of state.pool, or
/// BACKEND_TILE_UNMAPPED) and tell the backend, in order with the recorded
/// work. Everything or nothing: E_OUTOFMEMORY when pool memory cannot be had.
static u32 apply_tile_mappings(D3D11Manager::TiledState& state,
                               graphics::GraphicsBackendDevice& backend,
                               graphics::BackendTexture2D* texture,
                               graphics::BackendBuffer* buffer,
                               const FinalMappings& changes) {
    std::vector<graphics::BackendTileMapping> updates;
    std::vector<u64> referenced;
    graphics::BackendTilePool* pool =
        state.pool ? state.pool->backend.get() : nullptr;
    for (const auto& [tile, pool_tile] : changes) {
        auto old = state.mapped.find(tile);
        const u64 before = old != state.mapped.end()
                               ? old->second
                               : graphics::BACKEND_TILE_UNMAPPED;
        if (before == pool_tile) continue;
        if (pool_tile != graphics::BACKEND_TILE_UNMAPPED) {
            if (pool == nullptr || !pool->reference(pool_tile)) {
                for (const u64 taken : referenced) pool->unreference(taken);
                return 0x8007000Eu;  // E_OUTOFMEMORY, mappings unchanged
            }
            referenced.push_back(pool_tile);
        }
        updates.push_back({tile_coord(state, tile), pool_tile});
    }
    if (updates.empty()) return d3d11::S_OK;
    // Commit: drop the old references, keep the new ones.
    for (const auto& [tile, pool_tile] : changes) {
        auto old = state.mapped.find(tile);
        const u64 before = old != state.mapped.end()
                               ? old->second
                               : graphics::BACKEND_TILE_UNMAPPED;
        if (before == pool_tile) continue;
        if (before != graphics::BACKEND_TILE_UNMAPPED) {
            if (pool != nullptr) pool->unreference(before);
            auto uses = state.pool_tile_uses.find(before);
            if (uses != state.pool_tile_uses.end()) {
                if (uses->second == 2) --state.shared_pool_tiles;
                if (--uses->second == 0) state.pool_tile_uses.erase(uses);
            }
        }
        if (pool_tile == graphics::BACKEND_TILE_UNMAPPED) {
            state.mapped.erase(tile);
        } else {
            state.mapped[tile] = pool_tile;
            if (++state.pool_tile_uses[pool_tile] == 2) {
                ++state.shared_pool_tiles;
            }
        }
    }
    const bool self_aliased = state.shared_pool_tiles != 0;
    const bool recorded =
        texture != nullptr
            ? backend.update_tile_mappings(texture, pool, updates.data(),
                                           updates.size(), self_aliased)
            : backend.update_tile_mappings(buffer, pool, updates.data(),
                                           updates.size(), self_aliased);
    if (!recorded) {
        d3d11_unimplemented_once("UpdateTileMappings",
                                 "backend refused a tile-mapping update");
    }
    return d3d11::S_OK;
}

/// Move @p state to @p pool: a tiled resource points into one pool at a time,
/// and naming another clears every mapping it had (Learn UpdateTileMappings;
/// measured for CopyTileMappings too, d3d11_tiled_probe [data] T10).
static void switch_tile_pool(D3D11Manager::TiledState& state,
                             graphics::GraphicsBackendDevice& backend,
                             graphics::BackendTexture2D* texture,
                             graphics::BackendBuffer* buffer,
                             const std::shared_ptr<D3D11Manager::TilePool>& pool) {
    if (state.pool == pool) return;
    if (!state.mapped.empty()) {
        FinalMappings cleared;
        for (const auto& [tile, pool_tile] : state.mapped) {
            cleared[tile] = graphics::BACKEND_TILE_UNMAPPED;
        }
        (void)apply_tile_mappings(state, backend, texture, buffer, cleared);
    }
    state.pool = pool;
}

// -- UpdateTileMappings -----------------------------------------------------------

u32 D3D11Manager::update_tile_mappings(Marshaler& m) {
    // UpdateTileMappings(pTiledResource, NumTiledResourceRegions,
    // pTiledResourceRegionStartCoordinates, pTiledResourceRegionSizes,
    // pTilePool, NumRanges, pRangeFlags, pTilePoolStartOffsets,
    // pRangeTileCounts, Flags).
    const GuestAddr resource = m.arg_ptr(1);
    const u32 region_count_arg = static_cast<u32>(m.arg(2));
    const GuestAddr coordinates = m.arg_ptr(3);
    const GuestAddr sizes = m.arg_ptr(4);
    const GuestAddr pool_address = m.arg_ptr(5);
    const u32 range_count = static_cast<u32>(m.arg(6));
    const GuestAddr range_flags = m.arg_ptr(7);
    const GuestAddr pool_offsets = m.arg_ptr(8);
    const GuestAddr range_tile_counts = m.arg_ptr(9);
    const u32 flags = static_cast<u32>(m.arg(10));

    TiledState* state = nullptr;
    graphics::BackendTexture2D* texture = nullptr;
    graphics::BackendBuffer* buffer = nullptr;
    if (auto b = m_buffers.find(resource); b != m_buffers.end()) {
        state = b->second.tiled.get();
        buffer = b->second.backend.get();
    } else if (auto t = m_textures.find(resource); t != m_textures.end()) {
        state = t->second.tiled.get();
        texture = t->second.backend.get();
    }
    // Measured E_INVALIDARG: a resource that is not tiled; any Flags bit but
    // NO_OVERWRITE; no regions or no ranges; several regions without
    // coordinates.
    if (state == nullptr ||
        (flags & ~D3D11_TILE_MAPPING_NO_OVERWRITE) != 0 ||
        region_count_arg == 0 || range_count == 0 ||
        (region_count_arg > 1 && coordinates == 0)) {
        return d3d11::E_INVALIDARG;
    }
    if (region_count_arg > state->total_tiles + 1ull ||
        range_count > 0x100000u) {
        return d3d11::E_INVALIDARG;
    }

    // The regions. Learn: with one region either array may be NULL -- NULL
    // coordinates are all zeros; NULL sizes with NULL coordinates are the
    // whole resource, with coordinates one tile each.
    std::vector<u32> tiles;
    for (u32 r = 0; r < region_count_arg; ++r) {
        Coordinate at{};
        if (coordinates != 0 &&
            !read_coordinate(m_memory, coordinates + r * kCoordinateBytes,
                             at)) {
            return d3d11::E_INVALIDARG;
        }
        RegionSize region{};
        if (sizes != 0) {
            if (!read_region(m_memory, sizes + r * kRegionBytes, region)) {
                return d3d11::E_INVALIDARG;
            }
        } else {
            region.num_tiles = coordinates == 0 ? state->total_tiles : 1u;
        }
        if (!region_tiles(*state, at, region, false, tiles)) {
            return d3d11::E_INVALIDARG;
        }
    }

    // The ranges. pRangeTileCounts NULL: one range covers every region tile;
    // several are one tile each (Learn's disjoint-tiles example; measured: 2
    // ranges for 2 tiles S_OK, for 4 E_INVALIDARG). Flags other than 0, NULL,
    // SKIP and REUSE_SINGLE_TILE (and their combinations) are E_INVALIDARG;
    // offsets are needed by the ranges that map (measured).
    struct Range {
        u32 flag{0};
        u32 offset{0};
        u64 count{0};
    };
    std::vector<Range> ranges(range_count);
    u64 range_total = 0;
    bool needs_pool = false;
    for (u32 i = 0; i < range_count; ++i) {
        Range& range = ranges[i];
        if (range_flags != 0) {
            const auto flag = m_memory.read_u32(range_flags + i * 4u);
            if (flag.is_err()) return d3d11::E_INVALIDARG;
            range.flag = flag.value();
        }
        if (range.flag != 0 && range.flag != D3D11_TILE_RANGE_NULL &&
            range.flag != D3D11_TILE_RANGE_SKIP &&
            range.flag != D3D11_TILE_RANGE_REUSE_SINGLE_TILE) {
            return d3d11::E_INVALIDARG;
        }
        const bool maps = range.flag == 0 ||
                          range.flag == D3D11_TILE_RANGE_REUSE_SINGLE_TILE;
        if (maps) {
            if (pool_offsets == 0) return d3d11::E_INVALIDARG;
            const auto offset = m_memory.read_u32(pool_offsets + i * 4u);
            if (offset.is_err()) return d3d11::E_INVALIDARG;
            range.offset = offset.value();
        }
        if (range.flag != D3D11_TILE_RANGE_NULL) needs_pool = true;
        if (range_tile_counts != 0) {
            const auto count = m_memory.read_u32(range_tile_counts + i * 4u);
            if (count.is_err()) return d3d11::E_INVALIDARG;
            range.count = count.value();
        } else {
            range.count = range_count == 1 ? tiles.size() : 1u;
        }
        range_total += range.count;
    }
    if (range_total != tiles.size()) return d3d11::E_INVALIDARG;

    // The pool. A named one must be a tile pool; with none, the resource's
    // current pool, which only NULL mappings can do without (measured: SKIP
    // and REUSE on a never-mapped resource with pTilePool NULL are
    // E_INVALIDARG, NULL mappings S_OK).
    std::shared_ptr<TilePool> pool = state->pool;
    if (pool_address != 0) {
        auto p = m_buffers.find(pool_address);
        if (p == m_buffers.end() || !p->second.tile_pool) {
            return d3d11::E_INVALIDARG;
        }
        pool = p->second.tile_pool;
    }
    if (needs_pool && !pool) return d3d11::E_INVALIDARG;
    // New mappings stay inside the pool's current size (ResizeTilePool
    // remarks; measured: a tile past a shrunk pool's end is E_INVALIDARG).
    for (const Range& range : ranges) {
        if (range.count == 0) continue;
        if (range.flag == 0 &&
            static_cast<u64>(range.offset) + range.count > pool->tiles) {
            return d3d11::E_INVALIDARG;
        }
        if (range.flag == D3D11_TILE_RANGE_REUSE_SINGLE_TILE &&
            range.offset >= pool->tiles) {
            return d3d11::E_INVALIDARG;
        }
    }

    // Walk the region tiles against the ranges; later tiles overwrite
    // earlier ones.
    FinalMappings changes;
    usize next = 0;
    for (const Range& range : ranges) {
        for (u64 j = 0; j < range.count; ++j, ++next) {
            const u32 tile = tiles[next];
            if (tile == kOutsideTile ||
                range.flag == D3D11_TILE_RANGE_SKIP) {
                continue;
            }
            u64 target = graphics::BACKEND_TILE_UNMAPPED;
            if (range.flag == 0) target = static_cast<u64>(range.offset) + j;
            if (range.flag == D3D11_TILE_RANGE_REUSE_SINGLE_TILE) {
                target = range.offset;
            }
            changes[tile] = target;
        }
    }
    if (pool_address != 0) {
        switch_tile_pool(*state, m_backend, texture, buffer, pool);
    }
    return apply_tile_mappings(*state, m_backend, texture, buffer, changes);
}

// -- CopyTileMappings -------------------------------------------------------------

u32 D3D11Manager::copy_tile_mappings(Marshaler& m) {
    // CopyTileMappings(pDestTiledResource, pDestRegionStartCoordinate,
    // pSourceTiledResource, pSourceRegionStartCoordinate, pTileRegionSize,
    // Flags).
    const GuestAddr dst = m.arg_ptr(1);
    const GuestAddr dst_coordinate = m.arg_ptr(2);
    const GuestAddr src = m.arg_ptr(3);
    const GuestAddr src_coordinate = m.arg_ptr(4);
    const GuestAddr size_address = m.arg_ptr(5);
    const u32 flags = static_cast<u32>(m.arg(6));
    const auto find_state = [this](GuestAddr address,
                                   graphics::BackendTexture2D*& texture,
                                   graphics::BackendBuffer*& buffer)
        -> TiledState* {
        if (auto b = m_buffers.find(address); b != m_buffers.end()) {
            buffer = b->second.backend.get();
            return b->second.tiled.get();
        }
        if (auto t = m_textures.find(address); t != m_textures.end()) {
            texture = t->second.backend.get();
            return t->second.tiled.get();
        }
        return nullptr;
    };
    graphics::BackendTexture2D* dst_texture = nullptr;
    graphics::BackendBuffer* dst_buffer = nullptr;
    graphics::BackendTexture2D* src_texture = nullptr;
    graphics::BackendBuffer* src_buffer = nullptr;
    TiledState* dst_state = find_state(dst, dst_texture, dst_buffer);
    TiledState* src_state = find_state(src, src_texture, src_buffer);
    // Learn: "invalid flags or passing in non Tiled Resources result in call
    // being dropped with E_INVALIDARG"; the regions "must each entirely fit
    // in their resource" (WARP removes the device; refused here).
    Coordinate dst_at{};
    Coordinate src_at{};
    RegionSize region{};
    if (dst_state == nullptr || src_state == nullptr ||
        (flags & ~D3D11_TILE_MAPPING_NO_OVERWRITE) != 0 ||
        dst_coordinate == 0 || src_coordinate == 0 || size_address == 0 ||
        !read_coordinate(m_memory, dst_coordinate, dst_at) ||
        !read_coordinate(m_memory, src_coordinate, src_at) ||
        !read_region(m_memory, size_address, region)) {
        return d3d11::E_INVALIDARG;
    }
    std::vector<u32> dst_tiles;
    std::vector<u32> src_tiles;
    if (!region_tiles(*dst_state, dst_at, region, true, dst_tiles) ||
        !region_tiles(*src_state, src_at, region, true, src_tiles) ||
        dst_tiles.size() != src_tiles.size()) {
        return d3d11::E_INVALIDARG;
    }
    // "As if the source was saved to a temp location": read every source
    // mapping before writing any.
    FinalMappings changes;
    for (usize i = 0; i < dst_tiles.size(); ++i) {
        if (dst_tiles[i] == kOutsideTile) continue;
        u64 target = graphics::BACKEND_TILE_UNMAPPED;
        if (src_tiles[i] != kOutsideTile) {
            auto found = src_state->mapped.find(src_tiles[i]);
            if (found != src_state->mapped.end()) target = found->second;
        }
        changes[dst_tiles[i]] = target;
    }
    // The destination moves to the source's pool (measured T10).
    if (src_state->pool) {
        switch_tile_pool(*dst_state, m_backend, dst_texture, dst_buffer,
                         src_state->pool);
    }
    return apply_tile_mappings(*dst_state, m_backend, dst_texture, dst_buffer,
                               changes);
}

// -- ResizeTilePool ---------------------------------------------------------------

u32 D3D11Manager::resize_tile_pool(Marshaler& m) {
    // ResizeTilePool(pTilePool, UINT64 NewSizeInBytes). Learn: E_INVALIDARG
    // unless a multiple of 64 KB or 0, the pool unchanged; measured also for
    // a buffer that is not a tile pool and a size a UINT ByteWidth cannot
    // hold (4 GiB), while 0xFFFF0000 grows (x64).
    const GuestAddr pool_address = m.arg_ptr(1);
    const u64 bytes = arg_u64(m, 2);
    auto pool = m_buffers.find(pool_address);
    if (pool == m_buffers.end() || !pool->second.tile_pool ||
        bytes % kTileBytes != 0 || bytes > 0xFFFFFFFFull) {
        return d3d11::E_INVALIDARG;
    }
    TilePool& state = *pool->second.tile_pool;
    state.tiles = bytes / kTileBytes;
    state.backend->set_size(state.tiles);
    // GetDesc follows the new size (measured).
    pool->second.desc.byte_width = static_cast<u32>(bytes);
    return d3d11::S_OK;
}

// -- CopyTiles / UpdateTiles ------------------------------------------------------

namespace {

/// One tile's texels in the D3D linear layout of CopyTiles / UpdateTiles:
/// "linear in memory within 64 KB tiles ... the pitch ... is the full size in
/// bytes of the number pixels that would fit across the tile" (rows of 4x4
/// blocks for a block-compressed format).
struct TileBox {
    u32 level{0};
    u32 slice{0};
    u32 x{0};
    u32 y{0};
    u32 width{0};
    u32 height{0};
    u64 row_pitch{0};
    u64 bytes{0};  ///< bytes of the tile the box covers (from its start)
};

bool texture_tile_box(const D3D11Manager::TiledState& state,
                      const graphics::BackendTexture2DDesc& desc, u32 tile,
                      TileBox& box) {
    const graphics::BackendTileCoord coord = tile_coord(state, tile);
    if (coord.packed) return false;
    const graphics::FormatInfo format =
        graphics::format_table(desc.pixel_format);
    const u32 level_w = std::max(1u, desc.width >> coord.level);
    const u32 level_h = std::max(1u, desc.height >> coord.level);
    box.level = coord.level;
    box.slice = coord.slice;
    box.x = coord.x * state.tile_width;
    box.y = coord.y * state.tile_height;
    if (box.x >= level_w || box.y >= level_h) return false;
    box.width = std::min(state.tile_width, level_w - box.x);
    box.height = std::min(state.tile_height, level_h - box.y);
    if (format.is_compressed) {
        const u64 block = graphics::compressed_block_bytes(desc.pixel_format);
        box.row_pitch = (state.tile_width / 4u) * block;
        box.bytes = ((box.height + 3u) / 4u - 1u) * box.row_pitch +
                    ((box.width + 3u) / 4u) * block;
    } else {
        // A multisampled pixel is its samples in index order (Learn,
        // CopyTiles remarks).
        const u64 pixel = static_cast<u64>(format.bytes_per_pixel) *
                          std::max(1u, desc.sample_count);
        box.row_pitch = static_cast<u64>(state.tile_width) * pixel;
        box.bytes = static_cast<u64>(box.height - 1u) * box.row_pitch +
                    box.width * pixel;
    }
    return true;
}

}  // namespace

void D3D11Manager::copy_tiles(Marshaler& m) {
    // CopyTiles(pTiledResource, pTileRegionStartCoordinate, pTileRegionSize,
    // pBuffer, UINT64 BufferStartOffsetInBytes, Flags). void.
    m.set_return(0);
    const GuestAddr resource = m.arg_ptr(1);
    const GuestAddr coordinate = m.arg_ptr(2);
    const GuestAddr size_address = m.arg_ptr(3);
    const GuestAddr buffer_address = m.arg_ptr(4);
    const u64 offset = arg_u64(m, 5);
    const u32 flags = static_cast<u32>(m.arg(after_u64(5)));
    const u32 to_linear =
        D3D11_TILE_COPY_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER;
    const u32 to_tiled =
        D3D11_TILE_COPY_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE;
    // Exactly one direction, and nothing but NO_OVERWRITE besides (measured:
    // Flags 0, 6 and 0x80000002 drop the copy).
    const u32 direction = flags & (to_linear | to_tiled);
    if ((direction != to_linear && direction != to_tiled) ||
        (flags & ~(to_linear | to_tiled |
                   D3D11_TILE_COPY_NO_OVERWRITE)) != 0) {
        d3d11_diag("CopyTiles flags 0x%x -> dropped", flags);
        return;
    }
    auto linear = m_buffers.find(buffer_address);
    if (linear == m_buffers.end() || !linear->second.backend ||
        linear->second.tile_pool) {
        d3d11_diag("CopyTiles: no linear buffer -> dropped");
        return;
    }
    Buffer* tiled_buffer = nullptr;
    Texture2D* tiled_texture = nullptr;
    TiledState* state = nullptr;
    if (auto b = m_buffers.find(resource); b != m_buffers.end()) {
        tiled_buffer = &b->second;
        state = b->second.tiled.get();
    } else if (auto t = m_textures.find(resource); t != m_textures.end()) {
        tiled_texture = &t->second;
        state = t->second.tiled.get();
    }
    Coordinate at{};
    RegionSize region{};
    std::vector<u32> tiles;
    if (state == nullptr || coordinate == 0 || size_address == 0 ||
        !read_coordinate(m_memory, coordinate, at) ||
        !read_region(m_memory, size_address, region) ||
        !region_tiles(*state, at, region, true, tiles)) {
        d3d11_diag("CopyTiles: invalid resource or region -> dropped");
        return;
    }
    // The linear side holds every tile of the region, 64 KiB each (measured:
    // an overrun drops the copy; any byte offset works).
    const u64 span = static_cast<u64>(tiles.size()) * kTileBytes;
    const u64 linear_bytes = linear->second.desc.byte_width;
    if (offset > linear_bytes || span > linear_bytes - offset) {
        d3d11_diag("CopyTiles: linear buffer overrun -> dropped");
        return;
    }
    const bool into_tiles = direction == to_tiled;
    bool unimplemented = false;
    for (usize i = 0; i < tiles.size(); ++i) {
        const u64 at_linear = offset + static_cast<u64>(i) * kTileBytes;
        if (tiled_buffer != nullptr) {
            const u64 begin = static_cast<u64>(tiles[i]) * kTileBytes;
            const u64 bytes =
                std::min<u64>(kTileBytes, tiled_buffer->desc.byte_width - begin);
            const bool ok =
                into_tiles
                    ? m_backend.copy_buffer_region(
                          tiled_buffer->backend.get(), begin,
                          linear->second.backend.get(), at_linear, bytes)
                    : m_backend.copy_buffer_region(
                          linear->second.backend.get(), at_linear,
                          tiled_buffer->backend.get(), begin, bytes);
            unimplemented = unimplemented || !ok;
            continue;
        }
        TileBox box{};
        // Packed tiles: "results of the copy operation are undefined"
        // (Learn); nothing is copied for them.
        if (!texture_tile_box(*state, tiled_texture->backend->desc(),
                              tiles[i], box)) {
            continue;
        }
        const bool ok = m_backend.copy_texture_buffer_region(
            tiled_texture->backend.get(), box.level, box.slice, box.x, box.y,
            box.width, box.height, linear->second.backend.get(), at_linear,
            box.row_pitch, into_tiles);
        unimplemented = unimplemented || !ok;
    }
    if (unimplemented) {
        d3d11_unimplemented_once("CopyTiles", "tile the backend cannot copy");
    }
    if (!into_tiles) {
        // The linear buffer was written on the GPU: a Map(READ) of a staging
        // one reads it back.
        linear->second.gpu_newer = true;
    }
    note_gpu_use(linear->second);
}

void D3D11Manager::update_tiles(Marshaler& m) {
    // UpdateTiles(pDestTiledResource, pDestTileRegionStartCoordinate,
    // pDestTileRegionSize, pSourceTileData, Flags). void.
    m.set_return(0);
    const GuestAddr resource = m.arg_ptr(1);
    const GuestAddr coordinate = m.arg_ptr(2);
    const GuestAddr size_address = m.arg_ptr(3);
    const GuestAddr source = m.arg_ptr(4);
    const u32 flags = static_cast<u32>(m.arg(5));
    // Learn: "The only valid value is D3D11_TILE_COPY_NO_OVERWRITE"; measured,
    // LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE ("basically what UpdateTiles
    // does") is taken as well, SWIZZLED_TO_LINEAR and undefined bits drop it.
    if ((flags & ~(D3D11_TILE_COPY_NO_OVERWRITE |
                   D3D11_TILE_COPY_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE)) !=
        0) {
        d3d11_diag("UpdateTiles flags 0x%x -> dropped", flags);
        return;
    }
    Buffer* tiled_buffer = nullptr;
    Texture2D* tiled_texture = nullptr;
    TiledState* state = nullptr;
    if (auto b = m_buffers.find(resource); b != m_buffers.end()) {
        tiled_buffer = &b->second;
        state = b->second.tiled.get();
    } else if (auto t = m_textures.find(resource); t != m_textures.end()) {
        tiled_texture = &t->second;
        state = t->second.tiled.get();
    }
    Coordinate at{};
    RegionSize region{};
    std::vector<u32> tiles;
    if (state == nullptr || source == 0 || coordinate == 0 ||
        size_address == 0 || !read_coordinate(m_memory, coordinate, at) ||
        !read_region(m_memory, size_address, region) ||
        !region_tiles(*state, at, region, true, tiles)) {
        d3d11_diag("UpdateTiles: invalid resource or region -> dropped");
        return;
    }
    bool unimplemented = false;
    std::vector<u8> bytes;
    for (usize i = 0; i < tiles.size(); ++i) {
        const GuestAddr from = source + static_cast<GuestAddr>(i) * kTileBytes;
        if (tiled_buffer != nullptr) {
            const u64 begin = static_cast<u64>(tiles[i]) * kTileBytes;
            const u64 count =
                std::min<u64>(kTileBytes, tiled_buffer->desc.byte_width - begin);
            bytes.resize(static_cast<usize>(count));
            if (m_memory.read_bytes(from, bytes.data(), bytes.size())
                    .is_err()) {
                return;
            }
            // In order on the timeline (a tiled buffer's storage is private:
            // the update goes through a staging copy).
            unimplemented = unimplemented ||
                            !tiled_buffer->backend->update(
                                static_cast<usize>(begin), bytes.data(),
                                bytes.size(), graphics::BufferWrite::Ordered);
            continue;
        }
        TileBox box{};
        if (!texture_tile_box(*state, tiled_texture->backend->desc(), tiles[i],
                              box)) {
            continue;  // packed: undefined (Learn), nothing written
        }
        bytes.resize(static_cast<usize>(box.bytes));
        if (m_memory.read_bytes(from, bytes.data(), bytes.size()).is_err()) {
            return;
        }
        graphics::BackendBufferDesc staging_desc{};
        staging_desc.byte_width = bytes.size();
        staging_desc.cpu_writable = true;
        auto staging = m_backend.create_buffer(staging_desc, bytes.data());
        const bool ok =
            staging != nullptr &&
            m_backend.copy_texture_buffer_region(
                tiled_texture->backend.get(), box.level, box.slice, box.x,
                box.y, box.width, box.height, staging.get(), 0, box.row_pitch,
                true);
        unimplemented = unimplemented || !ok;
    }
    if (unimplemented) {
        d3d11_unimplemented_once("UpdateTiles",
                                 "tile the backend cannot update");
    }
}

void D3D11Manager::tiled_resource_barrier(Marshaler& m) {
    // TiledResourceBarrier(pBefore, pAfter), resources, views or NULL. The
    // accesses before complete before those after start: the backend's
    // barrier orders everything, which covers every pair it could name.
    m.set_return(0);
    if (!m_backend.supports_tiled_resources()) return;
    if (!m_backend.tiled_resource_barrier()) {
        d3d11_unimplemented_once("TiledResourceBarrier", "backend barrier");
    }
}

}  // namespace vitreo::winapi
