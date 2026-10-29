// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_dxbc_to_spirv_control_flow.cpp
/// @brief Control flow, discard, loop iteration guard.

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

// A `ret` inside an if is an early-out with instructions after it. Stopping
// translation there abandoned the rest of the program and left the if stack
// unbalanced, which surfaced as the misleading "IF: missing ENDIF".
TEST(DxbcToSpirvTest, EarlyReturnInsideAnIfKeepsTranslatingTheRestOfTheShader) {
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
    append_word(0x00010050u); append_word(14u);
    append_word(31u | (3u << 24) | 0x00040000u);    // if_nz v0.x
    append_word(0x0010100Au); append_word(0u);
    append_word(0x0100003Eu);                       // ret  (early out)
    append_word(21u | (1u << 24));                  // endif
    // Everything below here was previously never translated.
    append_word(54u | (5u << 24));                  // mov o0, v0
    append_word(0x001020F2u); append_word(0u);
    append_word(0x001010E4u); append_word(0u);
    append_word(0x0100003Eu);                       // ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    ASSERT_TRUE(compiled.valid());
    const std::string msl(compiled.msl_text());
    // The early return survives, and so does the code that follows the endif.
    EXPECT_NE(msl.find("return"), std::string::npos) << msl;
    EXPECT_NE(msl.find("POSITION"), std::string::npos) << msl;
}

TEST(DxbcToSpirvTest, PixelDiscardUsesExactZeroAndNonzeroTests) {
    for (const u32 model : {4u, 5u}) {
        for (const bool nonzero : {false, true}) {
            DxbcContainer container{};
            container.shader_model_major = model;
            container.shader_model_minor = 0;
            container.stage = ShaderStage::Fragment;
            container.input_signature.push_back({"TEXCOORD", 0, 0, 0xFu, 3});
            container.output_signature.push_back({"SV_TARGET", 0, 0, 0xFu, 3});
            const auto append_word = [&](u32 word) {
                const auto* bytes = reinterpret_cast<const u8*>(&word);
                container.bytecode.insert(
                    container.bytecode.end(), bytes, bytes + sizeof(word));
            };
            append_word(model << 4); // ps_4_0 / ps_5_0
            append_word(11u);
            append_word(13u | (3u << 24) |
                        (nonzero ? 0x00040000u : 0u)); // discard_nz/z
            append_word(0x0010102Au); append_word(0u); // v0.z selected
            append_word(54u | (5u << 24)); // mov o0, v0
            append_word(0x001020F2u); append_word(0u);
            append_word(0x001010E4u); append_word(0u);
            append_word(62u | (1u << 24));

            const auto result = translate_dxbc_to_spirv(container);
            ASSERT_TRUE(result.valid()) << result.failure;
            EXPECT_TRUE(spirv_contains_opcode(
                result.spirv, nonzero ? /*OpINotEqual=*/171u
                                      : /*OpIEqual=*/170u));
            EXPECT_TRUE(spirv_contains_opcode(
                result.spirv, /*OpSelectionMerge=*/247u));
            EXPECT_TRUE(spirv_contains_opcode(
                result.spirv, /*OpBranchConditional=*/250u));
            EXPECT_FALSE(spirv_contains_opcode(result.spirv, /*OpKill=*/252u));
            EXPECT_TRUE(spirv_contains_opcode(
                result.spirv, /*OpDemoteToHelperInvocationEXT=*/5380u));
            const auto compiled = compile_spirv_to_msl(
                result.spirv.data(), result.spirv.size(), ShaderStage::Fragment);
            ASSERT_TRUE(compiled.valid());
            MslCompileOptions old_msl{};
            old_msl.msl_version = 20200u;
            EXPECT_FALSE(compile_spirv_to_msl(result.spirv.data(),
                result.spirv.size(), ShaderStage::Fragment, old_msl).valid());
            EXPECT_NE(std::string(compiled.msl_text()).find("discard_fragment"),
                      std::string::npos);
        }
    }
}

TEST(DxbcToSpirvTest, IfElseUsesBitLevelScalarTestAndStructuredBranches) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Fragment;
    container.input_signature.push_back({"TEXCOORD", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"SV_TARGET", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00000040u); // ps_4_0
    append_word(24u);
    append_word(31u | 0x00040000u | (3u << 24)); // if_nz v0.x
    append_word(0x0010100Au); append_word(0u);
    append_word(54u | (8u << 24)); // mov o0, l(0.25, 0.75, 0.5, 1)
    append_word(0x001020F2u); append_word(0u);
    append_word(0x00004002u);
    append_word(0x3e800000u); append_word(0x3f400000u);
    append_word(0x3f000000u); append_word(0x3f800000u);
    append_word(18u | (1u << 24)); // else
    append_word(54u | (8u << 24)); // mov o0, l(0.75, 0.25, 0.5, 1)
    append_word(0x001020F2u); append_word(0u);
    append_word(0x00004002u);
    append_word(0x3f400000u); append_word(0x3e800000u);
    append_word(0x3f000000u); append_word(0x3f800000u);
    append_word(21u | (1u << 24)); // endif
    append_word(62u | (1u << 24)); // ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpSelectionMerge=*/247u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpBranchConditional=*/250u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpINotEqual=*/171u));

    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("if ("), std::string::npos);
    EXPECT_NE(msl.find("else"), std::string::npos);
}

TEST(DxbcToSpirvTest, IfWithoutElseAndMalformedStructureAreHandledHonestly) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"TEXCOORD", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"SV_POSITION", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00010040u); // vs_4_0
    append_word(15u);
    append_word(31u | (3u << 24)); // if_z v0.y
    append_word(0x0010101Au); append_word(0u);
    append_word(54u | (8u << 24)); // mov o0, l(0, 0, 0, 1)
    append_word(0x001020F2u); append_word(0u);
    append_word(0x00004002u);
    append_word(0u); append_word(0u); append_word(0u);
    append_word(0x3f800000u);
    append_word(21u | (1u << 24)); // endif
    append_word(62u | (1u << 24));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpIEqual=*/170u));

    DxbcContainer missing_endif = container;
    missing_endif.bytecode.resize(missing_endif.bytecode.size() - 2u * 4u);
    const u32 shortened = 13u;
    std::memcpy(missing_endif.bytecode.data() + sizeof(u32),
                &shortened, sizeof(shortened));
    const auto malformed = translate_dxbc_to_spirv(missing_endif);
    EXPECT_FALSE(malformed.valid());
    EXPECT_NE(malformed.failure.find("missing ENDIF"), std::string::npos);
}

TEST(DxbcToSpirvTest, RetcConditionallyReturnsOnRawLaneBits) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Fragment;
    container.input_signature.push_back({"TEXCOORD", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"SV_TARGET", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00000040u); // ps_4_0
    append_word(22u);
    append_word(54u | (8u << 24)); // mov o0, green
    append_word(0x001020F2u); append_word(0u);
    append_word(0x00004002u);
    append_word(0u); append_word(0x3f800000u);
    append_word(0u); append_word(0x3f800000u);
    append_word(63u | 0x00040000u | (3u << 24)); // retc_nz v0.x
    append_word(0x0010100Au); append_word(0u);
    append_word(54u | (8u << 24)); // mov o0, red
    append_word(0x001020F2u); append_word(0u);
    append_word(0x00004002u);
    append_word(0x3f800000u); append_word(0u);
    append_word(0u); append_word(0x3f800000u);
    append_word(62u | (1u << 24));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpSelectionMerge=*/247u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpBranchConditional=*/250u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpReturn=*/253u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpINotEqual=*/171u));

    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment);
    ASSERT_TRUE(compiled.valid());
    EXPECT_NE(std::string(compiled.msl_text()).find("return"),
              std::string::npos);

    DxbcContainer return_on_zero = container;
    const u32 retc_z = 63u | (3u << 24);
    std::memcpy(return_on_zero.bytecode.data() + 10u * sizeof(u32),
                &retc_z, sizeof(retc_z));
    const auto zero_result = translate_dxbc_to_spirv(return_on_zero);
    ASSERT_TRUE(zero_result.valid()) << zero_result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        zero_result.spirv, /*OpIEqual=*/170u));

    DxbcContainer final_retc = container;
    final_retc.bytecode.erase(final_retc.bytecode.begin() + 13u * 4u,
                              final_retc.bytecode.end());
    const u32 shortened = 13u;
    std::memcpy(final_retc.bytecode.data() + sizeof(u32),
                &shortened, sizeof(shortened));
    const auto malformed = translate_dxbc_to_spirv(final_retc);
    EXPECT_FALSE(malformed.valid());
    EXPECT_NE(malformed.failure.find("final instruction"), std::string::npos);
}

TEST(DxbcToSpirvTest, LoopConditionalBreakAndContinueUseStructuredTargets) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Fragment;
    container.input_signature.push_back({"TEXCOORD", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"SV_TARGET", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00000040u); // ps_4_0
    append_word(11u);
    append_word(48u | (1u << 24)); // loop
    append_word(8u | (3u << 24)); // continuec_z v0.x
    append_word(0x0010100Au); append_word(0u);
    append_word(3u | 0x00040000u | (3u << 24)); // breakc_nz v0.y
    append_word(0x0010101Au); append_word(0u);
    append_word(22u | (1u << 24)); // endloop
    append_word(62u | (1u << 24));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpLoopMerge=*/246u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpBranchConditional=*/250u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpIEqual=*/170u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpINotEqual=*/171u));
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("for (;;"), std::string::npos);
    EXPECT_NE(msl.find("continue"), std::string::npos);
    EXPECT_NE(msl.find("break"), std::string::npos);
}

TEST(DxbcToSpirvTest, UnconditionalLoopTransfersAndMalformedNestingAreHonest) {
    const auto make_shader = [](u32 transfer_opcode) {
        DxbcContainer container{};
        container.shader_model_major = 4;
        container.shader_model_minor = 0;
        container.stage = ShaderStage::Vertex;
        const std::array<u32, 6> words = {
            0x00010040u, 6u,
            48u | (1u << 24),
            transfer_opcode | (1u << 24),
            22u | (1u << 24),
            62u | (1u << 24),
        };
        container.bytecode.resize(sizeof(words));
        std::memcpy(container.bytecode.data(), words.data(), sizeof(words));
        return container;
    };

    for (u32 opcode : {2u, 7u}) { // break, continue
        const auto result = translate_dxbc_to_spirv(make_shader(opcode));
        ASSERT_TRUE(result.valid()) << result.failure;
        EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpLoopMerge=*/246u));
    }

    DxbcContainer unmatched = make_shader(2u);
    const std::array<u32, 3> bad_words = {
        0x00010040u, 3u, 2u | (1u << 24),
    };
    unmatched.bytecode.resize(sizeof(bad_words));
    std::memcpy(unmatched.bytecode.data(), bad_words.data(),
                sizeof(bad_words));
    const auto bad_break = translate_dxbc_to_spirv(unmatched);
    EXPECT_FALSE(bad_break.valid());
    EXPECT_NE(bad_break.failure.find("not inside"), std::string::npos);
}

TEST(DxbcToSpirvTest, SwitchCasesDefaultAndBreakUseStructuredDispatch) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Fragment;
    container.input_signature.push_back({"TEXCOORD", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"SV_TARGET", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00000040u); append_word(32u);
    append_word(76u | (3u << 24)); // switch v0.x
    append_word(0x0010100Au); append_word(0u);
    append_word(6u | (3u << 24)); // case 1
    append_word(0x00004001u); append_word(1u);
    append_word(54u | (8u << 24));
    append_word(0x001020F2u); append_word(0u); append_word(0x00004002u);
    append_word(0x3f800000u); append_word(0u); append_word(0u);
    append_word(0x3f800000u);
    append_word(2u | (1u << 24)); // break
    append_word(6u | (3u << 24)); // case 2
    append_word(0x00004001u); append_word(2u);
    append_word(10u | (1u << 24)); // default shares case 2 body
    append_word(54u | (8u << 24));
    append_word(0x001020F2u); append_word(0u); append_word(0x00004002u);
    append_word(0u); append_word(0x3f800000u); append_word(0u);
    append_word(0x3f800000u);
    append_word(2u | (1u << 24));
    append_word(23u | (1u << 24)); // endswitch
    append_word(62u | (1u << 24));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpSwitch=*/251u));
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("switch ("), std::string::npos);
    EXPECT_NE(msl.find("case 1"), std::string::npos);
    EXPECT_NE(msl.find("default:"), std::string::npos);

    DxbcContainer duplicate = container;
    const u32 duplicate_value = 1u;
    std::memcpy(duplicate.bytecode.data() + 19u * sizeof(u32),
                &duplicate_value, sizeof(duplicate_value));
    const auto duplicate_result = translate_dxbc_to_spirv(duplicate);
    EXPECT_FALSE(duplicate_result.valid());
    EXPECT_NE(duplicate_result.failure.find("duplicate immediate"),
              std::string::npos);
}

TEST(DxbcToSpirvTest, CallInlinesLabelBodyWithSharedRegisterVisibility) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Fragment;
    container.output_signature.push_back({"SV_TARGET", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00000040u); append_word(26u);
    append_word(54u | (8u << 24)); // mov o0, red
    append_word(0x001020F2u); append_word(0u); append_word(0x00004002u);
    append_word(0x3f800000u); append_word(0u); append_word(0u);
    append_word(0x3f800000u);
    append_word(4u | (3u << 24)); // call l0
    append_word(0x0010A000u); append_word(0u);
    append_word(62u | (1u << 24)); // main ret
    append_word(44u | (3u << 24)); // label l0
    append_word(0x0010A000u); append_word(0u);
    append_word(54u | (8u << 24)); // mov o0, green
    append_word(0x001020F2u); append_word(0u); append_word(0x00004002u);
    append_word(0u); append_word(0x3f800000u); append_word(0u);
    append_word(0x3f800000u);
    append_word(62u | (1u << 24)); // subroutine ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment);
    ASSERT_TRUE(compiled.valid());
    EXPECT_NE(std::string(compiled.msl_text()).find("float4(0.0, 1.0"),
              std::string::npos);

    DxbcContainer recursive = container;
    const u32 recursive_call = 4u | (3u << 24);
    const u32 label_operand = 0x0010A000u;
    const u32 zero_index = 0u;
    const u32 ret = 62u | (1u << 24);
    const u32 recursive_length = 21u;
    std::memcpy(recursive.bytecode.data() + 17u * sizeof(u32),
                &recursive_call, sizeof(recursive_call));
    std::memcpy(recursive.bytecode.data() + 18u * sizeof(u32),
                &label_operand, sizeof(label_operand));
    std::memcpy(recursive.bytecode.data() + 19u * sizeof(u32),
                &zero_index, sizeof(zero_index));
    std::memcpy(recursive.bytecode.data() + 20u * sizeof(u32),
                &ret, sizeof(ret));
    std::memcpy(recursive.bytecode.data() + sizeof(u32),
                &recursive_length, sizeof(recursive_length));
    recursive.bytecode.resize(recursive_length * sizeof(u32));
    const auto recursion = translate_dxbc_to_spirv(recursive);
    EXPECT_FALSE(recursion.valid());
    EXPECT_NE(recursion.failure.find("recursion"), std::string::npos);
}

TEST(DxbcToSpirvTest, EveryLoopCarriesAnIterationGuard) {
    // TDR stand-in (shader_loop_iteration_cap): a loop with no reachable
    // break must still end. This checks the generated MSL without running it.
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.stage = ShaderStage::Compute;
    const std::vector<u32> words = {
        0x00050050u, 0u, 0x0100086Au,
        0x02000068u, 1u,                           // dcl_temps 1
        0x0400009Bu, 1u, 1u, 1u,                   // dcl_thread_group 1,1,1
        0x05000036u, 0x00100012u, 0u, 0x00004001u, 0u,  // mov r0.x, l(0)
        0x01000030u,                               // loop
        0x0700001Eu, 0x00100012u, 0u, 0x0010000Au, 0u,
            0x00004001u, 1u,                       //   iadd r0.x, r0.x, l(1)
        0x01000016u,                               // endloop (no break at all)
        0x0100003Eu,                               // ret
    };
    for (u32 word : words) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    }
    const u32 total = static_cast<u32>(words.size());
    std::memcpy(container.bytecode.data() + 4, &total, sizeof(total));
    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpUGreaterThan=*/172u));
    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Compute,
        options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    const std::string cap = std::to_string(shader_loop_iteration_cap()) + "u";
    EXPECT_NE(msl.find(cap), std::string::npos)
        << "the guard compares against the cap\n" << msl;
    EXPECT_NE(msl.find("break;"), std::string::npos)
        << "an unbreakable loop now has a break\n" << msl;
}
