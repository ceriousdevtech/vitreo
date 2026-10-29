// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_dxbc_to_spirv_bits_convert.cpp
/// @brief Bit manipulation, shifts, float and integer conversions.

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

// cs_5_0: bfrev r0.xyzw, r0.xyzw. Opcode 141 is D3D11_SB_OPCODE_BFREV,
// the entry between the measured BFI (140) and SWAPC (142). Operand
// tokens match the temp forms already used by the typed-UAV fixtures.
DxbcContainer create_synthetic_bfrev_compute_shader() {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Compute;
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00050050u); // cs_5_0
    append_word(15u);
    append_word(0x0100086Au); // dcl_globalFlags refactoringAllowed
    append_word(0x02000068u); append_word(1u); // dcl_temps 1
    append_word(0x0400009Bu); append_word(1u); append_word(1u);
    append_word(1u); // dcl_thread_group 1, 1, 1
    append_word(0x0500008Du); // bfrev, 5 dwords
    append_word(0x001000F2u); append_word(0u); // r0.xyzw dest
    append_word(0x00100E46u); append_word(0u); // r0.xyzw source
    append_word(0x0100003Eu); // ret
    return container;
}

} // namespace

// Microsoft Learn (bfi): dest = ((src2 << src1) & bitmask) | (src3 & ~bitmask)
// with bitmask = ((1 << src0) - 1) << src1, and both the width and the offset
// reduced to five bits.
TEST(DxbcToSpirvTest, TranslatesBitFieldInsertWithFiveBitWidthAndOffset) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 1});
    container.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00010050u); append_word(15u);
    append_word(140u | (11u << 24));                // bfi o0, ...
    append_word(0x001020F2u); append_word(0u);      // dst o0.xyzw
    append_word(0x00004001u); append_word(8u);      // width l(8)
    append_word(0x00004001u); append_word(4u);      // offset l(4)
    append_word(0x001010E4u); append_word(0u);      // insert v0
    append_word(0x001010E4u); append_word(0u);      // base v0
    append_word(0x0100003Eu);                       // ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    ASSERT_TRUE(compiled.valid());
    const std::string msl(compiled.msl_text());
    // Both operands reduced to five bits, and the mask built as ~(~0 << width)
    // shifted into place — never a subtraction that could underflow.
    EXPECT_NE(msl.find("31u"), std::string::npos) << msl;
    EXPECT_NE(msl.find("4294967295u"), std::string::npos) << msl;
}

TEST(DxbcToSpirvTest, TranslatesSignedAndUnsignedBitFieldExtract) {
    for (const u32 opcode : {138u, 139u}) {
        DxbcContainer container{};
        container.shader_model_major = 5;
        container.stage = ShaderStage::Vertex;
        container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 1});
        container.output_signature.push_back(
            {"SV_Position", 0, 0, 0xFu, 3});
        const auto append_word = [&](u32 word) {
            const auto* bytes = reinterpret_cast<const u8*>(&word);
            container.bytecode.insert(
                container.bytecode.end(), bytes, bytes + 4);
        };
        append_word(0x00010050u); append_word(12u);
        append_word(opcode | (9u << 24));           // ubfe/ibfe o0, ...
        append_word(0x001020F2u); append_word(0u);
        append_word(0x00004001u); append_word(8u);  // width
        append_word(0x00004001u); append_word(4u);  // offset
        append_word(0x001010E4u); append_word(0u);  // value
        append_word(0x0100003Eu);

        const auto result = translate_dxbc_to_spirv(container);
        ASSERT_TRUE(result.valid()) << result.failure;
        const auto compiled = compile_spirv_to_msl(
            result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
        EXPECT_TRUE(compiled.valid());
    }
}

TEST(DxbcToSpirvTest, TranslatesSm5BitScanAndPopulationCount) {
    for (const u32 opcode : {134u, 135u, 136u, 137u}) {
        DxbcContainer container{};
        container.shader_model_major = 5;
        container.stage = ShaderStage::Vertex;
        container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 1});
        container.output_signature.push_back(
            {"SV_Position", 0, 0, 0xFu, 3});
        const auto append_word = [&](u32 word) {
            const auto* bytes = reinterpret_cast<const u8*>(&word);
            container.bytecode.insert(
                container.bytecode.end(), bytes, bytes + 4);
        };
        append_word(0x00010050u); append_word(8u);
        append_word(opcode | (5u << 24));
        append_word(0x001020F2u); append_word(0u);
        append_word(0x001010E4u); append_word(0u);
        append_word(0x0100003Eu);

        const auto result = translate_dxbc_to_spirv(container);
        ASSERT_TRUE(result.valid()) << result.failure;
        const auto compiled = compile_spirv_to_msl(
            result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
        EXPECT_TRUE(compiled.valid());
    }
}

TEST(DxbcToSpirvTest, UshrUsesLogicalShiftAndLowFiveBitsOfScalarCount) {
    DxbcContainer container = create_binary_vertex_shader(/*USHR=*/85u);
    const u32 masked_dst = 0x00102052u; // o0.xz
    const u32 reversed_value = 0x001011B6u; // v0.wzyx
    const u32 scalar_z_count = 0x0010102Bu; // v1.z select-component
    std::memcpy(container.bytecode.data() + 3u * sizeof(u32),
                &masked_dst, sizeof(masked_dst));
    std::memcpy(container.bytecode.data() + 5u * sizeof(u32),
                &reversed_value, sizeof(reversed_value));
    std::memcpy(container.bytecode.data() + 7u * sizeof(u32),
                &scalar_z_count, sizeof(scalar_z_count));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpShiftRightLogical=*/194u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpBitwiseAnd=*/199u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpVectorShuffle=*/79u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeConstruct=*/80u));

    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, IshrSignExtendsWithLowFiveBitsOfScalarCount) {
    DxbcContainer container = create_binary_vertex_shader(/*ISHR=*/42u);
    const u32 masked_dst = 0x001020A2u; // o0.yw
    const u32 reversed_value = 0x001011B6u; // v0.wzyx
    const u32 scalar_y_count = 0x0010101Bu; // v1.y select-component
    std::memcpy(container.bytecode.data() + 3u * sizeof(u32),
                &masked_dst, sizeof(masked_dst));
    std::memcpy(container.bytecode.data() + 5u * sizeof(u32),
                &reversed_value, sizeof(reversed_value));
    std::memcpy(container.bytecode.data() + 7u * sizeof(u32),
                &scalar_y_count, sizeof(scalar_y_count));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpShiftRightArithmetic=*/195u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpBitwiseAnd=*/199u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpVectorShuffle=*/79u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeConstruct=*/80u));

    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, IshlDiscardsHighBitsWithLowFiveBitScalarCount) {
    DxbcContainer container = create_binary_vertex_shader(/*ISHL=*/41u);
    const u32 masked_dst = 0x00102052u; // o0.xz
    const u32 reversed_value = 0x001011B6u; // v0.wzyx
    const u32 scalar_w_count = 0x0010103Bu; // v1.w select-component
    std::memcpy(container.bytecode.data() + 3u * sizeof(u32),
                &masked_dst, sizeof(masked_dst));
    std::memcpy(container.bytecode.data() + 5u * sizeof(u32),
                &reversed_value, sizeof(reversed_value));
    std::memcpy(container.bytecode.data() + 7u * sizeof(u32),
                &scalar_w_count, sizeof(scalar_w_count));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpShiftLeftLogical=*/196u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpBitwiseAnd=*/199u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpVectorShuffle=*/79u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeConstruct=*/80u));

    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, FtoiTruncatesWithExactClampAndNanResults) {
    const auto result = translate_dxbc_to_spirv(
        create_unary_vertex_shader(/*FTOI=*/27u));
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpConvertFToS=*/110u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpIsNan=*/156u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpFOrdLessThan=*/184u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpFOrdGreaterThanEqual=*/190u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpSelect=*/169u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));

    // Exact scalar constants used by the per-lane boundary selects.
    EXPECT_NE(std::find(result.spirv.begin(), result.spirv.end(),
                        0xCF000000u), result.spirv.end()); // -2^31f
    EXPECT_NE(std::find(result.spirv.begin(), result.spirv.end(),
                        0x4F000000u), result.spirv.end()); // +2^31f
    EXPECT_NE(std::find(result.spirv.begin(), result.spirv.end(),
                        0x80000000u), result.spirv.end()); // INT_MIN
    EXPECT_NE(std::find(result.spirv.begin(), result.spirv.end(),
                        0x7FFFFFFFu), result.spirv.end()); // INT_MAX

    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, FtouTruncatesWithExactClampNanAndSourceModifiers) {
    DxbcContainer container = create_unary_vertex_shader(/*FTOU=*/28u);
    const u32 instruction = 28u | (6u << 24);
    const u32 extended_source = 0x801011B6u; // extended v0.wzyx
    const u32 abs_neg_modifier = 1u | (3u << 6); // -abs(src)
    const u32 total_words = 9u;
    std::memcpy(container.bytecode.data() + 1u * sizeof(u32),
                &total_words, sizeof(total_words));
    std::memcpy(container.bytecode.data() + 2u * sizeof(u32),
                &instruction, sizeof(instruction));
    std::memcpy(container.bytecode.data() + 5u * sizeof(u32),
                &extended_source, sizeof(extended_source));
    container.bytecode.insert(
        container.bytecode.begin() + 6u * sizeof(u32),
        reinterpret_cast<const u8*>(&abs_neg_modifier),
        reinterpret_cast<const u8*>(&abs_neg_modifier) +
            sizeof(abs_neg_modifier));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpConvertFToU=*/109u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpFNegate=*/127u));
    EXPECT_TRUE(spirv_contains_ext_inst_instruction(
        result.spirv, /*GLSL.std.450 FAbs=*/4u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpIsNan=*/156u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpFOrdLessThan=*/184u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpFOrdGreaterThanEqual=*/190u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpSelect=*/169u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
    EXPECT_NE(std::find(result.spirv.begin(), result.spirv.end(),
                        0x4F800000u), result.spirv.end()); // 2^32f
    EXPECT_NE(std::find(result.spirv.begin(), result.spirv.end(),
                        0xFFFFFFFFu), result.spirv.end()); // UINT_MAX

    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, ItofConvertsSignedBitsWithSwizzleAndWriteMask) {
    DxbcContainer container = create_unary_vertex_shader(/*ITOF=*/43u);
    const u32 masked_dst = 0x00102052u; // o0.xz
    const u32 reversed_src = 0x001011B6u; // v0.wzyx
    std::memcpy(container.bytecode.data() + 3u * sizeof(u32),
                &masked_dst, sizeof(masked_dst));
    std::memcpy(container.bytecode.data() + 5u * sizeof(u32),
                &reversed_src, sizeof(reversed_src));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpConvertSToF=*/111u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpVectorShuffle=*/79u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeExtract=*/81u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeConstruct=*/80u));

    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, UtofConvertsUnsignedBitsWithSwizzleAndWriteMask) {
    DxbcContainer container = create_unary_vertex_shader(/*UTOF=*/86u);
    const u32 masked_dst = 0x001020A2u; // o0.yw
    const u32 reversed_src = 0x001011B6u; // v0.wzyx
    std::memcpy(container.bytecode.data() + 3u * sizeof(u32),
                &masked_dst, sizeof(masked_dst));
    std::memcpy(container.bytecode.data() + 5u * sizeof(u32),
                &reversed_src, sizeof(reversed_src));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpConvertUToF=*/112u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpVectorShuffle=*/79u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeExtract=*/81u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeConstruct=*/80u));

    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, BfrevReversesEach32BitComponent) {
    const auto result = translate_dxbc_to_spirv(
        create_synthetic_bfrev_compute_shader());
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitReverse=*/204u));
}
