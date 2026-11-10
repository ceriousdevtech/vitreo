// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_gs_mesh_select.cpp
/// @brief WS5 GS path selection (d3d11_gs_mesh_select.hpp).

#include "winapi/graphics/d3d11/d3d11_gs_mesh_select.hpp"

#include <vitreo/ab_cycle.hpp>

#include <atomic>
#include <cstdlib>

namespace vitreo::winapi::gs_mesh {
namespace {
std::atomic<int> g_force{0};
}  // namespace

void force_for_test(Force force) noexcept {
    g_force.store(static_cast<int>(force), std::memory_order_relaxed);
}

Force forced() noexcept {
    return static_cast<Force>(g_force.load(std::memory_order_relaxed));
}

bool kill_switch_off() noexcept {
    static const bool off = [] {
        const char* v = std::getenv("VITREO_WS5");
        return v != nullptr && v[0] == '0';
    }();
    return off;
}

bool order_independent(const GsDrawFacts& f) noexcept {
    if (f.ps_uav || f.ps_writes_coverage || f.alpha_to_coverage) return false;
    const auto minmax = [](u8 op) { return op == kBlendOpMin || op == kBlendOpMax; };
    bool color_written = false;
    for (const RenderTargetFacts& rt : f.rts) {
        if (!rt.bound || rt.write_mask == 0) continue;
        color_written = true;
        if (rt.logic_op || !rt.blend || !rt.minmax_exact ||
            !minmax(rt.op_color) || !minmax(rt.op_alpha)) {
            return false;
        }
    }
    if (f.depth_enable && f.depth_write) {
        // With colour written, which fragments pass a writing depth test
        // depends on their order. Without, the stored depth is the min / max
        // (or unchanged) whatever the order.
        if (color_written) return false;
        switch (f.depth_func) {
        case kComparisonNever:
        case kComparisonLess:
        case kComparisonEqual:
        case kComparisonLessEqual:
        case kComparisonGreater:
        case kComparisonGreaterEqual:
            break;
        default:
            return false;  // ALWAYS / NOT_EQUAL: the last write wins
        }
    }
    if (f.stencil_enable && f.stencil_write_mask != 0) {
        for (const u8 op : f.stencil_ops) {
            if (op != kStencilOpKeep) return false;
        }
    }
    return true;
}

bool single_mesh(const GsMeshStatic& s, const GsDrawFacts& f) noexcept {
    return f.invocations != 0 && s.fit.invocations_per_group != 0 &&
           f.invocations <= s.fit.invocations_per_group;
}

GsPathChoice select_gs_path(const GsMeshStatic& s, const GsDrawFacts& f) noexcept {
    const auto three = [](const char* why) { return GsPathChoice{GsPath::ThreePass, why}; };
    if (!s.eligible) return three(s.why != nullptr ? s.why : "not eligible");
    if (!f.device_supports_mesh) return three("the device has no mesh pipelines");
    if (f.indirect) return three("indirect draw");
    if (f.statistics_query) return three("a pipeline statistics query is building");
    if (f.stream_output) return three("stream output");
    if (f.line_quads) return three("lines drawn as quads");
    if (f.wireframe) return three("wireframe fill");
    if (f.ps_reads_builtin_primitive_id) {
        return three("the pixel shader reads SV_PrimitiveID the geometry shader does not write");
    }
    if (f.ps_reads_builtin_layer_or_viewport) {
        return three("the pixel shader reads SV_RenderTargetArrayIndex / SV_ViewportArrayIndex");
    }
    if (!f.ps_inputs_covered) {
        return three("the pixel shader reads an input the geometry shader does not write");
    }
    // A pixel-shader UAV write stays three-pass even in one mesh (G20).
    if (!order_independent(f) && (f.ps_uav || !single_mesh(s, f))) {
        return three("order-dependent output");
    }
    if (kill_switch_off()) return three("VITREO_WS5=0");
    if (diagnostics::ab_cycle_off("ws5-gs-mesh")) return three("ab-cycle off arm");
    if (forced() == Force::ThreePass) return three("forced three-pass");
    return {GsPath::Mesh, ""};
}

}  // namespace vitreo::winapi::gs_mesh
