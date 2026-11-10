// SPDX-License-Identifier: LGPL-2.1-only
/// @file ia_cut_rewrite.hpp
/// @brief Index-buffer strip cuts Metal's primitive restart cannot express.
///
/// Windows (measured, tools/oracle_probe/d3d12_a61_ia_probe.expected on
/// WARP): an index whose value EQUALS the cut value ends a strip, whatever
/// the index format, and in a list or point list it discards the partial
/// primitive before it and assembly restarts after it (D3D11.3 8.12, 8.13).
/// D3D11 uses the format's maximum as the cut value; D3D12 takes it from
/// D3D12_INDEX_BUFFER_STRIP_CUT_VALUE, where DISABLED means no cut value:
/// 0xFFFF in an R16 strip is an ordinary vertex, and so is 0xFFFF in an R32
/// strip whose cut value is 0xFFFFFFFF.
///
/// Metal (measured, tools/oracle_probe/metal/metal_restart_point_facts.m,
/// Apple M4 Pro): a strip always restarts at the index type's maximum and
/// never at any other value, and a list or point list never restarts.
///
/// A draw whose index data holds a value the two treat differently is drawn
/// from a rewritten 32-bit index stream instead:
///  - Widen: an R16 strip without the 0xFFFF cut. As 32-bit indices 0xFFFF
///    is no longer Metal's restart value; the topology stays a strip.
///  - Expand: an R32 strip without a cut. Each primitive becomes a list
///    primitive in the order D3D assembles it (an odd triangle is n, n+2,
///    n+1; graphics::assemble_primitive_vertices), so 0xFFFFFFFF is ordinary.
///  - Assemble: everything else (an R32 strip cut at 0xFFFF; a list or point
///    list holding its cut value): graphics::assemble_primitive_vertices with
///    the cut, as a list.
/// Index values are kept (base vertex unchanged), so SV_VertexID and the
/// fetched vertex are the draw's own; primitive order and SV_PrimitiveID are
/// unchanged.
///
/// vitreo_ia_cut_rewrite does the same on the GPU timeline for an index
/// buffer the CPU cannot read without waiting (or an indirect draw), and
/// writes the argument block of the indexed draw that follows.

#pragma once

#include "stream_out_repack.hpp"

#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace vitreo::graphics {

/// The strip-cut value an indexed draw assembles with.
struct IndexCut {
    bool active{false};
    u32 value{0};
};

/// D3D11's cut value: the format's maximum (D3D11.3 8.12).
[[nodiscard]] constexpr IndexCut format_max_cut(bool index32) {
    return {true, index32 ? 0xFFFFFFFFu : 0xFFFFu};
}

enum class IaCutRewrite : u32 { Native = 0, Widen = 1, Expand = 2, Assemble = 3 };

/// What the index values must hold before a draw needs a rewrite, and which
/// rewrite it then needs. @p strip is a line or triangle strip (without
/// adjacency); a list or point list otherwise.
struct IaCutPlan {
    IaCutRewrite rewrite{IaCutRewrite::Native};
    /// Values that make Metal's assembly differ from Windows' (count 0..2).
    std::array<u32, 2> special{};
    u32 special_count{0};
};

[[nodiscard]] constexpr IaCutPlan ia_cut_plan(bool strip, bool index32,
                                              IndexCut cut) {
    IaCutPlan plan{};
    const u32 format_max = index32 ? 0xFFFFFFFFu : 0xFFFFu;
    // A cut value the format cannot hold never matches an index.
    const bool cut_live = cut.active && cut.value <= format_max;
    if (strip) {
        if (cut_live && cut.value == format_max) return plan;  // Metal's own
        plan.special[plan.special_count++] = format_max;
        if (cut_live) plan.special[plan.special_count++] = cut.value;
        plan.rewrite = !index32       ? IaCutRewrite::Widen
                       : !cut_live    ? IaCutRewrite::Expand
                                      : IaCutRewrite::Assemble;
        return plan;
    }
    if (!cut_live) return plan;
    plan.special[plan.special_count++] = cut.value;
    plan.rewrite = IaCutRewrite::Assemble;
    return plan;
}

[[nodiscard]] inline bool ia_cut_needed(const IaCutPlan& plan,
                                        const std::vector<u32>& indices) {
    if (plan.rewrite == IaCutRewrite::Native) return false;
    for (const u32 v : indices) {
        for (u32 s = 0; s < plan.special_count; ++s) {
            if (v == plan.special[s]) return true;
        }
    }
    return false;
}

/// The rewritten stream (CPU, and the model of the kernel). @p vpp is the
/// topology's vertices per primitive (1 points, 2 lines, 3 triangles).
[[nodiscard]] inline std::vector<u32> ia_cut_rewrite_indices(
    const IaCutPlan& plan, IndexCut cut, bool strip, u32 vpp,
    const std::vector<u32>& indices) {
    switch (plan.rewrite) {
    case IaCutRewrite::Native:
    case IaCutRewrite::Widen:
        return indices;
    case IaCutRewrite::Expand:
        return assemble_primitive_vertices(indices, false, 0, 0, vpp,
                                           PrimitiveAssembly::Strip);
    case IaCutRewrite::Assemble:
        break;
    }
    return assemble_primitive_vertices(
        indices, cut.active, cut.value, 0, vpp,
        strip ? PrimitiveAssembly::Strip : PrimitiveAssembly::List);
}

/// Constants of vitreo_ia_cut_rewrite (inline constant buffer, index 0).
struct IaCutParams {
    u32 index_32bit{0};
    /// The bound index buffer's offset, in indices, from the bound word.
    u32 index_first{0};
    /// Indices the view holds past `index_first` (reads beyond read 0).
    u32 index_elements{0};
    u32 vertices_per_primitive{1};
    u32 strip{0};
    u32 rewrite{0};  ///< IaCutRewrite
    u32 cut_active{0};
    u32 cut_value{0};
    /// 32-bit indices the output holds; a draw needing more draws nothing.
    u32 capacity{0};
    u32 pad[3]{};
};
static_assert(sizeof(IaCutParams) == 48);

inline constexpr unsigned IA_CUT_ARGS_INDEX = 1;     ///< D3D12_DRAW_INDEXED_ARGUMENTS
inline constexpr unsigned IA_CUT_INDICES_INDEX = 2;  ///< the bound index buffer
inline constexpr unsigned IA_CUT_OUT_INDEX = 3;      ///< the 32-bit stream
inline constexpr unsigned IA_CUT_DRAW_INDEX = 4;     ///< the draw's argument block
inline constexpr unsigned IA_CUT_THREADS = 256;
inline constexpr const char* kIaCutEntryPoint = "vitreo_ia_cut_rewrite";

/// Output indices a rewrite of @p count indices can produce.
[[nodiscard]] constexpr u64 ia_cut_capacity(IaCutRewrite rewrite, u32 vpp,
                                            u64 count) {
    return rewrite == IaCutRewrite::Expand || rewrite == IaCutRewrite::Assemble
               ? count * vpp
               : count;
}

[[nodiscard]] inline std::string ia_cut_rewrite_msl() {
    char source[8192];
    std::snprintf(source, sizeof(source), R"(#include <metal_stdlib>
using namespace metal;

struct IaCutParams {
    uint index_32bit;
    uint index_first;
    uint index_elements;
    uint vertices_per_primitive;
    uint strip;
    uint rewrite;
    uint cut_active;
    uint cut_value;
    uint capacity;
    uint pad0, pad1, pad2;
};

static uint cut_index(constant IaCutParams& p, device const uint* ib, uint element)
{
    if (element >= p.index_elements) return 0u;
    const uint e = p.index_first + element;
    return p.index_32bit != 0u ? ib[e]
                               : (ib[e >> 1] >> ((e & 1u) * 16u)) & 0xFFFFu;
}

kernel void %s(
    constant IaCutParams& p [[buffer(0)]],
    device const uint* args [[buffer(%u)]],
    device const uint* ib [[buffer(%u)]],
    device uint* out [[buffer(%u)]],
    device uint* draw [[buffer(%u)]],
    uint t [[thread_index_in_threadgroup]])
{
    threadgroup atomic_uint cut_seen;
    const uint vpp = max(p.vertices_per_primitive, 1u);
    const uint count = args[0];
    const uint start = args[2];
    if (t == 0u) atomic_store_explicit(&cut_seen, 0u, memory_order_relaxed);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint rewrite = p.rewrite;
    if (rewrite == 3u) {
        // Assemble only when the cut value occurs; otherwise a strip expands
        // and a list is copied.
        for (uint i = t; i < count; i += %uu) {
            if (cut_index(p, ib, start + i) == p.cut_value) {
                atomic_store_explicit(&cut_seen, 1u, memory_order_relaxed);
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (atomic_load_explicit(&cut_seen, memory_order_relaxed) == 0u) {
            rewrite = p.strip != 0u ? 2u : 1u;
        }
    }
    uint produced = 0u;
    if (rewrite == 1u) {
        produced = count;
        if (produced <= p.capacity) {
            for (uint i = t; i < count; i += %uu) out[i] = cut_index(p, ib, start + i);
        }
    } else if (rewrite == 2u) {
        const uint primitives = count >= vpp ? count - (vpp - 1u) : 0u;
        produced = primitives * vpp;
        if (produced <= p.capacity) {
            for (uint n = t; n < primitives; n += %uu) {
                for (uint k = 0u; k < vpp; ++k) {
                    const uint kk = (vpp == 3u && (n & 1u) != 0u && k != 0u) ? 3u - k : k;
                    out[n * vpp + k] = cut_index(p, ib, start + n + kk);
                }
            }
        }
    } else if (t == 0u) {
        // Serial: a cut restarts assembly, so every index depends on the
        // ones before it.
        uint held = 0u;
        uint begin = 0u;
        for (uint i = 0u; i <= count; ++i) {
            const bool end = i == count;
            const uint v = end ? 0u : cut_index(p, ib, start + i);
            const bool cut = !end && v == p.cut_value;
            if (p.strip == 0u) {
                if (end) break;
                if (cut) { held = 0u; continue; }
                if (held == 0u) begin = i;
                if (++held < vpp) continue;
                held = 0u;
                for (uint k = 0u; k < vpp; ++k) {
                    if (produced < p.capacity) out[produced] = cut_index(p, ib, start + begin + k);
                    ++produced;
                }
                continue;
            }
            if (!end && !cut) continue;
            const uint length = i - begin;
            for (uint n = 0u; n + vpp <= length; ++n) {
                for (uint k = 0u; k < vpp; ++k) {
                    const uint kk = (vpp == 3u && (n & 1u) != 0u && k != 0u) ? 3u - k : k;
                    if (produced < p.capacity) out[produced] = cut_index(p, ib, start + begin + n + kk);
                    ++produced;
                }
            }
            begin = i + 1u;
        }
    }
    if (t == 0u) {
        // D3D12_DRAW_INDEXED_ARGUMENTS = MTLDrawIndexedPrimitivesIndirectArguments.
        draw[0] = produced <= p.capacity ? produced : 0u;
        draw[1] = args[1];
        draw[2] = 0u;
        draw[3] = args[3];
        draw[4] = args[4];
    }
}
)",
                  kIaCutEntryPoint, IA_CUT_ARGS_INDEX, IA_CUT_INDICES_INDEX,
                  IA_CUT_OUT_INDEX, IA_CUT_DRAW_INDEX, IA_CUT_THREADS,
                  IA_CUT_THREADS, IA_CUT_THREADS);
    return source;
}

}  // namespace vitreo::graphics
