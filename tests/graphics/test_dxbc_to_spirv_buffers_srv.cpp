// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_dxbc_to_spirv_buffers_srv.cpp
/// @brief SRV and constant-buffer loads, bounds guards.

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

// Microsoft Learn (dcl_resource_structured / ld_structured): a t# register can
// name a StructuredBuffer instead of a texture. The element type is opaque to
// the ISA — a load addresses it as element index * stride + byte offset — so
// the translator models it as a storage buffer of 32-bit words. Unity's
// instancing vertex shaders use this, and rejecting the declaration failed the
// shader outright.
TEST(DxbcToSpirvTest, TranslatesStructuredBufferLoadAsAStorageBuffer) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3});

    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00010050u);            // vs_5_0
    append_word(16u);                    // total length in DWORDs
    append_word(162u | (4u << 24));      // dcl_resource_structured
    append_word(0x00107000u);            // t0
    append_word(0u);
    append_word(16u);                    // structure byte stride
    append_word(167u | (9u << 24));      // ld_structured
    append_word(0x001020F2u); append_word(0u);  // dst o0.xyzw
    append_word(0x001010E4u); append_word(0u);  // element index from v0
    append_word(0x001010E4u); append_word(0u);  // byte offset from v0
    append_word(0x001070E4u); append_word(0u);  // t0.xyzw
    append_word(62u | (1u << 24));       // ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    // Match how the D3D11 pipeline compiles: discrete bindings, not argument
    // buffers, which is what makes the explicit buffer index observable.
    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex,
        msl_options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl(compiled.msl_text());
    // A structured SRV must reach Metal as a read-only device buffer, at the
    // index the backend binds t# structured buffers to (2 + 4 cbuffer slots).
    EXPECT_NE(msl.find("buffer(6)"), std::string::npos) << msl;
    EXPECT_NE(msl.find("const device"), std::string::npos) << msl;
}

// Microsoft Learn (dcl_resource with dimension BUFFER / Buffer.Load): a t#
// register can also name a *typed* buffer. Unlike a StructuredBuffer its
// element size and component layout live in the view's DXGI format, which the
// shader cannot see — the declaration records only the scalar return type. It
// is therefore a sampled image with Dim=Buffer, not a storage buffer, so the
// hardware performs the format decode. Metal expresses that as
// `texture_buffer<T>`, addressed by a single element index with no mip.
TEST(DxbcToSpirvTest, TranslatesTypedBufferLoadAsATextureBuffer) {
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
    append_word(0x00010050u);                        // vs_5_0
    append_word(14u);                                // total length in DWORDs
    // dcl_resource_buffer: dimension BUFFER (1) in opcode-token bits 11..15.
    append_word(88u | (1u << 11) | (4u << 24));
    append_word(0x00107000u); append_word(0u);       // t0
    append_word(0x4444u);                            // return type UINT x4
    append_word(45u | (7u << 24));                   // ld
    append_word(0x001020F2u); append_word(0u);       // dst o0.xyzw
    append_word(0x001010E4u); append_word(0u);       // address from v0
    append_word(0x001070E4u); append_word(0u);       // t0.xyzw
    append_word(62u | (1u << 24));                   // ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex,
        msl_options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl(compiled.msl_text());
    // The uint return type must survive: a Buffer<uint> read is not a float
    // read of the same bits.
    EXPECT_NE(msl.find("texture_buffer<uint>"), std::string::npos) << msl;
    // It occupies the t# texture slot, like every other shader resource.
    EXPECT_NE(msl.find("[[texture(0)]]"), std::string::npos) << msl;
    EXPECT_NE(msl.find(".read("), std::string::npos) << msl;
    // Not a 2D texture, and not a storage buffer: it has no mip, no sampler,
    // and its element size is not baked into the shader.
    EXPECT_EQ(msl.find("texture2d"), std::string::npos) << msl;
    EXPECT_EQ(msl.find("const device"), std::string::npos) << msl;
}

TEST(DxbcToSpirvTest, GuardsStructuredBufferLoadsAgainstTheViewSizeOnRequest) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00010050u);            // vs_5_0
    append_word(16u);
    append_word(162u | (4u << 24));      // dcl_resource_structured t0, 16
    append_word(0x00107000u);
    append_word(0u);
    append_word(16u);
    append_word(167u | (9u << 24));      // ld_structured o0, v0, v0, t0
    append_word(0x001020F2u); append_word(0u);
    append_word(0x001010E4u); append_word(0u);
    append_word(0x001010E4u); append_word(0u);
    append_word(0x001070E4u); append_word(0u);
    append_word(62u | (1u << 24));       // ret

    const auto bare = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(bare.valid()) << bare.failure;
    EXPECT_FALSE(spirv_contains_opcode(bare.spirv, /*OpSelect=*/169u))
        << "no guard without the option";

    DxbcTranslationOptions options{};
    options.guard_buffer_bounds = true;
    const auto guarded = translate_dxbc_to_spirv(container, options);
    ASSERT_TRUE(guarded.valid()) << guarded.failure;
    EXPECT_TRUE(spirv_contains_opcode(guarded.spirv, /*OpSelect=*/169u));
    EXPECT_TRUE(spirv_contains_opcode(guarded.spirv, /*OpULessThan=*/176u));
    // The size comes from the push block, so the block must be part of the
    // vertex stage's interface (SPIRV-Cross otherwise leaves it undeclared:
    // "use of undeclared identifier 'vitreo_sampler_lod_biases'").
    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        guarded.spirv.data(), guarded.spirv.size(), ShaderStage::Vertex,
        msl_options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl(compiled.msl_text());
    EXPECT_NE(msl.find("vitreo_sampler_lod_biases"), std::string::npos) << msl;
}

TEST(DxbcToSpirvTest, GuardsEachBufferSlotAgainstItsOwnSizeEntry) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Compute;
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00050050u);            // cs_5_0
    append_word(35u);
    append_word(162u | (4u << 24));      // dcl_resource_structured t9, 16
    append_word(0x00107000u); append_word(9u); append_word(16u);
    append_word(158u | (4u << 24));      // dcl_uav_structured u1, 4
    append_word(0x0011E000u); append_word(1u); append_word(4u);
    append_word(155u | (4u << 24));      // dcl_thread_group 1, 1, 1
    append_word(1u); append_word(1u); append_word(1u);
    append_word(0x02000068u); append_word(1u);  // dcl_temps 1
    // ld_structured r0.x, l(3), l(4), t9.xxxx
    append_word(167u | (9u << 24));
    append_word(0x00100012u); append_word(0u);
    append_word(0x00004001u); append_word(3u);
    append_word(0x00004001u); append_word(4u);
    append_word(0x00107006u); append_word(9u);
    // store_structured u1.x, l(2), l(0), r0.x
    append_word(168u | (9u << 24));
    append_word(0x0011E012u); append_word(1u);
    append_word(0x00004001u); append_word(2u);
    append_word(0x00004001u); append_word(0u);
    append_word(0x00100006u); append_word(0u);
    append_word(62u | (1u << 24));       // ret
    DxbcTranslationOptions options{};
    options.guard_buffer_bounds = true;
    const auto guarded = translate_dxbc_to_spirv(container, options);
    ASSERT_TRUE(guarded.valid()) << guarded.failure;
    MslCompileOptions msl_options{};
    msl_options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        guarded.spirv.data(), guarded.spirv.size(), ShaderStage::Compute,
        msl_options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl(compiled.msl_text());
    EXPECT_NE(msl.find("_m2[9u]"), std::string::npos) << msl;
    EXPECT_EQ(msl.find("_m2[1u]"), std::string::npos) << msl;
    EXPECT_NE(msl.find("_m2[129u]"), std::string::npos) << msl;
}

TEST(DxbcToSpirvTest, MadLoadsImmediatePlusRelativeConstantBufferIndex) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 3});
    container.output_signature.push_back(
        {"SV_Position", 0, 0, 0xFu, 3});
    container.resource_bindings.push_back({"cb0", 0, 0, 1, 0, 0});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };

    append_word(0x00010040u); // vs_4_0
    append_word(16u);
    append_word(50u | (13u << 24)); // mad
    append_word(0x001020F2u); append_word(0u); // o0.xyzw
    // cb0[6 + r0.w]. Microsoft tokenized-program index representation 3 is
    // one immediate DWORD followed by the nested relative operand.
    append_word(0x06208A06u); append_word(0u); append_word(6u);
    append_word(0x0010003Au); append_word(0u); // r0.w
    append_word(0x00208446u); append_word(0u); append_word(1u); // cb0[1]
    append_word(0x00101446u); append_word(0u); // v0.xxxx
    append_word(62u | (1u << 24)); // ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpIAdd=*/128u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpAccessChain=*/65u));

    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex,
        options);
    ASSERT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, MadLoadsRelativeOnlyConstantBufferIndex) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 3});
    container.output_signature.push_back(
        {"SV_Position", 0, 0, 0xFu, 3});
    container.resource_bindings.push_back({"cb0", 0, 0, 1, 0, 0});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };

    append_word(0x00010040u); // vs_4_0
    append_word(15u);
    append_word(50u | (12u << 24)); // mad
    append_word(0x001020F2u); append_word(0u); // o0.xyzw
    // cb0[r0.w]. Index representation 2 is a nested relative operand with no
    // preceding immediate index.
    append_word(0x04208A06u); append_word(0u);
    append_word(0x0010003Au); append_word(0u); // r0.w
    append_word(0x00208446u); append_word(0u); append_word(1u); // cb0[1]
    append_word(0x00101446u); append_word(0u); // v0.xxxx
    append_word(62u | (1u << 24)); // ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpAccessChain=*/65u));
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}

TEST(DxbcToSpirvTest, MulLoadsRelativeImmediateConstantBufferIndex) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 3});
    container.output_signature.push_back(
        {"SV_Position", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };

    append_word(0x00010050u); append_word(24u); // vs_5_0
    append_word(104u | (2u << 24)); append_word(1u); // dcl_temps 1
    append_word(0x00001835u); append_word(6u); // immediate-constant-buffer data
    append_word(0x3f800000u); append_word(0x40000000u);
    append_word(0x40400000u); append_word(0x40800000u);
    append_word(54u | (5u << 24)); // mov r0, v0
    append_word(0x001000F2u); append_word(0u);
    append_word(0x001014E6u); append_word(0u);
    append_word(56u | (8u << 24)); // mul o0, v0, icb[r0.x]
    append_word(0x001020F2u); append_word(0u);
    append_word(0x001014E6u); append_word(0u);
    append_word(0x0090900Au);
    append_word(0x0010000Au); append_word(0u);
    append_word(62u | (1u << 24)); // ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex);
    EXPECT_TRUE(compiled.valid());
}
