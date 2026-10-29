// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_dxbc_to_spirv_alu_integer.cpp
/// @brief Integer and logic ALU.

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

DxbcContainer create_imad_vertex_shader() {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 3});
    container.input_signature.push_back({"TEXCOORD", 0, 1, 0xFu, 3});
    container.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00010040u); // vs_4_0
    append_word(13u);
    append_word(35u | (10u << 24)); // imad
    append_word(0x001020A2u); append_word(0u); // o0.yw
    append_word(0x801011B6u); // extended v0.wzyx
    append_word(0x00000041u); // NEG
    append_word(0u);
    append_word(0x001010E4u); append_word(1u); // v1.xyzw
    append_word(0x001010E4u); append_word(0u); // v0.xyzw
    append_word(62u | (1u << 24));
    return container;
}

DxbcContainer create_imul_vertex_shader() {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 3});
    container.input_signature.push_back({"TEXCOORD", 0, 1, 0xFu, 3});
    container.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"TEXCOORD", 0, 1, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00010040u); // vs_4_0
    append_word(13u);
    append_word(38u | (10u << 24)); // imul
    append_word(0x00102052u); append_word(0u); // high -> o0.xz
    append_word(0x001020A2u); append_word(1u); // low -> o1.yw
    append_word(0x801011B6u); // extended v0.wzyx
    append_word(0x00000041u); // NEG
    append_word(0u);
    append_word(0x001010E4u); append_word(1u); // v1.xyzw
    append_word(62u | (1u << 24));
    return container;
}

DxbcContainer create_udiv_vertex_shader(bool null_quotient,
                                         bool null_remainder = false) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 3});
    container.input_signature.push_back({"TEXCOORD", 0, 1, 0xFu, 3});
    container.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"TEXCOORD", 0, 1, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00010040u); // vs_4_0
    const u32 operand_words = (null_quotient ? 1u : 2u) +
                              (null_remainder ? 1u : 2u) + 4u;
    append_word(4u + operand_words);
    append_word(78u | ((1u + operand_words) << 24)); // udiv
    if (null_quotient) {
        append_word(0x0000D000u); // NULL quotient
    } else {
        append_word(0x00102052u); append_word(0u); // quotient -> o0.xz
    }
    if (null_remainder) {
        append_word(0x0000D000u); // NULL remainder
    } else {
        append_word(0x001020A2u); append_word(1u); // remainder -> o1.yw
    }
    append_word(0x001011B6u); append_word(0u); // v0.wzyx dividend
    append_word(0x00101039u); append_word(1u); // v1.yzwx divisor
    append_word(62u | (1u << 24));
    return container;
}

DxbcContainer create_umul_vertex_shader(bool null_high, bool null_low) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 3});
    container.input_signature.push_back({"TEXCOORD", 0, 1, 0xFu, 3});
    container.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"TEXCOORD", 0, 1, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    const u32 operand_words = (null_high ? 1u : 2u) +
                              (null_low ? 1u : 2u) + 4u;
    append_word(0x00010040u); // vs_4_0
    append_word(4u + operand_words);
    append_word(81u | ((1u + operand_words) << 24)); // umul
    if (null_high) {
        append_word(0x0000D000u);
    } else {
        append_word(0x00102052u); append_word(0u); // high -> o0.xz
    }
    if (null_low) {
        append_word(0x0000D000u);
    } else {
        append_word(0x001020A2u); append_word(1u); // low -> o1.yw
    }
    append_word(0x001011B6u); append_word(0u); // v0.wzyx
    append_word(0x00101039u); append_word(1u); // v1.yzwx
    append_word(62u | (1u << 24));
    return container;
}

} // namespace

TEST(DxbcToSpirvTest, TranslatesAndBitwise) {
    auto result = translate_dxbc_to_spirv(
        create_binary_vertex_shader(/*AND=*/1u));
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpBitwiseAnd=*/199u));

    auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, XorUsesSwizzledBitsAndDestinationWriteMask) {
    DxbcContainer container = create_binary_vertex_shader(/*XOR=*/87u);
    const u32 masked_dst = 0x00102052u; // o0.xz
    const u32 reversed_a = 0x001011B6u; // v0.wzyx
    const u32 rotated_b = 0x00101039u; // v1.yzwx
    std::memcpy(container.bytecode.data() + 3u * sizeof(u32),
                &masked_dst, sizeof(masked_dst));
    std::memcpy(container.bytecode.data() + 5u * sizeof(u32),
                &reversed_a, sizeof(reversed_a));
    std::memcpy(container.bytecode.data() + 7u * sizeof(u32),
                &rotated_b, sizeof(rotated_b));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitwiseXor=*/198u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpVectorShuffle=*/79u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeConstruct=*/80u));

    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, NotUsesOnesComplementWithSwizzleAndWriteMask) {
    DxbcContainer container = create_unary_vertex_shader(/*NOT=*/59u);
    const u32 masked_dst = 0x001020A2u; // o0.yw
    const u32 reversed_source = 0x001011B6u; // v0.wzyx
    std::memcpy(container.bytecode.data() + 3u * sizeof(u32),
                &masked_dst, sizeof(masked_dst));
    std::memcpy(container.bytecode.data() + 5u * sizeof(u32),
                &reversed_source, sizeof(reversed_source));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpNot=*/200u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpVectorShuffle=*/79u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeConstruct=*/80u));

    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, IntegerComparisonsUseTypedPredicatesAndD3DBooleanBits) {
    struct ComparisonCase {
        u32 dxbc_opcode;
        u32 spirv_opcode;
    };
    const ComparisonCase cases[] = {
        {/*IEQ=*/32u, /*OpIEqual=*/170u},
        {/*INE=*/39u, /*OpINotEqual=*/171u},
        {/*ILT=*/34u, /*OpSLessThan=*/177u},
        {/*IGE=*/33u, /*OpSGreaterThanEqual=*/175u},
        {/*ULT=*/79u, /*OpULessThan=*/176u},
        {/*UGE=*/80u, /*OpUGreaterThanEqual=*/174u},
    };
    for (const auto& comparison : cases) {
        DxbcContainer container =
            create_binary_vertex_shader(comparison.dxbc_opcode);
        const u32 masked_dst = 0x00102052u; // o0.xz
        const u32 reversed_a = 0x001011B6u; // v0.wzyx
        const u32 rotated_b = 0x00101039u; // v1.yzwx
        std::memcpy(container.bytecode.data() + 3u * sizeof(u32),
                    &masked_dst, sizeof(masked_dst));
        std::memcpy(container.bytecode.data() + 5u * sizeof(u32),
                    &reversed_a, sizeof(reversed_a));
        std::memcpy(container.bytecode.data() + 7u * sizeof(u32),
                    &rotated_b, sizeof(rotated_b));

        const auto result = translate_dxbc_to_spirv(container);
        ASSERT_TRUE(result.valid()) << "DXBC opcode "
                                    << comparison.dxbc_opcode << ": "
                                    << result.failure;
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, comparison.spirv_opcode));
        EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpSelect=*/169u));
        EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
        EXPECT_NE(std::find(result.spirv.begin(), result.spirv.end(),
                            0xFFFFFFFFu), result.spirv.end());
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, /*OpVectorShuffle=*/79u));
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, /*OpCompositeConstruct=*/80u));

        const auto compiled = compile_spirv_to_msl(
            result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
        EXPECT_TRUE(compiled.valid()) << "DXBC opcode "
                                      << comparison.dxbc_opcode;
    }
}

TEST(DxbcToSpirvTest, IaddWrapsWithNegatedSwizzledSourceAndWriteMask) {
    DxbcContainer container = create_binary_vertex_shader(/*IADD=*/30u);
    const u32 instruction = 30u | (8u << 24);
    const u32 masked_dst = 0x001020A2u; // o0.yw
    const u32 negated_reversed_src =
        0x801011B6u; // extended v0.wzyx
    const u32 modifier_neg = 0x00000041u; // type 1, NEG in bits 13:6
    const u32 total_words = 11u;
    std::memcpy(container.bytecode.data() + 1u * sizeof(u32),
                &total_words, sizeof(total_words));
    std::memcpy(container.bytecode.data() + 2u * sizeof(u32),
                &instruction, sizeof(instruction));
    std::memcpy(container.bytecode.data() + 3u * sizeof(u32),
                &masked_dst, sizeof(masked_dst));
    std::memcpy(container.bytecode.data() + 5u * sizeof(u32),
                &negated_reversed_src, sizeof(negated_reversed_src));
    container.bytecode.insert(
        container.bytecode.begin() + 6u * sizeof(u32),
        reinterpret_cast<const u8*>(&modifier_neg),
        reinterpret_cast<const u8*>(&modifier_neg) + sizeof(modifier_neg));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpSNegate=*/126u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpIAdd=*/128u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpVectorShuffle=*/79u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeConstruct=*/80u));

    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, ImadKeepsLowProductBitsThenAddsWithNegateAndMask) {
    const auto result = translate_dxbc_to_spirv(create_imad_vertex_shader());
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpIMul=*/132u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpIAdd=*/128u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpSNegate=*/126u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpVectorShuffle=*/79u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeConstruct=*/80u));

    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, InegUsesTwosComplementWithSwizzleAndWriteMask) {
    DxbcContainer container = create_unary_vertex_shader(/*INEG=*/40u);
    const u32 masked_dst = 0x001020A2u; // o0.yw
    const u32 reversed_source = 0x001011B6u; // v0.wzyx
    std::memcpy(container.bytecode.data() + 3u * sizeof(u32),
                &masked_dst, sizeof(masked_dst));
    std::memcpy(container.bytecode.data() + 5u * sizeof(u32),
                &reversed_source, sizeof(reversed_source));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpSNegate=*/126u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpVectorShuffle=*/79u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeConstruct=*/80u));

    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, ImulProducesMaskedSignedHighAndLowProducts) {
    const auto result = translate_dxbc_to_spirv(create_imul_vertex_shader());
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpSMulExtended=*/152u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpSNegate=*/126u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeExtract=*/81u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeConstruct=*/80u));

    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, UdivProducesQuotientRemainderAndExactZeroResults) {
    for (const auto& null_destinations :
         {std::pair{false, false}, std::pair{true, false},
          std::pair{false, true}}) {
        const auto result = translate_dxbc_to_spirv(
            create_udiv_vertex_shader(null_destinations.first,
                                      null_destinations.second));
        ASSERT_TRUE(result.valid()) << result.failure;
        EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpUDiv=*/134u));
        EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpUMod=*/137u));
        EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpIEqual=*/170u));
        EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpSelect=*/169u));
        EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
        EXPECT_NE(std::find(result.spirv.begin(), result.spirv.end(),
                            0xFFFFFFFFu), result.spirv.end());
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, /*OpVectorShuffle=*/79u));
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, /*OpCompositeConstruct=*/80u));

        const auto compiled = compile_spirv_to_msl(
            result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
        EXPECT_TRUE(compiled.valid());
    }
}

TEST(DxbcToSpirvTest, UmulProducesMaskedUnsignedHighAndLowProducts) {
    for (const auto& null_destinations :
         {std::pair{false, false}, std::pair{true, false},
          std::pair{false, true}}) {
        const auto result = translate_dxbc_to_spirv(create_umul_vertex_shader(
            null_destinations.first, null_destinations.second));
        ASSERT_TRUE(result.valid()) << result.failure;
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, /*OpUMulExtended=*/151u));
        EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, /*OpCompositeExtract=*/81u));
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, /*OpCompositeConstruct=*/80u));
        const auto compiled = compile_spirv_to_msl(
            result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
        EXPECT_TRUE(compiled.valid());
    }
}

TEST(DxbcToSpirvTest, UmadKeepsLowUnsignedProductThenWrapsAdd) {
    DxbcContainer container = create_binary_vertex_shader(/*UMAD=*/82u);
    const u32 instruction = 82u | (10u << 24);
    const u32 masked_dst = 0x001020A2u; // o0.yw
    const u32 rotated_c = 0x00101039u; // v1.yzwx
    const u32 total_words = 13u;
    std::memcpy(container.bytecode.data() + sizeof(u32),
                &total_words, sizeof(total_words));
    std::memcpy(container.bytecode.data() + 2u * sizeof(u32),
                &instruction, sizeof(instruction));
    std::memcpy(container.bytecode.data() + 3u * sizeof(u32),
                &masked_dst, sizeof(masked_dst));
    const auto* c_bytes = reinterpret_cast<const u8*>(&rotated_c);
    const u32 c_index = 1u;
    container.bytecode.insert(container.bytecode.begin() + 9u * sizeof(u32),
                              c_bytes, c_bytes + sizeof(rotated_c));
    const auto* index_bytes = reinterpret_cast<const u8*>(&c_index);
    container.bytecode.insert(container.bytecode.begin() + 10u * sizeof(u32),
                              index_bytes, index_bytes + sizeof(c_index));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpIMul=*/132u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpIAdd=*/128u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpCompositeConstruct=*/80u));
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, UnsignedMinMaxUseSwizzlesMasksAndUnsignedOrdering) {
    for (const u32 opcode : {/*UMAX=*/83u, /*UMIN=*/84u}) {
        DxbcContainer container = create_binary_vertex_shader(opcode);
        const u32 masked_dst = 0x00102052u; // o0.xz
        const u32 reversed_a = 0x001011B6u; // v0.wzyx
        const u32 rotated_b = 0x00101039u; // v1.yzwx
        std::memcpy(container.bytecode.data() + 3u * sizeof(u32),
                    &masked_dst, sizeof(masked_dst));
        std::memcpy(container.bytecode.data() + 5u * sizeof(u32),
                    &reversed_a, sizeof(reversed_a));
        std::memcpy(container.bytecode.data() + 7u * sizeof(u32),
                    &rotated_b, sizeof(rotated_b));

        const auto result = translate_dxbc_to_spirv(container);
        ASSERT_TRUE(result.valid()) << result.failure;
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, /*OpULessThan=*/176u));
        EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpSelect=*/169u));
        EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, /*OpVectorShuffle=*/79u));
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, /*OpCompositeConstruct=*/80u));
        const auto compiled = compile_spirv_to_msl(
            result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
        EXPECT_TRUE(compiled.valid());
    }
}

TEST(DxbcToSpirvTest, SignedMinMaxUseNegateSwizzlesMasksAndSignedOrdering) {
    for (const u32 opcode : {/*IMAX=*/36u, /*IMIN=*/37u}) {
        DxbcContainer container = create_binary_vertex_shader(opcode);
        const u32 total_words = 11u;
        const u32 instruction = opcode | (8u << 24);
        const u32 masked_dst = 0x001020A2u; // o0.yw
        const u32 extended_reversed_a = 0x801011B6u; // extended v0.wzyx
        const u32 negate = 0x00000041u;
        const u32 rotated_b = 0x00101039u; // v1.yzwx
        std::memcpy(container.bytecode.data() + sizeof(u32),
                    &total_words, sizeof(total_words));
        std::memcpy(container.bytecode.data() + 2u * sizeof(u32),
                    &instruction, sizeof(instruction));
        std::memcpy(container.bytecode.data() + 3u * sizeof(u32),
                    &masked_dst, sizeof(masked_dst));
        std::memcpy(container.bytecode.data() + 5u * sizeof(u32),
                    &extended_reversed_a, sizeof(extended_reversed_a));
        const auto* modifier_bytes = reinterpret_cast<const u8*>(&negate);
        container.bytecode.insert(container.bytecode.begin() + 6u * sizeof(u32),
                                  modifier_bytes,
                                  modifier_bytes + sizeof(negate));
        std::memcpy(container.bytecode.data() + 8u * sizeof(u32),
                    &rotated_b, sizeof(rotated_b));

        const auto result = translate_dxbc_to_spirv(container);
        ASSERT_TRUE(result.valid()) << result.failure;
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, /*OpSLessThan=*/177u));
        EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpSelect=*/169u));
        EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpSNegate=*/126u));
        EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, /*OpVectorShuffle=*/79u));
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, /*OpCompositeConstruct=*/80u));
        const auto compiled = compile_spirv_to_msl(
            result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
        EXPECT_TRUE(compiled.valid());
    }
}
