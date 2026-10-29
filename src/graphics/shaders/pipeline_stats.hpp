// SPDX-License-Identifier: LGPL-2.1-only
/// @file pipeline_stats.hpp
/// @brief D3D11_QUERY_PIPELINE_STATISTICS counters that only the GPU knows:
///        the statistics record, the counting kernels, and their C++ models.
///
/// D3D11.3 20.4.7 fixes some counters and gives ranges for the rest (vertex
/// reuse, helper pixels, early depth, guard band). Where it leaves a range,
/// Vitreo reproduces what the reference rasterizer does, as measured on
/// Windows (WARP) by tools/oracle_probe/d3d11_pstats_probe:
///
///  - VSInvocations: the input assembler's primitives (strip cuts and list
///    primitives holding the cut value dropped) are taken in batches of
///    512 / (vertices per primitive) primitives per instance -- 170
///    triangles, 256 lines, 512 points, 85 triangles with adjacency -- and a
///    batch shades each distinct vertex once. Non-indexed vertices are all
///    distinct, so a list shades 3 per triangle and a strip re-shades the two
///    vertices a batch boundary splits; a spare vertex no primitive uses is
///    never shaded (Draw(7) of a list: IAVertices 7, VSInvocations 6).
///  - CInvocations counts every primitive reaching the clipper (degenerate
///    and back-facing ones too: culling follows clipping). CPrimitives is
///    what a Sutherland-Hodgman clipper makes of it against -w <= x, y <= w,
///    0 <= z <= w (z only with DepthClipEnable) and each SV_ClipDistance,
///    a vertex being inside when its distance is >= 0, with no merging of
///    the coincident vertices a plane through a vertex produces: a polygon
///    of n vertices is n - 2 triangles (a triangle over a viewport corner
///    that lies on its edge is 3); a line or point that survives is 1. No
///    guard band: WARP clips at the viewport.
///  - DSInvocations: the domain shader runs for every vertex of every output
///    primitive (3 per triangle, 2 per line -- no reuse), and once per unique
///    point for point output (D3D11.3 20.4.7 "exactly the number of unique
///    points").
///  - GSPrimitives counts the primitives of every stream; CInvocations after
///    a geometry shader counts the rasterized stream's.
///
/// The record is 11 counters in d3d11.h's struct order, each a 64-bit sum
/// kept as {low, high} 32-bit words: Metal has no 64-bit atomic add, so a
/// kernel adds to the low word and carries into the high one when it
/// wrapped. A flags word follows (STATS_FLAGS_WORD).

#pragma once

#include "../contracts/backend_device.hpp"
#include "gs_passthrough.hpp"
#include "ia_resolve.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace vitreo::graphics {

/// Counter indices: D3D11_QUERY_DATA_PIPELINE_STATISTICS field order in
/// d3d11.h, where D3D10's eight fields come first and HS, DS, CS follow
/// (checked by tools/audit_d3d11_query_constants.py).
inline constexpr u32 STAT_IA_VERTICES = 0;
inline constexpr u32 STAT_IA_PRIMITIVES = 1;
inline constexpr u32 STAT_VS_INVOCATIONS = 2;
inline constexpr u32 STAT_GS_INVOCATIONS = 3;
inline constexpr u32 STAT_GS_PRIMITIVES = 4;
inline constexpr u32 STAT_C_INVOCATIONS = 5;
inline constexpr u32 STAT_C_PRIMITIVES = 6;
inline constexpr u32 STAT_PS_INVOCATIONS = 7;
inline constexpr u32 STAT_HS_INVOCATIONS = 8;
inline constexpr u32 STAT_DS_INVOCATIONS = 9;
inline constexpr u32 STAT_CS_INVOCATIONS = 10;
inline constexpr u32 STATS_COUNTERS = 11;
/// Record words: 2 per counter, the flags word -- bit c set: counter c could
/// not be counted (an overflowed resolve or tessellation batch, a key past
/// the capture) -- and padding to 16 bytes.
inline constexpr u32 STATS_FLAGS_WORD = 2 * STATS_COUNTERS;
inline constexpr u32 STATS_RECORD_WORDS = 24;

/// The 64-bit sum of counter @p counter in a record read back as words.
[[nodiscard]] constexpr u64 stats_record_value(const u32* words, u32 counter) {
    return static_cast<u64>(words[2 * counter]) |
           (static_cast<u64>(words[2 * counter + 1]) << 32);
}

/// Primitives one VSInvocations batch holds (WARP, measured).
[[nodiscard]] constexpr u32 stats_vertex_batch(u32 vertices_per_primitive) {
    return vertices_per_primitive == 0
               ? 1u
               : std::max(512u / vertices_per_primitive, 1u);
}

/// VSInvocations of one instance whose assembled primitives are @p keys
/// (@p vpp keys a primitive; the vertex id -- an index value, or the vertex
/// number of a non-indexed draw). The C++ model of vitreo_stats_vertices.
[[nodiscard]] inline u64 stats_vertex_invocations(const std::vector<u32>& keys,
                                                  u32 vpp) {
    if (vpp == 0) return 0;
    const u64 primitives = keys.size() / vpp;
    const u64 batch = stats_vertex_batch(vpp);
    u64 total = 0;
    std::vector<u32> seen;
    for (u64 first = 0; first < primitives; first += batch) {
        const u64 last = std::min(primitives, first + batch);
        seen.assign(keys.begin() + static_cast<std::ptrdiff_t>(first * vpp),
                    keys.begin() + static_cast<std::ptrdiff_t>(last * vpp));
        std::sort(seen.begin(), seen.end());
        total += static_cast<u64>(
            std::unique(seen.begin(), seen.end()) - seen.begin());
    }
    return total;
}

/// VSInvocations of one instance of a NON-indexed draw: list-like
/// assemblies shade vpp per primitive, a strip (@p strip, vertices shared by
/// consecutive primitives) its batch's k primitives' k + vpp - 1 vertices.
[[nodiscard]] constexpr u64 stats_vertex_invocations_non_indexed(
    u64 primitives, u32 vpp, bool strip) {
    if (vpp == 0 || primitives == 0) return 0;
    if (!strip) return primitives * vpp;
    const u64 batch = stats_vertex_batch(vpp);
    const u64 full = primitives / batch;
    const u64 rest = primitives % batch;
    return full * (batch + vpp - 1) + (rest != 0 ? rest + vpp - 1 : 0);
}

/// One vertex entering the clipper: clip-space position and user clip
/// distances.
struct StatsClipVertex {
    std::array<float, 4> position{};
    std::array<float, 8> distances{};
};

/// Primitives the clipper makes of one primitive of @p count (1-3) vertices
/// (see the file comment). @p distances: SV_ClipDistance components in use.
/// The C++ model of the kernels' stats_clip.
[[nodiscard]] inline u32 stats_clip_primitives(const StatsClipVertex* in,
                                               u32 count, u32 distances,
                                               bool depth_clip) {
    const u32 planes = 6 + distances;
    const auto plane = [&](const StatsClipVertex& v, u32 p) -> float {
        switch (p) {
        case 0: return v.position[0] + v.position[3];
        case 1: return v.position[3] - v.position[0];
        case 2: return v.position[1] + v.position[3];
        case 3: return v.position[3] - v.position[1];
        case 4: return v.position[2];
        case 5: return v.position[3] - v.position[2];
        default: return v.distances[p - 6];
        }
    };
    const auto active = [&](u32 p) { return depth_clip || (p != 4 && p != 5); };
    const auto lerp = [](const StatsClipVertex& s, const StatsClipVertex& e,
                         float t) {
        StatsClipVertex r{};
        for (u32 k = 0; k < 4; ++k) {
            r.position[k] = s.position[k] + t * (e.position[k] - s.position[k]);
        }
        for (u32 k = 0; k < 8; ++k) {
            r.distances[k] =
                s.distances[k] + t * (e.distances[k] - s.distances[k]);
        }
        return r;
    };
    if (count == 1) {
        for (u32 p = 0; p < planes; ++p) {
            if (active(p) && !(plane(in[0], p) >= 0.0f)) return 0;
        }
        return 1;
    }
    if (count == 2) {
        StatsClipVertex a = in[0];
        StatsClipVertex b = in[1];
        for (u32 p = 0; p < planes; ++p) {
            if (!active(p)) continue;
            const float da = plane(a, p);
            const float db = plane(b, p);
            const bool ia = da >= 0.0f;
            const bool ib = db >= 0.0f;
            if (!ia && !ib) return 0;
            if (!ia) a = lerp(a, b, da / (da - db));
            if (!ib) b = lerp(b, a, db / (db - da));
        }
        return 1;
    }
    std::vector<StatsClipVertex> poly(in, in + 3);
    std::vector<StatsClipVertex> out;
    for (u32 p = 0; p < planes && !poly.empty(); ++p) {
        if (!active(p)) continue;
        out.clear();
        for (usize i = 0; i < poly.size(); ++i) {
            const StatsClipVertex& s = poly[(i + poly.size() - 1) % poly.size()];
            const StatsClipVertex& e = poly[i];
            const float ds = plane(s, p);
            const float de = plane(e, p);
            const bool is = ds >= 0.0f;
            const bool ie = de >= 0.0f;
            if (ie) {
                if (!is) out.push_back(lerp(s, e, ds / (ds - de)));
                out.push_back(e);
            } else if (is) {
                out.push_back(lerp(s, e, ds / (ds - de)));
            }
        }
        poly.swap(out);
    }
    return poly.size() >= 3 ? static_cast<u32>(poly.size() - 2) : 0u;
}

/// Where a record buffer holds what the clipper reads: the vec4 slot of the
/// position, and the slot and component of each SV_ClipDistance component
/// (in clip-distance order).
struct StatsClipLayout {
    bool valid{false};
    u32 position_slot{0};
    u32 distances{0};
    std::array<u32, 8> distance_slot{};
    std::array<u32, 8> distance_component{};
};

/// Kernel constants (inline constant buffer, index 0) of the three
/// primitive kernels. One layout serves all three.
struct StatsKernelParams {
    /// vitreo_stats_vertices / _primitives: vertices of an input-assembler
    /// primitive; _geometry / _tessellation: of an output primitive.
    u32 vertices_per_primitive{0};
    /// Primitives a VSInvocations batch holds.
    u32 batch{0};
    /// Bit 0: add IAVertices from the argument block (count x instances);
    /// bit 1: IAPrimitives; bit 2: CInvocations; bit 3: HSInvocations; bit 4:
    /// VSInvocations (all from the resolve). _primitives / _geometry /
    /// _tessellation: bit 5 clip (CInvocations + CPrimitives); bit 6
    /// DSInvocations; bit 7 depth clip; bit 8 counts come from the resolve
    /// (else the constants below).
    u32 flags{0};
    /// Where the IA primitive's rasterized vertices are, as three 8-bit
    /// indices (adjacency primitives rasterize 0,2,4 or 1,2), and how many.
    u32 raster_pick{0x020100u};
    u32 raster_vertices{3};
    /// How _primitives finds a primitive's keys: 0 the ids buffer (every
    /// instance, a resolve's), 1 the ids buffer (instance 0) plus instance x
    /// key_stride, 2 a non-indexed list, 3 a non-indexed strip, 4 a
    /// non-indexed triangle strip with adjacency.
    u32 key_mode{0};
    u32 primitives_per_instance{0};
    u32 instances{0};
    u32 key_stride{0};
    /// Records: vec4s each, vec4 offset of record 0 in the buffer.
    u32 record_vec4s{0};
    u32 record_base{0};
    u32 position_slot{0};
    u32 distances{0};
    /// Slot << 2 | component of each clip distance.
    std::array<u32, 8> distance_slots{};
    /// _geometry: invocations, the geometry program's instances, vertex slots
    /// per invocation, stream mask, rasterized stream (4: none).
    u32 invocations{0};
    u32 gs_instances{1};
    u32 vertices_per_invocation{0};
    u32 stream_mask{1};
    u32 raster_stream{0};
    /// Grid bound: primitives (or invocations) the dispatch covers.
    u32 capacity{0};
    std::array<u32, 5> pad{};
};
static_assert(sizeof(StatsKernelParams) == 128);

inline constexpr u32 STATS_FLAG_ADD_IA_VERTICES = 1u << 0;
inline constexpr u32 STATS_FLAG_ADD_IA_PRIMITIVES = 1u << 1;
inline constexpr u32 STATS_FLAG_ADD_C_INVOCATIONS = 1u << 2;
inline constexpr u32 STATS_FLAG_ADD_HS_INVOCATIONS = 1u << 3;
inline constexpr u32 STATS_FLAG_ADD_VS_INVOCATIONS = 1u << 4;
inline constexpr u32 STATS_FLAG_CLIP = 1u << 5;
inline constexpr u32 STATS_FLAG_ADD_DS_INVOCATIONS = 1u << 6;
inline constexpr u32 STATS_FLAG_DEPTH_CLIP = 1u << 7;
inline constexpr u32 STATS_FLAG_RESOLVED_COUNTS = 1u << 8;

/// The software tessellator's batch-block words vitreo_stats_tessellation
/// reads (tess::TESS_BLOCK_INDICES / _PRIMITIVES / _OVERFLOW; checked where
/// both headers meet, d3d11_tessellation.cpp).
inline constexpr u32 STATS_TESS_BLOCK_INDICES = 8;
inline constexpr u32 STATS_TESS_BLOCK_PRIMITIVES = 9;
inline constexpr u32 STATS_TESS_BLOCK_OVERFLOW = 10;

/// Buffer indices of the kernels.
inline constexpr unsigned STATS_RECORD_INDEX = 1;
inline constexpr unsigned STATS_COUNTS_INDEX = 2;   ///< resolve / GS counter / tess block
inline constexpr unsigned STATS_IDS_INDEX = 3;      ///< keys / tess indices
inline constexpr unsigned STATS_RECORDS_INDEX = 4;  ///< vertex records
inline constexpr unsigned STATS_ARGS_INDEX = 5;     ///< draw argument block
inline constexpr unsigned STATS_THREADS = 64;
/// vitreo_stats_vertices: one threadgroup per batch, two keys a thread.
inline constexpr unsigned STATS_BATCH_THREADS = 256;
inline constexpr const char* kStatsVerticesEntryPoint = "vitreo_stats_vertices";
inline constexpr const char* kStatsPrimitivesEntryPoint =
    "vitreo_stats_primitives";
inline constexpr const char* kStatsGeometryEntryPoint = "vitreo_stats_geometry";
inline constexpr const char* kStatsTessellationEntryPoint =
    "vitreo_stats_tessellation";

/// The four kernels (one library).
[[nodiscard]] inline std::string pipeline_stats_msl() {
    const auto u = [](u32 v) { return std::to_string(v) + "u"; };
    std::string msl = R"(#include <metal_stdlib>
using namespace metal;

struct StatsParams {
    uint vpp;
    uint batch;
    uint flags;
    uint raster_pick;
    uint raster_vertices;
    uint key_mode;
    uint primitives_per_instance;
    uint instances;
    uint key_stride;
    uint record_vec4s;
    uint record_base;
    uint position_slot;
    uint distances;
    uint distance_slots[8];
    uint invocations;
    uint gs_instances;
    uint vertices_per_invocation;
    uint stream_mask;
    uint raster_stream;
    uint capacity;
    uint pad[5];
};

// 64-bit sums from 32-bit atomics: add to the low word, carry on wrap.
static void stats_add(device atomic_uint* r, uint counter, ulong value)
{
    const uint lo = uint(value);
    uint hi = uint(value >> 32);
    if (lo != 0u) {
        const uint old = atomic_fetch_add_explicit(r + 2u * counter, lo,
                                                   memory_order_relaxed);
        if (old + lo < old) hi += 1u;
    }
    if (hi != 0u) {
        atomic_fetch_add_explicit(r + 2u * counter + 1u, hi,
                                  memory_order_relaxed);
    }
}

static void stats_flag(device atomic_uint* r, uint bits)
{
    atomic_fetch_or_explicit(r + )" + u(STATS_FLAGS_WORD) + R"(, bits,
                             memory_order_relaxed);
}

struct ClipV {
    float4 p;
    float d[8];
};

static float clip_plane(thread const ClipV& v, uint plane)
{
    switch (plane) {
    case 0u: return v.p.x + v.p.w;
    case 1u: return v.p.w - v.p.x;
    case 2u: return v.p.y + v.p.w;
    case 3u: return v.p.w - v.p.y;
    case 4u: return v.p.z;
    case 5u: return v.p.w - v.p.z;
    default: return v.d[plane - 6u];
    }
}

static ClipV clip_lerp(thread const ClipV& s, thread const ClipV& e, float t)
{
    ClipV r;
    r.p = s.p + t * (e.p - s.p);
    for (uint k = 0u; k < 8u; ++k) r.d[k] = s.d[k] + t * (e.d[k] - s.d[k]);
    return r;
}

// graphics::stats_clip_primitives: Sutherland-Hodgman, inside when >= 0,
// coincident vertices kept, n vertices = n - 2 triangles.
static uint stats_clip(thread ClipV* v, uint count, uint distances,
                       bool depth_clip)
{
    const uint planes = 6u + distances;
    bool all_in = true;
    for (uint p = 0u; p < planes; ++p) {
        if (!depth_clip && (p == 4u || p == 5u)) continue;
        uint outside = 0u;
        for (uint i = 0u; i < count; ++i) {
            if (!(clip_plane(v[i], p) >= 0.0f)) ++outside;
        }
        if (outside == count) return 0u;
        if (outside != 0u) all_in = false;
    }
    if (all_in) return 1u;
    if (count == 2u) {
        ClipV a = v[0];
        ClipV b = v[1];
        for (uint p = 0u; p < planes; ++p) {
            if (!depth_clip && (p == 4u || p == 5u)) continue;
            const float da = clip_plane(a, p);
            const float db = clip_plane(b, p);
            if (!(da >= 0.0f) && !(db >= 0.0f)) return 0u;
            if (!(da >= 0.0f)) a = clip_lerp(a, b, da / (da - db));
            if (!(db >= 0.0f)) b = clip_lerp(b, a, db / (db - da));
        }
        return 1u;
    }
    ClipV poly[2][24];
    uint n = 3u;
    uint cur = 0u;
    for (uint i = 0u; i < 3u; ++i) poly[0][i] = v[i];
    for (uint p = 0u; p < planes && n != 0u; ++p) {
        if (!depth_clip && (p == 4u || p == 5u)) continue;
        uint m = 0u;
        for (uint i = 0u; i < n; ++i) {
            const ClipV s = poly[cur][(i + n - 1u) % n];
            const ClipV e = poly[cur][i];
            const float ds = clip_plane(s, p);
            const float de = clip_plane(e, p);
            const bool is = ds >= 0.0f;
            const bool ie = de >= 0.0f;
            if (ie) {
                if (!is && m < 24u) poly[1u - cur][m++] =
                    clip_lerp(s, e, ds / (ds - de));
                if (m < 24u) poly[1u - cur][m++] = e;
            } else if (is && m < 24u) {
                poly[1u - cur][m++] = clip_lerp(s, e, ds / (ds - de));
            }
        }
        cur = 1u - cur;
        n = m;
    }
    return n >= 3u ? n - 2u : 0u;
}

static ClipV clip_vertex(constant StatsParams& p, device const float4* records,
                         uint record)
{
    ClipV v;
    const uint base = p.record_base + record * p.record_vec4s;
    v.p = records[base + p.position_slot];
    for (uint k = 0u; k < 8u; ++k) {
        v.d[k] = 0.0f;
        if (k < p.distances) {
            const uint slot = p.distance_slots[k];
            v.d[k] = records[base + (slot >> 2)][slot & 3u];
        }
    }
    return v;
}

// A simdgroup's sum, added once.
static void stats_add_simd(device atomic_uint* r, uint counter, uint value)
{
    const uint total = simd_sum(value);
    if (simd_is_first() && total != 0u) stats_add(r, counter, ulong(total));
}

// graphics::ia_primitives' non-indexed keys (ia_resolve.hpp).
static uint strip_adjacency_key(uint i, uint k, uint triangles)
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

// Key k of primitive i of instance 0 (key modes 1-4; mode 0 reads ids).
static uint primitive_key(constant StatsParams& p, device const uint* ids,
                          uint i, uint k)
{
    switch (p.key_mode) {
    case 2u: return i * p.vpp + k;
    case 3u: {
        const uint kk = (p.vpp == 3u && (i & 1u) != 0u && k != 0u) ? 3u - k : k;
        return i + kk;
    }
    case 4u: return strip_adjacency_key(i, k, p.primitives_per_instance);
    default: return ids[i * p.vpp + k];
    }
}

// VSInvocations (and the other input-assembler counts) of a draw whose
// primitives a resolve assembled: one threadgroup per batch of instance 0,
// times the instances (every instance assembles alike).
kernel void )" + std::string(kStatsVerticesEntryPoint) + R"((
    constant StatsParams& p [[buffer(0)]],
    device atomic_uint* record [[buffer()" + u(STATS_RECORD_INDEX) + R"()]],
    device const uint* resolved [[buffer()" + u(STATS_COUNTS_INDEX) + R"()]],
    device const uint* ids [[buffer()" + u(STATS_IDS_INDEX) + R"()]],
    device const uint* args [[buffer()" + u(STATS_ARGS_INDEX) + R"()]],
    uint group [[threadgroup_position_in_grid]],
    uint t [[thread_index_in_threadgroup]])
{
    threadgroup uint keys[512];
    threadgroup atomic_uint unique;
    const uint per_instance = resolved[)" + u(IA_RESOLVED_PRIMITIVES_PER_INSTANCE) + R"(];
    const uint instances = resolved[)" + u(IA_RESOLVED_INSTANCES) + R"(];
    if (group == 0u && t == 0u) {
        if (resolved[)" + u(IA_RESOLVED_OVERFLOW) + R"(] != 0u) stats_flag(record, 0x7FFu);
        const ulong primitives = ulong(resolved[)" + u(IA_RESOLVED_PRIMITIVES) + R"(]);
        if ((p.flags & )" + u(STATS_FLAG_ADD_IA_VERTICES) + R"() != 0u) {
            stats_add(record, )" + u(STAT_IA_VERTICES) + R"(,
                      ulong(args[0]) * ulong(args[1]));
        }
        if ((p.flags & )" + u(STATS_FLAG_ADD_IA_PRIMITIVES) + R"() != 0u) {
            stats_add(record, )" + u(STAT_IA_PRIMITIVES) + R"(, primitives);
        }
        if ((p.flags & )" + u(STATS_FLAG_ADD_C_INVOCATIONS) + R"() != 0u) {
            stats_add(record, )" + u(STAT_C_INVOCATIONS) + R"(, primitives);
        }
        if ((p.flags & )" + u(STATS_FLAG_ADD_HS_INVOCATIONS) + R"() != 0u) {
            stats_add(record, )" + u(STAT_HS_INVOCATIONS) + R"(, primitives);
        }
    }
    if ((p.flags & )" + u(STATS_FLAG_ADD_VS_INVOCATIONS) + R"() == 0u) return;
    const uint first = group * p.batch;
    if (first >= per_instance || instances == 0u) return;
    const uint in_batch = min(p.batch, per_instance - first);
    const uint n = in_batch * p.vpp;
    if (t == 0u) atomic_store_explicit(&unique, 0u, memory_order_relaxed);
    for (uint s = t; s < n; s += )" + u(STATS_BATCH_THREADS) + R"() {
        keys[s] = ids[first * p.vpp + s];
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint mine = 0u;
    for (uint s = t; s < n; s += )" + u(STATS_BATCH_THREADS) + R"() {
        bool repeat = false;
        for (uint r = 0u; r < s && !repeat; ++r) repeat = keys[r] == keys[s];
        if (!repeat) ++mine;
    }
    atomic_fetch_add_explicit(&unique, mine, memory_order_relaxed);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (t == 0u) {
        stats_add(record, )" + u(STAT_VS_INVOCATIONS) + R"(,
                  ulong(atomic_load_explicit(&unique, memory_order_relaxed)) *
                      ulong(instances));
    }
}

// CPrimitives of a rasterized input-assembler draw: one thread per
// primitive (every instance), clipping the vertex shader's captured
// positions.
kernel void )" + std::string(kStatsPrimitivesEntryPoint) + R"((
    constant StatsParams& p [[buffer(0)]],
    device atomic_uint* record [[buffer()" + u(STATS_RECORD_INDEX) + R"()]],
    device const uint* resolved [[buffer()" + u(STATS_COUNTS_INDEX) + R"()]],
    device const uint* ids [[buffer()" + u(STATS_IDS_INDEX) + R"()]],
    device const float4* records [[buffer()" + u(STATS_RECORDS_INDEX) + R"()]],
    uint q [[thread_position_in_grid]])
{
    const bool resolved_counts =
        (p.flags & )" + u(STATS_FLAG_RESOLVED_COUNTS) + R"() != 0u;
    const uint total = resolved_counts
        ? resolved[)" + u(IA_RESOLVED_PRIMITIVES) + R"(]
        : p.primitives_per_instance * p.instances;
    // The capture's {stride, capacity} header: a key past the capacity was
    // not captured.
    const uint captured = as_type<uint4>(records[0]).y;
    uint made = 0u;
    if (q < total && q < p.capacity) {
        ClipV v[3];
        for (uint k = 0u; k < p.raster_vertices; ++k) {
            const uint pick = (p.raster_pick >> (8u * k)) & 0xFFu;
            uint key;
            if (p.key_mode == 0u) {
                key = ids[q * p.vpp + pick];
            } else {
                const uint j = q / p.primitives_per_instance;
                const uint i = q - j * p.primitives_per_instance;
                key = primitive_key(p, ids, i, pick) + j * p.key_stride;
            }
            if (key >= captured) {
                stats_flag(record, 1u << )" + u(STAT_C_PRIMITIVES) + R"();
                key = 0u;
            }
            v[k] = clip_vertex(p, records, key);
        }
        made = stats_clip(v, p.raster_vertices, p.distances,
                          (p.flags & )" + u(STATS_FLAG_DEPTH_CLIP) + R"() != 0u);
    }
    stats_add_simd(record, )" + u(STAT_C_PRIMITIVES) + R"(, made);
}

// A geometry-shader expansion: GSInvocations, GSPrimitives of every stream,
// and CInvocations / CPrimitives of the rasterized one. One thread per
// invocation; the counter buffer holds the vertices each wrote.
kernel void )" + std::string(kStatsGeometryEntryPoint) + R"((
    constant StatsParams& p [[buffer(0)]],
    device atomic_uint* record [[buffer()" + u(STATS_RECORD_INDEX) + R"()]],
    device const uint* counter [[buffer()" + u(STATS_COUNTS_INDEX) + R"()]],
    device const float4* records [[buffer()" + u(STATS_RECORDS_INDEX) + R"()]],
    uint i [[thread_position_in_grid]])
{
    const uint limited =
        (counter[)" + u(kGsControlFlags) + R"(] & )" + u(kGsFlagInvocationLimit) + R"() != 0u
            ? min(p.invocations, counter[)" + u(kGsControlInvocationLimit) + R"(])
            : p.invocations;
    const bool ran = i < limited;
    uint primitives = 0u;
    uint rasterized = 0u;
    uint made = 0u;
    if (ran) {
        const uint stride = counter[)" + u(kGsControlStreamStride) + R"(];
        for (uint s = 0u; s < 4u; ++s) {
            if ((p.stream_mask & (1u << s)) == 0u) continue;
            const uint written =
                counter[)" + u(kGsInvocationCountBase) + R"( + s * stride + i];
            const uint prims = written / p.vpp;
            primitives += prims;
            if (s != p.raster_stream ||
                (p.flags & )" + u(STATS_FLAG_CLIP) + R"() == 0u) {
                continue;
            }
            rasterized += prims;
            const uint region = (s * p.invocations + i) *
                                p.vertices_per_invocation;
            for (uint n = 0u; n < prims; ++n) {
                ClipV v[3];
                for (uint k = 0u; k < p.vpp; ++k) {
                    v[k] = clip_vertex(p, records, region + n * p.vpp + k);
                }
                made += stats_clip(v, p.vpp, p.distances,
                                   (p.flags & )" + u(STATS_FLAG_DEPTH_CLIP) + R"() != 0u);
            }
        }
    }
    stats_add_simd(record, )" + u(STAT_GS_INVOCATIONS) + R"(, ran ? 1u : 0u);
    stats_add_simd(record, )" + u(STAT_GS_PRIMITIVES) + R"(, primitives);
    stats_add_simd(record, )" + u(STAT_C_INVOCATIONS) + R"(, rasterized);
    stats_add_simd(record, )" + u(STAT_C_PRIMITIVES) + R"(, made);
}

// A software-tessellator batch: DSInvocations (every vertex of every output
// primitive; unique points for point output -- one index each), and for a
// rasterized domain shader CInvocations / CPrimitives over its records.
kernel void )" + std::string(kStatsTessellationEntryPoint) + R"((
    constant StatsParams& p [[buffer(0)]],
    device atomic_uint* record [[buffer()" + u(STATS_RECORD_INDEX) + R"()]],
    device const uint* block [[buffer()" + u(STATS_COUNTS_INDEX) + R"()]],
    device const uint* indices [[buffer()" + u(STATS_IDS_INDEX) + R"()]],
    device const float4* records [[buffer()" + u(STATS_RECORDS_INDEX) + R"()]],
    uint q [[thread_position_in_grid]])
{
    if (q == 0u) {
        if (block[)" + u(STATS_TESS_BLOCK_OVERFLOW) + R"(] != 0u) {
            stats_flag(record, 0x7FFu);
        }
        if ((p.flags & )" + u(STATS_FLAG_ADD_DS_INVOCATIONS) + R"() != 0u) {
            stats_add(record, )" + u(STAT_DS_INVOCATIONS) + R"(,
                      ulong(block[)" + u(STATS_TESS_BLOCK_INDICES) + R"(]));
        }
    }
    const uint total = block[)" + u(STATS_TESS_BLOCK_PRIMITIVES) + R"(];
    uint made = 0u;
    uint counted = 0u;
    if ((p.flags & )" + u(STATS_FLAG_CLIP) + R"() != 0u && q < total &&
        q < p.capacity) {
        ClipV v[3];
        for (uint k = 0u; k < p.vpp; ++k) {
            v[k] = clip_vertex(p, records, indices[q * p.vpp + k]);
        }
        made = stats_clip(v, p.vpp, p.distances,
                          (p.flags & )" + u(STATS_FLAG_DEPTH_CLIP) + R"() != 0u);
        counted = 1u;
    }
    stats_add_simd(record, )" + u(STAT_C_INVOCATIONS) + R"(, counted);
    stats_add_simd(record, )" + u(STAT_C_PRIMITIVES) + R"(, made);
}
)";
    return msl;
}

}  // namespace vitreo::graphics
