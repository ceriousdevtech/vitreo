// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_raster_emulation_metal.cpp
/// @brief The raster-emulation pass C (raster_emulation.hpp) must link with
///        the pixel shaders translated for it: matching user(locnN) indices
///        and types for the guest's varyings and the triangle's flat edges.
///        Only a Metal pipeline build checks that.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/raster_emulation.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"

#include "../oracle_data/d3d12_a31_raster_shaders.h"

#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

std::string pixel_msl(const unsigned char* blob, usize size,
                      const RasterEmulationOptions& raster) {
    const auto container = parse_dxbc(blob, size);
    DxbcTranslationOptions options{};
    options.raster_emulation = raster;
    options.line_quad_primitive_divisor = kRasterTrianglesPerPrimitive;
    const auto result = translate_dxbc_to_spirv(container, options);
    EXPECT_TRUE(result.valid()) << result.failure;
    if (!result.valid()) return {};
    MslCompileOptions msl{};
    msl.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment, msl);
    EXPECT_TRUE(compiled.valid());
    return compiled.valid() ? std::string(compiled.msl_text()) : std::string();
}

bool links(const std::string& vertex_msl, const std::string& fragment_msl,
           unsigned samples, u64 key) {
    BackendPipelineDesc desc{};
    desc.vertex_shader_msl = vertex_msl;
    desc.fragment_shader_msl = fragment_msl;
    desc.vertex_entry_point = kGsPassthroughEntryPoint;
    desc.fragment_entry_point = "main0";
    desc.vertex_attributes = {};
    desc.vertex_stride = 0;
    desc.color_format = PixelFormat::R32Uint;
    desc.sample_count = samples;
    desc.cache_key = key;
    auto pipeline = MetalDevice::instance().create_pipeline(desc);
    const bool ok = pipeline != nullptr && pipeline->valid();
    if (!ok) {
        ADD_FAILURE() << "Metal rejected the raster pass C pipeline.\n"
                      << "--- vertex ---\n" << vertex_msl
                      << "\n--- fragment ---\n" << fragment_msl;
    }
    return ok;
}

}  // namespace

// vs_pass's outputs (o0 SV_Position, o1 ATTR) drawn by every pass-C source
// -- vertex-capture record ids, geometry-expansion regions, a tessellator
// index list with the GPU's count -- with ps_coverage under each mode.
TEST(RasterEmulationMetalTest, PassCLinksWithEmulatedPixelShaders) {
    if (!MetalDevice::instance().available()) {
        GTEST_SKIP() << "no Metal device on this machine";
    }
    const std::vector<GsPassthroughOutput> outputs{
        {0u, 3u, /*is_position=*/true},
        {1u, 3u, false},
    };
    std::array<RasterVarying, 32> modes{};
    RasterEmulationSource ids{};
    RasterEmulationSource regions{};
    regions.vertices_per_invocation = 6;
    RasterEmulationSource tess{};
    tess.gpu_primitive_limit = true;
    u64 key = 0x5A57E500u;
    for (const RasterEmulationSource& source : {ids, regions, tess}) {
        const std::string vertex =
            generate_raster_emulation_vertex_msl(outputs, source, modes);
        ASSERT_FALSE(vertex.empty());
        RasterEmulationOptions tir{};
        tir.mode = RasterEmulationMode::TargetIndependent;
        tir.samples = 16;
        EXPECT_TRUE(links(vertex, pixel_msl(RSHADER(ps_coverage), tir), 1, ++key));
        RasterEmulationOptions centre{};
        centre.mode = RasterEmulationMode::TargetIndependent;
        centre.samples = 1;
        EXPECT_TRUE(
            links(vertex, pixel_msl(RSHADER(ps_coverage), centre), 4, ++key));
        RasterEmulationOptions conservative{};
        conservative.mode = RasterEmulationMode::Conservative;
        conservative.samples = 4;
        EXPECT_TRUE(links(vertex, pixel_msl(RSHADER(ps_coverage), conservative),
                          4, ++key));
    }
    // Every interpolation the pixel shader can ask for places attributes.
    for (const RasterVarying mode :
         {RasterVarying::NoPerspective, RasterVarying::Constant}) {
        modes[1] = mode;
        const std::string vertex =
            generate_raster_emulation_vertex_msl(outputs, ids, modes);
        RasterEmulationOptions tir{};
        tir.mode = RasterEmulationMode::TargetIndependent;
        tir.samples = 4;
        EXPECT_TRUE(links(vertex, pixel_msl(RSHADER(ps_coverage), tir), 1, ++key));
    }
}

TEST(RasterEmulationMetalTest, PassCRefusesRecordsWithNoPosition) {
    const std::array<RasterVarying, 32> modes{};
    EXPECT_TRUE(generate_raster_emulation_vertex_msl({{0u, 3u, false}},
                                                     RasterEmulationSource{},
                                                     modes)
                    .empty());
    EXPECT_TRUE(generate_raster_emulation_vertex_msl({}, RasterEmulationSource{},
                                                     modes)
                    .empty());
}

// A raster-emulated pipeline with no pixel shader (a conservative
// depth-only pass) runs the generated coverage-test fragment function.
TEST(RasterEmulationMetalTest, CoverageOnlyFragmentLinks) {
    if (!MetalDevice::instance().available()) {
        GTEST_SKIP() << "no Metal device on this machine";
    }
    const std::array<RasterVarying, 32> modes{};
    const std::string vertex = generate_raster_emulation_vertex_msl(
        {{0u, 3u, /*is_position=*/true}, {1u, 3u, false}},
        RasterEmulationSource{}, modes);
    ASSERT_FALSE(vertex.empty());
    RasterEmulationOptions tir{};
    tir.mode = RasterEmulationMode::TargetIndependent;
    tir.samples = 8;
    RasterEmulationOptions conservative{};
    conservative.mode = RasterEmulationMode::Conservative;
    conservative.samples = 1;
    u64 key = 0x5A57E5F0u;
    for (const RasterEmulationOptions& raster : {tir, conservative}) {
        const std::string fragment =
            generate_raster_emulation_fragment_msl(raster);
        ASSERT_FALSE(fragment.empty());
        BackendPipelineDesc desc{};
        desc.vertex_shader_msl = vertex;
        desc.fragment_shader_msl = fragment;
        desc.vertex_entry_point = kGsPassthroughEntryPoint;
        desc.fragment_entry_point = "main0";
        desc.color_format = PixelFormat::Unknown;
        desc.depth_attachment = true;
        desc.depth_format = PixelFormat::D32Float;
        desc.cache_key = ++key;
        auto pipeline = MetalDevice::instance().create_pipeline(desc);
        EXPECT_TRUE(pipeline != nullptr && pipeline->valid())
            << "--- fragment ---\n" << fragment;
    }
    EXPECT_TRUE(
        generate_raster_emulation_fragment_msl(RasterEmulationOptions{}).empty());
}
