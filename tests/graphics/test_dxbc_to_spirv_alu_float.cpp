// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_dxbc_to_spirv_alu_float.cpp
/// @brief Float ALU, precision contract, source and destination modifiers.

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

// Microsoft Learn (dcl_resource_raw / ByteAddressBuffer.Load): a raw buffer
// has no element type and no format — it is addressed by byte offset and read
// as 32-bit words. Unlike a typed buffer there is nothing for the sampler to
// decode, so it stays a storage buffer rather than becoming a texture. UE5's
// RHI creates one during initialisation, so refusing the declaration is fatal.
// Learn (StructuredBuffer / ByteAddressBuffer, resource bounds checking):
// a load past the view returns 0 and a store is dropped. With the frontend's
// guard option the translator reads the view's word count from the push
// block's third member and selects 0 / branches around the store; without
// it (paths that bind no push block) the access is emitted bare. Measured
// against Windows with tools/oracle_probe/oob_read_cs.hlsl.
// D3D11 functional spec / HLSL `precise`: OpcodeToken0 bits 22:19 mark an
// instruction the driver may not fuse, reassociate or otherwise refactor.
// UE marks every clip-position op precise so its depth prepass and base
// pass (different shaders) produce bit-identical z for the GREATER_EQUAL
// depth test. SPIR-V carries that as NoContraction, which the MSL compiler
// lowers to optnone helpers: SPIRV-Cross's spvFAdd, and vitreoFMul for the
// multiply -- fma(l, r, -0.0), because spvFMul's fma(l, r, 0) turns a -0
// product into +0 (D3D11.3 3.1.3.1: an IEEE-754 multiply; -1 * 0 is -0).
TEST(DxbcToSpirvTest, PreciseInstructionsDecorateNoContraction) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3});
    const auto build = [&](bool precise) {
        container.bytecode.clear();
        const auto append_word = [&](u32 word) {
            const auto* bytes = reinterpret_cast<const u8*>(&word);
            container.bytecode.insert(container.bytecode.end(), bytes,
                                      bytes + 4);
        };
        append_word(0x00010050u);                       // vs_5_0
        append_word(9u);
        // mul o0.xyzw, v0.xyzw, v0.xyzw  (opcode 56, length 7)
        append_word(56u | (7u << 24) | (precise ? (0xFu << 19) : 0u));
        append_word(0x001020F2u); append_word(0u);
        append_word(0x001010E4u); append_word(0u);
        append_word(0x001010E4u); append_word(0u);
        append_word(62u | (1u << 24));                  // ret
        return translate_dxbc_to_spirv(container);
    };
    const auto plain = build(false);
    ASSERT_TRUE(plain.valid()) << plain.failure;
    EXPECT_FALSE(spirv_contains_decoration(plain.spirv,
                                           /*NoContraction=*/42u));
    const auto precise = build(true);
    ASSERT_TRUE(precise.valid()) << precise.failure;
    EXPECT_TRUE(spirv_contains_decoration(precise.spirv,
                                          /*NoContraction=*/42u));
    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        precise.spirv.data(), precise.spirv.size(), ShaderStage::Vertex,
        msl_options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl(compiled.msl_text());
    EXPECT_NE(msl.find("vitreoFMul(in."), std::string::npos) << msl;
    EXPECT_NE(msl.find("metal::fma(l, r, T(-0.0))"), std::string::npos)
        << msl;
}

// MslCompileOptions::precise_without_helpers (the D3D11 and D3D12 frontends,
// whose precise shaders compile in MTLMathModeSafe, which never reassociates):
// a precise op is the plain operator and the shader turns contraction off,
// instead of a call to an optnone helper. Measured on Apple silicon
// (scratchpad microbenchmark, 2026-10-09): Safe mode alone fuses a*b+c into
// an FMA, `#pragma clang fp contract(off)` stops it, bit-identical to the
// helpers, at 2.2x their speed. A shader with nothing precise is unchanged.
TEST(DxbcToSpirvTest, PreciseOpsArePlainOperatorsWithContractionOffOnRequest) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3});
    const auto build = [&](bool precise) {
        container.bytecode.clear();
        const auto append_word = [&](u32 word) {
            const auto* bytes = reinterpret_cast<const u8*>(&word);
            container.bytecode.insert(container.bytecode.end(), bytes,
                                      bytes + 4);
        };
        append_word(0x00010050u);                       // vs_5_0
        append_word(9u);
        // mul o0.xyzw, v0.xyzw, v0.xyzw  (opcode 56, length 7)
        append_word(56u | (7u << 24) | (precise ? (0xFu << 19) : 0u));
        append_word(0x001020F2u); append_word(0u);
        append_word(0x001010E4u); append_word(0u);
        append_word(0x001010E4u); append_word(0u);
        append_word(62u | (1u << 24));                  // ret
        return translate_dxbc_to_spirv(container);
    };
    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    msl_options.precise_without_helpers = true;
    const auto compile = [&](const DxbcToSpirvResult& spirv) {
        const auto compiled = compile_spirv_to_msl(
            spirv.spirv.data(), spirv.spirv.size(), ShaderStage::Vertex,
            msl_options);
        EXPECT_TRUE(compiled.valid());
        return std::string(compiled.msl_text());
    };
    const auto precise = build(true);
    ASSERT_TRUE(precise.valid()) << precise.failure;
    const std::string msl = compile(precise);
    EXPECT_NE(msl.find("#pragma clang fp contract(off)"), std::string::npos)
        << msl;
    EXPECT_EQ(msl.find("vitreoFMul(in."), std::string::npos) << msl;
    EXPECT_EQ(msl.find("spvFMul(in."), std::string::npos) << msl;
    EXPECT_NE(msl.find("in.POSITION0 * in.POSITION0"),
              std::string::npos) << msl;

    const auto plain = build(false);
    ASSERT_TRUE(plain.valid()) << plain.failure;
    EXPECT_EQ(compile(plain).find("#pragma clang fp contract(off)"),
              std::string::npos)
        << "nothing precise: the shader keeps contraction";
}

// The guard's size table has one entry per slot: t9's load is checked
// against entry 9 (not t1's, as a `slot & 7` index did) and u1 against the
// first UAV entry after all 128 t# entries.
// D3D11.3 22.3.2 / 22.19.2: refactoring is allowed only under
// dcl_globalFlags refactoringAllowed (fxc: 0x0100086a) and never on a
// precise instruction. Declarations reuse OpcodeToken0 bits 16-22 --
// dcl_resource texture2dms carries its sample count there -- so only
// instructions may count as precise.
TEST(DxbcToSpirvTest, MathContractReadsRefactoringFlagAndInstructionPrecise) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.stage = ShaderStage::Fragment;
    const auto build = [&](bool refactoring, bool precise_mul) {
        container.bytecode.clear();
        const auto append_word = [&](u32 word) {
            const auto* bytes = reinterpret_cast<const u8*>(&word);
            container.bytecode.insert(container.bytecode.end(), bytes,
                                      bytes + 4);
        };
        append_word(0x00000050u);  // ps_5_0
        append_word(15u);
        append_word(refactoring ? 0x0100086Au : 0x0100006Au);
        // dcl_resource_texture2dms(4) t0: sample count 4 in bits 16-22.
        append_word(88u | (6u << 11) | (4u << 16) | (4u << 24));
        append_word(0x00107000u); append_word(0u); append_word(0x00005555u);
        // mul r0.x, r0.x, r0.x (opcode 56), optionally precise(x).
        append_word(56u | (7u << 24) | (precise_mul ? (1u << 19) : 0u));
        append_word(0x00100012u); append_word(0u);
        append_word(0x0010000Au); append_word(0u);
        append_word(0x0010000Au); append_word(0u);
        append_word(62u | (1u << 24));  // ret
    };
    build(true, false);
    auto contract = dxbc_math_contract(container);
    EXPECT_TRUE(contract.refactoring_allowed);
    EXPECT_FALSE(contract.uses_precise)
        << "the dcl_resource sample count is not a precise mask";
    build(true, true);
    contract = dxbc_math_contract(container);
    EXPECT_TRUE(contract.refactoring_allowed);
    EXPECT_TRUE(contract.uses_precise);
    build(false, false);
    contract = dxbc_math_contract(container);
    EXPECT_FALSE(contract.refactoring_allowed);
    EXPECT_FALSE(contract.uses_precise);
}

// fxc /T cs_5_0 /O0 of (tools/oracle_probe/movc_neg.hlsl, compiled on
// Windows 2026-09-27):
//   float f = asfloat(b[0]); uint c = b[1];
//   float r = c ? f : -f;  sincos(-f, s, k);  sqrt(abs(f));  frac(-f);  (int)(-f);
// which fxc emits as
//   movc r0.x, r0.x, r0.y, -r0.y
//   sincos r0.x, r1.x, -r0.y
//   sqrt r0.x, |r0.y|
//   frc r0.x, -r0.y
//   ftoi r0.y, -r0.y
// Source operand modifiers on these instructions were silently dropped:
// UE's FFT bloom flips its twiddle angle with exactly that movc, so the
// inverse transform ran forward and the game's bloom was garbage. Measured
// against a real D3D11 runtime with tools/oracle_probe/d3d11_dxbc_compute_probe.c.
static const u8 cs_operand_modifiers[] = {
    0x44, 0x58, 0x42, 0x43, 0x84, 0x6f, 0x01, 0x62, 0xc2, 0xe2, 0x67, 0x19,
    0x09, 0xb7, 0x8c, 0x1d, 0xd4, 0xc3, 0x82, 0x60, 0x01, 0x00, 0x00, 0x00,
    0xd0, 0x03, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00,
    0xe4, 0x00, 0x00, 0x00, 0xf4, 0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00,
    0x34, 0x03, 0x00, 0x00, 0x52, 0x44, 0x45, 0x46, 0xa8, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x3c, 0x00, 0x00, 0x00, 0x00, 0x05, 0x53, 0x43, 0x00, 0x41, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x52, 0x44, 0x31, 0x31, 0x3c, 0x00, 0x00, 0x00,
    0x18, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00,
    0x24, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x7c, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x62, 0x00, 0x6f, 0x00, 0x4d, 0x69, 0x63, 0x72,
    0x6f, 0x73, 0x6f, 0x66, 0x74, 0x20, 0x28, 0x52, 0x29, 0x20, 0x48, 0x4c,
    0x53, 0x4c, 0x20, 0x53, 0x68, 0x61, 0x64, 0x65, 0x72, 0x20, 0x43, 0x6f,
    0x6d, 0x70, 0x69, 0x6c, 0x65, 0x72, 0x20, 0x31, 0x30, 0x2e, 0x31, 0x00,
    0x49, 0x53, 0x47, 0x4e, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x4f, 0x53, 0x47, 0x4e, 0x08, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x53, 0x48, 0x45, 0x58,
    0x28, 0x02, 0x00, 0x00, 0x50, 0x00, 0x05, 0x00, 0x8a, 0x00, 0x00, 0x00,
    0x6a, 0x08, 0x00, 0x01, 0x58, 0x08, 0x00, 0x04, 0x00, 0x70, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x44, 0x44, 0x00, 0x00, 0x9c, 0x08, 0x00, 0x04,
    0x00, 0xe0, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x44, 0x00, 0x00,
    0x68, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x9b, 0x00, 0x00, 0x04,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x2d, 0x00, 0x00, 0x8c, 0x42, 0x00, 0x00, 0x80, 0x03, 0x11, 0x11, 0x00,
    0x12, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x40, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x46, 0x7e, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2d, 0x00, 0x00, 0x8c, 0x42, 0x00, 0x00, 0x80, 0x03, 0x11, 0x11, 0x00,
    0x22, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x40, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x16, 0x7e, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x37, 0x00, 0x00, 0x0a, 0x12, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x10, 0x80, 0x41, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xa4, 0x00, 0x00, 0x0a, 0xf2, 0xe0, 0x11, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x06, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4d, 0x00, 0x00, 0x08,
    0x12, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x00, 0x10, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x10, 0x80, 0x41, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xa4, 0x00, 0x00, 0x0a, 0xf2, 0xe0, 0x11, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x06, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa4, 0x00, 0x00, 0x0a,
    0xf2, 0xe0, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x40, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x06, 0x00, 0x10, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x4b, 0x00, 0x00, 0x06, 0x12, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x1a, 0x00, 0x10, 0x80, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xa4, 0x00, 0x00, 0x0a, 0xf2, 0xe0, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x40, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x06, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x06, 0x12, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x10, 0x80, 0x41, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x06, 0x22, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x10, 0x80, 0x41, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xa4, 0x00, 0x00, 0x0a, 0xf2, 0xe0, 0x11, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x40, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x06, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa4, 0x00, 0x00, 0x0a,
    0xf2, 0xe0, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x40, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x00, 0x56, 0x05, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x3e, 0x00, 0x00, 0x01, 0x53, 0x54, 0x41, 0x54, 0x94, 0x00, 0x00, 0x00,
    0x0e, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x06, 0x00, 0x00, 0x00,
};

TEST(DxbcToSpirvTest, AppliesSourceModifiersOnMovcSincosSqrtFrcFtoi) {
    const auto container =
        parse_dxbc(cs_operand_modifiers, sizeof(cs_operand_modifiers));
    ASSERT_TRUE(container.valid());
    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    // OpFNegate (127) for the four negated float sources and GLSL.std.450
    // FAbs (4) for |r0.y|. The shader has no other float negation, so a
    // translation that ignores the modifiers emits none.
    constexpr u32 kOpFNegate = 127u;
    constexpr u32 kOpExtInst = 12u;
    unsigned negates = 0, fabs = 0;
    const auto& words = result.spirv;
    for (usize i = 5; i < words.size();) {
        const u32 op = words[i] & 0xFFFFu;
        const u32 len = words[i] >> 16;
        if (len == 0) break;
        if (op == kOpFNegate) ++negates;
        if (op == kOpExtInst && len >= 6 && words[i + 4] == 4u) ++fabs;
        i += len;
    }
    EXPECT_GE(negates, 4u);
    EXPECT_GE(fabs, 1u);
    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Compute,
        msl_options);
    ASSERT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, TranslatesHalfFloatConversions) {
    // Instruction words copied verbatim from a real fxc /Fo of
    //   float f = asfloat(sbu[0]); uint h = f32tof16(f); ... f16tof32(sbu[3])
    // so the opcode numbers AND the operand encoding are measured rather than
    // assembled from an assumption. See
    // tools/oracle_probe/dxbc_opcode_probe.expected.
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.stage = ShaderStage::Compute;
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00050050u); append_word(20u);
    append_word(0x0100086Au);
    append_word(0x02000068u); append_word(1u);      // dcl_temps 1
    append_word(0x0400009Bu); append_word(1u);
    append_word(1u); append_word(1u);               // dcl_thread_group 1,1,1
    // f32tof16 r0.x, r0.x
    append_word(0x05000082u); append_word(0x00100012u); append_word(0u);
    append_word(0x0010000Au); append_word(0u);
    // f16tof32 r0.y, r0.y
    append_word(0x05000083u); append_word(0x00100022u); append_word(0u);
    append_word(0x0010001Au); append_word(0u);
    append_word(0x0100003Eu);                       // ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Compute,
        msl_options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl(compiled.msl_text());
    // Both directions must go through a real 16-bit float, not a plain cast:
    // a truncation to float would keep compiling and be silently wrong.
    EXPECT_NE(msl.find("half"), std::string::npos) << msl;
}

TEST(DxbcToSpirvTest, TranslatesMaxWithDirect3DNaNSelection) {
    auto result = translate_dxbc_to_spirv(
        create_binary_vertex_shader(/*MAX=*/52u));
    ASSERT_TRUE(result.valid()) << result.failure;

    // Khronos SPIR-V unified opcodes. The explicit NaN tests preserve the
    // Shader Model 4 rule that a single NaN selects the other operand.
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpIsNan=*/156u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpFOrdGreaterThanEqual=*/190u));

    auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, TranslatesMinWithDirect3DNaNSelection) {
    auto result = translate_dxbc_to_spirv(
        create_binary_vertex_shader(/*MIN=*/51u));
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpIsNan=*/156u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpFOrdLessThan=*/184u));

    auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, NeUsesUnorderedNotEqualOpcode) {
    auto result = translate_dxbc_to_spirv(
        create_binary_vertex_shader(/*NE=*/57u));
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpFUnordNotEqual=*/183u));
}

TEST(DxbcToSpirvTest, EqUsesOrderedEqualOpcode) {
    auto result = translate_dxbc_to_spirv(
        create_binary_vertex_shader(/*EQ=*/24u));
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpFOrdEqual=*/180u));

    auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, LtUsesOrderedLessThanOpcode) {
    auto result = translate_dxbc_to_spirv(
        create_binary_vertex_shader(/*LT=*/49u));
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpFOrdLessThan=*/184u));

    auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, GeUsesOrderedGreaterThanEqualOpcode) {
    auto result = translate_dxbc_to_spirv(
        create_binary_vertex_shader(/*GE=*/29u));
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpFOrdGreaterThanEqual=*/190u));

    auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, TranslatesDp4DotProduct) {
    auto result = translate_dxbc_to_spirv(
        create_binary_vertex_shader(/*DP4=*/17u));
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpDot=*/148u));

    auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, TranslatesDiv) {
    auto result = translate_dxbc_to_spirv(
        create_binary_vertex_shader(/*DIV=*/14u));
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpFDiv=*/136u));

    auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

namespace {

/// `add o0.xyzw, v0.xyzw, [-]v0.xyzw`, optionally with the SATURATE
/// instruction modifier (OpcodeToken0 bit 13). Microsoft Learn
/// "Instruction Return Type and Modifiers" defines both: the extended
/// operand token carries the float NEG/ABS source modifier in bits 13:6,
/// and bit 13 of the opcode token clamps every written destination
/// component to [0,1].
DxbcContainer create_modified_add_vertex_shader(u32 source_modifier,
                                                bool saturate) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3});

    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    const bool extended = source_modifier != 0u;
    const u32 length = extended ? 8u : 7u;
    append_word(0x00010040u); // vs_4_0
    append_word(2u + length + 1u);
    append_word(/*ADD=*/0u | (length << 24) |
                (saturate ? 0x00002000u : 0u));
    append_word(0x001020F2u); append_word(0u);  // o0.xyzw
    append_word(0x001010E4u); append_word(0u);  // v0.xyzw
    if (extended) {
        append_word(0x801010E4u);               // v0.xyzw, extended
        append_word(1u | (source_modifier << 6));
        append_word(0u);
    } else {
        append_word(0x001010E4u); append_word(0u);
    }
    append_word(62u | (1u << 24)); // ret
    return container;
}

} // namespace

TEST(DxbcToSpirvTest, AddAppliesNegAndAbsSourceModifiers) {
    // Learn: NEG (1), ABS (2) and ABSNEG (3) are float source modifiers
    // applied after the swizzle. Dropping them silently computes the wrong
    // value — `add r, a, -b` becomes an addition.
    for (const auto& [modifier, expect_negate, expect_abs] :
         std::initializer_list<std::tuple<u32, bool, bool>>{
             {0u, false, false},
             {1u, true, false},
             {2u, false, true},
             {3u, true, true},
         }) {
        const auto container =
            create_modified_add_vertex_shader(modifier, /*saturate=*/false);
        const auto result = translate_dxbc_to_spirv(container);
        ASSERT_TRUE(result.valid()) << result.failure;
        EXPECT_EQ(spirv_contains_opcode(result.spirv, /*OpFNegate=*/127u),
                  expect_negate)
            << "modifier=" << modifier;
        EXPECT_EQ(spirv_contains_ext_inst_instruction(result.spirv,
                                                      /*GLSL FAbs=*/4u),
                  expect_abs)
            << "modifier=" << modifier;
        const auto compiled = compile_spirv_to_msl(
            result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
        ASSERT_TRUE(compiled.valid());
    }
}

TEST(DxbcToSpirvTest, AddAppliesDestinationSaturateModifier) {
    // Learn: the SATURATE instruction modifier clamps each written component
    // to [0,1]. A dropped saturate leaves an unclamped value that multiplies
    // through the rest of the shader.
    const auto unsaturated =
        create_modified_add_vertex_shader(/*modifier=*/0u, false);
    const auto plain = translate_dxbc_to_spirv(unsaturated);
    ASSERT_TRUE(plain.valid()) << plain.failure;
    EXPECT_FALSE(spirv_contains_opcode(plain.spirv, /*OpSelect=*/169u));

    const auto container =
        create_modified_add_vertex_shader(/*modifier=*/0u, true);
    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    // saturate(x) == min(max(x, 0), 1) with saturate(NaN) == 0, built from
    // ordered compares so NaN takes the 0 branch.
    EXPECT_TRUE(spirv_contains_opcode(result.spirv,
                                      /*OpFOrdGreaterThanEqual=*/190u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpFOrdLessThan=*/184u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpSelect=*/169u));
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    ASSERT_TRUE(compiled.valid());
}
