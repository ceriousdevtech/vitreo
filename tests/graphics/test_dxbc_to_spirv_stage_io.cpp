// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_dxbc_to_spirv_stage_io.cpp
/// @brief Stage interfaces, system values, signatures, reflection fallback.

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

TEST(DxbcToSpirvTest, PreservesTypedVertexInputsAndOutputsInMsl) {
    for (const auto& [component_type, msl_type] :
         std::initializer_list<std::pair<u8, const char*>>{
             {1u, "uint4"}, {2u, "int4"}, {3u, "float4"},
         }) {
        DxbcContainer container{};
        container.shader_model_major = 4;
        container.shader_model_minor = 0;
        container.stage = ShaderStage::Vertex;
        container.input_signature.push_back(
            {"POSITION", 0, 0, 0xFu, 3u});
        // Chromium's glyph pass binds DXGI_FORMAT_R16G16_UINT at TEXCOORD.
        // The shader interface is typed even though SM4 registers retain raw
        // lane bits internally.
        container.input_signature.push_back(
            {"TEXCOORD", 0, 1, 0x3u, component_type});
        container.output_signature.push_back(
            {"SV_Position", 0, 0, 0xFu, 3u});
        container.output_signature.push_back(
            {"TEXCOORD", 0, 1, 0x3u, component_type});
        const u32 words[] = {
            0x00010040u, 13u,                    // vs_4_0
            54u | (5u << 24),                    // mov o0, v0
            0x001020F2u, 0u,
            0x001010E4u, 0u,
            54u | (5u << 24),                    // mov o1, v1
            0x001020F2u, 1u,
            0x001010E4u, 1u,
            62u | (1u << 24),                   // ret
        };
        const auto* bytes = reinterpret_cast<const u8*>(words);
        container.bytecode.assign(bytes, bytes + sizeof(words));

        const auto result = translate_dxbc_to_spirv(container);
        ASSERT_TRUE(result.valid()) << result.failure;
        EXPECT_EQ(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u),
                  component_type != 3u);

        MslCompileOptions options{};
        options.argument_buffers = false;
        const auto compiled = compile_spirv_to_msl(
            result.spirv.data(), result.spirv.size(), ShaderStage::Vertex,
            options);
        ASSERT_TRUE(compiled.valid());
        const std::string msl = compiled.msl_text();
        EXPECT_NE(msl.find(std::string(msl_type) + " TEXCOORD0"),
                  std::string::npos) << msl;
        if (component_type != 3u) {
            EXPECT_TRUE(spirv_contains_decoration(
                result.spirv, /*Flat=*/14u));
        }
    }
}

TEST(DxbcToSpirvTest, SpecializesVertexFetchTypeForD3DLayoutMismatch) {
    for (const u8 signature_type :
         std::initializer_list<u8>{1u, 2u, 3u}) {
        for (const auto& [storage_type, msl_type] :
             std::initializer_list<std::pair<u8, const char*>>{
                 {1u, "uint4 TEXCOORD0"},
                 {2u, "int4 TEXCOORD0"},
                 {3u, "float4 TEXCOORD0"},
             }) {
            DxbcContainer container =
                create_unary_vertex_shader(/*MOV=*/54u);
            container.input_signature[0].semantic_name = "TEXCOORD";
            container.input_signature[0].component_type = signature_type;
            DxbcTranslationOptions options{};
            options.vertex_input_component_types[0] = storage_type;

            const auto result = translate_dxbc_to_spirv(container, options);
            ASSERT_TRUE(result.valid()) << result.failure;
            const auto compiled = compile_spirv_to_msl(
                result.spirv.data(), result.spirv.size(),
                ShaderStage::Vertex);
            ASSERT_TRUE(compiled.valid());
            EXPECT_NE(std::string(compiled.msl_text()).find(msl_type),
                      std::string::npos)
                << "signature=" << static_cast<unsigned>(signature_type)
                << " storage=" << static_cast<unsigned>(storage_type)
                << '\n' << compiled.msl_text();
        }
    }
}

TEST(DxbcToSpirvTest, PreservesTypedFragmentInputsAndTargetsInMsl) {
    for (const auto& [component_type, msl_type] :
         std::initializer_list<std::pair<u8, const char*>>{
             {1u, "uint4"}, {2u, "int4"}, {3u, "float4"},
         }) {
        DxbcContainer container{};
        container.shader_model_major = 4;
        container.shader_model_minor = 0;
        container.stage = ShaderStage::Fragment;
        container.input_signature.push_back(
            {"TEXCOORD", 0, 0, 0xFu, component_type});
        container.output_signature.push_back(
            {"SV_TARGET", 0, 0, 0xFu, component_type});
        const u32 words[] = {
            0x00000040u, 8u,                     // ps_4_0
            54u | (5u << 24),                    // mov o0, v0
            0x001020F2u, 0u,
            0x001010E4u, 0u,
            62u | (1u << 24),
        };
        const auto* bytes = reinterpret_cast<const u8*>(words);
        container.bytecode.assign(bytes, bytes + sizeof(words));

        const auto result = translate_dxbc_to_spirv(container);
        ASSERT_TRUE(result.valid()) << result.failure;
        const auto compiled = compile_spirv_to_msl(
            result.spirv.data(), result.spirv.size(), ShaderStage::Fragment);
        ASSERT_TRUE(compiled.valid());
        const std::string msl = compiled.msl_text();
        EXPECT_NE(msl.find(std::string(msl_type) + " TEXCOORD0"),
                  std::string::npos) << msl;
        EXPECT_NE(msl.find(std::string(msl_type) + " SV_TARGET0"),
                  std::string::npos) << msl;
        if (component_type != 3u) {
            EXPECT_TRUE(spirv_contains_decoration(
                result.spirv, /*Flat=*/14u));
        }
    }
}

TEST(DxbcToSpirvTest, FlatDecorationAppliesOnlyToIntegerVaryings) {
    DxbcContainer vertex = create_unary_vertex_shader(/*MOV=*/54u);
    vertex.input_signature[0].component_type = 1u;
    vertex.output_signature[0].component_type = 1u;
    const auto vertex_result = translate_dxbc_to_spirv(vertex);
    ASSERT_TRUE(vertex_result.valid()) << vertex_result.failure;
    EXPECT_FALSE(spirv_contains_decoration(
        vertex_result.spirv, /*Flat=*/14u));
    const auto vertex_msl = compile_spirv_to_msl(
        vertex_result.spirv.data(), vertex_result.spirv.size(),
        ShaderStage::Vertex);
    ASSERT_TRUE(vertex_msl.valid());
    EXPECT_NE(std::string(vertex_msl.msl_text()).find(
                  "float4 gl_Position [[position]]"),
              std::string::npos);

    DxbcContainer fragment{};
    fragment.shader_model_major = 4;
    fragment.shader_model_minor = 0;
    fragment.stage = ShaderStage::Fragment;
    fragment.input_signature.push_back(
        {"SV_Position", 0, 0, 0xFu, 1u, 1u});
    fragment.output_signature.push_back(
        {"SV_TARGET", 0, 0, 0xFu, 1u});
    const u32 words[] = {
        0x00000040u, 8u,
        54u | (5u << 24),
        0x001020F2u, 0u,
        0x001010E4u, 0u,
        62u | (1u << 24),
    };
    const auto* bytes = reinterpret_cast<const u8*>(words);
    fragment.bytecode.assign(bytes, bytes + sizeof(words));
    const auto fragment_result = translate_dxbc_to_spirv(fragment);
    ASSERT_TRUE(fragment_result.valid()) << fragment_result.failure;
    EXPECT_FALSE(spirv_contains_decoration(
        fragment_result.spirv, /*Flat=*/14u));
    const auto fragment_msl = compile_spirv_to_msl(
        fragment_result.spirv.data(), fragment_result.spirv.size(),
        ShaderStage::Fragment);
    ASSERT_TRUE(fragment_msl.valid());
    EXPECT_NE(std::string(fragment_msl.msl_text()).find(
                  "float4 gl_FragCoord [[position]]"),
              std::string::npos);
}

TEST(DxbcToSpirvTest, PreservesTypedGeometryInterfaceBitsInSpirv) {
    for (const u8 component_type :
         std::initializer_list<u8>{1u, 2u, 3u}) {
        DxbcContainer container = create_fxc_point_geometry_shader();
        container.input_signature[1].component_type = component_type;
        container.output_signature[1].component_type = component_type;
        const auto result = translate_dxbc_to_spirv(container);
        ASSERT_TRUE(result.valid()) << result.failure;
        EXPECT_EQ(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u),
                  component_type != 3u);
    }
}

TEST(DxbcToSpirvTest, RejectsUnsupportedSystemValueInterfacesHonestly) {
    // A vertex shader's INPUTS are vertex data plus SV_VertexID (6) and
    // SV_InstanceID (8); no other system value can arrive there.
    for (const u32 system_value : {2u, 3u, 4u, 5u, 7u, 9u, 10u}) {
        DxbcContainer input = create_unary_vertex_shader(/*MOV=*/54u);
        input.input_signature[0].system_value_type = system_value;
        const auto input_result = translate_dxbc_to_spirv(input);
        EXPECT_FALSE(input_result.valid()) << system_value;
        EXPECT_NE(input_result.failure.find("unsupported input system value"),
                  std::string::npos) << input_result.failure;
    }
    // Vertex OUTPUTS: SV_ClipDistance (2), SV_RenderTargetArrayIndex (4) and
    // SV_ViewportArrayIndex (5) translate (SystemValues GPU tests).
    // SV_CullDistance (3) is refused with the reason; values that are not
    // vertex outputs at all are refused generically.
    const std::vector<std::pair<u32, const char*>> refused{
        {3u, "Metal has no cull distances"},
        {7u, "unsupported output system value"},
        {9u, "unsupported output system value"},
        {10u, "unsupported output system value"},
    };
    for (const auto& [system_value, reason] : refused) {
        DxbcContainer output = create_unary_vertex_shader(/*MOV=*/54u);
        output.output_signature[0].system_value_type = system_value;
        output.output_signature[0].semantic_name = "SYSTEM";
        const auto output_result = translate_dxbc_to_spirv(output);
        EXPECT_FALSE(output_result.valid()) << system_value;
        EXPECT_NE(output_result.failure.find(reason), std::string::npos)
            << system_value << ": " << output_result.failure;
    }
}

// Microsoft Learn (Semantics): SV_VertexID and SV_InstanceID come from the
// input assembler, not a vertex buffer, so SPIR-V models them as scalar
// builtins rather than location-bound varyings. Unity's instanced draws
// declare SV_InstanceID; rejecting it failed the vertex shader and left the
// whole frame on Unity's magenta error material.
TEST(DxbcToSpirvTest, TranslatesVertexStageIdBuiltinsAsScalarSystemValues) {
    for (const u32 system_value : {6u, 8u}) {
        DxbcContainer container = create_unary_vertex_shader(/*MOV=*/54u);
        container.input_signature[0].system_value_type = system_value;
        container.input_signature[0].semantic_name =
            system_value == 6u ? "SV_VertexID" : "SV_InstanceID";
        container.input_signature[0].component_mask = 0x1u;
        const auto result = translate_dxbc_to_spirv(container);
        ASSERT_TRUE(result.valid()) << system_value << ": " << result.failure;
        const auto compiled = compile_spirv_to_msl(
            result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
        ASSERT_TRUE(compiled.valid()) << system_value;
        const std::string msl(compiled.msl_text());
        EXPECT_NE(msl.find(system_value == 6u ? "vertex_id" : "instance_id"),
                  std::string::npos)
            << system_value << " did not reach an MSL stage-in attribute:\n"
            << msl;
        // D3D11: SV_InstanceID restarts at 0 in every draw whatever
        // StartInstanceLocation is; Metal's [[instance_id]] includes the base
        // instance, so the translation must subtract [[base_instance]].
        // SV_VertexID likewise excludes StartVertexLocation/BaseVertexLocation
        // (D3D11.3 8.16; Windows d3d11_a07_ia / d3d11_a09b: Draw(3, start 5)
        // reads 0 1 2) -- this test used to assert the opposite -- so it
        // subtracts [[base_vertex]].
        if (system_value == 8u) {
            EXPECT_NE(msl.find("[[base_instance]]"), std::string::npos) << msl;
            EXPECT_NE(msl.find("gl_InstanceIndex - gl_BaseInstance"),
                      std::string::npos)
                << msl;
        } else {
            EXPECT_NE(msl.find("[[base_vertex]]"), std::string::npos) << msl;
            EXPECT_NE(msl.find("gl_VertexIndex - gl_BaseVertex"),
                      std::string::npos)
                << msl;
        }
    }
}

TEST(DxbcToSpirvTest, MapsIsFrontFaceToFragmentBooleanBuiltin) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.stage = ShaderStage::Fragment;
    container.input_signature.push_back(
        {"SV_IsFrontFace", 0, 3, 0x1u, 1u, 9u});
    container.output_signature.push_back(
        {"SV_TARGET", 0, 0, 0xFu, 1u, 64u});
    const u32 words[] = {
        0x00000050u, 8u,
        54u | (5u << 24),
        0x001020F2u, 0u,
        0x00101000u, 3u,
        62u | (1u << 24),
    };
    const auto* bytes = reinterpret_cast<const u8*>(words);
    container.bytecode.assign(bytes, bytes + sizeof(words));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment);
    ASSERT_TRUE(compiled.valid());
    EXPECT_NE(std::string(compiled.msl_text()).find("front_facing"),
              std::string::npos);
}

TEST(DxbcToSpirvTest, MapsPixelDepthOutputToFragDepth) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.stage = ShaderStage::Fragment;
    container.output_signature.push_back(
        {"SV_Depth", 0, 0, 0x1u, 3u, 65u});
    const u32 words[] = {
        0x00000050u, 7u,
        54u | (4u << 24),             // mov oDepth, l(0.5)
        0x0000C001u,
        0x00004001u, 0x3f000000u,
        62u | (1u << 24),
    };
    const auto* bytes = reinterpret_cast<const u8*>(words);
    container.bytecode.assign(bytes, bytes + sizeof(words));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment);
    ASSERT_TRUE(compiled.valid());
    EXPECT_NE(std::string(compiled.msl_text()).find("[[depth(any)]]"),
              std::string::npos);
}

TEST(DxbcToSpirvTest, TypedFragmentTargetSystemValueRemainsAColorOutput) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Fragment;
    container.input_signature.push_back(
        {"TEXCOORD", 0, 0, 0xFu, 1u});
    container.output_signature.push_back(
        {"SV_TARGET", 0, 0, 0xFu, 1u, 64u});
    const u32 words[] = {
        0x00000040u, 8u,
        54u | (5u << 24),
        0x001020F2u, 0u,
        0x001010E4u, 0u,
        62u | (1u << 24),
    };
    const auto* bytes = reinterpret_cast<const u8*>(words);
    container.bytecode.assign(bytes, bytes + sizeof(words));
    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment);
    ASSERT_TRUE(compiled.valid());
    EXPECT_NE(std::string(compiled.msl_text()).find(
                  "uint4 SV_TARGET0 [[color(0)]]"),
              std::string::npos);
}

TEST(DxbcToSpirvTest, StrippedReflectionUsesConstantBufferDeclaration) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3});

    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00010040u); // vs_4_0
    append_word(13u);
    append_word(89u | (4u << 24)); // dcl_constantbuffer cb0[1]
    append_word(0x00208E46u);
    append_word(0u);
    append_word(1u);
    append_word(54u | (6u << 24)); // mov o0, cb0[0]
    append_word(0x001020F2u);
    append_word(0u);
    append_word(0x00208046u);
    append_word(0u);
    append_word(0u);
    append_word(62u | (1u << 24));

    ASSERT_TRUE(container.resource_bindings.empty());
    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    ASSERT_EQ(result.cbuffer_bindings.size(), 1u);
    EXPECT_EQ(result.cbuffer_bindings[0].bind_point, 0u);
}

TEST(DxbcToSpirvTest, UppercaseSystemPositionBecomesVertexPositionBuiltin) {
    DxbcContainer container = create_binary_vertex_shader(54u); // mov
    ASSERT_EQ(container.output_signature.size(), 1u);
    container.output_signature[0].semantic_name = "SV_POSITION";
    container.output_signature[0].system_value_type = 1;

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;

    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("[[position]]"), std::string::npos);
    EXPECT_EQ(msl.find("SV_POSITION0 [[user("), std::string::npos);
}

// fxc (tools/oracle_probe/vs_position_semantics.expected): only SV_Position
// carries SysValue POS; POSITION1/POSITION2 are NONE, ordinary user outputs.
// Treating them as positions gave Metal three [[position]] members and the
// vertex function failed to compile (Skyrim SE world/previous positions).
TEST(DxbcToSpirvTest, UserPositionSemanticsStayOrdinaryVertexOutputs) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 3u});
    container.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3u});
    container.output_signature.back().system_value_type = 1;
    container.output_signature.push_back({"POSITION", 1, 1, 0xFu, 3u});
    container.output_signature.push_back({"POSITION", 2, 2, 0xFu, 3u});
    const u32 words[] = {
        0x00010050u, 18u,                    // vs_5_0
        54u | (5u << 24), 0x001020F2u, 0u, 0x001010E4u, 0u,  // mov o0, v0
        54u | (5u << 24), 0x001020F2u, 1u, 0x001010E4u, 0u,  // mov o1, v0
        54u | (5u << 24), 0x001020F2u, 2u, 0x001010E4u, 0u,  // mov o2, v0
        62u | (1u << 24),                    // ret
    };
    const auto* bytes = reinterpret_cast<const u8*>(words);
    container.bytecode.assign(bytes, bytes + sizeof(words));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex,
        options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    usize positions = 0;
    for (usize at = msl.find("[[position"); at != std::string::npos;
         at = msl.find("[[position", at + 1)) {
        ++positions;
    }
    EXPECT_EQ(positions, 1u) << msl;
    EXPECT_NE(msl.find("POSITION1 [[user("), std::string::npos) << msl;
    EXPECT_NE(msl.find("POSITION2 [[user("), std::string::npos) << msl;
}

TEST(DxbcToSpirvTest, PixelSystemPositionBecomesFragmentCoordinateBuiltin) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 1;
    container.stage = ShaderStage::Fragment;
    // FXC's real signature shape uses uppercase spelling and the structured
    // D3D_NAME_POSITION value.
    container.input_signature.push_back(
        {"SV_POSITION", 0, 0, 0xFu, 3, 1});
    container.output_signature.push_back(
        {"SV_TARGET", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00000041u); // ps_4_1
    append_word(8u);
    append_word(54u | (5u << 24)); // mov o0, v0
    append_word(0x001020F2u); append_word(0u);
    append_word(0x001010E4u); append_word(0u);
    append_word(62u | (1u << 24));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_builtin(
        result.spirv, /*BuiltIn FragCoord=*/15u));
    EXPECT_FALSE(spirv_contains_builtin(
        result.spirv, /*BuiltIn Position=*/0u));

    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("[[position]]"), std::string::npos);
    EXPECT_EQ(msl.find("SV_POSITION0 [[user("), std::string::npos);

    // Stripped/synthetic signatures may retain the semantic name while the
    // structured system-value field is zero; semantic matching remains
    // case-insensitive for that valid shape.
    container.input_signature[0].semantic_name = "SV_Position";
    container.input_signature[0].system_value_type = 0;
    const auto semantic_result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(semantic_result.valid()) << semantic_result.failure;
    EXPECT_TRUE(spirv_contains_builtin(
        semantic_result.spirv, /*BuiltIn FragCoord=*/15u));
}

TEST(DxbcToSpirvTest, TranslatesInputSignature) {
    auto dxbc_blob = create_minimal_vertex_shader_dxbc();
    auto container = parse_dxbc(dxbc_blob.data(), dxbc_blob.size());
    
    ASSERT_EQ(container.input_signature.size(), 1u);
    EXPECT_EQ(container.input_signature[0].semantic_name, "POSITION");
    EXPECT_EQ(container.input_signature[0].semantic_index, 0u);
    EXPECT_EQ(container.input_signature[0].register_num, 0u);
    
    auto result = translate_dxbc_to_spirv(container);
    EXPECT_TRUE(result.valid());
}

TEST(DxbcToSpirvTest, TranslatesOutputSignature) {
    auto dxbc_blob = create_minimal_vertex_shader_dxbc();
    auto container = parse_dxbc(dxbc_blob.data(), dxbc_blob.size());
    
    ASSERT_EQ(container.output_signature.size(), 1u);
    EXPECT_EQ(container.output_signature[0].semantic_name, "SV_Position");
    EXPECT_EQ(container.output_signature[0].semantic_index, 0u);
    
    auto result = translate_dxbc_to_spirv(container);
    EXPECT_TRUE(result.valid());
}
