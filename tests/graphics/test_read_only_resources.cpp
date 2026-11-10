// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/graphics/d3d11/d3d11_read_only_resources.hpp"
#include <gtest/gtest.h>
#include "pixel_uav_shaders.h"
#include "register_gap_shaders.h"
#include "dxbc_to_spirv_test_shaders.h"
#include "../oracle_data/d3d12_sm51/d3d12_sm51_shaders.h"
#include <map>
#include <set>

using namespace vitreo;
using namespace vitreo::graphics;
using namespace vitreo::winapi::d3d11;

TEST(ReadOnlyIndirectResources, RequiresSuccessfulOriginalGraphicsTranslation) {
    DxbcToSpirvResult result{};
    EXPECT_FALSE(original_dxbc_resources_read_only(result));
    result.ok = true;
    EXPECT_FALSE(original_dxbc_resources_read_only(result));
    result.spirv = {1}; // The helper requires a successful nonempty result.
    for (auto stage : {ShaderStage::Vertex, ShaderStage::Fragment}) {
        result.stage = stage;
        EXPECT_TRUE(original_dxbc_resources_read_only(result));
    }
    result.stage = ShaderStage::Compute;
    EXPECT_FALSE(original_dxbc_resources_read_only(result));
    result.stage = ShaderStage::Vertex;
    result.capture_registers = {0};
    EXPECT_FALSE(original_dxbc_resources_read_only(result));
}

TEST(ReadOnlyIndirectResources, RejectsEveryUavShapeRangeAndSpace) {
    DxbcToSpirvResult result{};
    result.ok = true; result.spirv = {1}; result.stage = ShaderStage::Vertex;
    // Classification is independent of dimension/opcode: typed textures,
    // typed buffers, raw and structured buffers all carry UnorderedAccess.
    for (u32 dimension : {0u, 1u, 2u, 3u, 7u, 8u}) {
        for (u32 space : {0u, 1u, 47u}) {
            DxbcResolvedBinding binding{};
            binding.declaration.register_class = DxbcRegisterClass::UnorderedAccess;
            binding.declaration.dimension = dimension;
            binding.declaration.space = space;
            binding.declaration.upper = kDxbcUnboundedRange;
            result.bindings = {binding};
            EXPECT_FALSE(original_dxbc_resources_read_only(result));
        }
    }
    result.bindings.clear();
    result.uav_counter_registers = {3};
    EXPECT_FALSE(original_dxbc_resources_read_only(result));
    result.uav_counter_registers.clear(); result.uav_counter_spaces = {2};
    EXPECT_FALSE(original_dxbc_resources_read_only(result));
}

TEST(ReadOnlyIndirectResources, VertexInputSpecializationKeepsTheTableProof) {
    EXPECT_TRUE(descriptor_table_matches_original_proof(
        false, true, 0, 0, 0, 0, false, true));
    EXPECT_FALSE(descriptor_table_matches_original_proof(
        true, true, 0, 0, 0, 0, false, true));
    EXPECT_FALSE(descriptor_table_matches_original_proof(
        false, false, 1, 0, 0, 0, false, true));
    EXPECT_FALSE(descriptor_table_matches_original_proof(
        false, false, 0, 0, 0, 0, false, false));
}

TEST(ReadOnlyIndirectResources, EveryFinalRouteFactIsRequired) {
    EXPECT_FALSE(ReadOnlyIndirectDrawProof{}.valid());
    ReadOnlyIndirectDrawProof proof{true,true,true,true,true,true,true};
    ASSERT_TRUE(proof.valid());
    bool ReadOnlyIndirectDrawProof::* const fields[] = {
        &ReadOnlyIndirectDrawProof::vertex_shader,
        &ReadOnlyIndirectDrawProof::pixel_shader,
        &ReadOnlyIndirectDrawProof::original_shaders,
        &ReadOnlyIndirectDrawProof::ordinary_stages,
        &ReadOnlyIndirectDrawProof::no_stream_output,
        &ReadOnlyIndirectDrawProof::no_statistics,
        &ReadOnlyIndirectDrawProof::no_injected_writes};
    for (auto field : fields) { auto missing = proof; missing.*field = false;
        EXPECT_FALSE(missing.valid()); }
}

// These are actual fxc-produced Windows oracle blobs, not optimized/reflected
// masks. The same classifier handles vertex UAVs and every writable form.
TEST(ReadOnlyIndirectResources, ActualTranslatedOracleShaders) {
    struct Blob { const unsigned char* data; usize size; };
    const Blob writable[] = {
        {pixel_uav_ps_typed_texture, sizeof(pixel_uav_ps_typed_texture)},
        {pixel_uav_ps_typed_buffer, sizeof(pixel_uav_ps_typed_buffer)},
        {pixel_uav_ps_raw_structured, sizeof(pixel_uav_ps_raw_structured)},
        {pixel_uav_ps_early_coherent, sizeof(pixel_uav_ps_early_coherent)}};
    for (const auto& blob : writable) {
        const auto result = translate_dxbc_to_spirv(parse_dxbc(blob.data, blob.size));
        ASSERT_TRUE(result.valid()) << result.failure;
        EXPECT_FALSE(original_dxbc_resources_read_only(result));
    }
    // Feature level 11_0 currently refuses vertex UAVs. A refused translation
    // must never create a positive certificate either.
    const auto refused_vs = translate_dxbc_to_spirv(parse_dxbc(
        pixel_uav_vs_uav, sizeof(pixel_uav_vs_uav)));
    ASSERT_FALSE(refused_vs.valid());
    EXPECT_FALSE(original_dxbc_resources_read_only(refused_vs));
    const auto vs = dxbc_spirv_test::create_minimal_vertex_shader_dxbc();
    const auto translated_vs = translate_dxbc_to_spirv(parse_dxbc(vs.data(), vs.size()));
    ASSERT_TRUE(translated_vs.valid()) << translated_vs.failure;
    EXPECT_TRUE(original_dxbc_resources_read_only(translated_vs));
    const auto translated_ps = translate_dxbc_to_spirv(parse_dxbc(ps_t3_s2, sizeof(ps_t3_s2)));
    ASSERT_TRUE(translated_ps.valid()) << translated_ps.failure;
    EXPECT_TRUE(original_dxbc_resources_read_only(translated_ps));
}

TEST(ReadOnlyIndirectResources, ActualNonzeroSpaceUavArraysStayWritable) {
    const auto container = parse_dxbc(sm51_cs_arrays, sizeof(sm51_cs_arrays));
    ASSERT_TRUE(container.valid());
    DxbcTranslationOptions options{};
    std::map<DxbcRegisterClass, std::set<u32>> spaces;
    for (const auto& declaration : container.binding_declarations)
        spaces[declaration.register_class].insert(declaration.space);
    u32 descriptor_set = 0;
    for (const auto& [kind, entries] : spaces) {
        u32 binding = 0;
        for (auto space : entries) {
            options.register_remap.push_back(
                {kind, space, 0u, kDxbcUnboundedRange, descriptor_set, binding});
            binding += 32;
        }
        ++descriptor_set;
    }
    const auto result = translate_dxbc_to_spirv(container, options);
    ASSERT_TRUE(result.valid()) << result.failure;
    bool nonzero_uav = false;
    for (const auto& binding : result.bindings)
        nonzero_uav |= binding.declaration.register_class ==
            DxbcRegisterClass::UnorderedAccess && binding.declaration.space != 0;
    ASSERT_TRUE(nonzero_uav);
    EXPECT_FALSE(original_dxbc_resources_read_only(result));
}
