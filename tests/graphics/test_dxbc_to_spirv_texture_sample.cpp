// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_dxbc_to_spirv_texture_sample.cpp
/// @brief Texture sampling: register numbering, offsets, bias, lod, gradients, cube, array, comparison.

#include <gtest/gtest.h>

#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "register_gap_shaders.h"

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

DxbcContainer create_comparison_sample_shader(u32 opcode,
                                               u32 resource_dimension,
                                               ShaderStage stage) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = stage;
    container.input_signature.push_back({"TEXCOORD", 0, 0, 0xFu, 3});
    container.output_signature.push_back(
        {stage == ShaderStage::Vertex ? "SV_Position" : "SV_TARGET",
         0, 0, 0xFu,
         static_cast<u8>(stage == ShaderStage::Vertex ? 1u : 3u)});

    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(stage == ShaderStage::Vertex ? 0x00010040u : 0x00000040u);
    append_word(21u);
    append_word(88u | (resource_dimension << 11) | (4u << 24));
    append_word(0x00107000u); // dcl_resource t0
    append_word(0u);
    append_word(0x00005555u); // float return type, xyzw
    append_word(90u | (3u << 24)); // dcl_sampler
    append_word(0x00106000u); // s0
    append_word(0u);
    append_word(opcode | (11u << 24));
    append_word(0x001020F2u); // o0.xyzw
    append_word(0u);
    append_word(0x001010E4u); // v0.xyzw
    append_word(0u);
    append_word(0x0010700Au); // t0.r
    append_word(0u);
    append_word(0x00106000u); // s0
    append_word(0u);
    append_word(0x00004001u); // scalar immediate32 reference
    append_word(0x3F000000u); // 0.5f
    append_word(62u | (1u << 24)); // ret
    return container;
}

} // namespace

TEST(DxbcToSpirvTest, TexturesAndSamplersKeepTheirRegisterNumbers) {
    // Frontends bind t# at Metal texture #, s# at sampler #. A shader whose
    // only registers are t3 and s2 once compiled to texture(0) / sampler(0),
    // because nothing told SPIRV-Cross otherwise, and read what t0/s0 held.
    const auto container = parse_dxbc(ps_t3_s2, sizeof(ps_t3_s2));
    ASSERT_TRUE(container.valid());
    const auto spirv = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    const auto msl = compile_spirv_to_msl(spirv.spirv.data(),
                                          spirv.spirv.size(),
                                          ShaderStage::Fragment, flat);
    ASSERT_TRUE(msl.valid());
    const std::string text(msl.msl_text());
    EXPECT_NE(text.find("t3 [[texture(3)]]"), std::string::npos) << text;
    EXPECT_NE(text.find("s2 [[sampler(2)]]"), std::string::npos) << text;
}

// Microsoft Learn (sample, aoffimmi): the sample-controls extended opcode token
// carries three 4-bit *signed* texel offsets applied before the fetch. SPIR-V
// spells that as the ConstOffset image operand. UE5's filter kernels use these
// constantly, and refusing them failed the whole shader.
TEST(DxbcToSpirvTest, TranslatesSampleImmediateTexelOffsets) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Fragment;
    container.input_signature.push_back({"TEXCOORD", 0, 0, 0xFu, 3u});
    container.output_signature.push_back({"SV_TARGET", 0, 0, 0xFu, 3u, 64u});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00000050u); append_word(21u);
    append_word(0x0100086Au);
    append_word(0x0300005Au); append_word(0x00106000u); append_word(0u); // sampler s0
    append_word(0x04001858u); append_word(0x00107000u);
    append_word(0u); append_word(0x00005555u);      // dcl_resource_texture2d t0
    // sample with an extended sample-controls token: u = -1, v = +2.
    // sample (opcode 69) with the extended bit set, length 10 tokens.
    append_word(69u | (10u << 24) | 0x80000000u);
    // Extended token type 1 = SAMPLE_CONTROLS; u = -1 (0xF), v = +2.
    append_word(1u | (0xFu << 9) | (0x2u << 13));
    append_word(0x001020F2u); append_word(0u);       // dst o0
    append_word(0x00101E46u); append_word(0u);       // v0 coordinate
    append_word(0x00107E46u); append_word(0u);       // t0
    append_word(0x00106000u); append_word(0u);       // s0
    append_word(0x0100003Eu);                        // ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment);
    ASSERT_TRUE(compiled.valid());
    const std::string msl(compiled.msl_text());
    // The offsets survive as a signed 2D texel offset on the sample.
    EXPECT_NE(msl.find("int2(-1, 2)"), std::string::npos) << msl;
}

TEST(DxbcToSpirvTest, TranslatesTexture2DSampleWithLodBias) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Fragment;
    container.input_signature.push_back(
        {"TEXCOORD", 0, 0, 0xFu, 3});
    container.output_signature.push_back(
        {"SV_TARGET", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00000050u); // ps_5_0
    append_word(21u);         // total DWORD count
    append_word(88u | (3u << 11) | (4u << 24)); // dcl_resource_texture2d
    append_word(0x00107000u); // t0
    append_word(0u);
    append_word(0x00005555u); // float return type, xyzw
    append_word(90u | (3u << 24)); // dcl_sampler
    append_word(0x00106000u); // s0
    append_word(0u);
    append_word(74u | (11u << 24)); // sample_b, eleven DWORDs
    append_word(0x001020F2u); // o0.xyzw
    append_word(0u);
    append_word(0x00101046u); // v0.xyxx
    append_word(0u);
    append_word(0x00107E46u); // t0.xyzw
    append_word(0u);
    append_word(0x00106000u); // s0
    append_word(0u);
    append_word(0x00004001u); // scalar immediate32
    append_word(0xBF000000u); // -0.5f LOD bias
    append_word(62u | (1u << 24)); // ret

    auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpFAdd=*/129u));
    ASSERT_TRUE(container.resource_bindings.empty());
    ASSERT_EQ(result.texture_bindings.size(), 1u);
    ASSERT_EQ(result.sampler_bindings.size(), 1u);

    MslCompileOptions options{};
    options.argument_buffers = false;
    auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment,
        options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("texture2d<float>"), std::string::npos);
    EXPECT_NE(msl.find("sampler"), std::string::npos);
    EXPECT_NE(msl.find("bias("), std::string::npos);
    EXPECT_NE(msl.find("vitreo_sampler_lod_biases"), std::string::npos);
    EXPECT_NE(msl.find("[[buffer(0)]]"), std::string::npos);
}

TEST(DxbcToSpirvTest, TranslatesTexture2DSampleWithExplicitLod) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Fragment;
    container.input_signature.push_back({"TEXCOORD", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"SV_TARGET", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00000050u); // ps_5_0
    append_word(21u);         // total DWORD count
    append_word(88u | (3u << 11) | (4u << 24)); // dcl_resource_texture2d
    append_word(0x00107000u); // t0
    append_word(0u);
    append_word(0x00005555u); // float return type, xyzw
    append_word(90u | (3u << 24)); // dcl_sampler
    append_word(0x00106000u); // s0
    append_word(0u);
    append_word(72u | (11u << 24)); // sample_l, eleven DWORDs
    append_word(0x001020F2u); // o0.xyzw
    append_word(0u);
    append_word(0x00101046u); // v0.xyxx
    append_word(0u);
    append_word(0x00107E46u); // t0.xyzw
    append_word(0u);
    append_word(0x00106000u); // s0
    append_word(0u);
    append_word(0x00004001u); // scalar immediate32
    append_word(0x40000000u); // 2.0f LOD
    append_word(62u | (1u << 24)); // ret

    auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpImageSampleExplicitLod=*/88u));

    MslCompileOptions options{};
    options.argument_buffers = false;
    auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment,
        options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("level("), std::string::npos);
    EXPECT_EQ(msl.find("bias("), std::string::npos);
    // D3D11.3 22.4.18: the sampler state's MipLODBias applies to sample_l
    // too (Windows d3d11_a07 sampler M6, d3d11_a09b [cs_sampler_bias]);
    // Metal samplers have no bias, so the level adds the push-block entry.
    EXPECT_NE(msl.find("vitreo_sampler_lod_biases"), std::string::npos);
}

TEST(DxbcToSpirvTest, TranslatesTexture2DSampleWithExplicitGradients) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Fragment;
    container.input_signature.push_back({"TEXCOORD", 0, 0, 0xFu, 3});
    container.input_signature.push_back({"TEXCOORD", 1, 1, 0xFu, 3});
    container.input_signature.push_back({"TEXCOORD", 2, 2, 0xFu, 3});
    container.output_signature.push_back({"SV_TARGET", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00000050u); // ps_5_0
    append_word(23u);         // total DWORD count
    append_word(88u | (3u << 11) | (4u << 24)); // dcl_resource_texture2d
    append_word(0x00107000u); // t0
    append_word(0u);
    append_word(0x00005555u); // float return type, xyzw
    append_word(90u | (3u << 24)); // dcl_sampler
    append_word(0x00106000u); // s0
    append_word(0u);
    append_word(73u | (13u << 24)); // sample_d
    append_word(0x001020F2u); // o0.xyzw
    append_word(0u);
    append_word(0x001010E4u); // v0 address
    append_word(0u);
    append_word(0x00107E46u); // t0.xyzw
    append_word(0u);
    append_word(0x00106000u); // s0
    append_word(0u);
    append_word(0x001010E4u); // v1 ddx
    append_word(1u);
    append_word(0x001010E4u); // v2 ddy
    append_word(2u);
    append_word(62u | (1u << 24)); // ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_gradient_sample(result.spirv));

    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment,
        options);
    ASSERT_TRUE(compiled.valid());
    EXPECT_NE(std::string(compiled.msl_text()).find("gradient2d("),
              std::string::npos);

    // The same opcode on a cube array retains the address.w layer but narrows
    // both gradients to the direction xyz; the array layer has no derivative.
    DxbcContainer cube_array = container;
    const u32 cube_array_decl = 88u | (10u << 11) | (4u << 24);
    std::memcpy(cube_array.bytecode.data() + 2u * sizeof(u32),
                &cube_array_decl, sizeof(cube_array_decl));
    const auto cube_result = translate_dxbc_to_spirv(cube_array);
    ASSERT_TRUE(cube_result.valid()) << cube_result.failure;
    EXPECT_TRUE(spirv_contains_arrayed_cube_image(cube_result.spirv));
    EXPECT_TRUE(spirv_contains_gradient_sample(cube_result.spirv));
    const auto cube_compiled = compile_spirv_to_msl(
        cube_result.spirv.data(), cube_result.spirv.size(),
        ShaderStage::Fragment, options);
    ASSERT_TRUE(cube_compiled.valid());
    const std::string cube_msl = cube_compiled.msl_text();
    EXPECT_NE(cube_msl.find("texturecube_array<float>"), std::string::npos);
    EXPECT_NE(cube_msl.find("gradientcube("), std::string::npos);
}

TEST(DxbcToSpirvTest, TranslatesTextureCubeSampleWithXyzCoordinates) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Fragment;
    container.input_signature.push_back({"TEXCOORD", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"SV_TARGET", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00000050u); // ps_5_0
    append_word(19u);         // total DWORD count
    append_word(88u | (6u << 11) | (4u << 24)); // dcl_resource_texturecube
    append_word(0x00107000u); // t0
    append_word(0u);
    append_word(0x00005555u); // float return type, xyzw
    append_word(90u | (3u << 24)); // dcl_sampler
    append_word(0x00106000u); // s0
    append_word(0u);
    append_word(69u | (9u << 24)); // sample
    append_word(0x001020F2u); // o0.xyzw
    append_word(0u);
    append_word(0x001010E4u); // v0.xyzw
    append_word(0u);
    append_word(0x00107E46u); // t0.xyzw
    append_word(0u);
    append_word(0x00106000u); // s0
    append_word(0u);
    append_word(62u | (1u << 24)); // ret

    auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    ASSERT_EQ(result.texture_bindings.size(), 1u);
    EXPECT_EQ(result.texture_bindings.front().dimension, 9u);

    MslCompileOptions options{};
    options.argument_buffers = false;
    auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment,
        options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("texturecube<float>"), std::string::npos);
}

TEST(DxbcToSpirvTest, TranslatesTexture2DArraySampleWithXyzCoordinates) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Fragment;
    container.input_signature.push_back({"TEXCOORD", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"SV_TARGET", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00000050u); // ps_5_0
    append_word(19u);         // total DWORD count
    append_word(88u | (8u << 11) | (4u << 24));
    append_word(0x00107000u); // dcl_resource_texture2darray t0
    append_word(0u);
    append_word(0x00005555u); // float return type, xyzw
    append_word(90u | (3u << 24)); // dcl_sampler
    append_word(0x00106000u); // s0
    append_word(0u);
    append_word(69u | (9u << 24)); // sample
    append_word(0x001020F2u); // o0.xyzw
    append_word(0u);
    append_word(0x001010E4u); // v0.xyzw: uv in xy, array location in z
    append_word(0u);
    append_word(0x00107E46u); // t0.xyzw
    append_word(0u);
    append_word(0x00106000u); // s0
    append_word(0u);
    append_word(62u | (1u << 24)); // ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    ASSERT_EQ(result.texture_bindings.size(), 1u);
    EXPECT_EQ(result.texture_bindings.front().dimension, 5u);
    EXPECT_TRUE(spirv_contains_arrayed_2d_image(result.spirv));

    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment,
        options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("texture2d_array<float>"), std::string::npos);

    // RDEF reports D3D_SRV_DIMENSION_TEXTURE2DARRAY=5. It must select the
    // same arrayed image type instead of masking the executable declaration.
    container.resource_bindings.push_back({"textures", 2, 0, 1, 5, 5});
    const auto reflected_result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(reflected_result.valid()) << reflected_result.failure;
    EXPECT_TRUE(spirv_contains_arrayed_2d_image(reflected_result.spirv));
    const auto reflected_compiled = compile_spirv_to_msl(
        reflected_result.spirv.data(), reflected_result.spirv.size(),
        ShaderStage::Fragment, options);
    ASSERT_TRUE(reflected_compiled.valid());
    EXPECT_NE(std::string(reflected_compiled.msl_text())
                  .find("texture2d_array<float>"),
              std::string::npos);
}

TEST(DxbcToSpirvTest, TranslatesTextureCubeArraySampleWithXyzwCoordinates) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Fragment;
    container.input_signature.push_back({"TEXCOORD", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"SV_TARGET", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00000050u); // ps_5_0
    append_word(19u);         // total DWORD count
    append_word(88u | (10u << 11) | (4u << 24));
    append_word(0x00107000u); // dcl_resource_texturecubearray t0
    append_word(0u);
    append_word(0x00005555u); // float return type, xyzw
    append_word(90u | (3u << 24)); // dcl_sampler
    append_word(0x00106000u); // s0
    append_word(0u);
    append_word(69u | (9u << 24)); // sample
    append_word(0x001020F2u); // o0.xyzw
    append_word(0u);
    append_word(0x001010E4u); // v0.xyzw: direction xyz, cube index w
    append_word(0u);
    append_word(0x00107E46u); // t0.xyzw
    append_word(0u);
    append_word(0x00106000u); // s0
    append_word(0u);
    append_word(62u | (1u << 24)); // ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    ASSERT_EQ(result.texture_bindings.size(), 1u);
    EXPECT_EQ(result.texture_bindings.front().dimension, 10u);
    EXPECT_TRUE(spirv_contains_arrayed_cube_image(result.spirv));

    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment,
        options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("texturecube_array<float>"), std::string::npos);
    EXPECT_NE(msl.find("uint("), std::string::npos);

    // RDEF uses D3D_SRV_DIMENSION_TEXTURECUBEARRAY=10 and must preserve the
    // same cube-array type instead of masking the executable declaration.
    container.resource_bindings.push_back({"cubes", 2, 0, 1, 5, 10});
    const auto reflected_result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(reflected_result.valid()) << reflected_result.failure;
    EXPECT_TRUE(spirv_contains_arrayed_cube_image(reflected_result.spirv));
    const auto reflected_compiled = compile_spirv_to_msl(
        reflected_result.spirv.data(), reflected_result.spirv.size(),
        ShaderStage::Fragment, options);
    ASSERT_TRUE(reflected_compiled.valid());
    EXPECT_NE(std::string(reflected_compiled.msl_text())
                  .find("texturecube_array<float>"),
              std::string::npos);
}

TEST(DxbcToSpirvTest, TranslatesTexture2DComparisonSample) {
    const auto result = translate_dxbc_to_spirv(
        create_comparison_sample_shader(
            /*sample_c=*/70u, /*TEXTURE2D=*/3u, ShaderStage::Fragment));
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpImageSampleDrefImplicitLod=*/89u));

    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment,
        options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("depth2d<float>"), std::string::npos);
    EXPECT_NE(msl.find("sample_compare"), std::string::npos);
}

TEST(DxbcToSpirvTest, TranslatesTexture2DArrayComparisonSampleAtLevelZero) {
    const auto result = translate_dxbc_to_spirv(
        create_comparison_sample_shader(
            /*sample_c_lz=*/71u, /*TEXTURE2DARRAY=*/8u,
            ShaderStage::Vertex));
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpImageSampleDrefExplicitLod=*/90u));

    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Vertex,
        options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("depth2d_array<float>"), std::string::npos);
    EXPECT_NE(msl.find("sample_compare"), std::string::npos);
    // 22.4.20: sample_c_lz is LOD 0 plus the sampler's MipLODBias, which
    // the push block carries (Windows d3d11_a09b [cs_sampler_bias]).
    EXPECT_NE(msl.find("level("), std::string::npos);
    EXPECT_NE(msl.find("vitreo_sampler_lod_biases"), std::string::npos);
}

TEST(DxbcToSpirvTest, TranslatesTextureCubeComparisonSample) {
    const auto result = translate_dxbc_to_spirv(
        create_comparison_sample_shader(
            /*sample_c=*/70u, /*TEXTURECUBE=*/6u, ShaderStage::Fragment));
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpImageSampleDrefImplicitLod=*/89u));

    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment,
        options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("depthcube<float>"), std::string::npos);
    EXPECT_NE(msl.find("sample_compare"), std::string::npos);
}

TEST(DxbcToSpirvTest, RejectsImplicitComparisonSampleOutsidePixelShader) {
    const auto result = translate_dxbc_to_spirv(
        create_comparison_sample_shader(
            /*sample_c=*/70u, /*TEXTURE2D=*/3u, ShaderStage::Vertex));
    EXPECT_FALSE(result.valid());
    EXPECT_NE(result.failure.find("implicit-LOD"), std::string::npos);
}

TEST(DxbcToSpirvTest, RejectsTexture3DComparisonSampleHonestly) {
    const auto result = translate_dxbc_to_spirv(
        create_comparison_sample_shader(
            /*sample_c=*/70u, /*TEXTURE3D=*/5u, ShaderStage::Fragment));
    EXPECT_FALSE(result.valid());
    EXPECT_NE(result.failure.find("Texture3D"), std::string::npos);
}
