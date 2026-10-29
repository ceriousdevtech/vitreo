// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_dxbc_to_spirv_geometry.cpp
/// @brief Geometry-shader translation and lowering, SM5 GS, adjacency and primitive assembly tables.

#include <gtest/gtest.h>

#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "graphics/contracts/backend_device.hpp"

#include "dxbc_to_spirv_test_shaders.h"
#include "dxbc_to_spirv_test_spirv.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;
using namespace vitreo::graphics::dxbc_spirv_test;

namespace {

DxbcContainer create_fxc_strip_geometry_shader(u32 primitive_width,
                                                u32 output_topology) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Geometry;
    container.input_signature.push_back(
        {"SV_Position", 0, 0, 0xFu, 3, 1u});
    container.input_signature.push_back({"COLOR", 0, 1, 0xFu, 3});
    container.output_signature.push_back(
        {"SV_Position", 0, 0, 0xFu, 3, 1u});
    container.output_signature.push_back({"COLOR", 0, 1, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };

    // Exact instruction shape emitted by Microsoft D3DCompiler 47 for the
    // line and triangle HLSL probes. Full DXBC SHA-256 values:
    // line: 108c6a44c414f0eb0b87dfe11f57f4022b08ed30147bb959f9242975a98c4481
    // tri:  2b70e53eefbae9f4fafa072c8fc97d33a81fce0e21aafd5cfe1716fc94b7cc09
    append_word(0x00020040u); // gs_4_0
    append_word(24u + 13u * primitive_width);
    append_word(0x05000061u); append_word(0x002010F2u);
    append_word(primitive_width); append_word(0u); append_word(1u);
    append_word(0x0400005Fu); append_word(0x002010F2u);
    append_word(primitive_width); append_word(1u);
    append_word(0x0100005Du | (primitive_width << 11));
    append_word(0x0100005Cu | (output_topology << 11));
    append_word(0x04000067u); append_word(0x001020F2u);
    append_word(0u); append_word(1u);
    append_word(0x03000065u); append_word(0x001020F2u); append_word(1u);
    append_word(0x0200005Eu); append_word(primitive_width);
    for (u32 vertex = 0; vertex < primitive_width; ++vertex) {
        append_word(0x06000036u); append_word(0x001020F2u); append_word(0u);
        append_word(0x00201E46u); append_word(vertex); append_word(0u);
        append_word(0x06000036u); append_word(0x001020F2u); append_word(1u);
        append_word(0x00201E46u); append_word(vertex); append_word(1u);
        append_word(0x01000013u); // emit
    }
    append_word(0x01000009u); // cut
    append_word(0x0100003Eu); // ret
    return container;
}

} // namespace

TEST(DxbcToSpirvTest, GeometryPointStreamEmitsValidExecutionContract) {
    DxbcContainer container = create_fxc_point_geometry_shader();
    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_EQ(result.stage, ShaderStage::Geometry);
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpEmitVertex=*/218u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpEndPrimitive=*/219u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpAccessChain=*/65u));
    EXPECT_TRUE(spirv_contains_builtin(
        result.spirv, /*BuiltIn Position=*/0u));

    const auto msl = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Geometry);
    EXPECT_FALSE(msl.valid())
        << "Metal has no native geometry stage; do not expose placeholder MSL";

    // Adjacency input is served (11.30): D3D10_SB_PRIMITIVE_LINE_ADJ == 6 in
    // bits 11..16 is a 4-vertex input primitive (measured with fxc).
    constexpr usize input_primitive_word = 11u;
    const u32 adjacency_declaration = 0x0100305Du;
    std::memcpy(container.bytecode.data() +
                    input_primitive_word * sizeof(u32),
                &adjacency_declaration, sizeof(adjacency_declaration));
    const auto adjacency = translate_dxbc_to_spirv(container);
    EXPECT_TRUE(adjacency.valid()) << adjacency.failure;
    EXPECT_EQ(adjacency.gs_input_primitive, 4u);
    // A value that names no primitive stays refused.
    const u32 undefined_declaration = 0x0100285Du;  // 5
    std::memcpy(container.bytecode.data() +
                    input_primitive_word * sizeof(u32),
                &undefined_declaration, sizeof(undefined_declaration));
    const auto unsupported = translate_dxbc_to_spirv(container);
    EXPECT_FALSE(unsupported.valid());
    EXPECT_NE(unsupported.failure.find("input primitive"), std::string::npos);
}

TEST(DxbcToSpirvTest, CapturesVertexOutputsForTheGeometryExpansion) {
    // Pass A of the geometry emulation: an ordinary vertex shader that ALSO
    // writes its outputs into the buffer the expansion pass reads. Keeping
    // pass A a real draw is what lets hardware vertex fetch, the input layout
    // and the vertex buffers work untouched.
    DxbcTranslationOptions options{};
    options.capture_vertex_outputs_to_buffer = true;
    const auto result = translate_dxbc_to_spirv(
        create_binary_vertex_shader(/*OpAdd=*/0u), options);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(result.captures_vertex_outputs);
    EXPECT_EQ(result.stage, ShaderStage::Vertex);
    // One output register (SV_Position), so one vec4 per record.
    EXPECT_EQ(result.capture_record_vec4s, 1u);
    // The record is keyed by VertexIndex, which Metal spells [[vertex_id]].
    EXPECT_TRUE(spirv_contains_builtin(result.spirv, /*VertexIndex=*/42u));

    // It must declare NO stage outputs, not even position. Metal refuses a
    // rasterization-disabled vertex function whose return type is not void,
    // and SPIRV-Cross only emits void when the entry point has no Output
    // variables. Pass A discards its primitives by design: rasterising them
    // would draw the unexpanded geometry over the frame.
    EXPECT_FALSE(spirv_contains_builtin(result.spirv, /*Position=*/0u));

    // Without the option nothing changes.
    const auto plain =
        translate_dxbc_to_spirv(create_binary_vertex_shader(0u));
    ASSERT_TRUE(plain.valid()) << plain.failure;
    EXPECT_FALSE(plain.captures_vertex_outputs);
    EXPECT_EQ(plain.capture_record_vec4s, 0u);

    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex,
        msl_options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl(compiled.msl_text());
    // The capture buffer must reach the vertex stage at the expansion window.
    EXPECT_NE(msl.find("vitreo_gs_in"), std::string::npos) << msl;
    EXPECT_NE(msl.find("vertex void"), std::string::npos) << msl;

    // ...and at an index NO vertex buffer can occupy. Pass A is a real draw on
    // purpose -- hardware vertex fetch reads the guest's vertex buffers, bound
    // at BACKEND_VERTEX_BUFFER_BASE onward in this same stage. The compute
    // expansion window starts at that very index, which is safe for the compute
    // pass (no input-assembler streams there) and NOT for this one. Checking
    // only that the name appears could not see the two share an index.
    const auto capture_at = msl.find("vitreo_gs_in [[buffer(");
    ASSERT_NE(capture_at, std::string::npos) << msl;
    const unsigned capture_index = static_cast<unsigned>(std::stoul(
        msl.substr(capture_at + std::string("vitreo_gs_in [[buffer(").size())));
    EXPECT_FALSE(capture_index >= BACKEND_VERTEX_BUFFER_BASE &&
                 capture_index <
                     BACKEND_VERTEX_BUFFER_BASE + BACKEND_MAX_VERTEX_BUFFERS)
        << "the vertex-stage capture buffer is at [[buffer(" << capture_index
        << ")]], inside the vertex-buffer window "
        << BACKEND_VERTEX_BUFFER_BASE << ".."
        << (BACKEND_VERTEX_BUFFER_BASE + BACKEND_MAX_VERTEX_BUFFERS - 1)
        << ": pass A would alias the guest's vertex buffer "
        << (capture_index - BACKEND_VERTEX_BUFFER_BASE);
}

TEST(DxbcToSpirvTest, LowersAPointGeometryShaderToComputeExpansion) {
    // Metal has no geometry stage, so the emulation runs the GS as compute:
    // one thread per input primitive, appending output vertices to a buffer.
    // See .docs/design-geometry-shader-emulation.md.
    DxbcTranslationOptions options{};
    options.emulate_geometry_with_compute = true;
    const auto result =
        translate_dxbc_to_spirv(create_fxc_point_geometry_shader(), options);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(result.gs_lowered_to_compute);
    EXPECT_EQ(result.stage, ShaderStage::Compute);

    // The host sizes the expansion buffer and picks the draw topology from
    // these, so they must survive the lowering.
    EXPECT_EQ(result.gs_input_primitive, 1u);        // point
    EXPECT_EQ(result.gs_output_topology, 1u);        // pointlist
    EXPECT_EQ(result.gs_max_output_vertices, 1u);
    EXPECT_EQ(result.gs_output_registers, 2u);       // SV_Position + COLOR

    // No geometry-stage construct may survive: SPIRV-Cross cannot lower one
    // to a Metal stage that does not exist, and leaving one behind is how a
    // half-lowered module would look valid.
    EXPECT_FALSE(spirv_contains_opcode(result.spirv, /*OpEmitVertex=*/218u));
    EXPECT_FALSE(spirv_contains_opcode(result.spirv, /*OpEndPrimitive=*/219u));
    EXPECT_FALSE(spirv_contains_execution_mode(result.spirv, /*InputPoints=*/19u));
    EXPECT_FALSE(spirv_contains_capability(result.spirv, /*Geometry=*/2u));

    // ...and the compute contract must be there instead.
    EXPECT_TRUE(spirv_contains_execution_mode(result.spirv, /*LocalSize=*/17u));
    EXPECT_TRUE(spirv_contains_builtin(
        result.spirv, /*GlobalInvocationId=*/28u));
    // Each invocation appends into its OWN region, so there is no shared
    // atomic cursor: one would put output primitives in GPU-scheduling order
    // rather than D3D's input-primitive order. The region is bounded by
    // maxvertexcount (an unsigned <= guard).
    EXPECT_FALSE(spirv_contains_opcode(result.spirv, /*OpAtomicIAdd=*/234u));
    EXPECT_TRUE(
        spirv_contains_opcode(result.spirv, /*OpULessThanEqual=*/178u));

    // Ordinary (unlowered) translation must be unaffected by the option
    // existing -- it still produces a real geometry shader.
    const auto unlowered =
        translate_dxbc_to_spirv(create_fxc_point_geometry_shader());
    ASSERT_TRUE(unlowered.valid()) << unlowered.failure;
    EXPECT_FALSE(unlowered.gs_lowered_to_compute);
    EXPECT_EQ(unlowered.stage, ShaderStage::Geometry);
    EXPECT_TRUE(spirv_contains_opcode(unlowered.spirv, /*OpEmitVertex=*/218u));

    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Compute,
        msl_options);
    ASSERT_TRUE(compiled.valid())
        << "the lowered module must compile as a compute kernel";
    const std::string msl(compiled.msl_text());
    EXPECT_NE(msl.find("kernel void"), std::string::npos) << msl;
}

TEST(DxbcToSpirvTest, LoweredTriangleStripExpandsToIndependentTriangles) {
    // D3D's GS output is a STRIP and `cut` restarts it, but Metal has no
    // primitive restart for either indexed or non-indexed draws. So the
    // expansion writes independent triangles, alternating winding the way a
    // strip does -- which makes the draw pass a plain triangle list.
    DxbcTranslationOptions options{};
    options.emulate_geometry_with_compute = true;
    const auto result = translate_dxbc_to_spirv(
        create_fxc_strip_geometry_shader(
            /*D3D_PRIMITIVE_TRIANGLE=*/3u,
            /*D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP=*/5u),
        options);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(result.gs_lowered_to_compute);
    EXPECT_EQ(result.stage, ShaderStage::Compute);
    EXPECT_EQ(result.gs_input_primitive, 3u);
    EXPECT_EQ(result.gs_output_topology, 5u);

    EXPECT_FALSE(spirv_contains_opcode(result.spirv, /*OpEmitVertex=*/218u));
    EXPECT_FALSE(spirv_contains_execution_mode(
        result.spirv, /*OutputTriangleStrip=*/29u));
    // The alternating winding is an OpSelect between the two held vertices,
    // deliberately a data choice rather than a second branch.
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpSelect=*/169u));
    EXPECT_FALSE(spirv_contains_opcode(result.spirv, /*OpAtomicIAdd=*/234u));

    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Compute,
        msl_options);
    ASSERT_TRUE(compiled.valid()) << "lowered strip GS must compile as compute";
}

TEST(DxbcToSpirvTest, GeometryLineAndTriangleStreamsUseExactArrayModes) {
    const auto line = translate_dxbc_to_spirv(
        create_fxc_strip_geometry_shader(
            /*D3D_PRIMITIVE_LINE=*/2u,
            /*D3D_PRIMITIVE_TOPOLOGY_LINESTRIP=*/3u));
    ASSERT_TRUE(line.valid()) << line.failure;
    EXPECT_TRUE(spirv_contains_execution_mode(
        line.spirv, /*InputLines=*/20u));
    EXPECT_TRUE(spirv_contains_execution_mode(
        line.spirv, /*OutputLineStrip=*/28u));
    EXPECT_TRUE(spirv_contains_execution_mode(
        line.spirv, /*OutputVertices=*/26u, 2u));
    EXPECT_TRUE(spirv_contains_opcode(line.spirv, /*OpAccessChain=*/65u));
    EXPECT_TRUE(spirv_contains_opcode(line.spirv, /*OpEmitVertex=*/218u));
    EXPECT_TRUE(spirv_contains_opcode(line.spirv, /*OpEndPrimitive=*/219u));

    DxbcContainer triangle = create_fxc_strip_geometry_shader(
        /*D3D_PRIMITIVE_TRIANGLE=*/3u,
        /*D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP=*/5u);
    const auto translated = translate_dxbc_to_spirv(triangle);
    ASSERT_TRUE(translated.valid()) << translated.failure;
    EXPECT_TRUE(spirv_contains_execution_mode(
        translated.spirv, /*Triangles=*/22u));
    EXPECT_TRUE(spirv_contains_execution_mode(
        translated.spirv, /*OutputTriangleStrip=*/29u));
    EXPECT_TRUE(spirv_contains_execution_mode(
        translated.spirv, /*OutputVertices=*/26u, 3u));

    // Narrow only the declared input primitive to line while retaining FXC's
    // third-vertex reads. The v[2][register] operand must be rejected.
    constexpr usize input_primitive_word = 11u;
    const u32 line_declaration = 0x0100105Du;
    std::memcpy(triangle.bytecode.data() +
                    input_primitive_word * sizeof(u32),
                &line_declaration, sizeof(line_declaration));
    const auto out_of_range = translate_dxbc_to_spirv(triangle);
    EXPECT_FALSE(out_of_range.valid());
    EXPECT_NE(out_of_range.failure.find("vertex index"), std::string::npos);
}

// Real fxc gs_5_0 output (tools/oracle_probe/gs_sm5_shaders.hlsl). SM5 puts
// the geometry output signature in OSG5, whose elements carry a stream index,
// and always uses dcl_stream m0 / emit_stream m0 -- before either was
// understood, every gs_5_0 shader failed CreateGeometryShader.
#include "gs_sm5_shaders.h"

TEST(DxbcToSpirvTest, ParsesTheSm5GeometryOutputSignature) {
    const auto container = parse_dxbc(gs5_gs_layered, sizeof(gs5_gs_layered));
    ASSERT_TRUE(container.valid());
    ASSERT_EQ(container.output_signature.size(), 5u);
    const auto& out = container.output_signature;
    EXPECT_EQ(out[0].semantic_name, "SV_Position");
    EXPECT_EQ(out[0].system_value_type, 1u);
    EXPECT_EQ(out[1].semantic_name, "COLOR");
    EXPECT_EQ(out[1].register_num, 1u);
    EXPECT_EQ(out[2].system_value_type, 4u);  // SV_RenderTargetArrayIndex
    EXPECT_EQ(out[2].register_num, 2u);
    EXPECT_EQ(out[2].component_mask, 0x1u);
    EXPECT_EQ(out[3].system_value_type, 5u);  // SV_ViewportArrayIndex
    EXPECT_EQ(out[3].component_mask, 0x2u);
    EXPECT_EQ(out[4].system_value_type, 2u);  // SV_ClipDistance
    EXPECT_EQ(out[4].register_num, 3u);
    for (const auto& element : out) EXPECT_EQ(element.stream, 0u);
}

TEST(DxbcToSpirvTest, LowersSm5InstancedGeometryShaders) {
    DxbcTranslationOptions options{};
    options.emulate_geometry_with_compute = true;
    const auto quad =
        translate_dxbc_to_spirv(parse_dxbc(gs5_gs_quad, sizeof(gs5_gs_quad)),
                                options);
    ASSERT_TRUE(quad.valid()) << quad.failure;
    EXPECT_TRUE(quad.gs_lowered_to_compute);
    EXPECT_EQ(quad.gs_instance_count, 1u);
    EXPECT_EQ(quad.gs_max_output_vertices, 4u);

    const auto layered = translate_dxbc_to_spirv(
        parse_dxbc(gs5_gs_layered, sizeof(gs5_gs_layered)), options);
    ASSERT_TRUE(layered.valid()) << layered.failure;
    EXPECT_EQ(layered.gs_instance_count, 4u);   // dcl_gsinstances 4
    EXPECT_EQ(layered.gs_output_registers, 4u);  // o0..o3 in the record
}

// The TRIANGLESTRIP_ADJ vertex table against the Windows measurement
// (tools/oracle_probe/d3d11_tristrip_adj_probe.expected), verbatim.
#include "graphics/shaders/gs_passthrough.hpp"

TEST(GeometryAdjacencyTest, TriangleStripAdjacencyMatchesTheMeasuredTable) {
    struct Case {
        u32 vertices;
        std::vector<std::array<u32, 6>> triangles;
    };
    const std::vector<Case> measured{
        {6, {{0, 1, 2, 5, 4, 3}}},
        {7, {{0, 1, 2, 5, 4, 3}}},
        {8, {{0, 1, 2, 6, 4, 3}, {2, 5, 6, 7, 4, 0}}},
        {9, {{0, 1, 2, 6, 4, 3}, {2, 5, 6, 7, 4, 0}}},
        {10, {{0, 1, 2, 6, 4, 3}, {2, 5, 6, 8, 4, 0}, {4, 2, 6, 9, 8, 7}}},
        {12,
         {{0, 1, 2, 6, 4, 3},
          {2, 5, 6, 8, 4, 0},
          {4, 2, 6, 10, 8, 7},
          {6, 9, 10, 11, 8, 4}}},
        {14,
         {{0, 1, 2, 6, 4, 3},
          {2, 5, 6, 8, 4, 0},
          {4, 2, 6, 10, 8, 7},
          {6, 9, 10, 12, 8, 4},
          {8, 6, 10, 13, 12, 11}}},
    };
    for (const auto& c : measured) {
        const u32 count = triangle_strip_adjacency_count(c.vertices);
        ASSERT_EQ(count, c.triangles.size()) << c.vertices << " vertices";
        for (u32 i = 0; i < count; ++i) {
            for (u32 k = 0; k < 6; ++k) {
                EXPECT_EQ(triangle_strip_adjacency_vertex(i, k, count),
                          c.triangles[i][k])
                    << c.vertices << " vertices, triangle " << i
                    << ", input[" << k << "]";
            }
        }
    }
    EXPECT_EQ(triangle_strip_adjacency_count(5), 0u);
}

// Primitive assembly against tools/oracle_probe/d3d11_strip_order_probe.expected.
#include "graphics/shaders/stream_out_repack.hpp"

TEST(GeometryAdjacencyTest, PrimitiveAssemblyMatchesTheMeasuredStripOrder) {
    using graphics::PrimitiveAssembly;
    const auto flat = [](std::vector<std::vector<u32>> rows) {
        std::vector<u32> out;
        for (const auto& r : rows) out.insert(out.end(), r.begin(), r.end());
        return out;
    };
    // TRIANGLESTRIP n=6, non-indexed: odd triangles as (n, n+2, n+1).
    EXPECT_EQ(assemble_primitive_vertices({0, 1, 2, 3, 4, 5}, false, 0xFFFF, 0,
                                          3, PrimitiveAssembly::Strip),
              flat({{0, 1, 2}, {1, 3, 2}, {2, 3, 4}, {3, 5, 4}}));
    // Indexed, with a cut: the parity restarts at 14.
    const std::vector<u32> cut16{10, 11, 12, 13, 0xFFFF, 14, 15, 16, 17, 18};
    EXPECT_EQ(assemble_primitive_vertices(cut16, true, 0xFFFF, 0, 3,
                                          PrimitiveAssembly::Strip),
              flat({{10, 11, 12},
                    {11, 13, 12},
                    {14, 15, 16},
                    {15, 17, 16},
                    {16, 17, 18}}));
    // LINESTRIP indexed with a cut.
    EXPECT_EQ(assemble_primitive_vertices({10, 11, 12, 0xFFFF, 13, 14}, true,
                                          0xFFFF, 0, 2,
                                          PrimitiveAssembly::Strip),
              flat({{10, 11}, {11, 12}, {13, 14}}));
    // TRIANGLELIST indexed: the cut discards the partial primitive before it
    // and assembly restarts after it (13, 14 never complete one).
    EXPECT_EQ(assemble_primitive_vertices({10, 11, 12, 0xFFFF, 13, 14}, true,
                                          0xFFFF, 0, 3,
                                          PrimitiveAssembly::List),
              flat({{10, 11, 12}}));
    // The case that tells restart from "drop the primitive holding the cut"
    // (tools/oracle_probe/d3d11_a07_ia_probe.expected, `A idx16 TRIANGLELIST
    // containing 0xFFFF`): 13 is discarded and 14 starts the next triangle.
    EXPECT_EQ(assemble_primitive_vertices(
                  {10, 11, 12, 13, 0xFFFF, 14, 15, 16, 17, 18, 19, 20}, true,
                  0xFFFF, 0, 3, PrimitiveAssembly::List),
              flat({{10, 11, 12}, {14, 15, 16}, {17, 18, 19}}));
    // LINELIST {10,11,FFFF,12,13,14,15,FFFF,16,17} (same probe).
    EXPECT_EQ(assemble_primitive_vertices(
                  {10, 11, 0xFFFF, 12, 13, 14, 15, 0xFFFF, 16, 17}, true,
                  0xFFFF, 0, 2, PrimitiveAssembly::List),
              flat({{10, 11}, {12, 13}, {14, 15}, {16, 17}}));
    // Non-indexed, 0xFFFF is an ordinary vertex; base vertex is added.
    EXPECT_EQ(assemble_primitive_vertices({1, 2, 3}, false, 3, 5, 3,
                                          PrimitiveAssembly::List),
              flat({{6, 7, 8}}));
}
