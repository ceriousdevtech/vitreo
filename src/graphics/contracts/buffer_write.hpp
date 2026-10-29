// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file buffer_write.hpp
/// @brief How a CPU write into a buffer is ordered against GPU work.

#include <vitreo/common.hpp>

namespace vitreo::graphics {

/// The ordering contract of one CPU write into a buffer, against GPU work
/// already recorded that reads the buffer. Each frontend names the mode its
/// API defines for the call; the backend never guesses it from the range.
///
/// Sources: Learn D3D11_MAP and "How to: Use dynamic resources"; D3DLOCK and
/// "Using Dynamic Vertex and Index Buffers"; ID3D11DeviceContext::
/// UpdateSubresource.
enum class BufferWrite : u8 {
    /// Ordered after recorded work: D3D11 Map(WRITE / READ_WRITE) + Unmap,
    /// UpdateSubresource, a CPU-side Copy*, UpdateTiles; D3D9 Lock with
    /// neither DISCARD nor NOOVERWRITE ("the driver must finish using the
    /// resource before returning from locking"), ProcessVertices. Work
    /// recorded before the write reads the old bytes and work recorded after
    /// reads the new ones. The backend renames (keeping every other byte),
    /// stages and copies on the GPU timeline, or waits -- it never writes
    /// storage the GPU may still read.
    Ordered,
    /// D3D11_MAP_WRITE_DISCARD / D3DLOCK_DISCARD: "If the GPU is still using
    /// the buffer ... a pointer to a new region of memory is returned instead
    /// of the old buffer data. This allows the GPU to continue using the old
    /// data". Renamed while in use; when renaming memory runs out the backend
    /// waits for the GPU. Never written in place under in-flight reads.
    Discard,
    /// D3D11_MAP_WRITE_NO_OVERWRITE / D3DLOCK_NOOVERWRITE: the application
    /// promises the bytes it writes are not used by work in flight, so they go
    /// into the existing storage -- renaming would split the buffer the
    /// application is appending to.
    NoOverwrite,
};

}  // namespace vitreo::graphics
