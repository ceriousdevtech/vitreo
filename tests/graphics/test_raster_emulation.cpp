// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_raster_emulation.cpp
/// @brief Target-independent / conservative rasterization: the standard
///        sample patterns against the oracle's measurement, and the pixel
///        shaders of d3d12_a31_raster_probe translated for the emulation.

#include <gtest/gtest.h>

#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/raster_emulation_types.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"

#include "../oracle_data/d3d12_a31_raster_shaders.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

/// The positions a `samplepos texture Nx` row of the WARP baseline lists,
/// in 1/16 pixel.
std::vector<std::pair<int, int>> oracle_texture_pattern(u32 count) {
    std::ifstream in(std::string(VITREO_SOURCE_DIR) +
                     "/tests/oracle_data/d3d12_a31_raster_probe.expected");
    const std::string key =
        "samplepos texture " + std::to_string(count) + "x: count=";
    std::string line;
    std::vector<std::pair<int, int>> out;
    while (std::getline(in, line)) {
        if (line.rfind(key, 0) != 0) continue;
        const char* p = line.c_str() + key.size();
        while ((p = std::strchr(p, '(')) != nullptr) {
            float x = 0, y = 0;
            if (std::sscanf(p, "(%f,%f)", &x, &y) != 2) break;
            out.emplace_back(static_cast<int>(x * 16.0f),
                             static_cast<int>(y * 16.0f));
            ++p;
        }
    }
    return out;
}

std::string translate(const unsigned char* blob, usize size,
                      const RasterEmulationOptions& raster, bool& ok,
                      std::string& failure) {
    const auto container = parse_dxbc(blob, size);
    EXPECT_TRUE(container.valid());
    DxbcTranslationOptions options{};
    options.raster_emulation = raster;
    const auto result = translate_dxbc_to_spirv(container, options);
    ok = result.valid();
    failure = result.failure;
    if (!ok) return {};
    MslCompileOptions msl{};
    msl.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), ShaderStage::Fragment, msl);
    EXPECT_TRUE(compiled.valid());
    return compiled.valid() ? std::string(compiled.msl_text()) : std::string();
}

}  // namespace

// D3D11.3 19.2.4 defines the patterns as pictures; the table is checked
// against what Windows reports (WARP, which also has 16x textures).
TEST(RasterEmulationTest, StandardPatternsAreTheOracleTexturePositions) {
    for (const u32 count : {1u, 2u, 4u, 8u, 16u}) {
        const StandardSample* pattern = standard_sample_pattern(count);
        ASSERT_NE(pattern, nullptr) << count;
        if (count == 1) {
            EXPECT_EQ(pattern[0].x, 0);
            EXPECT_EQ(pattern[0].y, 0);
            continue;
        }
        const auto oracle = oracle_texture_pattern(count);
        ASSERT_EQ(oracle.size(), count) << "no oracle row for " << count << "x";
        for (u32 s = 0; s < count; ++s) {
            EXPECT_EQ(pattern[s].x, oracle[s].first) << count << "x sample " << s;
            EXPECT_EQ(pattern[s].y, oracle[s].second) << count << "x sample " << s;
        }
    }
    EXPECT_EQ(standard_sample_pattern(3), nullptr);
    EXPECT_EQ(standard_sample_pattern(32), nullptr);
}

// The probe's SV_Coverage shader under every forced pattern: the prologue
// reads pass C's edges and answers vCoverage itself.
TEST(RasterEmulationTest, CoverageShaderTranslatesForEveryPattern) {
    for (const u32 samples : {1u, 4u, 8u, 16u}) {
        RasterEmulationOptions raster{};
        raster.mode = RasterEmulationMode::TargetIndependent;
        raster.samples = samples;
        bool ok = false;
        std::string failure;
        const std::string msl = translate(RSHADER(ps_coverage_uav), raster, ok,
                                          failure);
        ASSERT_TRUE(ok) << failure;
        EXPECT_NE(msl.find("vitreo_edge0"), std::string::npos);
        EXPECT_NE(msl.find("locn38"), std::string::npos);
        EXPECT_NE(msl.find("long"), std::string::npos) << msl;
    }
    RasterEmulationOptions none{};
    none.mode = RasterEmulationMode::TargetIndependent;
    none.samples = 2;  // a standard pattern, though no forced count uses it
    bool ok = false;
    std::string failure;
    (void)translate(RSHADER(ps_coverage_uav), none, ok, failure);
    EXPECT_TRUE(ok) << failure;
    none.samples = 3;
    (void)translate(RSHADER(ps_coverage_uav), none, ok, failure);
    EXPECT_FALSE(ok);
}

// SV_InnerCoverage exists only under conservative rasterization (the
// runtime refuses it otherwise: d3d12_a31_raster_probe `pso inner cr=off`).
TEST(RasterEmulationTest, InnerCoverageNeedsConservativeRasterization) {
    RasterEmulationOptions off{};
    bool ok = true;
    std::string failure;
    (void)translate(RSHADER(ps_inner_uav), off, ok, failure);
    EXPECT_FALSE(ok);
    RasterEmulationOptions conservative{};
    conservative.mode = RasterEmulationMode::Conservative;
    conservative.samples = 1;
    const std::string msl =
        translate(RSHADER(ps_inner_uav), conservative, ok, failure);
    ASSERT_TRUE(ok) << failure;
    EXPECT_NE(msl.find("vitreo_inner_coverage"), std::string::npos);
}
