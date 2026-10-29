// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_dxbc_to_spirv_pixel_uav.cpp
/// @brief Pixel-stage UAVs (feature level 11_0).

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

#include "pixel_uav_shaders.h"

namespace {

/// True when @p spirv holds an instruction with opcode @p op whose operand
/// @p operand_index (0-based, after the opcode word) equals @p value.
bool spirv_has(const std::vector<u32>& spirv, u32 op, usize operand_index,
               u32 value) {
    for (usize i = 5; i < spirv.size();) {
        const u32 count = spirv[i] >> 16;
        if (count == 0) return false;
        if ((spirv[i] & 0xFFFFu) == op && operand_index + 1 < count &&
            spirv[i + 1 + operand_index] == value) {
            return true;
        }
        i += count;
    }
    return false;
}

std::string pixel_msl(const DxbcToSpirvResult& result) {
    MslCompileOptions options{};
    options.argument_buffers = false;
    if (result.uses_typed_uav_atomics) options.msl_version = 30100u;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment,
        options);
    return compiled.valid() ? std::string(compiled.msl_text()) : std::string{};
}

std::string index_of(unsigned base, unsigned slot, const char* kind) {
    return std::string("[[") + kind + "(" + std::to_string(base + slot) + ")]]";
}

}  // namespace

// The Satisfactory (UE5, D3D11 SM5) main-menu failure: CreatePixelShader of a
// ps_5_0 using store_uav_typed returned E_FAIL because the translator refused
// typed UAV access outside compute. Learn (dcl_uav_typed, ld_uav_typed,
// store_uav_typed): the pixel stage has u# at feature level 11_0, and the
// measured CreatePixelShader of this blob returns S_OK
// (tools/oracle_probe/d3d11_stage_uav_probe.expected).
TEST(DxbcToSpirvTest, PixelShaderTypedTextureUavLoadsAndStores) {
    const auto container = parse_dxbc(pixel_uav_ps_typed_texture,
                                      sizeof(pixel_uav_ps_typed_texture));
    ASSERT_TRUE(container.valid());
    ASSERT_EQ(container.stage, ShaderStage::Fragment);
    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    const std::string msl = pixel_msl(result);
    ASSERT_FALSE(msl.empty());
    EXPECT_NE(msl.find("fragment "), std::string::npos) << msl;
    EXPECT_NE(msl.find("texture2d<float, access::read_write> u1 " +
                       index_of(BACKEND_UAV_TEXTURE_BASE, 1, "texture")),
              std::string::npos)
        << msl;
    EXPECT_NE(msl.find(".read("), std::string::npos) << msl;
    EXPECT_NE(msl.find(".write("), std::string::npos) << msl;
    // No [earlydepthstencil]: the fragment keeps late tests.
    EXPECT_EQ(msl.find("early_fragment_tests"), std::string::npos) << msl;
}

// RWBuffer<uint> at u2: a read_write texture_buffer in the u# texture window,
// with a typed-UAV atomic (atomic_iadd / imm_atomic_umax) on it.
TEST(DxbcToSpirvTest, PixelShaderTypedBufferUavStoresAndAtomics) {
    const auto container = parse_dxbc(pixel_uav_ps_typed_buffer,
                                      sizeof(pixel_uav_ps_typed_buffer));
    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(result.uses_typed_uav_atomics);
    const std::string msl = pixel_msl(result);
    ASSERT_FALSE(msl.empty());
    EXPECT_NE(msl.find("texture_buffer<uint, access::read_write> u2 " +
                       index_of(BACKEND_UAV_TEXTURE_BASE, 2, "texture")),
              std::string::npos)
        << msl;
    EXPECT_NE(msl.find("atomic_fetch_add"), std::string::npos) << msl;
    EXPECT_NE(msl.find("atomic_fetch_max"), std::string::npos) << msl;
}

// RWByteAddressBuffer at u3 and RWStructuredBuffer at u4: storage buffers in
// the u# buffer window, with imm_atomic_iadd and atomic_or on them.
TEST(DxbcToSpirvTest, PixelShaderRawAndStructuredUavs) {
    const auto container = parse_dxbc(pixel_uav_ps_raw_structured,
                                      sizeof(pixel_uav_ps_raw_structured));
    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    const std::string msl = pixel_msl(result);
    ASSERT_FALSE(msl.empty());
    EXPECT_NE(msl.find(index_of(BACKEND_UAV_BUFFER_BASE, 3, "buffer")),
              std::string::npos)
        << msl;
    EXPECT_NE(msl.find(index_of(BACKEND_UAV_BUFFER_BASE, 4, "buffer")),
              std::string::npos)
        << msl;
    EXPECT_NE(msl.find("atomic_fetch_add"), std::string::npos) << msl;
    EXPECT_NE(msl.find("atomic_fetch_or"), std::string::npos) << msl;
    // No `_glc`: neither buffer is declared globally coherent.
    EXPECT_EQ(msl.find("volatile"), std::string::npos) << msl;
}

// Measured with fxc: [earlydepthstencil] sets dcl_globalFlags bit 13 and
// `globallycoherent` sets the declaration's bit 16. The first becomes
// ExecutionMode EarlyFragmentTests (9) -> [[early_fragment_tests]]; the
// second the Coherent decoration (23) on the u# variable.
TEST(DxbcToSpirvTest, PixelShaderEarlyDepthStencilAndGloballyCoherent) {
    const auto container = parse_dxbc(pixel_uav_ps_early_coherent,
                                      sizeof(pixel_uav_ps_early_coherent));
    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(result.valid()) << result.failure;
    constexpr u32 kOpExecutionMode = 16, kOpDecorate = 71;
    EXPECT_TRUE(spirv_has(result.spirv, kOpExecutionMode, 1, 9u));
    EXPECT_TRUE(spirv_has(result.spirv, kOpDecorate, 1, 23u));
    const std::string msl = pixel_msl(result);
    ASSERT_FALSE(msl.empty());
    EXPECT_NE(msl.find("[[ early_fragment_tests ]] fragment"),
              std::string::npos)
        << msl;
    // The other blobs carry only refactoringAllowed: no early tests, and no
    // Coherent on a UAV declared without `_glc`.
    const auto plain = translate_dxbc_to_spirv(parse_dxbc(
        pixel_uav_ps_typed_texture, sizeof(pixel_uav_ps_typed_texture)));
    ASSERT_TRUE(plain.valid()) << plain.failure;
    EXPECT_FALSE(spirv_has(plain.spirv, kOpExecutionMode, 1, 9u));
    EXPECT_FALSE(spirv_has(plain.spirv, kOpDecorate, 1, 23u));
}

// Learn (sync, sm5 asm): sync_uglobal is the one sync a pixel shader may use.
// SPIRV-Cross emits no fence outside compute, so it is refused by name rather
// than dropped.
TEST(DxbcToSpirvTest, PixelShaderSyncUglobalIsRefusedByName) {
    const auto result = translate_dxbc_to_spirv(
        parse_dxbc(pixel_uav_ps_sync, sizeof(pixel_uav_ps_sync)));
    ASSERT_FALSE(result.valid());
    EXPECT_NE(result.failure.find("sync_uglobal"), std::string::npos)
        << result.failure;
}

// Learn (dcl_uav_typed): UAVs outside the pixel and compute stages need the
// 11.1 runtime at feature level 11_1; measured, CreateVertexShader of this
// vs_5_0 blob at 11_0 returns E_INVALIDARG. The translator refuses it too.
TEST(DxbcToSpirvTest, VertexShaderUavIsRefusedBelowFeatureLevel11_1) {
    const auto container =
        parse_dxbc(pixel_uav_vs_uav, sizeof(pixel_uav_vs_uav));
    ASSERT_TRUE(container.valid());
    ASSERT_EQ(container.stage, ShaderStage::Vertex);
    EXPECT_TRUE(dxbc_declares_uavs(container));
    EXPECT_FALSE(dxbc_declares_uavs(parse_dxbc(ps_t3_s2, sizeof(ps_t3_s2))));
    EXPECT_TRUE(dxbc_declares_uavs(parse_dxbc(
        pixel_uav_ps_raw_structured, sizeof(pixel_uav_ps_raw_structured))));
    const auto result = translate_dxbc_to_spirv(container);
    ASSERT_FALSE(result.valid());
    EXPECT_NE(result.failure.find("outside the pixel and compute"),
              std::string::npos)
        << result.failure;
}

// A pixel shader on descriptor tables keeps set 3 discrete (its UAVs bind
// flat, from OMSetRenderTargetsAndUnorderedAccessViews), so its u# must still
// land on the flat u# windows the backend binds -- not wherever SPIRV-Cross
// would number them.
TEST(DxbcToSpirvTest, PixelUavsKeepFlatIndicesWhenOtherSetsAreTables) {
    for (const auto& [blob, size, kind, base, slot] :
         {std::tuple{pixel_uav_ps_typed_texture,
                     sizeof(pixel_uav_ps_typed_texture), "texture",
                     BACKEND_UAV_TEXTURE_BASE, 1u},
          std::tuple{pixel_uav_ps_raw_structured,
                     sizeof(pixel_uav_ps_raw_structured), "buffer",
                     BACKEND_UAV_BUFFER_BASE, 4u}}) {
        const auto result = translate_dxbc_to_spirv(parse_dxbc(blob, size));
        ASSERT_TRUE(result.valid()) << result.failure;
        MslCompileOptions options{};
        options.argument_buffers = true;
        options.argument_buffer_sets[1] = {
            4, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 1,
            MslDescriptorClass::Texture};
        const auto compiled = compile_spirv_to_msl(
            result.spirv.data(), result.spirv.size(), ShaderStage::Fragment,
            options);
        ASSERT_TRUE(compiled.valid());
        const std::string msl(compiled.msl_text());
        EXPECT_NE(msl.find(index_of(base, slot, kind)), std::string::npos)
            << msl;
    }
}
