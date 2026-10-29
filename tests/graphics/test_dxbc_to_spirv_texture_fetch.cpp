// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_dxbc_to_spirv_texture_fetch.cpp
/// @brief Texture fetch and query: resinfo, ld, multisampled ld.

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

TEST(DxbcToSpirvTest, ResinfoUintQueriesTexture2DDimensionsAndMipCount) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Fragment;
    container.output_signature.push_back({"SV_TARGET", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00000040u); // ps_4_0
    append_word(17u);
    append_word(0x04001858u); // dcl_resource_texture2d t0
    append_word(0x00107000u); append_word(0u);
    append_word(0x00005555u); // float return components
    append_word(101u | (3u << 24)); // dcl_output o0
    append_word(0x001020F2u); append_word(0u);
    append_word(61u | 0x00001000u | (7u << 24)); // resinfo_uint
    append_word(0x001020F2u); append_word(0u); // o0
    append_word(0x00004001u); append_word(0u); // mip 0
    append_word(0x00107E46u); append_word(0u); // t0.xyzw
    append_word(62u | (1u << 24));

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpImageQuerySizeLod=*/103u));
    EXPECT_TRUE(spirv_contains_opcode(
        result.spirv, /*OpImageQueryLevels=*/106u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpBitcast=*/124u));

    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment,
        options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("get_width("), std::string::npos);
    EXPECT_NE(msl.find("get_height("), std::string::npos);
    EXPECT_NE(msl.find("get_num_mip_levels("), std::string::npos);

    // The float return mode (GetDimensions with float outputs) converts
    // the same query (D3D11.3 22.4.14; Windows d3d11_a09b [cs_resinfo_f]).
    DxbcContainer float_mode = container;
    const u32 float_opcode = 61u | (7u << 24);
    std::memcpy(float_mode.bytecode.data() + 9u * sizeof(u32),
                &float_opcode, sizeof(float_opcode));
    const auto float_result = translate_dxbc_to_spirv(float_mode);
    ASSERT_TRUE(float_result.valid()) << float_result.failure;
    EXPECT_TRUE(spirv_contains_opcode(float_result.spirv,
                                      /*OpConvertSToF=*/111u));
    // Reserved return type 3 is refused.
    DxbcContainer reserved = container;
    const u32 reserved_opcode = 61u | (3u << 11) | (7u << 24);
    std::memcpy(reserved.bytecode.data() + 9u * sizeof(u32),
                &reserved_opcode, sizeof(reserved_opcode));
    EXPECT_FALSE(translate_dxbc_to_spirv(reserved).valid());
}

TEST(DxbcToSpirvTest, TranslatesTexture2DLoadWithIntegerAddressAndMip) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Fragment;
    container.input_signature.push_back({"TEXCOORD", 0, 0, 0xFu, 1});
    container.output_signature.push_back({"SV_TARGET", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00000050u); // ps_5_0
    append_word(14u);         // total DWORD count
    append_word(88u | (3u << 11) | (4u << 24)); // dcl_resource_texture2d
    append_word(0x00107000u); // t0
    append_word(0u);
    append_word(0x00005555u); // float return type, xyzw
    append_word(45u | (7u << 24)); // ld
    append_word(0x001020F2u); // o0.xyzw
    append_word(0u);
    append_word(0x001010E4u); // v0 address: xy texel, w mip
    append_word(0u);
    append_word(0x00107E46u); // t0.xyzw
    append_word(0u);
    append_word(62u | (1u << 24)); // ret

    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpImageFetch=*/95u));
    EXPECT_TRUE(result.sampler_bindings.empty());

    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment,
        options);
    ASSERT_TRUE(compiled.valid());
    const std::string msl = compiled.msl_text();
    EXPECT_NE(msl.find("texture2d<float>"), std::string::npos);
    EXPECT_NE(msl.find(".read("), std::string::npos);
    EXPECT_EQ(msl.find("sampler"), std::string::npos);

    DxbcContainer offset_load = container;
    const u32 offset_total = 15u;
    const u32 offset_opcode = 45u | (8u << 24) | 0x80000000u;
    const u32 offset_extension = 0x0001C201u; // U=+1, V=-2, W=0
    std::memcpy(offset_load.bytecode.data() + sizeof(u32),
                &offset_total, sizeof(offset_total));
    std::memcpy(offset_load.bytecode.data() + 6u * sizeof(u32),
                &offset_opcode, sizeof(offset_opcode));
    const auto* extension_bytes =
        reinterpret_cast<const u8*>(&offset_extension);
    offset_load.bytecode.insert(
        offset_load.bytecode.begin() + 7u * sizeof(u32),
        extension_bytes, extension_bytes + sizeof(offset_extension));
    const auto offset_result = translate_dxbc_to_spirv(offset_load);
    ASSERT_TRUE(offset_result.valid()) << offset_result.failure;
    EXPECT_TRUE(spirv_contains_fetch_const_offset(offset_result.spirv));
    const auto offset_compiled = compile_spirv_to_msl(
        offset_result.spirv.data(), offset_result.spirv.size(),
        ShaderStage::Fragment, options);
    ASSERT_TRUE(offset_compiled.valid());
    const std::string offset_msl = offset_compiled.msl_text();
    EXPECT_NE(offset_msl.find(".read("), std::string::npos);
    EXPECT_NE(offset_msl.find("int2(1, -2)"), std::string::npos);

    for (const auto& [return_token, metal_type] :
         std::initializer_list<std::pair<u32, const char*>>{
             {0x00004444u, "texture2d<uint>"},
             {0x00003333u, "texture2d<int>"}}) {
        DxbcContainer typed = container;
        std::memcpy(typed.bytecode.data() + 5u * sizeof(u32),
                    &return_token, sizeof(return_token));
        const auto typed_result = translate_dxbc_to_spirv(typed);
        ASSERT_TRUE(typed_result.valid()) << typed_result.failure;
        EXPECT_TRUE(spirv_contains_opcode(
            typed_result.spirv, /*OpImageFetch=*/95u));
        EXPECT_TRUE(spirv_contains_opcode(
            typed_result.spirv, /*OpBitcast=*/124u));
        const auto typed_compiled = compile_spirv_to_msl(
            typed_result.spirv.data(), typed_result.spirv.size(),
            ShaderStage::Fragment, options);
        ASSERT_TRUE(typed_compiled.valid());
        const std::string typed_msl = typed_compiled.msl_text();
        EXPECT_NE(typed_msl.find(metal_type), std::string::npos);
        EXPECT_NE(typed_msl.find(".read("), std::string::npos);
    }

    // Texture2DArray and Texture3D consume address.xyz while retaining .w as
    // the mip selector. Their direct-image fetches must remain sampler-free.
    for (const u32 token_dimension : {8u, 5u}) {
        DxbcContainer shaped = container;
        const u32 declaration =
            88u | (token_dimension << 11) | (4u << 24);
        std::memcpy(shaped.bytecode.data() + 2u * sizeof(u32),
                    &declaration, sizeof(declaration));
        const auto shaped_result = translate_dxbc_to_spirv(shaped);
        ASSERT_TRUE(shaped_result.valid()) << shaped_result.failure;
        EXPECT_TRUE(spirv_contains_opcode(
            shaped_result.spirv, /*OpImageFetch=*/95u));
        const auto shaped_compiled = compile_spirv_to_msl(
            shaped_result.spirv.data(), shaped_result.spirv.size(),
            ShaderStage::Fragment, options);
        ASSERT_TRUE(shaped_compiled.valid());
        const std::string shaped_msl = shaped_compiled.msl_text();
        EXPECT_NE(shaped_msl.find(".read("), std::string::npos);
        EXPECT_EQ(shaped_msl.find("sampler"), std::string::npos);
    }

    DxbcContainer cube = container;
    const u32 cube_declaration = 88u | (6u << 11) | (4u << 24);
    std::memcpy(cube.bytecode.data() + 2u * sizeof(u32),
                &cube_declaration, sizeof(cube_declaration));
    const auto cube_result = translate_dxbc_to_spirv(cube);
    EXPECT_FALSE(cube_result.valid());
    EXPECT_NE(cube_result.failure.find("TextureCube"), std::string::npos);
}

TEST(DxbcToSpirvTest, TranslatesMultisampledTextureLoadWithSampleIndex) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 1;
    container.stage = ShaderStage::Fragment;
    container.input_signature.push_back({"TEXCOORD", 0, 0, 0xFu, 1});
    container.output_signature.push_back({"SV_TARGET", 0, 0, 0xFu, 3});
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00000041u); // ps_4_1
    append_word(16u);
    append_word(0x04042058u); // dcl_resource_texture2dms(4) t0
    append_word(0x00107000u);
    append_word(0u);
    append_word(0x00005555u);
    append_word(46u | (9u << 24)); // ld_ms
    append_word(0x001020F2u); append_word(0u); // o0
    append_word(0x001010E4u); append_word(0u); // v0 address
    append_word(0x00107E46u); append_word(0u); // t0
    append_word(0x00004001u); append_word(2u); // sample index 2
    append_word(62u | (1u << 24));

    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_multisample_fetch(result.spirv));
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment,
        options);
    ASSERT_TRUE(compiled.valid());
    EXPECT_NE(std::string(compiled.msl_text()).find("texture2d_ms<float>"),
              std::string::npos);

    DxbcContainer array = container;
    const u32 array_decl = 0x04044858u; // texture2dmsarray(4)
    std::memcpy(array.bytecode.data() + 2u * sizeof(u32),
                &array_decl, sizeof(array_decl));
    const auto array_result = translate_dxbc_to_spirv(array);
    ASSERT_TRUE(array_result.valid()) << array_result.failure;
    EXPECT_TRUE(spirv_contains_multisample_fetch(array_result.spirv));
    const auto array_compiled = compile_spirv_to_msl(
        array_result.spirv.data(), array_result.spirv.size(),
        ShaderStage::Fragment, options);
    ASSERT_TRUE(array_compiled.valid());
    EXPECT_NE(std::string(array_compiled.msl_text()).find(
                  "texture2d_ms_array<float>"), std::string::npos);

    for (const auto& [return_token, metal_type] :
         std::initializer_list<std::pair<u32, const char*>>{
             {0x00004444u, "texture2d_ms<uint>"},
             {0x00003333u, "texture2d_ms<int>"}}) {
        DxbcContainer typed = container;
        std::memcpy(typed.bytecode.data() + 5u * sizeof(u32),
                    &return_token, sizeof(return_token));
        const auto typed_result = translate_dxbc_to_spirv(typed);
        ASSERT_TRUE(typed_result.valid()) << typed_result.failure;
        EXPECT_TRUE(spirv_contains_multisample_fetch(typed_result.spirv));
        EXPECT_TRUE(spirv_contains_opcode(
            typed_result.spirv, /*OpBitcast=*/124u));
        const auto typed_compiled = compile_spirv_to_msl(
            typed_result.spirv.data(), typed_result.spirv.size(),
            ShaderStage::Fragment, options);
        ASSERT_TRUE(typed_compiled.valid());
        EXPECT_NE(std::string(typed_compiled.msl_text()).find(metal_type),
                  std::string::npos);
    }

    DxbcContainer offset = container;
    const u32 offset_total = 17u;
    const u32 offset_opcode = 46u | (10u << 24) | 0x80000000u;
    const u32 offset_extension = 0x0001C201u; // U=+1, V=-2
    std::memcpy(offset.bytecode.data() + sizeof(u32),
                &offset_total, sizeof(offset_total));
    std::memcpy(offset.bytecode.data() + 6u * sizeof(u32),
                &offset_opcode, sizeof(offset_opcode));
    const auto* extension_bytes =
        reinterpret_cast<const u8*>(&offset_extension);
    offset.bytecode.insert(offset.bytecode.begin() + 7u * sizeof(u32),
                           extension_bytes, extension_bytes + sizeof(u32));
    const auto offset_result = translate_dxbc_to_spirv(offset);
    ASSERT_TRUE(offset_result.valid()) << offset_result.failure;
    EXPECT_TRUE(
        spirv_contains_multisample_fetch_const_offset(offset_result.spirv));
    const auto offset_compiled = compile_spirv_to_msl(
        offset_result.spirv.data(), offset_result.spirv.size(),
        ShaderStage::Fragment, options);
    ASSERT_TRUE(offset_compiled.valid());
    EXPECT_NE(std::string(offset_compiled.msl_text()).find("int2(1, -2)"),
              std::string::npos);

    // RDEF uses dimensions 6/7 for Texture2DMS/Texture2DMSArray, unlike the
    // tokenized-program dimensions 4/9. Both sources must select MS image
    // types without conflating RDEF dimension 9 (TextureCube).
    for (const auto& [rdef_dimension, token_dimension] :
         std::initializer_list<std::pair<u32, u32>>{{6u, 4u}, {7u, 9u}}) {
        DxbcContainer reflected = container;
        const u32 declaration =
            88u | (token_dimension << 11) | (4u << 16) | (4u << 24);
        std::memcpy(reflected.bytecode.data() + 2u * sizeof(u32),
                    &declaration, sizeof(declaration));
        reflected.resource_bindings.push_back(
            {"ms_texture", 2, 0, 1, 5, rdef_dimension});
        const auto reflected_result = translate_dxbc_to_spirv(reflected);
        ASSERT_TRUE(reflected_result.valid()) << reflected_result.failure;
        EXPECT_TRUE(
            spirv_contains_multisample_fetch(reflected_result.spirv));
    }
}
