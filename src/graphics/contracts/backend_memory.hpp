// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file backend_memory.hpp
/// @brief Placement heaps, shared CPU memory, and tiled-resource tile pools.

#include "graphics/contracts/backend_buffer.hpp"
#include <vitreo/common.hpp>
#include <vector>

namespace vitreo::graphics {

// =============================================================================
// Placement heaps
// =============================================================================
//
// D3D12's memory model: an ID3D12Heap is a block of memory, and a placed
// resource is a buffer or texture whose storage is a window of it. Resources
// placed over the same bytes alias -- a write through one is a write through
// the other -- and that aliasing is the contract, not an optimisation (Learn,
// "Memory aliasing and data inheritance"). A backend heap is that block.
//
// Every heap is also exposed as ONE buffer spanning it: a placed buffer is a
// (heap buffer, offset) window rather than a buffer object of its own, which
// is exactly the D3D12 model -- a buffer's GPU virtual address is its heap's
// base plus its offset, and views address it by byte.
//
// A CPU-visible heap (D3D12 UPLOAD / READBACK, or a CUSTOM heap with CPU
// pages) lives in host memory the backend allocates page-aligned, so a
// frontend can map it into a guest's address space once and hand out
// persistent pointers into it that the GPU reads and writes directly.

struct BackendHeapDesc {
    u64 size{0};
    /// Host-visible, CPU-coherent storage with a stable host_pointer().
    /// Holds buffers only.
    bool cpu_visible{false};
    /// Only buffers will be placed (D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS, and
    /// every committed buffer's implicit heap): the backend may back it with
    /// a plain buffer rather than a placement heap.
    bool buffers_only{false};
};

class BackendHeap {
public:
    virtual ~BackendHeap() = default;

    [[nodiscard]] virtual bool valid() const noexcept = 0;
    [[nodiscard]] virtual u64 size() const noexcept = 0;
    /// Byte 0 of a CPU-visible heap in host memory: page-aligned (the host
    /// page, not the guest one), stable for the heap's lifetime, and the same
    /// bytes the GPU reads and writes. Null for a GPU-private heap.
    [[nodiscard]] virtual void* host_pointer() const noexcept = 0;
    /// The whole heap as one buffer. Owned by the heap.
    [[nodiscard]] virtual BackendBuffer* buffer() noexcept = 0;
};

/// CPU memory another device or process can open by a global id: the
/// storage of a shared STAGING resource (D3D11_USAGE_STAGING with
/// D3D11_RESOURCE_MISC_SHARED). Every opening maps the same bytes; GPU copies
/// into and out of it go through GraphicsBackendDevice::
/// copy_texture_to_shared_memory / copy_shared_memory_to_texture.
class BackendSharedMemory {
public:
    virtual ~BackendSharedMemory() = default;
    [[nodiscard]] virtual u64 shared_handle() const noexcept = 0;
    [[nodiscard]] virtual u8* data() noexcept = 0;
    [[nodiscard]] virtual usize size() const noexcept = 0;
    [[nodiscard]] virtual const std::vector<u8>& shared_metadata()
        const noexcept = 0;
    /// Before the CPU reads or writes data(): wait until every write another
    /// device or process published to it has completed.
    [[nodiscard]] virtual bool acquire() = 0;
    /// After the CPU wrote data() (an Unmap), or after GPU copies into it
    /// were recorded: make them visible to other devices once complete.
    virtual void note_written() = 0;
};

// =============================================================================
// Tiled resources (D3D11.2 / D3D12 reserved resources)
// =============================================================================

/// A tile: D3D11_2_TILED_RESOURCE_TILE_SIZE_IN_BYTES (d3d11_2.h).
inline constexpr u64 BACKEND_TILE_BYTES = 65536;
/// BackendTileMapping::pool_tile of a tile that shows no memory.
inline constexpr u64 BACKEND_TILE_UNMAPPED = ~0ull;

/// A tile pool's memory (D3D11_RESOURCE_MISC_TILE_POOL), numbered in tiles
/// from 0. Memory behind a tile exists from the first mapping that references
/// it and is kept while any does -- also past the end of a pool that shrank,
/// where growing the pool again reuses it before allocating (Learn,
/// ID3D11DeviceContext2::ResizeTilePool remarks). A new tile's contents are
/// undefined (Learn, "Tiled resource creation parameters").
class BackendTilePool {
public:
    virtual ~BackendTilePool() = default;
    /// Count one more mapping of pool tile @p tile, making its memory exist.
    /// False (and nothing counted) when that memory cannot be allocated.
    [[nodiscard]] virtual bool reference(u64 tile) = 0;
    /// One mapping of @p tile went away.
    virtual void unreference(u64 tile) = 0;
    /// The pool is now @p tiles long: memory wholly past the end that no
    /// mapping references is released once the GPU is done with it.
    virtual void set_size(u64 tiles) = 0;
};

/// One tile of a tiled resource: a buffer's tile index in `x`; a texture's
/// tile (x, y) of mip `level` of array slice `slice`; or, `packed`, tile `x`
/// of `slice`'s packed mip tail (BackendTiledLayout).
struct BackendTileCoord {
    u32 level{0};
    u32 slice{0};
    u32 x{0};
    u32 y{0};
    bool packed{false};
};

/// What one tile of a tiled resource shows after an update: pool tile
/// `pool_tile`, or no memory (BACKEND_TILE_UNMAPPED).
struct BackendTileMapping {
    BackendTileCoord tile;
    u64 pool_tile{BACKEND_TILE_UNMAPPED};
};

/// How a tiled 2D texture divides into tiles: every mip below
/// `first_packed_level` in whole tiles of `tile_width` x `tile_height`
/// texels, the rest of each slice's chain packed into `packed_tiles` tiles
/// whose layout is the backend's own (D3D11_PACKED_MIP_DESC).
struct BackendTiledLayout {
    unsigned tile_width{0};
    unsigned tile_height{0};
    unsigned first_packed_level{0};
    unsigned packed_tiles{0};
};

} // namespace vitreo::graphics
