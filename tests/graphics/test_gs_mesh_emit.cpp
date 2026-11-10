// SPDX-License-Identifier: LGPL-2.1-only
//
// WS5: the mesh limits and fit (Task 2), the translator's mesh lowering
// (Task 5) and the MSL mesh emission (Task 6), without a GPU.

#include <gtest/gtest.h>

#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/msl_mesh_emit.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "gs_sm5_shaders.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

std::vector<GsPassthroughOutput> float_outputs(u32 count) {
    std::vector<GsPassthroughOutput> out;
    for (u32 i = 0; i < count; ++i) {
        GsPassthroughOutput o{};
        o.register_num = i;
        o.is_position = i == 0;
        out.push_back(o);
    }
    return out;
}

}  // namespace

TEST(GsMeshFit, PointToQuadFitsManyPerGroup) {
    const GsMeshFit fit = gs_mesh_fit(float_outputs(2), 5, 4);
    ASSERT_TRUE(fit.fits) << fit.why;
    EXPECT_EQ(fit.invocations_per_group, 42u);  // 256 / 6
    EXPECT_LE(fit.mesh_bytes, kMeshMaxBytes);
    EXPECT_LE(fit.scratch_bytes, kThreadgroupMemoryBytes);
}

TEST(GsMeshFit, LongStripExceedsVertexLimit) {
    const GsMeshFit fit = gs_mesh_fit(float_outputs(2), 5, 100);  // vpi 294
    EXPECT_FALSE(fit.fits);
    ASSERT_NE(fit.why, nullptr);
    EXPECT_EQ(std::string(fit.why), "maxvertexcount exceeds 256 mesh vertices");
}

TEST(GsMeshFit, WideRecordHitsByteLimit) {
    // 30 float4: V = 480 B; per invocation 6 V + 2 P (4 B) + 2 x 3 indices.
    const GsMeshFit fit = gs_mesh_fit(float_outputs(30), 5, 4);
    ASSERT_TRUE(fit.fits) << fit.why;
    const u32 per_k = 6u * 480u + 2u * 4u + 2u * 3u;
    EXPECT_EQ(fit.vertex_bytes, 480u);
    EXPECT_EQ(fit.mesh_bytes, fit.invocations_per_group * per_k);
    EXPECT_LE(fit.mesh_bytes, kMeshMaxBytes);
    EXPECT_GT((fit.invocations_per_group + 1u) * per_k, kMeshMaxBytes);
}

TEST(GsMeshFit, PointListUsesOneVertexPerPrimitive) {
    const GsMeshFit fit = gs_mesh_fit(float_outputs(2), 1, 1);
    ASSERT_TRUE(fit.fits) << fit.why;
    EXPECT_EQ(fit.invocations_per_group, 256u);
}

TEST(GsMeshFit, ScratchLimitBindsBeforeTheMeshForLongWideRecords) {
    // 16 float4 x tristrip(8): vpi 18, V 256 B. The mesh allows K = 3
    // (3 x 18 x 256 = 13824 + 18 x 4 + 18 x 3); scratch 3 x 18 x 16 x 16 =
    // 13824 + 12 fits too. Both limits hold at the chosen K.
    const GsMeshFit fit = gs_mesh_fit(float_outputs(16), 5, 8);
    ASSERT_TRUE(fit.fits) << fit.why;
    EXPECT_LE(fit.mesh_bytes, kMeshMaxBytes);
    EXPECT_LE(fit.scratch_bytes, kThreadgroupMemoryBytes);
}

// ---- Task 5: the translator's mesh lowering --------------------------------

namespace {

/// SPIR-V words of opcode `op`.
std::vector<std::vector<u32>> instructions(const std::vector<u32>& spirv, u32 op) {
    std::vector<std::vector<u32>> out;
    for (usize i = 5; i < spirv.size();) {
        const u32 words = spirv[i] >> 16;
        if (words == 0) break;
        if ((spirv[i] & 0xFFFFu) == op) {
            out.emplace_back(spirv.begin() + i, spirv.begin() + i + words);
        }
        i += words;
    }
    return out;
}

DxbcToSpirvResult translate_mesh(const unsigned char* blob, usize size, u32 k) {
    DxbcTranslationOptions options{};
    options.emulate_geometry_with_compute = true;
    options.emulate_geometry_with_mesh = true;
    options.gs_mesh_invocations_per_group = k;
    return translate_dxbc_to_spirv(parse_dxbc(blob, size), options);
}

/// gs_5_0, two streams: point in, pointlist out, maxvertexcount 2,
/// o0 = v[0][0] emitted to m0 and m1 (hand-assembled, opcode 143
/// dcl_stream, 117 emit_stream; operand type 16 = stream).
DxbcContainer two_stream_shader() {
    DxbcContainer c{};
    c.shader_model_major = 5;
    c.stage = ShaderStage::Geometry;
    c.input_signature.push_back({"SV_Position", 0, 0, 0xFu, 3, 1u});
    c.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3, 1u, 0});
    c.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3, 1u, 1});
    std::vector<u32> w = {
        0x00020050u, 0u,                     // gs_5_0, length patched below
        0x05000061u, 0x002010F2u, 1u, 0u, 1u,  // dcl_input_siv v[1][0], position
        0x0100085Du,                         // dcl_inputprimitive point
        0x0300008Fu, 0x00110000u, 0u,        // dcl_stream m0
        0x0100085Cu,                         // dcl_outputtopology pointlist
        0x04000067u, 0x001020F2u, 0u, 1u,    // dcl_output_siv o0, position
        0x0300008Fu, 0x00110000u, 1u,        // dcl_stream m1
        0x0100085Cu,
        0x04000067u, 0x001020F2u, 0u, 1u,
        0x0200005Eu, 2u,                     // dcl_maxout 2
        0x06000036u, 0x001020F2u, 0u, 0x00201E46u, 0u, 0u,  // mov o0, v[0][0]
        0x03000075u, 0x00110000u, 0u,        // emit_stream m0
        0x06000036u, 0x001020F2u, 0u, 0x00201E46u, 0u, 0u,
        0x03000075u, 0x00110000u, 1u,        // emit_stream m1
        0x0100003Eu,                         // ret
    };
    w[1] = static_cast<u32>(w.size());
    const auto* bytes = reinterpret_cast<const u8*>(w.data());
    c.bytecode.assign(bytes, bytes + w.size() * 4);
    return c;
}

}  // namespace

TEST(DxbcToSpirvGeometryMesh, QuadShaderLowersToWorkgroupScratch) {
    const auto r = translate_mesh(gs5_gs_quad, sizeof(gs5_gs_quad), 42);
    ASSERT_TRUE(r.valid()) << r.failure;
    EXPECT_TRUE(r.gs_lowered_to_compute);
    EXPECT_TRUE(r.gs_lowered_to_mesh);
    EXPECT_EQ(r.workgroup_size_x, 42u);

    const std::vector<u32>& spirv = r.spirv;
    const auto imports = instructions(spirv, 11);  // OpExtInstImport
    bool mesh_import = false;
    u32 mesh_set = 0;
    for (const auto& i : imports) {
        const std::string name(reinterpret_cast<const char*>(i.data() + 2));
        if (name == kMeshGsExtInstSet) {
            mesh_import = true;
            mesh_set = i[1];
        }
    }
    EXPECT_TRUE(mesh_import);
    // The scratch is a Workgroup variable; vitreo_gs_out (set 4 binding 1)
    // does not exist.
    u32 scratch = 0;
    for (const auto& n : instructions(spirv, 5)) {  // OpName
        if (std::string(reinterpret_cast<const char*>(n.data() + 2)) ==
            "vitreo_gs_scratch") {
            scratch = n[1];
        }
    }
    ASSERT_NE(scratch, 0u);
    bool scratch_is_workgroup = false;
    for (const auto& v : instructions(spirv, 59)) {  // OpVariable
        if (v[2] == scratch) scratch_is_workgroup = v[3] == 4u;  // Workgroup
    }
    EXPECT_TRUE(scratch_is_workgroup);
    std::vector<u32> set4;
    std::vector<std::pair<u32, u32>> bindings;
    for (const auto& d : instructions(spirv, 71)) {  // OpDecorate
        if (d[2] == 34u && d[3] == 4u) set4.push_back(d[1]);   // DescriptorSet 4
        if (d[2] == 33u) bindings.emplace_back(d[1], d[3]);    // Binding
    }
    for (const auto& [id, binding] : bindings) {
        if (std::find(set4.begin(), set4.end(), id) != set4.end()) {
            EXPECT_NE(binding, 1u) << "a set 4 binding 1 buffer remains";
        }
    }
    u32 epilogues = 0;
    for (const auto& e : instructions(spirv, 12)) {  // OpExtInst
        if (e[3] == mesh_set && e[4] == kMeshGsEpilogue) ++epilogues;
    }
    EXPECT_EQ(epilogues, 1u);
    EXPECT_EQ(instructions(spirv, 224).size(), 1u);  // OpControlBarrier
}

TEST(DxbcToSpirvGeometryMesh, MultiStreamShaderIsRefused) {
    DxbcTranslationOptions options{};
    options.emulate_geometry_with_compute = true;
    const auto compute = translate_dxbc_to_spirv(two_stream_shader(), options);
    ASSERT_TRUE(compute.valid()) << "the hand-assembled shader is malformed: "
                                 << compute.failure;
    EXPECT_EQ(compute.gs_stream_mask, 3u);
    options.emulate_geometry_with_mesh = true;
    options.gs_mesh_invocations_per_group = 4;
    const auto mesh = translate_dxbc_to_spirv(two_stream_shader(), options);
    EXPECT_FALSE(mesh.valid());
    EXPECT_NE(mesh.failure.find("single stream"), std::string::npos)
        << mesh.failure;
}

TEST(DxbcToSpirvGeometryMesh, ZeroInvocationsPerGroupIsRefused) {
    const auto r = translate_mesh(gs5_gs_quad, sizeof(gs5_gs_quad), 0);
    EXPECT_FALSE(r.valid());
}

// ---- Task 6: MSL mesh emission ---------------------------------------------

namespace {

/// The rasterized stream's outputs, as build_geometry_program builds them
/// for a single-stream shader.
std::vector<GsPassthroughOutput> outputs_of(const DxbcContainer& c) {
    std::vector<GsPassthroughOutput> outputs;
    for (const auto& e : c.output_signature) {
        auto same = std::find_if(outputs.begin(), outputs.end(), [&](const auto& o) {
            return o.register_num == e.register_num;
        });
        if (same == outputs.end()) {
            GsPassthroughOutput o{};
            o.register_num = e.register_num;
            o.component_type = e.component_type;
            outputs.push_back(o);
            same = std::prev(outputs.end());
        }
        if (e.system_value_type == 1u) same->is_position = true;
        if (e.system_value_type == 2u || e.system_value_type == 4u ||
            e.system_value_type == 5u) {
            for (u32 k = 0; k < 4; ++k) {
                if ((e.component_mask & (1u << k)) != 0u) {
                    same->component_system_values[k] =
                        static_cast<u8>(e.system_value_type);
                }
            }
        }
    }
    return outputs;
}

}  // namespace

/// Translate + compile one GS for a mesh pipeline; returns the MSL ("" on
/// failure).
std::string gs_mesh_msl_for_test(const unsigned char* blob, usize size,
                                 GsMeshLayout* layout_out) {
    const auto container = parse_dxbc(blob, size);
    DxbcTranslationOptions probe{};
    probe.emulate_geometry_with_compute = true;
    const auto shape = translate_dxbc_to_spirv(container, probe);
    if (!shape.valid()) return {};
    GsMeshLayout layout{};
    layout.outputs = outputs_of(container);
    layout.output_topology = shape.gs_output_topology;
    layout.vertices_per_invocation = gs_vertices_per_invocation(
        shape.gs_output_topology, shape.gs_max_output_vertices);
    layout.record_vec4s = shape.gs_output_registers;
    const GsMeshFit fit = gs_mesh_fit(layout.outputs, layout.output_topology,
                                      shape.gs_max_output_vertices);
    if (!fit.fits) return {};
    layout.invocations_per_group = fit.invocations_per_group;
    const auto lowered = translate_mesh(blob, size, fit.invocations_per_group);
    if (!lowered.valid()) return {};
    MslCompileOptions msl{};
    msl.mesh = &layout;
    const auto compiled = compile_spirv_to_msl(lowered.spirv.data(),
                                               lowered.spirv.size(),
                                               ShaderStage::Compute, msl);
    if (!compiled.valid()) return {};
    if (layout_out) *layout_out = layout;
    return compiled.msl_text();
}

TEST(GsMeshEmit, EntryIsMeshNotKernel) {
    const std::string msl =
        gs_mesh_msl_for_test(gs5_gs_quad, sizeof(gs5_gs_quad), nullptr);
    ASSERT_FALSE(msl.empty());
    EXPECT_NE(msl.find("[[mesh]] void main0("), std::string::npos) << msl;
    EXPECT_EQ(msl.find("kernel void main0("), std::string::npos) << msl;
    EXPECT_NE(msl.find("[[object]] void vitreo_gs_object("), std::string::npos);
    EXPECT_NE(msl.find("set_primitive_count"), std::string::npos);
}

TEST(GsMeshEmit, AMeshLayoutOnAPlainKernelIsRefused) {
    // A module without the Vitreo.MeshGS import is never labelled a mesh.
    DxbcTranslationOptions options{};
    options.emulate_geometry_with_compute = true;
    const auto plain = translate_dxbc_to_spirv(
        parse_dxbc(gs5_gs_quad, sizeof(gs5_gs_quad)), options);
    ASSERT_TRUE(plain.valid());
    GsMeshLayout layout{};
    layout.outputs = float_outputs(2);
    layout.output_topology = 5;
    layout.vertices_per_invocation = 6;
    layout.invocations_per_group = 42;
    layout.record_vec4s = 2;
    MslCompileOptions msl{};
    msl.mesh = &layout;
    EXPECT_FALSE(compile_spirv_to_msl(plain.spirv.data(), plain.spirv.size(),
                                      ShaderStage::Compute, msl)
                     .valid());
}
