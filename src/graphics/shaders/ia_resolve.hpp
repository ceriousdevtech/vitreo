// SPDX-License-Identifier: LGPL-2.1-only
/// @file ia_resolve.hpp
/// @brief Indirect draws through the geometry, stream-output and
///        tessellation emulations: primitive assembly on the GPU timeline.
///
/// DrawInstancedIndirect / DrawIndexedInstancedIndirect take their counts
/// from a buffer the GPU may have written moments before (a compute pass,
/// CopyStructureCount, DrawAuto's filled size). The emulations size their
/// passes on the CPU, so the counts cannot be read there without stalling on
/// -- or racing -- the GPU. Instead one kernel, recorded in command order,
/// reads the argument block and does on the GPU what the direct path does on
/// the CPU:
///
///  - primitive assembly: the vertex (record) keys of every primitive, as a
///    geometry shader or stream output receives them -- strip winding, strip
///    cuts and list primitives holding the cut value, the strip-adjacency
///    table (the rules of graphics::assemble_primitive_vertices, measured on
///    Windows), instance-major;
///  - the capture layout: records keyed by the draw-relative vertex
///    (SV_VertexID's value: the vertex index for a non-indexed draw, the index
///    value for an indexed one, D3D11.3 8.16) plus instance x stride, the
///    stride being the per-instance key range;
///  - the counts every later pass reads: primitives in all and per instance
///    (SV_PrimitiveID restarts per instance, 8.17), and the argument block of
///    the capture draw.
///
/// Capacities are the CPU's: a draw the buffers cannot hold is dropped whole
/// (every count zero) and flagged, and the host raises its capacity for the
/// next draw (never a partial draw, never a stall).

#pragma once

#include "../contracts/backend_device.hpp"
#include "gs_passthrough.hpp"
#include "stream_out_repack.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace vitreo::graphics {

/// Constants of vitreo_ia_resolve (inline constant buffer, index 0).
struct IaResolveParams {
    u32 indexed{0};
    u32 index_32bit{0};
    /// The bound index buffer's offset, in indices.
    u32 index_first{0};
    /// Indices the bound buffer holds past `index_first` (reads beyond read
    /// 0, D3D11.3 8.19).
    u32 index_elements{0};
    u32 vertices_per_primitive{1};
    /// 0 list, 1 strip, 2 triangle strip with adjacency.
    u32 assembly{0};
    /// Records the capture buffer holds after its header.
    u32 record_capacity{0};
    /// Primitives (over all instances) the ids buffer and the passes hold.
    u32 primitive_capacity{0};
};
static_assert(sizeof(IaResolveParams) == 32);

/// Resolved block, u32 words.
inline constexpr u32 IA_RESOLVED_PRIMITIVES = 0;           ///< all instances
inline constexpr u32 IA_RESOLVED_PRIMITIVES_PER_INSTANCE = 1;
inline constexpr u32 IA_RESOLVED_KEY_STRIDE = 2;           ///< keys/instance
inline constexpr u32 IA_RESOLVED_INSTANCES = 3;
inline constexpr u32 IA_RESOLVED_OVERFLOW = 4;             ///< 1: dropped
inline constexpr u32 IA_RESOLVED_VERTICES = 5;             ///< total keys used
/// The capture draw's argument block (4 or 5 words, D3D = Metal layout).
inline constexpr u32 IA_RESOLVED_CAPTURE_ARGS = 8;
inline constexpr u32 IA_RESOLVED_WORDS = 16;

inline constexpr unsigned IA_RESOLVE_ARGS_INDEX = 1;
inline constexpr unsigned IA_RESOLVE_INDICES_INDEX = 2;
inline constexpr unsigned IA_RESOLVE_IDS_INDEX = 3;
inline constexpr unsigned IA_RESOLVE_OUT_INDEX = 4;
inline constexpr unsigned IA_RESOLVE_CAPTURE_INDEX = 5;
inline constexpr unsigned IA_RESOLVE_THREADS = 256;
inline constexpr const char* kIaResolveEntryPoint = "vitreo_ia_resolve";

/// The draw-relative capture's header: the first 16 bytes of its buffer,
/// {key stride per instance, record capacity}; records follow.
inline constexpr u32 CAPTURE_HEADER_BYTES = 16;

/// Primitives of one instance of a non-indexed draw of @p count vertices.
[[nodiscard]] constexpr u32 ia_primitives(u32 assembly, u32 vpp, u32 count) {
    if (vpp == 0) return 0;
    if (assembly == 2) return triangle_strip_adjacency_count(count);
    if (assembly == 1) return count >= vpp ? count - (vpp - 1) : 0;
    return count / vpp;
}

/// The C++ model of vitreo_ia_resolve: @p args is the argument block,
/// @p indices the bound index buffer from `index_first`. Fills @p ids and
/// returns the resolved block.
[[nodiscard]] inline std::array<u32, IA_RESOLVED_WORDS> ia_resolve_model(
    const IaResolveParams& p, const std::array<u32, 5>& args,
    const std::vector<u32>& indices, std::vector<u32>& ids) {
    std::array<u32, IA_RESOLVED_WORDS> out{};
    const u32 vpp = std::max(p.vertices_per_primitive, 1u);
    const u32 count = args[0];
    u32 instances = args[1];
    std::vector<u32> local;
    u32 stride = 0;
    if (p.indexed == 0) {
        const u32 primitives = ia_primitives(p.assembly, vpp, count);
        for (u32 i = 0; i < primitives; ++i) {
            for (u32 k = 0; k < vpp; ++k) {
                u32 v = i * vpp + k;
                if (p.assembly == 1) {
                    const u32 kk =
                        (vpp == 3 && (i & 1u) != 0 && k != 0) ? 3u - k : k;
                    v = i + kk;
                } else if (p.assembly == 2) {
                    v = triangle_strip_adjacency_vertex(i, k, primitives);
                }
                local.push_back(v);
            }
        }
        stride = count;
    } else {
        const u32 cut = p.index_32bit != 0 ? 0xFFFFFFFFu : 0xFFFFu;
        std::vector<u32> values;
        for (u32 i = 0; i < count; ++i) {
            const u64 at = static_cast<u64>(args[2]) + i;
            values.push_back(at < indices.size() ? indices[at] : 0u);
        }
        local = assemble_primitive_vertices(
            values, true, cut, 0, vpp,
            p.assembly == 2   ? PrimitiveAssembly::StripAdjacency
            : p.assembly == 1 ? PrimitiveAssembly::Strip
                              : PrimitiveAssembly::List);
        u32 max_index = 0;
        bool any = false;
        for (const u32 v : values) {
            if (v == cut) continue;
            max_index = std::max(max_index, v);
            any = true;
        }
        stride = any ? max_index + 1 : 0;
    }
    const u32 primitives = static_cast<u32>(local.size() / vpp);
    const bool fits =
        static_cast<u64>(instances) * stride <= p.record_capacity &&
        static_cast<u64>(instances) * primitives <= p.primitive_capacity;
    if (!fits) instances = 0;
    ids.clear();
    for (u32 j = 0; j < instances; ++j) {
        for (const u32 key : local) ids.push_back(j * stride + key);
    }
    out[IA_RESOLVED_PRIMITIVES] = primitives * instances;
    out[IA_RESOLVED_PRIMITIVES_PER_INSTANCE] = fits ? primitives : 0u;
    out[IA_RESOLVED_KEY_STRIDE] = stride;
    out[IA_RESOLVED_INSTANCES] = instances;
    out[IA_RESOLVED_OVERFLOW] = fits ? 0u : 1u;
    out[IA_RESOLVED_VERTICES] = stride * instances;
    for (u32 w = 0; w < (p.indexed != 0 ? 5u : 4u); ++w) {
        out[IA_RESOLVED_CAPTURE_ARGS + w] = args[w];
    }
    out[IA_RESOLVED_CAPTURE_ARGS + 1] = instances;
    return out;
}

[[nodiscard]] inline std::string ia_resolve_msl() {
    char source[12288];
    std::snprintf(source, sizeof(source), R"(#include <metal_stdlib>
using namespace metal;

struct IaResolveParams {
    uint indexed;
    uint index_32bit;
    uint index_first;
    uint index_elements;
    uint vertices_per_primitive;
    uint assembly;
    uint record_capacity;
    uint primitive_capacity;
};

static uint ia_index(constant IaResolveParams& p, device const uint* ib,
                     uint element)
{
    // D3D11.3 8.19: an index read past the buffer is 0.
    if (element >= p.index_elements) return 0u;
    const uint e = p.index_first + element;
    return p.index_32bit != 0u ? ib[e]
                               : (ib[e >> 1] >> ((e & 1u) * 16u)) & 0xFFFFu;
}

// graphics::triangle_strip_adjacency_vertex (measured on Windows).
static uint ia_strip_adjacency(uint i, uint k, uint triangles)
{
    const bool last = i + 1u == triangles;
    const uint trailing = last ? 2u * i + 5u : 2u * i + 6u;
    if ((i & 1u) == 0u) {
        switch (k) {
        case 0u: return 2u * i;
        case 1u: return i == 0u ? 1u : 2u * i - 2u;
        case 2u: return 2u * i + 2u;
        case 3u: return trailing;
        case 4u: return 2u * i + 4u;
        default: return 2u * i + 3u;
        }
    }
    switch (k) {
    case 0u: return 2u * i;
    case 1u: return 2u * i + 3u;
    case 2u: return 2u * i + 4u;
    case 3u: return trailing;
    case 4u: return 2u * i + 2u;
    default: return 2u * i - 2u;
    }
}

kernel void %s(
    constant IaResolveParams& p [[buffer(0)]],
    device const uint* args [[buffer(%u)]],
    device const uint* ib [[buffer(%u)]],
    device uint* ids [[buffer(%u)]],
    device uint* out [[buffer(%u)]],
    device uint* capture [[buffer(%u)]],
    uint t [[thread_index_in_threadgroup]])
{
    threadgroup uint shared[4];
    const uint vpp = max(p.vertices_per_primitive, 1u);
    if (t == 0u) {
        const uint count = args[0];
        uint instances = args[1];
        uint primitives = 0u;
        uint stride = 0u;
        if (p.indexed == 0u) {
            primitives = p.assembly == 2u
                ? (count >= 6u ? (count - 4u) / 2u : 0u)
                : p.assembly == 1u ? (count >= vpp ? count - (vpp - 1u) : 0u)
                                   : count / vpp;
            stride = count;
        } else {
            // Serial: cuts and strip restarts depend on every index before.
            const uint cut = p.index_32bit != 0u ? 0xFFFFFFFFu : 0xFFFFu;
            const uint start = args[2];
            uint max_index = 0u;
            bool any = false;
            uint held = 0u;
            uint begin = 0u;
            uint written = 0u;
            const uint limit = p.primitive_capacity * vpp;
            for (uint i = 0u; i <= count; ++i) {
                const bool end = i == count;
                const uint v = end ? cut : ia_index(p, ib, start + i);
                if (!end && v != cut) {
                    max_index = max(max_index, v);
                    any = true;
                }
                if (p.assembly == 0u) {
                    if (end) break;
                    if (v == cut) { held = 0u; continue; }
                    if (held == 0u) begin = i;
                    if (++held < vpp) continue;
                    held = 0u;
                    for (uint k = 0u; k < vpp; ++k) {
                        if (written < limit) {
                            ids[written] = ia_index(p, ib, start + begin + k);
                        }
                        ++written;
                    }
                    continue;
                }
                // A strip segment ends at a cut or at the end.
                if (!end && v != cut) continue;
                const uint length = i - begin;
                if (p.assembly == 2u) {
                    const uint triangles = length >= 6u ? (length - 4u) / 2u : 0u;
                    for (uint n = 0u; n < triangles; ++n) {
                        for (uint k = 0u; k < 6u; ++k) {
                            if (written < limit) {
                                ids[written] = ia_index(
                                    p, ib,
                                    start + begin +
                                        ia_strip_adjacency(n, k, triangles));
                            }
                            ++written;
                        }
                    }
                } else if (length >= vpp) {
                    for (uint n = 0u; n + vpp <= length; ++n) {
                        for (uint k = 0u; k < vpp; ++k) {
                            const uint kk =
                                (vpp == 3u && (n & 1u) != 0u && k != 0u)
                                    ? 3u - k : k;
                            if (written < limit) {
                                ids[written] =
                                    ia_index(p, ib, start + begin + n + kk);
                            }
                            ++written;
                        }
                    }
                }
                begin = i + 1u;
            }
            primitives = written / vpp;
            stride = any ? max_index + 1u : 0u;
        }
        const bool fits =
            ulong(instances) * ulong(stride) <= ulong(p.record_capacity) &&
            ulong(instances) * ulong(primitives) <=
                ulong(p.primitive_capacity);
        if (!fits) instances = 0u;
        shared[0] = primitives;
        shared[1] = stride;
        shared[2] = instances;
        out[%u] = primitives * instances;
        out[%u] = fits ? primitives : 0u;
        out[%u] = stride;
        out[%u] = instances;
        out[%u] = fits ? 0u : 1u;
        out[%u] = stride * instances;
        for (uint w = 0u; w < 5u; ++w) {
            out[%u + w] = (w < 4u || p.indexed != 0u) ? args[w] : 0u;
        }
        out[%u + 1u] = instances;
        capture[0] = stride;
        capture[1] = p.record_capacity;
    }
    threadgroup_barrier(mem_flags::mem_device | mem_flags::mem_threadgroup);
    const uint primitives = shared[0];
    const uint stride = shared[1];
    const uint instances = shared[2];
    const uint total = primitives * instances;
    if (p.indexed == 0u) {
        for (uint q = t; q < total; q += %uu) {
            const uint j = q / primitives;
            const uint i = q - j * primitives;
            for (uint k = 0u; k < vpp; ++k) {
                uint v = i * vpp + k;
                if (p.assembly == 1u) {
                    const uint kk =
                        (vpp == 3u && (i & 1u) != 0u && k != 0u) ? 3u - k : k;
                    v = i + kk;
                } else if (p.assembly == 2u) {
                    v = ia_strip_adjacency(i, k, primitives);
                }
                ids[q * vpp + k] = j * stride + v;
            }
        }
    } else {
        // Instances past the first repeat the first one's keys, shifted.
        for (uint q = primitives + t; q < total; q += %uu) {
            const uint j = q / primitives;
            const uint i = q - j * primitives;
            for (uint k = 0u; k < vpp; ++k) {
                ids[q * vpp + k] = j * stride + ids[i * vpp + k];
            }
        }
    }
}
)",
                  kIaResolveEntryPoint, IA_RESOLVE_ARGS_INDEX,
                  IA_RESOLVE_INDICES_INDEX, IA_RESOLVE_IDS_INDEX,
                  IA_RESOLVE_OUT_INDEX, IA_RESOLVE_CAPTURE_INDEX,
                  IA_RESOLVED_PRIMITIVES, IA_RESOLVED_PRIMITIVES_PER_INSTANCE,
                  IA_RESOLVED_KEY_STRIDE, IA_RESOLVED_INSTANCES,
                  IA_RESOLVED_OVERFLOW, IA_RESOLVED_VERTICES,
                  IA_RESOLVED_CAPTURE_ARGS, IA_RESOLVED_CAPTURE_ARGS,
                  IA_RESOLVE_THREADS, IA_RESOLVE_THREADS);
    return source;
}

}  // namespace vitreo::graphics
