// SPDX-License-Identifier: LGPL-2.1-only
//
// WS5 Task 7: the GS path selector's truth table. Every row names a contract
// rule (host-offload plan WS5 G10-G20, D3)
// and the exact reason it gives.

#include <gtest/gtest.h>

#include "winapi/graphics/d3d11/d3d11_gs_mesh_select.hpp"

#include <functional>
#include <string>

using namespace vitreo;
using namespace vitreo::winapi::gs_mesh;

namespace {

GsMeshStatic eligible() {
    GsMeshStatic s{};
    s.eligible = true;
    s.why = nullptr;
    return s;
}

/// Max/Max into RGBA8, no depth: the mesh path's home case.
GsDrawFacts max_blend() {
    GsDrawFacts f{};
    f.device_supports_mesh = true;
    f.rts[0] = {true, 0xF, true, kBlendOpMax, kBlendOpMax, true, false};
    return f;
}

struct Row {
    const char* name;
    std::function<void(GsMeshStatic&, GsDrawFacts&)> edit;
    GsPath path;
    const char* why;
    Force force{Force::None};
};

}  // namespace

TEST(GsMeshSelect, TruthTable) {
    const Row rows[] = {
        {"blend Max/Max RGBA8", [](auto&, auto&) {}, GsPath::Mesh, ""},
        {"blend SrcAlpha/InvSrcAlpha (op Add)",
         [](auto&, GsDrawFacts& f) { f.rts[0].op_color = f.rts[0].op_alpha = 1; },
         GsPath::ThreePass, "order-dependent output"},
        {"blend off, colour written",
         [](auto&, GsDrawFacts& f) { f.rts[0].blend = false; },
         GsPath::ThreePass, "order-dependent output"},
        {"RT write mask 0, depth Less write",
         [](auto&, GsDrawFacts& f) {
             f.rts[0].write_mask = 0;
             f.depth_enable = f.depth_write = true;
             f.depth_func = kComparisonLess;
         },
         GsPath::Mesh, ""},
        {"depth-only LessEqual",
         [](auto&, GsDrawFacts& f) {
             f.rts[0] = {};
             f.depth_enable = f.depth_write = true;
             f.depth_func = kComparisonLessEqual;
         },
         GsPath::Mesh, ""},
        {"Max on R16G16B16A16_FLOAT",
         [](auto&, GsDrawFacts& f) { f.rts[0].minmax_exact = false; },
         GsPath::ThreePass, "order-dependent output"},
        {"Max colour, Add alpha",
         [](auto&, GsDrawFacts& f) { f.rts[0].op_alpha = 1; },
         GsPath::ThreePass, "order-dependent output"},
        {"second target blended Add",
         [](auto&, GsDrawFacts& f) { f.rts[1] = {true, 0xF, true, 1, 1, true, false}; },
         GsPath::ThreePass, "order-dependent output"},
        {"second target bound, write mask 0",
         [](auto&, GsDrawFacts& f) { f.rts[1] = {true, 0, false, 1, 1, true, false}; },
         GsPath::Mesh, ""},
        {"logic op",
         [](auto&, GsDrawFacts& f) { f.rts[0].logic_op = true; },
         GsPath::ThreePass, "order-dependent output"},
        {"colour Max with a writing depth test",
         [](auto&, GsDrawFacts& f) {
             f.depth_enable = f.depth_write = true;
             f.depth_func = kComparisonLess;
         },
         GsPath::ThreePass, "order-dependent output"},
        {"colour Max with a read-only depth test",
         [](auto&, GsDrawFacts& f) {
             f.depth_enable = true;
             f.depth_func = kComparisonLess;
         },
         GsPath::Mesh, ""},
        {"depth Always write",
         [](auto&, GsDrawFacts& f) {
             f.rts[0] = {};
             f.depth_enable = f.depth_write = true;
             f.depth_func = kComparisonAlways;
         },
         GsPath::ThreePass, "order-dependent output"},
        {"depth NotEqual write",
         [](auto&, GsDrawFacts& f) {
             f.rts[0] = {};
             f.depth_enable = f.depth_write = true;
             f.depth_func = kComparisonNotEqual;
         },
         GsPath::ThreePass, "order-dependent output"},
        {"stencil op Replace",
         [](auto&, GsDrawFacts& f) {
             f.stencil_enable = true;
             f.stencil_write_mask = 0xFF;
             f.stencil_ops[2] = 3;  // D3D11_STENCIL_OP_REPLACE
         },
         GsPath::ThreePass, "order-dependent output"},
        {"stencil Replace with write mask 0",
         [](auto&, GsDrawFacts& f) {
             f.stencil_enable = true;
             f.stencil_write_mask = 0;
             f.stencil_ops[2] = 3;
         },
         GsPath::Mesh, ""},
        {"PS UAV (G20)", [](auto&, GsDrawFacts& f) { f.ps_uav = true; },
         GsPath::ThreePass, "order-dependent output"},
        // One mesh is rasterized in primitive order, which is D3D's order.
        {"order-dependent blend, the draw is one mesh",
         [](GsMeshStatic& s, GsDrawFacts& f) {
             f.rts[0].op_color = f.rts[0].op_alpha = 1;
             s.fit.invocations_per_group = 8;
             f.invocations = 8;
         },
         GsPath::Mesh, ""},
        {"order-dependent blend, one invocation past the threadgroup",
         [](GsMeshStatic& s, GsDrawFacts& f) {
             f.rts[0].op_color = f.rts[0].op_alpha = 1;
             s.fit.invocations_per_group = 8;
             f.invocations = 9;
         },
         GsPath::ThreePass, "order-dependent output"},
        {"order-dependent blend, invocation count not known",
         [](GsMeshStatic& s, GsDrawFacts& f) {
             f.rts[0].blend = false;
             s.fit.invocations_per_group = 8;
             f.invocations = 0;
         },
         GsPath::ThreePass, "order-dependent output"},
        {"blend off and depth write, the draw is one mesh",
         [](GsMeshStatic& s, GsDrawFacts& f) {
             f.rts[0].blend = false;
             f.depth_enable = f.depth_write = true;
             f.depth_func = kComparisonAlways;
             s.fit.invocations_per_group = 4;
             f.invocations = 2;
         },
         GsPath::Mesh, ""},
        {"PS UAV stays three-pass even in one mesh (G20)",
         [](GsMeshStatic& s, GsDrawFacts& f) {
             f.ps_uav = true;
             s.fit.invocations_per_group = 8;
             f.invocations = 1;
         },
         GsPath::ThreePass, "order-dependent output"},
        {"PS reads a layer / viewport builtin the GS does not write",
         [](auto&, GsDrawFacts& f) { f.ps_reads_builtin_layer_or_viewport = true; },
         GsPath::ThreePass,
         "the pixel shader reads SV_RenderTargetArrayIndex / SV_ViewportArrayIndex"},
        {"PS SV_Coverage", [](auto&, GsDrawFacts& f) { f.ps_writes_coverage = true; },
         GsPath::ThreePass, "order-dependent output"},
        {"alpha to coverage", [](auto&, GsDrawFacts& f) { f.alpha_to_coverage = true; },
         GsPath::ThreePass, "order-dependent output"},
        {"indirect (G13)", [](auto&, GsDrawFacts& f) { f.indirect = true; },
         GsPath::ThreePass, "indirect draw"},
        {"statistics (G12)", [](auto&, GsDrawFacts& f) { f.statistics_query = true; },
         GsPath::ThreePass, "a pipeline statistics query is building"},
        {"stream output (G11)", [](auto&, GsDrawFacts& f) { f.stream_output = true; },
         GsPath::ThreePass, "stream output"},
        {"line quads (G14)", [](auto&, GsDrawFacts& f) { f.line_quads = true; },
         GsPath::ThreePass, "lines drawn as quads"},
        {"wireframe (G14)", [](auto&, GsDrawFacts& f) { f.wireframe = true; },
         GsPath::ThreePass, "wireframe fill"},
        {"PS builtin SV_PrimitiveID (G19)",
         [](auto&, GsDrawFacts& f) { f.ps_reads_builtin_primitive_id = true; },
         GsPath::ThreePass,
         "the pixel shader reads SV_PrimitiveID the geometry shader does not write"},
        {"PS input not written (G18)",
         [](auto&, GsDrawFacts& f) { f.ps_inputs_covered = false; },
         GsPath::ThreePass,
         "the pixel shader reads an input the geometry shader does not write"},
        {"no mesh device (M1)",
         [](auto&, GsDrawFacts& f) { f.device_supports_mesh = false; },
         GsPath::ThreePass, "the device has no mesh pipelines"},
        {"!eligible: multiple streams (G10)",
         [](GsMeshStatic& s, auto&) {
             s.eligible = false;
             s.why = "multiple streams";
         },
         GsPath::ThreePass, "multiple streams"},
        {"eligible, all clear, Force::ThreePass", [](auto&, auto&) {},
         GsPath::ThreePass, "forced three-pass", Force::ThreePass},
        {"!eligible stream output, Force::Mesh",
         [](GsMeshStatic& s, auto&) {
             s.eligible = false;
             s.why = "stream output";
         },
         GsPath::ThreePass, "stream output", Force::Mesh},
        {"order-dependent, Force::Mesh",
         [](auto&, GsDrawFacts& f) { f.rts[0].op_color = 1; },
         GsPath::ThreePass, "order-dependent output", Force::Mesh},
        {"eligible, Force::Mesh", [](auto&, auto&) {}, GsPath::Mesh, "", Force::Mesh},
    };
    const bool killed = kill_switch_off();
    for (const Row& row : rows) {
        GsMeshStatic s = eligible();
        GsDrawFacts f = max_blend();
        row.edit(s, f);
        force_for_test(row.force);
        const GsPathChoice c = select_gs_path(s, f);
        force_for_test(Force::None);
        // VITREO_WS5=0 outranks the default choice and the force, never a
        // contract reason.
        if (killed && (row.path == GsPath::Mesh ||
                       std::string(row.why) == "forced three-pass")) {
            EXPECT_EQ(c.path, GsPath::ThreePass) << row.name << " (VITREO_WS5=0)";
            EXPECT_EQ(std::string(c.why), "VITREO_WS5=0") << row.name;
            continue;
        }
        EXPECT_EQ(c.path, row.path) << row.name;
        EXPECT_EQ(std::string(c.why), row.why) << row.name;
    }
}
