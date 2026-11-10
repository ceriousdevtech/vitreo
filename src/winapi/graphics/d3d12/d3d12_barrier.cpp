// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_barrier.cpp
/// @brief The runtime's rules for enhanced barriers
/// (ID3D12GraphicsCommandList7::Barrier) and enhanced-barrier initial
/// layouts (CreateCommittedResource3 / CreatePlacedResource2 /
/// CreateReservedResource2).
///
/// Sources: DirectX-Specs "D3D12 Enhanced Barriers" (the queue, layout,
/// access and sync compatibility tables, aggregate sync scopes) and the
/// core runtime's answers without the debug layer, measured on winoracle
/// (tools/oracle_probe/d3d12_a07_barriers_probe.c sections enhanced_rules
/// and enhanced_create). Where the two differ the measurement wins and the
/// rule below says so.
///
/// Ordering itself is not decided here: the backend's hazard tracking orders
/// every read after every write it tracks (as for ResourceBarrier), which is
/// what each sync / access / layout transition promises. A DISCARD is the
/// documented "contents undefined" and needs no work on a backend with no
/// compression metadata.

#include "winapi/graphics/d3d12/d3d12_internal.hpp"

#include <algorithm>

namespace vitreo::winapi::d3d12 {

namespace {

constexpr u32 S_ALL = D3D12_BARRIER_SYNC_ALL;
constexpr u32 S_DRAW = D3D12_BARRIER_SYNC_DRAW;
constexpr u32 S_IA = D3D12_BARRIER_SYNC_INPUT_ASSEMBLER;  // INDEX_INPUT
constexpr u32 S_VS = D3D12_BARRIER_SYNC_VERTEX_SHADING;
constexpr u32 S_PS = D3D12_BARRIER_SYNC_PIXEL_SHADING;
constexpr u32 S_DS = D3D12_BARRIER_SYNC_DEPTH_STENCIL;
constexpr u32 S_RT = D3D12_BARRIER_SYNC_RENDER_TARGET;
constexpr u32 S_CS = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
constexpr u32 S_RAY = D3D12_BARRIER_SYNC_RAYTRACING;
constexpr u32 S_COPY = D3D12_BARRIER_SYNC_COPY;
constexpr u32 S_RESOLVE = D3D12_BARRIER_SYNC_RESOLVE;
constexpr u32 S_INDIRECT = D3D12_BARRIER_SYNC_EXECUTE_INDIRECT;  // = PREDICATION
constexpr u32 S_ALL_SHADING = D3D12_BARRIER_SYNC_ALL_SHADING;
constexpr u32 S_NON_PIXEL = D3D12_BARRIER_SYNC_NON_PIXEL_SHADING;
constexpr u32 S_EMIT_AS = D3D12_BARRIER_SYNC_EMIT_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO;
constexpr u32 S_CLEAR_UAV = D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW;
constexpr u32 S_VIDEO_DECODE = D3D12_BARRIER_SYNC_VIDEO_DECODE;
constexpr u32 S_VIDEO_PROCESS = D3D12_BARRIER_SYNC_VIDEO_PROCESS;
constexpr u32 S_VIDEO_ENCODE = D3D12_BARRIER_SYNC_VIDEO_ENCODE;
constexpr u32 S_BUILD_AS = D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE;
constexpr u32 S_COPY_AS = D3D12_BARRIER_SYNC_COPY_RAYTRACING_ACCELERATION_STRUCTURE;
constexpr u32 S_SPLIT = D3D12_BARRIER_SYNC_SPLIT;

/// Every D3D12_BARRIER_SYNC bit d3d12.h (SDK 10.0.26100.0) defines.
constexpr u32 kDefinedSync =
    S_ALL | S_DRAW | S_IA | S_VS | S_PS | S_DS | S_RT | S_CS | S_RAY | S_COPY |
    S_RESOLVE | S_INDIRECT | S_ALL_SHADING | S_NON_PIXEL | S_EMIT_AS |
    S_CLEAR_UAV | S_VIDEO_DECODE | S_VIDEO_PROCESS | S_VIDEO_ENCODE |
    S_BUILD_AS | S_COPY_AS | S_SPLIT;

constexpr u32 A_VB = D3D12_BARRIER_ACCESS_VERTEX_BUFFER;
constexpr u32 A_CB = D3D12_BARRIER_ACCESS_CONSTANT_BUFFER;
constexpr u32 A_IB = D3D12_BARRIER_ACCESS_INDEX_BUFFER;
constexpr u32 A_RT = D3D12_BARRIER_ACCESS_RENDER_TARGET;
constexpr u32 A_UAV = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
constexpr u32 A_DSW = D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
constexpr u32 A_DSR = D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
constexpr u32 A_SRV = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
constexpr u32 A_SO = D3D12_BARRIER_ACCESS_STREAM_OUTPUT;
constexpr u32 A_INDIRECT = D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT;  // = PREDICATION
constexpr u32 A_COPY_DEST = D3D12_BARRIER_ACCESS_COPY_DEST;
constexpr u32 A_COPY_SOURCE = D3D12_BARRIER_ACCESS_COPY_SOURCE;
constexpr u32 A_RESOLVE_DEST = D3D12_BARRIER_ACCESS_RESOLVE_DEST;
constexpr u32 A_RESOLVE_SOURCE = D3D12_BARRIER_ACCESS_RESOLVE_SOURCE;
constexpr u32 A_AS_READ = D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ;
constexpr u32 A_AS_WRITE = D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE;
constexpr u32 A_SHADING_RATE = D3D12_BARRIER_ACCESS_SHADING_RATE_SOURCE;
constexpr u32 A_VIDEO_DECODE_READ = D3D12_BARRIER_ACCESS_VIDEO_DECODE_READ;
constexpr u32 A_VIDEO_DECODE_WRITE = D3D12_BARRIER_ACCESS_VIDEO_DECODE_WRITE;
constexpr u32 A_VIDEO_PROCESS_READ = D3D12_BARRIER_ACCESS_VIDEO_PROCESS_READ;
constexpr u32 A_VIDEO_PROCESS_WRITE = D3D12_BARRIER_ACCESS_VIDEO_PROCESS_WRITE;
constexpr u32 A_VIDEO_ENCODE_READ = D3D12_BARRIER_ACCESS_VIDEO_ENCODE_READ;
constexpr u32 A_VIDEO_ENCODE_WRITE = D3D12_BARRIER_ACCESS_VIDEO_ENCODE_WRITE;
constexpr u32 A_NO_ACCESS = D3D12_BARRIER_ACCESS_NO_ACCESS;

/// Every D3D12_BARRIER_ACCESS bit d3d12.h defines.
constexpr u32 kDefinedAccess =
    A_VB | A_CB | A_IB | A_RT | A_UAV | A_DSW | A_DSR | A_SRV | A_SO |
    A_INDIRECT | A_COPY_DEST | A_COPY_SOURCE | A_RESOLVE_DEST |
    A_RESOLVE_SOURCE | A_AS_READ | A_AS_WRITE | A_SHADING_RATE |
    A_VIDEO_DECODE_READ | A_VIDEO_DECODE_WRITE | A_VIDEO_PROCESS_READ |
    A_VIDEO_PROCESS_WRITE | A_VIDEO_ENCODE_READ | A_VIDEO_ENCODE_WRITE |
    A_NO_ACCESS;

/// "Command Queue Sync Compatibility" (NONE is allowed everywhere).
[[nodiscard]] u32 queue_syncs(u32 list_type) {
    switch (list_type) {
    case D3D12_COMMAND_LIST_TYPE_DIRECT:
        return S_ALL | S_DRAW | S_IA | S_VS | S_PS | S_DS | S_RT | S_CS | S_RAY |
               S_COPY | S_RESOLVE | S_INDIRECT | S_ALL_SHADING | S_NON_PIXEL |
               S_BUILD_AS | S_COPY_AS | S_EMIT_AS | S_CLEAR_UAV | S_SPLIT;
    case D3D12_COMMAND_LIST_TYPE_COMPUTE:
        return S_ALL | S_CS | S_RAY | S_COPY | S_INDIRECT | S_ALL_SHADING |
               S_NON_PIXEL | S_BUILD_AS | S_COPY_AS | S_EMIT_AS | S_CLEAR_UAV |
               S_SPLIT;
    case D3D12_COMMAND_LIST_TYPE_COPY:
        return S_ALL | S_COPY | S_SPLIT;
    default:
        return 0;
    }
}

/// "Command Queue Access Compatibility" (COMMON and NO_ACCESS everywhere).
[[nodiscard]] u32 queue_accesses(u32 list_type) {
    switch (list_type) {
    case D3D12_COMMAND_LIST_TYPE_DIRECT:
        return A_VB | A_CB | A_IB | A_RT | A_UAV | A_DSW | A_DSR | A_SRV | A_SO |
               A_INDIRECT | A_COPY_DEST | A_COPY_SOURCE | A_RESOLVE_DEST |
               A_RESOLVE_SOURCE | A_AS_READ | A_AS_WRITE | A_SHADING_RATE;
    case D3D12_COMMAND_LIST_TYPE_COMPUTE:
        return A_CB | A_UAV | A_SRV | A_INDIRECT | A_COPY_DEST | A_COPY_SOURCE |
               A_AS_READ | A_AS_WRITE;
    case D3D12_COMMAND_LIST_TYPE_COPY:
        return A_COPY_DEST | A_COPY_SOURCE;
    default:
        return 0;
    }
}

/// "Aggregate Synchronization Scopes": the scopes a sync value covers.
[[nodiscard]] u32 expand_sync(u32 sync) {
    u32 out = sync;
    if ((sync & S_ALL) != 0) out |= kDefinedSync & ~S_SPLIT;
    if ((sync & S_DRAW) != 0) out |= S_IA | S_VS | S_PS | S_DS | S_RT;
    if ((sync & S_ALL_SHADING) != 0) out |= S_VS | S_PS | S_CS;
    if ((sync & S_NON_PIXEL) != 0) out |= S_VS | S_CS;
    return out;
}

/// "Access Bits Barrier Sync Compatibility": the sync bits compatible with
/// one access bit.
[[nodiscard]] u32 access_syncs(u32 access_bit) {
    switch (access_bit) {
    case A_VB: return S_ALL | S_VS | S_DRAW | S_ALL_SHADING;
    case A_CB: return S_ALL | S_VS | S_PS | S_CS | S_DRAW | S_ALL_SHADING;
    case A_IB: return S_ALL | S_IA | S_DRAW;
    case A_RT: return S_ALL | S_DRAW | S_RT;
    case A_UAV:
        return S_ALL | S_VS | S_PS | S_CS | S_DRAW | S_ALL_SHADING |
               S_CLEAR_UAV | S_EMIT_AS | S_RAY;
    case A_DSW:
    case A_DSR: return S_ALL | S_DRAW | S_DS;
    case A_SRV:
        return S_ALL | S_VS | S_PS | S_CS | S_DRAW | S_ALL_SHADING |
               S_BUILD_AS | S_RAY;
    case A_SO: return S_ALL | S_VS | S_DRAW | S_ALL_SHADING;
    case A_INDIRECT: return S_ALL | S_INDIRECT;
    case A_COPY_DEST:
    case A_COPY_SOURCE: return S_ALL | S_COPY;
    case A_RESOLVE_DEST:
    case A_RESOLVE_SOURCE: return S_ALL | S_RESOLVE;
    case A_AS_READ:
        return S_ALL | S_CS | S_RAY | S_ALL_SHADING | S_BUILD_AS | S_COPY_AS |
               S_EMIT_AS;
    case A_AS_WRITE:
        return S_ALL | S_CS | S_RAY | S_ALL_SHADING | S_BUILD_AS | S_COPY_AS;
    case A_SHADING_RATE: return S_ALL | S_PS | S_ALL_SHADING;
    case A_VIDEO_DECODE_READ:
    case A_VIDEO_DECODE_WRITE: return S_ALL | S_VIDEO_DECODE;
    case A_VIDEO_PROCESS_READ:
    case A_VIDEO_PROCESS_WRITE: return S_ALL | S_VIDEO_PROCESS;
    case A_VIDEO_ENCODE_READ:
    case A_VIDEO_ENCODE_WRITE: return S_ALL | S_VIDEO_ENCODE;
    default: return 0;
    }
}

/// One half (before or after) of a barrier's sync and access.
[[nodiscard]] const char* sync_access_violation(u32 sync, u32 access,
                                                u32 list_type) {
    if ((sync & ~kDefinedSync) != 0) return "undefined D3D12_BARRIER_SYNC bits";
    if ((sync & S_SPLIT) != 0 && sync != S_SPLIT) {
        return "SYNC_SPLIT combined with other sync bits";
    }
    if ((access & ~kDefinedAccess) != 0) {
        return "undefined D3D12_BARRIER_ACCESS bits";
    }
    if ((access & A_NO_ACCESS) != 0 && access != A_NO_ACCESS) {
        return "ACCESS_NO_ACCESS combined with other access bits";
    }
    // Measured: SYNC_NONE takes NO_ACCESS only (COMMON too is refused);
    // NO_ACCESS goes with any sync.
    if (sync == D3D12_BARRIER_SYNC_NONE && access != A_NO_ACCESS) {
        return "SYNC_NONE with an access other than NO_ACCESS";
    }
    if ((sync & ~queue_syncs(list_type)) != 0) {
        return "a sync scope this command list type does not have";
    }
    if (access == A_NO_ACCESS || access == D3D12_BARRIER_ACCESS_COMMON) {
        return nullptr;
    }
    if ((access & ~queue_accesses(list_type)) != 0) {
        return "an access this command list type does not have";
    }
    // Measured: a split half is not checked against its access, and an
    // access set is compatible when ANY of its bits is (COPY with
    // COPY_DEST|UNORDERED_ACCESS passes; COPY with UNORDERED_ACCESS fails).
    if (sync == S_SPLIT) return nullptr;
    u32 compatible = 0;
    for (u32 bits = access; bits != 0; bits &= bits - 1) {
        compatible |= access_syncs(bits & (~bits + 1));
    }
    if ((expand_sync(sync) & compatible) == 0) {
        return "an access with no compatible sync scope";
    }
    return nullptr;
}

/// Accesses a buffer can have (measured: render-target, depth and
/// shading-rate accesses are texture-only; UNORDERED_ACCESS needs
/// ALLOW_UNORDERED_ACCESS; acceleration-structure accesses need a buffer
/// created as one). Heap type is not checked (an UPLOAD buffer takes
/// COPY_DEST, a READBACK buffer SHADER_RESOURCE).
[[nodiscard]] bool buffer_access_ok(const Resource& r, u32 access) {
    if (access == A_NO_ACCESS) return true;
    if ((access & (A_RT | A_DSW | A_DSR | A_SHADING_RATE)) != 0) return false;
    if ((access & A_UAV) != 0 &&
        (r.desc.flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        return false;
    }
    if ((access & (A_AS_READ | A_AS_WRITE)) != 0 &&
        !r.desc.acceleration_structure) {
        return false;
    }
    return true;
}

/// A defined D3D12_BARRIER_LAYOUT (SDK 10.0.26100.0: 0..VIDEO_QUEUE_COMMON,
/// and UNDEFINED).
[[nodiscard]] bool layout_defined(u32 layout) {
    return layout == D3D12_BARRIER_LAYOUT_UNDEFINED ||
           layout <= D3D12_BARRIER_LAYOUT_VIDEO_QUEUE_COMMON;
}

/// "Command Queue Layout Compatibility". UNDEFINED is not a layout a queue
/// holds and is allowed on every list type.
[[nodiscard]] bool queue_layout_ok(u32 layout, u32 list_type) {
    if (layout == D3D12_BARRIER_LAYOUT_UNDEFINED ||
        layout == D3D12_BARRIER_LAYOUT_COMMON) {
        return true;
    }
    switch (list_type) {
    case D3D12_COMMAND_LIST_TYPE_DIRECT:
        return (layout >= D3D12_BARRIER_LAYOUT_GENERIC_READ &&
                layout <= D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE) ||
               (layout >= D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON &&
                layout <= D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_DEST);
    case D3D12_COMMAND_LIST_TYPE_COMPUTE:
        return layout == D3D12_BARRIER_LAYOUT_GENERIC_READ ||
               layout == D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS ||
               layout == D3D12_BARRIER_LAYOUT_SHADER_RESOURCE ||
               layout == D3D12_BARRIER_LAYOUT_COPY_SOURCE ||
               layout == D3D12_BARRIER_LAYOUT_COPY_DEST ||
               (layout >= D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COMMON &&
                layout <= D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_DEST);
    default:
        // COPY: COMMON only (copy queues make no layout transitions).
        return false;
    }
}

/// "Layout Access Compatibility": the accesses a layout allows (COMMON and
/// NO_ACCESS are always allowed; UNDEFINED is the caller's case).
[[nodiscard]] u32 layout_accesses(u32 layout) {
    switch (layout) {
    case D3D12_BARRIER_LAYOUT_COMMON: return A_SRV | A_COPY_DEST | A_COPY_SOURCE;
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COMMON:
        return A_SRV | A_COPY_DEST | A_COPY_SOURCE | A_UAV;
    case D3D12_BARRIER_LAYOUT_GENERIC_READ:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_GENERIC_READ:
        return A_SRV | A_COPY_SOURCE;
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ:
        return A_SRV | A_COPY_SOURCE | A_DSR | A_SHADING_RATE | A_RESOLVE_SOURCE;
    case D3D12_BARRIER_LAYOUT_RENDER_TARGET: return A_RT;
    case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_UNORDERED_ACCESS: return A_UAV;
    // The spec lists DEPTH_STENCIL_READ too; measured, DEPTH_STENCIL_READ
    // alone in DEPTH_STENCIL_WRITE is refused. It is allowed alongside the
    // write (see layout_access_ok).
    case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE: return A_DSW;
    case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ: return A_DSR;
    case D3D12_BARRIER_LAYOUT_SHADER_RESOURCE:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_SHADER_RESOURCE: return A_SRV;
    case D3D12_BARRIER_LAYOUT_COPY_SOURCE:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_SOURCE:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_SOURCE: return A_COPY_SOURCE;
    case D3D12_BARRIER_LAYOUT_COPY_DEST:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_DEST:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_DEST: return A_COPY_DEST;
    case D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE: return A_RESOLVE_SOURCE;
    case D3D12_BARRIER_LAYOUT_RESOLVE_DEST: return A_RESOLVE_DEST;
    case D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE: return A_SHADING_RATE;
    case D3D12_BARRIER_LAYOUT_VIDEO_DECODE_READ: return A_VIDEO_DECODE_READ;
    case D3D12_BARRIER_LAYOUT_VIDEO_DECODE_WRITE: return A_VIDEO_DECODE_WRITE;
    case D3D12_BARRIER_LAYOUT_VIDEO_PROCESS_READ: return A_VIDEO_PROCESS_READ;
    case D3D12_BARRIER_LAYOUT_VIDEO_PROCESS_WRITE: return A_VIDEO_PROCESS_WRITE;
    case D3D12_BARRIER_LAYOUT_VIDEO_ENCODE_READ: return A_VIDEO_ENCODE_READ;
    case D3D12_BARRIER_LAYOUT_VIDEO_ENCODE_WRITE: return A_VIDEO_ENCODE_WRITE;
    default: return 0;
    }
}

[[nodiscard]] bool layout_access_ok(u32 layout, u32 access, bool both_undefined) {
    if (access == A_NO_ACCESS || access == D3D12_BARRIER_ACCESS_COMMON) return true;
    if (layout == D3D12_BARRIER_LAYOUT_UNDEFINED) return both_undefined;
    u32 allowed = layout_accesses(layout);
    if (layout == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE && (access & A_DSW) != 0) {
        allowed |= A_DSR;
    }
    return (access & ~allowed) == 0;
}

/// The usage flag a layout needs on the texture (measured, both at creation
/// and in a barrier): render-target, unordered-access and depth layouts.
[[nodiscard]] bool layout_flags_ok(u32 layout, u32 flags) {
    switch (layout) {
    case D3D12_BARRIER_LAYOUT_RENDER_TARGET:
        return (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_UNORDERED_ACCESS:
        return (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE:
    case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ:
        return (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    default:
        return true;
    }
}

/// Texture accesses that need a usage flag (measured: DEPTH_STENCIL_READ on
/// a colour texture is refused even in a layout that lists it).
[[nodiscard]] bool texture_access_flags_ok(u32 access, u32 flags) {
    if (access == A_NO_ACCESS) return true;
    if ((access & A_RT) != 0 &&
        (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) {
        return false;
    }
    if ((access & A_UAV) != 0 &&
        (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        return false;
    }
    if ((access & (A_DSW | A_DSR)) != 0 &&
        (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) {
        return false;
    }
    return true;
}

} // namespace

const char* barrier_sync_access_violation(const EnhancedBarrier& b,
                                          u32 list_type) {
    if (const char* why =
            sync_access_violation(b.sync_before, b.access_before, list_type)) {
        return why;
    }
    return sync_access_violation(b.sync_after, b.access_after, list_type);
}

const char* buffer_barrier_violation(const EnhancedBarrier& b,
                                     const Resource* r, u32 list_type) {
    if (const char* why = barrier_sync_access_violation(b, list_type)) return why;
    if (r == nullptr) return "a buffer barrier without a resource";
    if (!r->is_buffer()) return "a buffer barrier on a texture";
    // D3D12_BUFFER_BARRIER: Offset must be 0, Size UINT64_MAX or the buffer
    // size in bytes (measured: 128 of 256, 512 and 0 are refused).
    if (b.offset != 0) return "a buffer barrier Offset other than 0";
    if (b.size != ~0ull && b.size != r->desc.width) {
        return "a buffer barrier Size other than UINT64_MAX or the buffer size";
    }
    if (!buffer_access_ok(*r, b.access_before) ||
        !buffer_access_ok(*r, b.access_after)) {
        return "an access this buffer cannot have";
    }
    return nullptr;
}

const char* texture_barrier_violation(const EnhancedBarrier& b,
                                      const Resource* r, u32 list_type) {
    if (const char* why = barrier_sync_access_violation(b, list_type)) return why;
    if (r == nullptr) return "a texture barrier without a resource";
    if (r->is_buffer()) return "a texture barrier on a buffer";
    for (const u32 layout : {b.layout_before, b.layout_after}) {
        if (!layout_defined(layout)) return "an undefined D3D12_BARRIER_LAYOUT";
        if (!queue_layout_ok(layout, list_type)) {
            return "a layout this command list type does not have";
        }
        if (!layout_flags_ok(layout, r->desc.flags)) {
            return "a layout without the resource flag it needs";
        }
    }
    const bool both_undefined =
        b.layout_before == D3D12_BARRIER_LAYOUT_UNDEFINED &&
        b.layout_after == D3D12_BARRIER_LAYOUT_UNDEFINED;
    if (!layout_access_ok(b.layout_before, b.access_before, both_undefined) ||
        !layout_access_ok(b.layout_after, b.access_after, both_undefined)) {
        return "an access its layout does not allow";
    }
    if (!texture_access_flags_ok(b.access_before, r->desc.flags) ||
        !texture_access_flags_ok(b.access_after, r->desc.flags)) {
        return "an access without the resource flag it needs";
    }
    // D3D12_BARRIER_SUBRESOURCE_RANGE: a subresource index (0xFFFFFFFF for
    // all) when NumMipLevels is 0, else a range of mips x slices x planes
    // that must lie inside the resource (empty slice / plane ranges pass,
    // measured).
    const u32 mips = std::max(1u, r->desc.mip_levels);
    const u32 slices = std::max(1u, r->array_size());
    const u32 planes = r->subresource_count() / (mips * slices);
    if (b.num_mips == 0) {
        if (b.index_or_first_mip != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES &&
            b.index_or_first_mip >= r->subresource_count()) {
            return "a subresource index out of range";
        }
    } else if (static_cast<u64>(b.index_or_first_mip) + b.num_mips > mips ||
               static_cast<u64>(b.first_slice) + b.num_slices > slices ||
               static_cast<u64>(b.first_plane) + b.num_planes > planes) {
        return "a subresource range out of range";
    }
    // D3D12_TEXTURE_BARRIER_FLAGS are not checked by the runtime (measured:
    // DISCARD from COMMON and an undefined 0x2 both pass). A discard leaves
    // the contents undefined, which the backend's memory already satisfies.
    return nullptr;
}

const char* initial_layout_violation(const ResourceDescription& d, u32 layout) {
    // Measured (a07 enhanced_create): a buffer is created UNDEFINED only; a
    // simultaneous-access texture COMMON only; any other texture takes any
    // value, defined or not, except a render-target, unordered-access or
    // depth layout without its usage flag.
    if (d.dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        return layout == D3D12_BARRIER_LAYOUT_UNDEFINED
                   ? nullptr
                   : "a buffer's initial layout must be UNDEFINED";
    }
    if ((d.flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0) {
        return layout == D3D12_BARRIER_LAYOUT_COMMON
                   ? nullptr
                   : "a simultaneous-access texture's initial layout must be COMMON";
    }
    if (!layout_flags_ok(layout, d.flags)) {
        return "an initial layout without the resource flag it needs";
    }
    return nullptr;
}

bool ends_attachment_writes(const EnhancedBarrier& b, u32 type) {
    // DirectX-Specs, Enhanced Barriers: AccessBefore names what the work
    // before the barrier did, and ACCESS_COMMON on a texture is every access
    // its LayoutBefore allows. Writes of RENDER_TARGET or
    // DEPTH_STENCIL_WRITE end here unless nothing changes (the same access
    // and layout on both sides), the barrier is the begin half of a split
    // (SyncAfter SPLIT: it orders nothing until its end half), or
    // NO_ACCESS says an earlier barrier already ended them.
    constexpr u32 kWrites = D3D12_BARRIER_ACCESS_RENDER_TARGET |
                            D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
    if (b.sync_after == D3D12_BARRIER_SYNC_SPLIT) return false;
    if ((b.access_before & D3D12_BARRIER_ACCESS_NO_ACCESS) != 0) return false;
    const bool texture = type == D3D12_BARRIER_TYPE_TEXTURE;
    const bool layout_writes =
        texture && (b.layout_before == D3D12_BARRIER_LAYOUT_RENDER_TARGET ||
                    b.layout_before == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE);
    const bool wrote = (b.access_before & kWrites) != 0 ||
                       (layout_writes &&
                        b.access_before == D3D12_BARRIER_ACCESS_COMMON);
    if (!wrote) return false;
    const bool changes = b.access_after != b.access_before ||
                         (texture && b.layout_after != b.layout_before);
    return changes;
}

} // namespace vitreo::winapi::d3d12
