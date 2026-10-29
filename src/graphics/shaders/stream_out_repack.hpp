// SPDX-License-Identifier: LGPL-2.1-only
/// @file stream_out_repack.hpp
/// @brief Stream output: the compute pass that writes captured vertices into
///        the guest's stream-out buffers.
///
/// Metal has no stream-output stage. The capture draw (pass A of the geometry
/// emulation, .docs/design-geometry-shader-emulation.md) writes every output
/// register of every vertex into a buffer keyed by vertex index. This kernel
/// then walks the draw's PRIMITIVES in order and writes each vertex's declared
/// components to the four stream-out targets, which is what D3D's stream-output
/// stage does after the vertex (or geometry) stage.
///
/// The ordering rules are measured, not assumed
/// (tools/oracle_probe/d3d11_stream_out_probe.c on Windows):
///
///  - Strips are written as LISTS. A line strip's line n is (n, n+1).
///  - A triangle strip's triangle n is (n, n+1, n+2) when n is even and
///    (n, n+2, n+1) when n is odd. That is D3D's winding fix-up, and it is NOT
///    the (n+1, n, n+2) Metal and OpenGL use for rasterisation.
///  - An indexed draw's vertex is the INDEX VALUE: records are in index order.
///  - Only whole primitives are written.
///
/// The kernel takes everything per draw from a control block of 32-bit words
/// (`build_stream_out_control`), so one pipeline serves every declaration.

#pragma once

#include "../contracts/backend_device.hpp"
#include "gs_passthrough.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace vitreo::graphics {

/// A declaration entry, already resolved against the capture record layout.
struct StreamOutRepackEntry {
    /// Stream-out target, 0..3.
    u32 slot{0};
    /// Where in the target's vertex the components go, in DWORDs.
    u32 dst_dword{0};
    /// vec4 slot within the captured record, or STREAM_OUT_GAP for a
    /// declaration gap (a NULL SemanticName), which writes nothing: the oracle
    /// shows the skipped bytes keep their previous contents.
    u32 src_record{0};
    u32 start_component{0};
    u32 component_count{0};
};

inline constexpr u32 STREAM_OUT_GAP = 0xFFFFFFFFu;

enum class StreamOutIndexMode : u32 { None = 0, Uint16 = 1, Uint32 = 2 };

struct StreamOutRepackDesc {
    StreamOutIndexMode index_mode{StreamOutIndexMode::None};
    /// Non-indexed: the first vertex id. Indexed: the first index ELEMENT,
    /// counted from the start of the bound index buffer.
    u32 first{0};
    /// Added to an index value to form the vertex id (BaseVertexLocation).
    i32 base_vertex{0};
    /// 1, 2 or 3.
    u32 vertices_per_primitive{1};
    /// Line strip or triangle strip.
    bool strip{false};
    /// Whole primitives to write.
    u32 primitive_count{0};
    /// vec4 slots per captured vertex record.
    u32 record_vec4s{0};
    /// Where this draw's first vertex goes in each target, in bytes.
    std::array<u32, BACKEND_MAX_STREAM_OUT_TARGETS> dst_offsets{};
    std::array<u32, BACKEND_MAX_STREAM_OUT_TARGETS> strides{};
    std::vector<StreamOutRepackEntry> entries;
};

/// Control-block layout, in 32-bit words. How many vertices the kernel
/// writes and where each target's first one lands are NOT here: they are
/// the stream-output accounting's (`vitreo_so_prepare`), made on the GPU
/// timeline and read from the op record (SO_OP_*).
inline constexpr u32 SO_CTL_INDEX_MODE = 0;
inline constexpr u32 SO_CTL_FIRST = 1;
inline constexpr u32 SO_CTL_BASE_VERTEX = 2;
inline constexpr u32 SO_CTL_VERTS_PER_PRIM = 3;
inline constexpr u32 SO_CTL_STRIP = 4;
inline constexpr u32 SO_CTL_RECORD_VEC4S = 5;
inline constexpr u32 SO_CTL_ENTRY_COUNT = 6;
inline constexpr u32 SO_CTL_STRIDES = 8;
inline constexpr u32 SO_CTL_ENTRIES = 12;
inline constexpr u32 SO_CTL_ENTRY_WORDS = 4;

/// Op record, written by `vitreo_so_prepare` and read by the repack: the
/// vertices to write (whole primitives that fit) and each slot's byte offset.
inline constexpr u32 SO_OP_VERTEX_COUNT = 0;
inline constexpr u32 SO_OP_DST_OFFSETS = 1;
inline constexpr u32 SO_OP_WORDS = 1 + BACKEND_MAX_STREAM_OUT_TARGETS;

/// Compute buffer indices. The kernel is Vitreo's own, so it borrows the SRV
/// and UAV windows rather than the guest's registers.
inline constexpr unsigned SO_REPACK_CAPTURE_INDEX = BACKEND_SRV_BUFFER_BASE;
inline constexpr unsigned SO_REPACK_CONTROL_INDEX = BACKEND_SRV_BUFFER_BASE + 1;
inline constexpr unsigned SO_REPACK_INDICES_INDEX = BACKEND_SRV_BUFFER_BASE + 2;
inline constexpr unsigned SO_REPACK_OP_INDEX = BACKEND_SRV_BUFFER_BASE + 3;
inline constexpr unsigned SO_REPACK_TARGET_BASE = BACKEND_UAV_BUFFER_BASE;
inline constexpr unsigned SO_REPACK_THREADS_PER_GROUP = 64;

inline constexpr const char* kStreamOutRepackEntryPoint = "vitreo_so_repack";

/// Whole primitives a draw of @p vertex_count vertices produces.
[[nodiscard]] constexpr u32 stream_out_primitive_count(u32 vertices_per_primitive,
                                                       bool strip,
                                                       u32 vertex_count) {
    if (vertices_per_primitive == 0) return 0;
    if (!strip) return vertex_count / vertices_per_primitive;
    return vertex_count >= vertices_per_primitive
               ? vertex_count - (vertices_per_primitive - 1)
               : 0;
}

/// Which vertex of the draw the @p output_vertex-th written vertex comes from.
/// The kernel computes exactly this; it is here so the rule is testable on the
/// CPU against the Windows measurement.
[[nodiscard]] constexpr u32 stream_out_source_vertex(u32 vertices_per_primitive,
                                                     bool strip,
                                                     u32 output_vertex) {
    const u32 primitive = output_vertex / vertices_per_primitive;
    const u32 k = output_vertex % vertices_per_primitive;
    if (!strip) return output_vertex;
    if (vertices_per_primitive == 3 && (primitive & 1u) != 0u && k != 0u) {
        return primitive + (3u - k);
    }
    return primitive + k;
}

enum class PrimitiveAssembly : u8 { List, Strip, StripAdjacency };

/// Primitive assembly as a geometry shader and stream output receive it: the
/// vertex ids (index + base vertex), `vertices_per_primitive` a primitive, in
/// order. Measured on Windows (tools/oracle_probe/d3d11_strip_order_probe.c,
/// d3d11_tristrip_adj_probe.c; .expected beside each):
///  - an ODD triangle of a strip arrives as (n, n+2, n+1), for a geometry
///    shader exactly as for stream output;
///  - in an indexed strip the cut value (0xFFFF / 0xFFFFFFFF) ends the strip
///    and the next vertex starts a new one, parity restarting with it;
///  - in an indexed list, the cut value discards the partial primitive
///    before it and assembly restarts after it (D3D11.3 8.12/8.13; measured
///    tools/oracle_probe/d3d11_a07_ia_probe.expected `A idx16 TRIANGLELIST
///    containing 0xFFFF`: {10,11,12,13,FFFF,14,...} gives (14,15,16) next);
///  - a triangle strip with adjacency takes its six vertices from
///    triangle_strip_adjacency_vertex, per strip segment.
/// @p cut is honoured only when @p indexed.
[[nodiscard]] inline std::vector<u32> assemble_primitive_vertices(
    const std::vector<u32>& indices, bool indexed, u32 cut, i32 base_vertex,
    u32 vertices_per_primitive, PrimitiveAssembly kind) {
    std::vector<u32> out;
    const u32 vpp = vertices_per_primitive;
    if (vpp == 0) return out;
    const auto id = [&](u32 index) {
        return static_cast<u32>(static_cast<i64>(index) + base_vertex);
    };
    if (kind == PrimitiveAssembly::List) {
        u32 held = 0;
        usize primitive_begin = 0;
        for (usize i = 0; i < indices.size(); ++i) {
            if (indexed && indices[i] == cut) {
                held = 0;
                continue;
            }
            if (held == 0) primitive_begin = i;
            if (++held < vpp) continue;
            held = 0;
            // No cut can sit inside [primitive_begin, i]: one resets held.
            for (usize k = primitive_begin; k <= i; ++k) {
                out.push_back(id(indices[k]));
            }
        }
        return out;
    }
    usize begin = 0;
    while (begin < indices.size()) {
        usize end = begin;
        while (end < indices.size() && !(indexed && indices[end] == cut)) {
            ++end;
        }
        const u32 length = static_cast<u32>(end - begin);
        const u32* s = indices.data() + begin;
        if (kind == PrimitiveAssembly::StripAdjacency) {
            const u32 triangles = triangle_strip_adjacency_count(length);
            for (u32 i = 0; i < triangles; ++i) {
                for (u32 k = 0; k < 6; ++k) {
                    out.push_back(
                        id(s[triangle_strip_adjacency_vertex(i, k, triangles)]));
                }
            }
        } else if (length >= vpp) {
            for (u32 j = 0; j + vpp <= length; ++j) {
                for (u32 k = 0; k < vpp; ++k) {
                    const u32 kk = (vpp == 3 && (j & 1u) != 0 && k != 0)
                                       ? 3u - k
                                       : k;
                    out.push_back(id(s[j + kk]));
                }
            }
        }
        begin = end + 1;
    }
    return out;
}

[[nodiscard]] inline std::vector<u32> build_stream_out_control(
    const StreamOutRepackDesc& desc) {
    std::vector<u32> words(SO_CTL_ENTRIES +
                           SO_CTL_ENTRY_WORDS * desc.entries.size());
    words[SO_CTL_INDEX_MODE] = static_cast<u32>(desc.index_mode);
    words[SO_CTL_FIRST] = desc.first;
    words[SO_CTL_BASE_VERTEX] = static_cast<u32>(desc.base_vertex);
    words[SO_CTL_VERTS_PER_PRIM] = desc.vertices_per_primitive;
    words[SO_CTL_STRIP] = desc.strip ? 1u : 0u;
    words[SO_CTL_RECORD_VEC4S] = desc.record_vec4s;
    words[SO_CTL_ENTRY_COUNT] = static_cast<u32>(desc.entries.size());
    for (u32 slot = 0; slot < BACKEND_MAX_STREAM_OUT_TARGETS; ++slot) {
        words[SO_CTL_STRIDES + slot] = desc.strides[slot];
    }
    for (usize i = 0; i < desc.entries.size(); ++i) {
        const auto& e = desc.entries[i];
        u32* w = words.data() + SO_CTL_ENTRIES + SO_CTL_ENTRY_WORDS * i;
        w[0] = e.slot;
        w[1] = e.dst_dword;
        w[2] = e.src_record;
        w[3] = (e.start_component & 0xFFu) |
               ((e.component_count & 0xFFu) << 8);
    }
    return words;
}

/// The op record a desc with CPU-known counts describes -- what
/// `vitreo_so_prepare` writes when every primitive fits at `dst_offsets`.
/// For running the repack on its own (tests).
[[nodiscard]] inline std::array<u32, SO_OP_WORDS> build_stream_out_op(
    const StreamOutRepackDesc& desc) {
    std::array<u32, SO_OP_WORDS> op{};
    op[SO_OP_VERTEX_COUNT] = desc.primitive_count * desc.vertices_per_primitive;
    for (u32 slot = 0; slot < BACKEND_MAX_STREAM_OUT_TARGETS; ++slot) {
        op[SO_OP_DST_OFFSETS + slot] = desc.dst_offsets[slot];
    }
    return op;
}

// -----------------------------------------------------------------------------
// Stream-output accounting on the GPU timeline
// -----------------------------------------------------------------------------
//
// D3D keeps each stream-output buffer's BufferFilledSize in memory the GPU
// owns (D3D11.3 14.4: "without the CPU knowing how full the Buffer is"), and
// the counts a draw streams can be GPU-sourced: an indirect draw's argument
// block, a geometry shader's emits, a tessellator's output, DrawAuto of
// another stream's output. So the accounting runs where those counts are:
//
//   vitreo_so_compact  (geometry programs) the vertices each invocation wrote
//                      to one stream, in D3D's order, as a list of vertex
//                      slots, and their total;
//   vitreo_so_prepare  one thread: how many whole primitives fit in every
//                      bound slot of the stream (the smallest decides, D3D11.3
//                      20.4.10 / measured), the op record the repack writes by,
//                      the advanced filled sizes, and the query counts
//                      {needed, written} of that stream;
//   vitreo_so_repack   the copy itself (above);
//   vitreo_so_drawauto DrawAuto's vertex count, (filled - offset) / stride
//                      (measured, d3d11_stream_out_probe), as indirect
//                      draw arguments.
//
// Each has a C++ model below with the same arithmetic, which the frontend's
// unit tests execute in place of the GPU.

/// Constants of vitreo_so_prepare (inline constant buffer, index 0).
struct SoPrepareParams {
    /// count_mode 0: the draw's primitives. 1: read from the count buffer
    /// as primitives; 2: read as vertices (a compaction total), divided by
    /// vertices_per_primitive.
    u32 primitives{0};
    u32 count_mode{0};
    u32 vertices_per_primitive{1};
    /// 0..3: which {needed, written} pair of the stats record to add to.
    u32 stream{0};
    /// Slots this stream writes, every one of them bound.
    u32 slot_mask{0};
    /// Count the storage needed and write nothing: no target bound at all, or
    /// a slot of this stream's declaration has no buffer (measured: that
    /// stream writes nothing, the others are unaffected, d3d11_gsso_probe).
    u32 write_nothing{0};
    u32 reserved0{0};
    u32 reserved1{0};
    std::array<u32, BACKEND_MAX_STREAM_OUT_TARGETS> strides{};
    std::array<u32, BACKEND_MAX_STREAM_OUT_TARGETS> sizes{};
};
static_assert(sizeof(SoPrepareParams) == 64);

inline constexpr unsigned SO_PREPARE_COUNT_INDEX = 1;
inline constexpr unsigned SO_PREPARE_FILLED_BASE = 2;  // 2..5, one per slot
inline constexpr unsigned SO_PREPARE_OP_INDEX = 6;
inline constexpr unsigned SO_PREPARE_STATS_INDEX = 7;
/// Stats record: {needed, written} per stream, u32 each.
inline constexpr u32 SO_STATS_WORDS = 2 * 4;
inline constexpr const char* kStreamOutPrepareEntryPoint = "vitreo_so_prepare";

/// The C++ model of vitreo_so_prepare. @p filled holds each slot's filled
/// size (updated), @p op the op record, @p stats the stats record.
inline void stream_out_prepare_model(
    const SoPrepareParams& p, u32 count_word,
    std::array<u32*, BACKEND_MAX_STREAM_OUT_TARGETS> filled,
    std::array<u32, SO_OP_WORDS>& op, std::array<u32, SO_STATS_WORDS>& stats) {
    const u32 vpp = p.vertices_per_primitive == 0 ? 1u : p.vertices_per_primitive;
    const u32 primitives = p.count_mode == 0   ? p.primitives
                           : p.count_mode == 1 ? count_word
                                               : count_word / vpp;
    u32 fit = p.write_nothing != 0 ? 0u : primitives;
    for (u32 s = 0; s < BACKEND_MAX_STREAM_OUT_TARGETS; ++s) {
        if ((p.slot_mask & (1u << s)) == 0 || filled[s] == nullptr) continue;
        const u32 f = *filled[s];
        const u32 per = p.strides[s] * vpp;
        const u32 room = f >= p.sizes[s] ? 0u : p.sizes[s] - f;
        fit = std::min(fit, per == 0 ? 0u : room / per);
    }
    op.fill(0);
    op[SO_OP_VERTEX_COUNT] = fit * vpp;
    for (u32 s = 0; s < BACKEND_MAX_STREAM_OUT_TARGETS; ++s) {
        if ((p.slot_mask & (1u << s)) == 0 || filled[s] == nullptr) continue;
        op[SO_OP_DST_OFFSETS + s] = *filled[s];
        *filled[s] += fit * vpp * p.strides[s];
    }
    stats[p.stream * 2] += primitives;
    stats[p.stream * 2 + 1] += fit;
}

/// Constants of vitreo_so_compact.
struct SoCompactParams {
    /// Invocations to walk; count_mode 1 reads them from the count buffer.
    u32 invocations{0};
    u32 count_mode{0};
    /// Vertex slots each invocation owns in the expansion buffer.
    u32 capacity{0};
    /// The first vertex slot of the stream's region.
    u32 region_base{0};
};
inline constexpr unsigned SO_COMPACT_COUNT_INDEX = 1;
/// The stream's per-invocation written counts (bound at the first one).
inline constexpr unsigned SO_COMPACT_COUNTS_INDEX = 2;
inline constexpr unsigned SO_COMPACT_ORDER_INDEX = 3;
inline constexpr unsigned SO_COMPACT_TOTAL_INDEX = 4;
inline constexpr unsigned SO_COMPACT_THREADS = 256;
inline constexpr const char* kStreamOutCompactEntryPoint = "vitreo_so_compact";

/// The C++ model of vitreo_so_compact: every vertex slot written, in
/// invocation order, then emission order.
[[nodiscard]] inline std::vector<u32> stream_out_compact_model(
    const SoCompactParams& p, u32 invocations,
    const std::vector<u32>& counts) {
    std::vector<u32> order;
    for (u32 i = 0; i < invocations && i < counts.size(); ++i) {
        const u32 written = std::min(counts[i], p.capacity);
        for (u32 k = 0; k < written; ++k) {
            order.push_back(p.region_base + i * p.capacity + k);
        }
    }
    return order;
}

inline constexpr unsigned SO_DRAWAUTO_FILLED_INDEX = 1;
inline constexpr unsigned SO_DRAWAUTO_ARGS_INDEX = 2;
inline constexpr const char* kStreamOutDrawAutoEntryPoint = "vitreo_so_drawauto";

/// DrawAuto (measured, d3d11_stream_out_probe): (filled - offset) / stride
/// vertices from IA slot 0, 0 for a buffer never streamed to.
[[nodiscard]] constexpr u32 stream_out_draw_auto_count(u32 filled, u32 offset,
                                                       u32 stride) {
    return stride == 0 || filled <= offset ? 0u : (filled - offset) / stride;
}

/// The repack kernel. Components are copied as raw 32-bit words: an integer
/// output was captured by reinterpretation and must reach the target the same
/// way, including NaN payloads a float round trip could canonicalise.
///
/// The same library holds the accounting kernels; each pipeline picks its
/// entry point.
[[nodiscard]] inline std::string stream_out_repack_msl() {
    char source[16384];
    std::snprintf(source, sizeof(source), R"(#include <metal_stdlib>
using namespace metal;

kernel void %s(
    device const uint4* capture [[buffer(%u)]],
    device const uint* control [[buffer(%u)]],
    device const uint* indices [[buffer(%u)]],
    device const uint* op [[buffer(%u)]],
    device uint* so0 [[buffer(%u)]],
    device uint* so1 [[buffer(%u)]],
    device uint* so2 [[buffer(%u)]],
    device uint* so3 [[buffer(%u)]],
    uint tid [[thread_position_in_grid]])
{
    if (tid >= op[%u]) return;
    const uint vpp = control[%u];
    const uint primitive = tid / vpp;
    const uint k = tid %% vpp;
    uint v = tid;
    if (control[%u] != 0u) {
        // D3D writes an odd triangle of a strip as (n, n+2, n+1).
        const uint kk = (vpp == 3u && (primitive & 1u) != 0u && k != 0u)
                            ? 3u - k : k;
        v = primitive + kk;
    }
    const uint mode = control[%u];
    uint vid;
    if (mode == 0u) {
        vid = control[%u] + v;
    } else {
        const uint e = control[%u] + v;
        const uint index = mode == 1u
            ? (indices[e >> 1] >> ((e & 1u) * 16u)) & 0xFFFFu
            : indices[e];
        vid = uint(int(index) + as_type<int>(control[%u]));
    }
    const uint regs = control[%u];
    const uint entries = control[%u];
    for (uint i = 0u; i < entries; ++i) {
        const uint base = %u + i * %u;
        const uint src = control[base + 2u];
        if (src == 0xFFFFFFFFu) continue;
        const uint slot = control[base];
        const uint start = control[base + 3u] & 0xFFu;
        const uint count = (control[base + 3u] >> 8) & 0xFFu;
        const uint4 record = capture[vid * regs + src];
        const uint dst = (op[%u + slot] + tid * control[%u + slot]) / 4u
                         + control[base + 1u];
        device uint* target = slot == 0u ? so0
                            : slot == 1u ? so1
                            : slot == 2u ? so2 : so3;
        for (uint c = 0u; c < count; ++c) {
            target[dst + c] = record[start + c];
        }
    }
}

struct SoPrepareParams {
    uint primitives;
    uint count_mode;
    uint vertices_per_primitive;
    uint stream;
    uint slot_mask;
    uint write_nothing;
    uint reserved0;
    uint reserved1;
    uint strides[4];
    uint sizes[4];
};

static void so_fit_slot(constant SoPrepareParams& p, uint s,
                        device const uint* filled, uint vpp, thread uint& fit)
{
    if ((p.slot_mask & (1u << s)) == 0u) return;
    const uint f = filled[0];
    const uint per = p.strides[s] * vpp;
    const uint room = f >= p.sizes[s] ? 0u : p.sizes[s] - f;
    fit = min(fit, per == 0u ? 0u : room / per);
}

static void so_advance_slot(constant SoPrepareParams& p, uint s,
                            device uint* filled, uint vpp, uint fit,
                            device uint* op)
{
    if ((p.slot_mask & (1u << s)) == 0u) return;
    op[%u + s] = filled[0];
    filled[0] = filled[0] + fit * vpp * p.strides[s];
}

kernel void %s(
    constant SoPrepareParams& p [[buffer(0)]],
    device const uint* count [[buffer(%u)]],
    device uint* filled0 [[buffer(%u)]],
    device uint* filled1 [[buffer(%u)]],
    device uint* filled2 [[buffer(%u)]],
    device uint* filled3 [[buffer(%u)]],
    device uint* op [[buffer(%u)]],
    device uint* stats [[buffer(%u)]],
    uint tid [[thread_position_in_grid]])
{
    if (tid != 0u) return;
    const uint vpp = max(p.vertices_per_primitive, 1u);
    const uint primitives = p.count_mode == 0u ? p.primitives
                          : p.count_mode == 1u ? count[0]
                                               : count[0] / vpp;
    uint fit = p.write_nothing != 0u ? 0u : primitives;
    so_fit_slot(p, 0u, filled0, vpp, fit);
    so_fit_slot(p, 1u, filled1, vpp, fit);
    so_fit_slot(p, 2u, filled2, vpp, fit);
    so_fit_slot(p, 3u, filled3, vpp, fit);
    for (uint w = 0u; w < %uu; ++w) op[w] = 0u;
    op[%u] = fit * vpp;
    so_advance_slot(p, 0u, filled0, vpp, fit, op);
    so_advance_slot(p, 1u, filled1, vpp, fit, op);
    so_advance_slot(p, 2u, filled2, vpp, fit, op);
    so_advance_slot(p, 3u, filled3, vpp, fit, op);
    stats[p.stream * 2u] += primitives;
    stats[p.stream * 2u + 1u] += fit;
}

struct SoCompactParams {
    uint invocations;
    uint count_mode;
    uint capacity;
    uint region_base;
};

kernel void %s(
    constant SoCompactParams& p [[buffer(0)]],
    device const uint* invocation_count [[buffer(%u)]],
    device const uint* counts [[buffer(%u)]],
    device uint* order [[buffer(%u)]],
    device uint* total [[buffer(%u)]],
    uint t [[thread_index_in_threadgroup]])
{
    threadgroup uint sums[%u];
    const uint n = p.count_mode != 0u ? invocation_count[0] : p.invocations;
    const uint chunk = (n + %uu - 1u) / %uu;
    const uint begin = min(t * chunk, n);
    const uint end = min(begin + chunk, n);
    uint sum = 0u;
    for (uint i = begin; i < end; ++i) sum += min(counts[i], p.capacity);
    sums[t] = sum;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (t == 0u) {
        uint running = 0u;
        for (uint i = 0u; i < %uu; ++i) {
            const uint s = sums[i];
            sums[i] = running;
            running += s;
        }
        total[0] = running;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint at = sums[t];
    for (uint i = begin; i < end; ++i) {
        const uint written = min(counts[i], p.capacity);
        for (uint k = 0u; k < written; ++k) {
            order[at++] = p.region_base + i * p.capacity + k;
        }
    }
}

kernel void %s(
    constant uint2& p [[buffer(0)]],
    device const uint* filled [[buffer(%u)]],
    device uint* args [[buffer(%u)]],
    uint tid [[thread_position_in_grid]])
{
    if (tid != 0u) return;
    const uint f = filled[0];
    args[0] = (p.y == 0u || f <= p.x) ? 0u : (f - p.x) / p.y;
    args[1] = 1u;
    args[2] = 0u;
    args[3] = 0u;
}
)",
                  kStreamOutRepackEntryPoint, SO_REPACK_CAPTURE_INDEX,
                  SO_REPACK_CONTROL_INDEX, SO_REPACK_INDICES_INDEX,
                  SO_REPACK_OP_INDEX, SO_REPACK_TARGET_BASE,
                  SO_REPACK_TARGET_BASE + 1, SO_REPACK_TARGET_BASE + 2,
                  SO_REPACK_TARGET_BASE + 3, SO_OP_VERTEX_COUNT,
                  SO_CTL_VERTS_PER_PRIM, SO_CTL_STRIP, SO_CTL_INDEX_MODE,
                  SO_CTL_FIRST, SO_CTL_FIRST, SO_CTL_BASE_VERTEX,
                  SO_CTL_RECORD_VEC4S, SO_CTL_ENTRY_COUNT, SO_CTL_ENTRIES,
                  SO_CTL_ENTRY_WORDS, SO_OP_DST_OFFSETS, SO_CTL_STRIDES,
                  // prepare
                  SO_OP_DST_OFFSETS, kStreamOutPrepareEntryPoint,
                  SO_PREPARE_COUNT_INDEX, SO_PREPARE_FILLED_BASE,
                  SO_PREPARE_FILLED_BASE + 1, SO_PREPARE_FILLED_BASE + 2,
                  SO_PREPARE_FILLED_BASE + 3, SO_PREPARE_OP_INDEX,
                  SO_PREPARE_STATS_INDEX, SO_OP_WORDS, SO_OP_VERTEX_COUNT,
                  // compact
                  kStreamOutCompactEntryPoint, SO_COMPACT_COUNT_INDEX,
                  SO_COMPACT_COUNTS_INDEX, SO_COMPACT_ORDER_INDEX,
                  SO_COMPACT_TOTAL_INDEX, SO_COMPACT_THREADS,
                  SO_COMPACT_THREADS, SO_COMPACT_THREADS, SO_COMPACT_THREADS,
                  // drawauto
                  kStreamOutDrawAutoEntryPoint, SO_DRAWAUTO_FILLED_INDEX,
                  SO_DRAWAUTO_ARGS_INDEX);
    return source;
}

}  // namespace vitreo::graphics
