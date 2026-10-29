// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_dxbc_to_spirv_alu_unary.cpp
/// @brief Unary float ops, derivatives, sincos with modifiers and saturate.

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

TEST(DxbcToSpirvTest, RsqUsesInverseSqrtWithModifiersSwizzleAndMask) {
    DxbcContainer container = create_unary_vertex_shader(/*RSQ=*/68u);
    const u32 total_words = 9u;
    const u32 instruction = 68u | (6u << 24);
    const u32 masked_dst = 0x00102052u; // o0.xz
    const u32 extended_reversed_src = 0x801011B6u; // v0.wzyx
    const u32 abs_neg = 1u | (3u << 6);
    std::memcpy(container.bytecode.data() + sizeof(u32),
                &total_words, sizeof(total_words));
    std::memcpy(container.bytecode.data() + 2u * sizeof(u32),
                &instruction, sizeof(instruction));
    std::memcpy(container.bytecode.data() + 3u * sizeof(u32),
                &masked_dst, sizeof(masked_dst));
    std::memcpy(container.bytecode.data() + 5u * sizeof(u32),
                &extended_reversed_src, sizeof(extended_reversed_src));
    const auto* modifier_bytes = reinterpret_cast<const u8*>(&abs_neg);
    container.bytecode.insert(container.bytecode.begin() + 6u * sizeof(u32),
                              modifier_bytes,
                              modifier_bytes + sizeof(abs_neg));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_ext_inst_instruction(
        result.spirv, /*GLSL.std.450 InverseSqrt=*/32u));
    EXPECT_TRUE(spirv_contains_ext_inst_instruction(
        result.spirv, /*GLSL.std.450 FAbs=*/4u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpFNegate=*/127u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpVectorShuffle=*/79u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeConstruct=*/80u));
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, SqrtUsesSqrtWithModifiersSwizzleAndMask) {
    DxbcContainer container = create_unary_vertex_shader(/*SQRT=*/75u);
    const u32 total_words = 9u;
    const u32 instruction = 75u | (6u << 24);
    const u32 masked_dst = 0x00102052u; // o0.xz
    const u32 extended_reversed_src = 0x801011B6u; // v0.wzyx
    const u32 abs_neg = 1u | (3u << 6);
    std::memcpy(container.bytecode.data() + sizeof(u32),
                &total_words, sizeof(total_words));
    std::memcpy(container.bytecode.data() + 2u * sizeof(u32),
                &instruction, sizeof(instruction));
    std::memcpy(container.bytecode.data() + 3u * sizeof(u32),
                &masked_dst, sizeof(masked_dst));
    std::memcpy(container.bytecode.data() + 5u * sizeof(u32),
                &extended_reversed_src, sizeof(extended_reversed_src));
    const auto* modifier_bytes = reinterpret_cast<const u8*>(&abs_neg);
    container.bytecode.insert(container.bytecode.begin() + 6u * sizeof(u32),
                              modifier_bytes,
                              modifier_bytes + sizeof(abs_neg));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_ext_inst_instruction(
        result.spirv, /*GLSL.std.450 Sqrt=*/31u));
    EXPECT_TRUE(spirv_contains_ext_inst_instruction(
        result.spirv, /*GLSL.std.450 FAbs=*/4u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpFNegate=*/127u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpVectorShuffle=*/79u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeConstruct=*/80u));
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    ASSERT_TRUE(compiled.valid());
    EXPECT_NE(std::string(compiled.msl_text()).find("sqrt"),
              std::string::npos);
}

/// Learn "Instruction Return Type and Modifiers": OpcodeToken0 bit 13
/// (SATURATE) clamps every written destination component to [0,1] for any
/// float-result instruction, with saturate(NaN) == 0. The translator applies
/// it once, in store_vec4, as ordered compares feeding selects.
void expect_saturated_destination(const DxbcToSpirvResult& result) {
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv,
                                      /*OpFOrdGreaterThanEqual=*/190u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpFOrdLessThan=*/184u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpSelect=*/169u));
}

TEST(DxbcToSpirvTest, SqrtRsqRcpApplyDestinationSaturateModifier) {
    for (const u32 opcode : {/*SQRT=*/75u, /*RSQ=*/68u, /*RCP=*/129u}) {
        DxbcContainer container = create_unary_vertex_shader(opcode);
        u32 instruction = 0;
        std::memcpy(&instruction,
                    container.bytecode.data() + 2u * sizeof(u32),
                    sizeof(instruction));
        instruction |= 0x00002000u;
        std::memcpy(container.bytecode.data() + 2u * sizeof(u32),
                    &instruction, sizeof(instruction));
        SCOPED_TRACE(opcode);
        expect_saturated_destination(translate_dxbc_to_spirv(container));
    }
}

TEST(DxbcToSpirvTest, RoundFamilyUsesExactModesModifiersSwizzlesAndMasks) {
    struct Mode {
        u32 opcode;
        u32 ext_inst;
        const char* msl_function;
    };
    for (const auto& [opcode, ext_inst, msl_function] :
         std::initializer_list<Mode>{
             {64u, 2u, "rint("},  // ROUND_NE -> RoundEven
             {65u, 8u, "floor("}, // ROUND_NI -> Floor
             {66u, 9u, "ceil("},  // ROUND_PI -> Ceil
             {67u, 3u, "trunc("}, // ROUND_Z -> Trunc
         }) {
        DxbcContainer container = create_unary_vertex_shader(opcode);
        const u32 total_words = 9u;
        const u32 instruction = opcode | (6u << 24);
        const u32 masked_dst = 0x00102052u; // o0.xz
        const u32 extended_reversed_src = 0x801011B6u; // v0.wzyx
        const u32 abs_neg = 1u | (3u << 6);
        std::memcpy(container.bytecode.data() + sizeof(u32),
                    &total_words, sizeof(total_words));
        std::memcpy(container.bytecode.data() + 2u * sizeof(u32),
                    &instruction, sizeof(instruction));
        std::memcpy(container.bytecode.data() + 3u * sizeof(u32),
                    &masked_dst, sizeof(masked_dst));
        std::memcpy(container.bytecode.data() + 5u * sizeof(u32),
                    &extended_reversed_src, sizeof(extended_reversed_src));
        const auto* modifier_bytes =
            reinterpret_cast<const u8*>(&abs_neg);
        container.bytecode.insert(
            container.bytecode.begin() + 6u * sizeof(u32), modifier_bytes,
            modifier_bytes + sizeof(abs_neg));

        const auto result = translate_dxbc_to_spirv(container);
        ASSERT_TRUE(result.valid()) << result.failure;
        EXPECT_TRUE(spirv_contains_ext_inst_instruction(
            result.spirv, ext_inst));
        EXPECT_TRUE(spirv_contains_ext_inst_instruction(
            result.spirv, /*GLSL.std.450 FAbs=*/4u));
        EXPECT_TRUE(spirv_contains_opcode(result.spirv,
                                          /*OpFNegate=*/127u));
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, /*OpVectorShuffle=*/79u));
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, /*OpCompositeConstruct=*/80u));
        const auto compiled = compile_spirv_to_msl(
            result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
        ASSERT_TRUE(compiled.valid());
        EXPECT_NE(std::string(compiled.msl_text()).find(msl_function),
                  std::string::npos);
    }
}

TEST(DxbcToSpirvTest, RoundFamilyAppliesDestinationSaturateModifier) {
    for (const u32 opcode : {64u, 65u, 66u, 67u}) {
        DxbcContainer container = create_unary_vertex_shader(opcode);
        u32 instruction = 0;
        std::memcpy(&instruction,
                    container.bytecode.data() + 2u * sizeof(u32),
                    sizeof(instruction));
        instruction |= 0x00002000u;
        std::memcpy(container.bytecode.data() + 2u * sizeof(u32),
                    &instruction, sizeof(instruction));
        const auto result = translate_dxbc_to_spirv(container);
        expect_saturated_destination(result);
    }
}

TEST(DxbcToSpirvTest, ExpLogUseBaseTwoWithModifiersSwizzlesAndMasks) {
    struct Operation {
        u32 opcode;
        u32 ext_inst;
        const char* msl_function;
    };
    for (const auto& [opcode, ext_inst, msl_function] :
         std::initializer_list<Operation>{
             {25u, 29u, "exp2("},
             {47u, 30u, "log2("},
         }) {
        DxbcContainer container = create_unary_vertex_shader(opcode);
        const u32 total_words = 9u;
        const u32 instruction = opcode | (6u << 24);
        const u32 masked_dst = 0x00102052u; // o0.xz
        const u32 extended_reversed_src = 0x801011B6u; // v0.wzyx
        const u32 abs_neg = 1u | (3u << 6);
        std::memcpy(container.bytecode.data() + sizeof(u32),
                    &total_words, sizeof(total_words));
        std::memcpy(container.bytecode.data() + 2u * sizeof(u32),
                    &instruction, sizeof(instruction));
        std::memcpy(container.bytecode.data() + 3u * sizeof(u32),
                    &masked_dst, sizeof(masked_dst));
        std::memcpy(container.bytecode.data() + 5u * sizeof(u32),
                    &extended_reversed_src, sizeof(extended_reversed_src));
        const auto* modifier_bytes =
            reinterpret_cast<const u8*>(&abs_neg);
        container.bytecode.insert(
            container.bytecode.begin() + 6u * sizeof(u32), modifier_bytes,
            modifier_bytes + sizeof(abs_neg));

        const auto result = translate_dxbc_to_spirv(container);
        ASSERT_TRUE(result.valid()) << result.failure;
        EXPECT_TRUE(spirv_contains_ext_inst_instruction(
            result.spirv, ext_inst));
        EXPECT_TRUE(spirv_contains_ext_inst_instruction(
            result.spirv, /*GLSL.std.450 FAbs=*/4u));
        EXPECT_TRUE(spirv_contains_opcode(result.spirv,
                                          /*OpFNegate=*/127u));
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, /*OpVectorShuffle=*/79u));
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, /*OpCompositeConstruct=*/80u));
        const auto compiled = compile_spirv_to_msl(
            result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
        ASSERT_TRUE(compiled.valid());
        EXPECT_NE(std::string(compiled.msl_text()).find(msl_function),
                  std::string::npos);
    }
}

TEST(DxbcToSpirvTest, ExpLogApplyDestinationSaturateModifier) {
    for (const u32 opcode : {25u, 47u}) {
        DxbcContainer container = create_unary_vertex_shader(opcode);
        u32 instruction = 0;
        std::memcpy(&instruction,
                    container.bytecode.data() + 2u * sizeof(u32),
                    sizeof(instruction));
        instruction |= 0x00002000u;
        std::memcpy(container.bytecode.data() + 2u * sizeof(u32),
                    &instruction, sizeof(instruction));
        const auto result = translate_dxbc_to_spirv(container);
        expect_saturated_destination(result);
    }
}

TEST(DxbcToSpirvTest, FrcUsesFractionWithModifiersSwizzleAndMask) {
    DxbcContainer container = create_unary_vertex_shader(/*FRC=*/26u);
    const u32 total_words = 9u;
    const u32 instruction = 26u | (6u << 24);
    const u32 masked_dst = 0x00102052u; // o0.xz
    const u32 extended_reversed_src = 0x801011B6u; // v0.wzyx
    const u32 abs_neg = 1u | (3u << 6);
    std::memcpy(container.bytecode.data() + sizeof(u32),
                &total_words, sizeof(total_words));
    std::memcpy(container.bytecode.data() + 2u * sizeof(u32),
                &instruction, sizeof(instruction));
    std::memcpy(container.bytecode.data() + 3u * sizeof(u32),
                &masked_dst, sizeof(masked_dst));
    std::memcpy(container.bytecode.data() + 5u * sizeof(u32),
                &extended_reversed_src, sizeof(extended_reversed_src));
    const auto* modifier_bytes = reinterpret_cast<const u8*>(&abs_neg);
    container.bytecode.insert(container.bytecode.begin() + 6u * sizeof(u32),
                              modifier_bytes,
                              modifier_bytes + sizeof(abs_neg));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    // D3D11.3 22.10.7: x - floor(x), clamped below 1.0 (MSL fract returns
    // 1.0 for a tiny negative, Windows frc(-FLT_MIN) = 0x3F7FFFFF), with
    // +-INF -> NaN -- so no GLSL Fract, but Floor, FMin and the NaN select.
    EXPECT_FALSE(spirv_contains_ext_inst_instruction(
        result.spirv, /*GLSL.std.450 Fract=*/10u));
    EXPECT_TRUE(spirv_contains_ext_inst_instruction(
        result.spirv, /*GLSL.std.450 Floor=*/8u));
    EXPECT_TRUE(spirv_contains_ext_inst_instruction(
        result.spirv, /*GLSL.std.450 FMin=*/37u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpSelect=*/169u));
    EXPECT_TRUE(spirv_contains_ext_inst_instruction(
        result.spirv, /*GLSL.std.450 FAbs=*/4u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpFNegate=*/127u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpVectorShuffle=*/79u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeConstruct=*/80u));
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    ASSERT_TRUE(compiled.valid());
    EXPECT_NE(std::string(compiled.msl_text()).find("floor("),
              std::string::npos);
}

TEST(DxbcToSpirvTest, FrcAppliesDestinationSaturateModifier) {
    DxbcContainer container = create_unary_vertex_shader(/*FRC=*/26u);
    u32 instruction = 0;
    std::memcpy(&instruction,
                container.bytecode.data() + 2u * sizeof(u32),
                sizeof(instruction));
    instruction |= 0x00002000u;
    std::memcpy(container.bytecode.data() + 2u * sizeof(u32),
                &instruction, sizeof(instruction));
    const auto result = translate_dxbc_to_spirv(container);
    expect_saturated_destination(result);
}

TEST(DxbcToSpirvTest, DerivativesUsePixelGradientWithModifiersSwizzleAndMask) {
    for (const auto& [opcode, spirv_opcode, msl_function] :
         std::initializer_list<std::tuple<u32, u32, const char*>>{
             {11u, 207u, "dfdx("},
             {12u, 208u, "dfdy("},
             // Captured from Chromium's forced GPU-raster tile shader. The
             // coarse forms are one pair per 2x2 quad (22.5.2/3): the fine
             // derivative (OpDPdxFine 210 / OpDPdyFine 211) broadcast from
             // the quad's first lane.
             {122u, 210u, "dfdx("},
             {124u, 211u, "dfdy("},
         }) {
        DxbcContainer container = create_unary_vertex_shader(opcode);
        container.stage = ShaderStage::Fragment;
        container.input_signature[0].semantic_name = "TEXCOORD";
        container.output_signature[0].semantic_name = "SV_TARGET";
        const u32 ps_version = 0x00000040u;
        const u32 total_words = 9u;
        const u32 instruction = opcode | (6u << 24);
        const u32 masked_dst = 0x00102052u; // o0.xz
        const u32 extended_reversed_src = 0x801011B6u; // v0.wzyx
        const u32 abs_neg = 1u | (3u << 6);
        std::memcpy(container.bytecode.data(), &ps_version,
                    sizeof(ps_version));
        std::memcpy(container.bytecode.data() + sizeof(u32),
                    &total_words, sizeof(total_words));
        std::memcpy(container.bytecode.data() + 2u * sizeof(u32),
                    &instruction, sizeof(instruction));
        std::memcpy(container.bytecode.data() + 3u * sizeof(u32),
                    &masked_dst, sizeof(masked_dst));
        std::memcpy(container.bytecode.data() + 5u * sizeof(u32),
                    &extended_reversed_src, sizeof(extended_reversed_src));
        const auto* modifier_bytes =
            reinterpret_cast<const u8*>(&abs_neg);
        container.bytecode.insert(
            container.bytecode.begin() + 6u * sizeof(u32), modifier_bytes,
            modifier_bytes + sizeof(abs_neg));

        const auto result = translate_dxbc_to_spirv(container);
        ASSERT_TRUE(result.valid()) << result.failure;
        EXPECT_TRUE(spirv_contains_opcode(result.spirv, spirv_opcode));
        EXPECT_TRUE(spirv_contains_ext_inst_instruction(
            result.spirv, /*GLSL.std.450 FAbs=*/4u));
        EXPECT_TRUE(spirv_contains_opcode(result.spirv,
                                          /*OpFNegate=*/127u));
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, /*OpVectorShuffle=*/79u));
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, /*OpCompositeConstruct=*/80u));
        const auto compiled = compile_spirv_to_msl(
            result.spirv.data(), result.spirv.size(), ShaderStage::Fragment);
        ASSERT_TRUE(compiled.valid());
        EXPECT_NE(std::string(compiled.msl_text()).find(msl_function),
                  std::string::npos);
    }
}

TEST(DxbcToSpirvTest, CapturedGpuRasterCoarseDerivativePairCompilesToMsl) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Fragment;
    container.input_signature.push_back({"TEXCOORD", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"SV_TARGET", 0, 0, 0xFu, 3});
    const u32 words[] = {
        0x00000050u, 25u,                         // ps_5_0
        0x02000068u, 1u,                         // dcl_temps 1
        0x05000036u, 0x001000F2u, 0u,            // mov r0.xyzw,
        0x001010E4u, 0u,                         //     v0.xyzw
        // Exact operand shapes from Chromium's GPU-raster tile shader:
        0x0500007Au, 0x00100022u, 0u,            // deriv_rtx_coarse r0.y,
        0x0010000Au, 0u,                         //     r0.x
        0x0500007Cu, 0x00100042u, 0u,            // deriv_rty_coarse r0.z,
        0x0010000Au, 0u,                         //     r0.x
        0x05000036u, 0x001020F2u, 0u,            // mov o0.xyzw,
        0x001000E4u, 0u,                         //     r0.xyzw
        0x0100003Eu,                             // ret
    };
    const auto* bytes = reinterpret_cast<const u8*>(words);
    container.bytecode.assign(bytes, bytes + sizeof(words));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    // D3D11.3 22.5.2/3: one derivative pair per 2x2 stamp (Windows
    // d3d11_a09b [PS derivatives]); MSL has no coarse form, so the quad's
    // first lane's fine derivative is broadcast to the quad.
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpDPdxFine=*/210u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpDPdyFine=*/211u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpGroupNonUniformQuadBroadcast=*/365u));
    EXPECT_TRUE(spirv_contains_capability(
        result.spirv, /*DerivativeControl=*/51u));
    EXPECT_TRUE(spirv_contains_capability(
        result.spirv, /*GroupNonUniformQuad=*/68u));
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment);
    ASSERT_TRUE(compiled.valid());
    EXPECT_NE(std::string(compiled.msl_text()).find("dfdx("),
              std::string::npos);
    EXPECT_NE(std::string(compiled.msl_text()).find("dfdy("),
              std::string::npos);
}

TEST(DxbcToSpirvTest, DerivativesRejectWrongStageAndApplySaturate) {
    DxbcContainer vertex = create_unary_vertex_shader(/*DERIV_RTX=*/11u);
    auto result = translate_dxbc_to_spirv(vertex);
    EXPECT_FALSE(result.valid());
    EXPECT_NE(result.failure.find("require a pixel shader"),
              std::string::npos);

    DxbcContainer pixel = create_unary_vertex_shader(/*DERIV_RTY=*/12u);
    pixel.stage = ShaderStage::Fragment;
    pixel.input_signature[0].semantic_name = "TEXCOORD";
    pixel.output_signature[0].semantic_name = "SV_TARGET";
    const u32 ps_version = 0x00000040u;
    std::memcpy(pixel.bytecode.data(), &ps_version, sizeof(ps_version));
    u32 instruction = 0;
    std::memcpy(&instruction,
                pixel.bytecode.data() + 2u * sizeof(u32),
                sizeof(instruction));
    instruction |= 0x00002000u;
    std::memcpy(pixel.bytecode.data() + 2u * sizeof(u32),
                &instruction, sizeof(instruction));
    result = translate_dxbc_to_spirv(pixel);
    expect_saturated_destination(result);
}

TEST(DxbcToSpirvTest, SincosWritesBothResultsWithModifiersSwizzleAndMasks) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 3});
    container.output_signature.push_back(
        {"SV_Position", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"TEXCOORD", 0, 1, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00010040u); // vs_4_0
    append_word(11u);
    append_word(77u | (8u << 24)); // sincos
    append_word(0x00102052u); append_word(0u); // sin -> o0.xz
    append_word(0x001020A2u); append_word(1u); // cos -> o1.yw
    append_word(0x801011B6u); // extended v0.wzyx
    append_word(1u | (3u << 6)); // abs-neg
    append_word(0u);
    append_word(62u | (1u << 24));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_ext_inst_instruction(
        result.spirv, /*GLSL.std.450 Sin=*/13u));
    EXPECT_TRUE(spirv_contains_ext_inst_instruction(
        result.spirv, /*GLSL.std.450 Cos=*/14u));
    EXPECT_TRUE(spirv_contains_ext_inst_instruction(
        result.spirv, /*GLSL.std.450 FAbs=*/4u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpFNegate=*/127u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpVectorShuffle=*/79u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeConstruct=*/80u));
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    ASSERT_TRUE(compiled.valid());
    EXPECT_NE(std::string(compiled.msl_text()).find("sin("),
              std::string::npos);
    EXPECT_NE(std::string(compiled.msl_text()).find("cos("),
              std::string::npos);
}

TEST(DxbcToSpirvTest, SincosSupportsNullDestinationsAndSaturate) {
    for (const bool null_sin : {false, true}) {
        DxbcContainer container{};
        container.shader_model_major = 4;
        container.shader_model_minor = 0;
        container.stage = ShaderStage::Vertex;
        container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 3});
        container.output_signature.push_back(
            {"SV_Position", 0, 0, 0xFu, 3});
        const auto append_word = [&](u32 word) {
            const auto* bytes = reinterpret_cast<const u8*>(&word);
            container.bytecode.insert(
                container.bytecode.end(), bytes, bytes + 4);
        };
        const bool null_cos = !null_sin;
        append_word(0x00010040u); // vs_4_0
        append_word(9u);
        append_word(77u | (6u << 24)); // sincos
        if (null_sin) append_word(0x0000D000u);
        else { append_word(0x001020F2u); append_word(0u); }
        if (null_cos) append_word(0x0000D000u);
        else { append_word(0x001020F2u); append_word(0u); }
        append_word(0x001010E4u); append_word(0u);
        append_word(62u | (1u << 24));

        auto result = translate_dxbc_to_spirv(container);
        ASSERT_TRUE(result.valid()) << result.failure;
        EXPECT_EQ(spirv_contains_ext_inst_instruction(result.spirv, 13u),
                  !null_sin);
        EXPECT_EQ(spirv_contains_ext_inst_instruction(result.spirv, 14u),
                  !null_cos);

        u32 instruction = 0;
        std::memcpy(&instruction,
                    container.bytecode.data() + 2u * sizeof(u32),
                    sizeof(instruction));
        instruction |= 0x00002000u;
        std::memcpy(container.bytecode.data() + 2u * sizeof(u32),
                    &instruction, sizeof(instruction));
        result = translate_dxbc_to_spirv(container);
        expect_saturated_destination(result);
    }
}

TEST(DxbcToSpirvTest, FineDerivativeUsesDPdxFine) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Fragment;
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00000050u); // ps_5_0, same version word as the sample fixture
    append_word(11u);
    append_word(0x0100086Au);
    append_word(0x02000068u); append_word(1u); // dcl_temps 1
    append_word(0x0500007Bu); // deriv_rtx_fine, opcode 123, 5 dwords
    append_word(0x001000F2u); append_word(0u);
    append_word(0x00100E46u); append_word(0u);
    append_word(0x0100003Eu);
    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpDPdxFine=*/210u));
    EXPECT_TRUE(spirv_contains_capability(
        result.spirv, /*DerivativeControl=*/51u));
}
