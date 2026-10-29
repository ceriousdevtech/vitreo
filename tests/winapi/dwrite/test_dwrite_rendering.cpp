// SPDX-License-Identifier: LGPL-2.1-only

#include "dwrite_manager_harness.hpp"

#include <algorithm>
#include <array>
#include <cstring>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::dwrite_test;

TEST_F(DWriteManagerTest, CreateRenderingParamsExposesDefaultPrimaryMonitorValues) {
    const GuestAddr factory = create_factory();
    ASSERT_NE(factory, 0u);

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
    manager.impl_CreateRenderingParams(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr params = memory.read_u64(QUERY_OUTPUT_ADDRESS).value();
    ASSERT_NE(params, 0u);
    const GuestAddr vtable = memory.read_u64(params).value();
    EXPECT_EQ(memory.read_u64(vtable + 3 * 8).value(),
              RENDERING_PARAMS_GET_GAMMA_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 7 * 8).value(),
              RENDERING_PARAMS_GET_RENDERING_MODE_THUNK);

    cpu.gpr.rcx = params;
    manager.impl_RenderingParamsGetGamma(marshaler);
    float gamma = 0;
    std::memcpy(&gamma, &cpu.sse.xmm[0].u32_[0], sizeof(gamma));
    EXPECT_FLOAT_EQ(gamma, 1.8f);

    cpu.gpr.rcx = params;
    manager.impl_RenderingParamsGetEnhancedContrast(marshaler);
    float contrast = 0;
    std::memcpy(&contrast, &cpu.sse.xmm[0].u32_[0], sizeof(contrast));
    EXPECT_FLOAT_EQ(contrast, 0.5f);

    cpu.gpr.rcx = params;
    manager.impl_RenderingParamsGetClearTypeLevel(marshaler);
    float clear_type = 0;
    std::memcpy(&clear_type, &cpu.sse.xmm[0].u32_[0], sizeof(clear_type));
    EXPECT_FLOAT_EQ(clear_type, 1.0f);

    cpu.gpr.rcx = params;
    manager.impl_RenderingParamsGetPixelGeometry(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    cpu.gpr.rcx = params;
    manager.impl_RenderingParamsGetRenderingMode(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = 0x10000; // arbitrary HMONITOR
    cpu.gpr.r8 = QUERY_OUTPUT_ADDRESS;
    manager.impl_CreateMonitorRenderingParams(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    EXPECT_NE(memory.read_u64(QUERY_OUTPUT_ADDRESS).value(), 0u);
}

// Skia reaches IDWriteFactory2 and calls its CreateGlyphRunAnalysis overload,
// which drops pixelsPerDip and inserts gridFitMode / antialiasMode before the
// baseline origin (dwrite_2.h). Slot 30 must be wired and must read that
// layout: with the slot unimplemented, every text run fell back to
// GetGlyphRunOutline instead of the antialiased raster path.
TEST_F(DWriteManagerTest, Factory2GlyphRunAnalysisUsesVersion2ArgumentLayout) {
    const GuestAddr face = create_font_face();
    ASSERT_NE(face, 0u);
    const GuestAddr factory = create_factory();
    const GuestAddr factory_vtable = memory.read_u64(factory).value_or(0);
    ASSERT_EQ(memory.read_u64(factory_vtable + 30 * 8).value_or(0),
              CREATE_GLYPH_RUN_ANALYSIS2_THUNK);

    constexpr GuestAddr CODEPOINT = DATA_BASE + 0x500;
    constexpr GuestAddr GLYPH = DATA_BASE + 0x510;
    constexpr GuestAddr ADVANCE = DATA_BASE + 0x518;
    constexpr GuestAddr RUN = DATA_BASE + 0x540;
    constexpr GuestAddr ANALYSIS_OUT = DATA_BASE + 0x580;
    constexpr GuestAddr BOUNDS = DATA_BASE + 0x590;
    constexpr GuestAddr TEXTURE = DATA_BASE + 0x600;
    const auto float_bits = [](float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    };

    ASSERT_TRUE(memory.write_u32(CODEPOINT, 'L').is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = face;
    cpu.gpr.rdx = CODEPOINT;
    cpu.gpr.r8 = 1;
    cpu.gpr.r9 = GLYPH;
    manager.impl_FaceGetGlyphIndices(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    ASSERT_TRUE(memory.write_u32(ADVANCE, float_bits(12.0f)).is_ok());

    ASSERT_TRUE(memory.write_u64(RUN, face).is_ok());
    ASSERT_TRUE(memory.write_u32(RUN + 8, float_bits(20.0f)).is_ok());
    ASSERT_TRUE(memory.write_u32(RUN + 12, 1).is_ok());
    ASSERT_TRUE(memory.write_u64(RUN + 16, GLYPH).is_ok());
    ASSERT_TRUE(memory.write_u64(RUN + 24, ADVANCE).is_ok());
    ASSERT_TRUE(memory.write_u64(RUN + 32, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(RUN + 40, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(RUN + 44, 0).is_ok());

    // glyphRun, transform, renderingMode, measuringMode, gridFitMode,
    // antialiasMode, baselineOriginX, baselineOriginY, out
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = RUN;
    cpu.gpr.r8 = 0;   // transform
    cpu.gpr.r9 = 5;   // DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 0).is_ok());  // measuring
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, 2).is_ok());  // gridfit
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x38, 0).is_ok());  // cleartype
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x40, float_bits(0.0f)).is_ok());
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x48, float_bits(20.0f)).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x50, ANALYSIS_OUT).is_ok());
    manager.impl_CreateGlyphRunAnalysis2(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr analysis = memory.read_u64(ANALYSIS_OUT).value_or(0);
    ASSERT_NE(analysis, 0u);

    cpu.gpr.rcx = analysis;
    cpu.gpr.rdx = 1;  // DWRITE_TEXTURE_CLEARTYPE_3x1
    cpu.gpr.r8 = BOUNDS;
    manager.impl_AnalysisGetAlphaTextureBounds(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const i32 left = static_cast<i32>(memory.read_u32(BOUNDS).value());
    const i32 top = static_cast<i32>(memory.read_u32(BOUNDS + 4).value());
    const i32 right = static_cast<i32>(memory.read_u32(BOUNDS + 8).value());
    const i32 bottom = static_cast<i32>(memory.read_u32(BOUNDS + 12).value());
    ASSERT_GT(right, left);
    ASSERT_GT(bottom, top);
    const u32 texture_size =
        static_cast<u32>((right - left) * (bottom - top) * 3);
    ASSERT_LT(texture_size, 0x800u);

    cpu.gpr.rcx = analysis;
    cpu.gpr.rdx = 1;
    cpu.gpr.r8 = BOUNDS;
    cpu.gpr.r9 = TEXTURE;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, texture_size).is_ok());
    manager.impl_AnalysisCreateAlphaTexture(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    // The run must produce a solidly covered stem, not a uniformly faint
    // wash — the symptom that appears when a caller is pushed off this path.
    u8 max_coverage = 0;
    for (u32 index = 0; index < texture_size; ++index) {
        max_coverage =
            std::max(max_coverage, memory.read_u8(TEXTURE + index).value_or(0));
    }
    EXPECT_GE(max_coverage, 200)
        << "glyph raster peaked at " << static_cast<u32>(max_coverage);
}

// gridFitMode and antialiasMode are enums; out-of-range values are rejected
// and must not leave a stale pointer in the out parameter.
TEST_F(DWriteManagerTest, Factory2GlyphRunAnalysisRejectsBadModes) {
    const GuestAddr factory = create_factory();
    constexpr GuestAddr ANALYSIS_OUT = DATA_BASE + 0x580;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(memory.write_u64(ANALYSIS_OUT, 0xDEADBEEF).is_ok());
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = DATA_BASE + 0x540;
    cpu.gpr.r8 = 0;
    cpu.gpr.r9 = 5;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, 9).is_ok());  // bad gridfit
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x38, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x40, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x48, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x50, ANALYSIS_OUT).is_ok());
    manager.impl_CreateGlyphRunAnalysis2(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0x80070057u);  // E_INVALIDARG
    EXPECT_EQ(memory.read_u64(ANALYSIS_OUT).value_or(1), 0u);
}

TEST_F(DWriteManagerTest, GlyphRunAnalysisCreatesAntialiasedTexture) {
    const GuestAddr face = create_font_face();
    ASSERT_NE(face, 0u);
    const GuestAddr factory = create_factory();
    const GuestAddr factory_vtable = memory.read_u64(factory).value_or(0);
    ASSERT_EQ(memory.read_u64(factory_vtable + 23 * 8).value_or(0),
              CREATE_GLYPH_RUN_ANALYSIS_THUNK);

    constexpr GuestAddr CODEPOINT = DATA_BASE + 0x500;
    constexpr GuestAddr GLYPH = DATA_BASE + 0x510;
    constexpr GuestAddr ADVANCE = DATA_BASE + 0x518;
    constexpr GuestAddr RUN = DATA_BASE + 0x540;
    constexpr GuestAddr ANALYSIS_OUT = DATA_BASE + 0x580;
    constexpr GuestAddr BOUNDS = DATA_BASE + 0x590;
    constexpr GuestAddr TEXTURE = DATA_BASE + 0x600;
    const auto float_bits = [](float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    };

    ASSERT_TRUE(memory.write_u32(CODEPOINT, 'L').is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = face;
    cpu.gpr.rdx = CODEPOINT;
    cpu.gpr.r8 = 1;
    cpu.gpr.r9 = GLYPH;
    manager.impl_FaceGetGlyphIndices(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    ASSERT_TRUE(memory.write_u32(ADVANCE, float_bits(12.0f)).is_ok());

    ASSERT_TRUE(memory.write_u64(RUN, face).is_ok());
    ASSERT_TRUE(memory.write_u32(RUN + 8, float_bits(20.0f)).is_ok());
    ASSERT_TRUE(memory.write_u32(RUN + 12, 1).is_ok());
    ASSERT_TRUE(memory.write_u64(RUN + 16, GLYPH).is_ok());
    ASSERT_TRUE(memory.write_u64(RUN + 24, ADVANCE).is_ok());
    ASSERT_TRUE(memory.write_u64(RUN + 32, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(RUN + 40, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(RUN + 44, 0).is_ok());

    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = RUN;
    cpu.sse.xmm[2].u32_[0] = float_bits(1.0f);
    cpu.gpr.r9 = 0;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 5).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x38, float_bits(0.0f)).is_ok());
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x40, float_bits(20.0f)).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x48, ANALYSIS_OUT).is_ok());
    manager.impl_CreateGlyphRunAnalysis(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr analysis = memory.read_u64(ANALYSIS_OUT).value_or(0);
    ASSERT_NE(analysis, 0u);
    const GuestAddr analysis_vtable = memory.read_u64(analysis).value_or(0);
    EXPECT_EQ(memory.read_u64(analysis_vtable + 3 * 8).value_or(0),
              ANALYSIS_GET_ALPHA_TEXTURE_BOUNDS_THUNK);
    EXPECT_EQ(memory.read_u64(analysis_vtable + 4 * 8).value_or(0),
              ANALYSIS_CREATE_ALPHA_TEXTURE_THUNK);
    EXPECT_EQ(memory.read_u64(analysis_vtable + 5 * 8).value_or(0),
              ANALYSIS_GET_ALPHA_BLEND_PARAMS_THUNK);

    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
    manager.impl_CreateRenderingParams(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr params = memory.read_u64(QUERY_OUTPUT_ADDRESS).value_or(0);
    ASSERT_NE(params, 0u);
    constexpr GuestAddr GAMMA_OUT = DATA_BASE + 0x780;
    constexpr GuestAddr CONTRAST_OUT = DATA_BASE + 0x784;
    constexpr GuestAddr CLEAR_TYPE_OUT = DATA_BASE + 0x788;
    cpu.gpr.rcx = analysis;
    cpu.gpr.rdx = params;
    cpu.gpr.r8 = GAMMA_OUT;
    cpu.gpr.r9 = CONTRAST_OUT;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, CLEAR_TYPE_OUT).is_ok());
    manager.impl_AnalysisGetAlphaBlendParams(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    float gamma = 0;
    float contrast = 0;
    float clear_type = 0;
    const u32 gamma_bits = memory.read_u32(GAMMA_OUT).value_or(0);
    const u32 contrast_bits = memory.read_u32(CONTRAST_OUT).value_or(0);
    const u32 clear_type_bits = memory.read_u32(CLEAR_TYPE_OUT).value_or(0);
    std::memcpy(&gamma, &gamma_bits, sizeof(gamma));
    std::memcpy(&contrast, &contrast_bits, sizeof(contrast));
    std::memcpy(&clear_type, &clear_type_bits, sizeof(clear_type));
    EXPECT_FLOAT_EQ(gamma, 1.8f);
    EXPECT_FLOAT_EQ(contrast, 0.5f);
    EXPECT_FLOAT_EQ(clear_type, 1.0f);

    cpu.gpr.rcx = analysis;
    cpu.gpr.rdx = 1;
    cpu.gpr.r8 = BOUNDS;
    manager.impl_AnalysisGetAlphaTextureBounds(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const i32 left = static_cast<i32>(memory.read_u32(BOUNDS).value());
    const i32 top = static_cast<i32>(memory.read_u32(BOUNDS + 4).value());
    const i32 right = static_cast<i32>(memory.read_u32(BOUNDS + 8).value());
    const i32 bottom = static_cast<i32>(memory.read_u32(BOUNDS + 12).value());
    ASSERT_GT(right, left);
    ASSERT_GT(bottom, top);
    const u32 texture_size =
        static_cast<u32>((right - left) * (bottom - top) * 3);
    ASSERT_LT(texture_size, 0x800u);

    cpu.gpr.rcx = analysis;
    cpu.gpr.rdx = 1;
    cpu.gpr.r8 = BOUNDS;
    cpu.gpr.r9 = TEXTURE;
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x28, texture_size).is_ok());
    manager.impl_AnalysisCreateAlphaTexture(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    bool has_coverage = false;
    for (u32 index = 0; index < texture_size; ++index) {
        has_coverage |= memory.read_u8(TEXTURE + index).value_or(0) != 0;
    }
    EXPECT_TRUE(has_coverage);
    const u32 row_size = static_cast<u32>(right - left) * 3;
    u32 top_coverage = 0;
    u32 bottom_coverage = 0;
    for (u32 index = 0; index < row_size; ++index) {
        top_coverage +=
            memory.read_u8(TEXTURE + index).value_or(0) != 0 ? 1u : 0u;
        bottom_coverage += memory.read_u8(
            TEXTURE + texture_size - row_size + index).value_or(0) != 0
            ? 1u : 0u;
    }
    EXPECT_GT(bottom_coverage, top_coverage)
        << "DirectWrite alpha textures are top-down";
}

// Measured on real Windows (Segoe UI 12px "Learn more", NATURAL_SYMMETRIC +
// gridFit ENABLED + GRAYSCALE): GetAlphaTextureBounds(ALIASED_1x1) returns a
// Grayscale antialiasing produces one coverage byte per pixel, so the
// analysis answers the 1x1 texture type rather than CLEARTYPE_3x1.
TEST_F(DWriteManagerTest, GrayscaleAnalysisAnswersAliased1x1NotClearType) {
    const GuestAddr face = create_font_face();
    ASSERT_NE(face, 0u);
    const GuestAddr factory = create_factory();

    constexpr GuestAddr CODEPOINT = DATA_BASE + 0x500;
    constexpr GuestAddr GLYPH = DATA_BASE + 0x510;
    constexpr GuestAddr ADVANCE = DATA_BASE + 0x518;
    constexpr GuestAddr RUN = DATA_BASE + 0x540;
    constexpr GuestAddr ANALYSIS_OUT = DATA_BASE + 0x580;
    constexpr GuestAddr BOUNDS = DATA_BASE + 0x590;
    constexpr GuestAddr TEXTURE = DATA_BASE + 0x600;
    const auto float_bits = [](float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    };

    ASSERT_TRUE(memory.write_u32(CODEPOINT, 'L').is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = face;
    cpu.gpr.rdx = CODEPOINT;
    cpu.gpr.r8 = 1;
    cpu.gpr.r9 = GLYPH;
    manager.impl_FaceGetGlyphIndices(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    ASSERT_TRUE(memory.write_u32(ADVANCE, float_bits(12.0f)).is_ok());

    ASSERT_TRUE(memory.write_u64(RUN, face).is_ok());
    ASSERT_TRUE(memory.write_u32(RUN + 8, float_bits(20.0f)).is_ok());
    ASSERT_TRUE(memory.write_u32(RUN + 12, 1).is_ok());
    ASSERT_TRUE(memory.write_u64(RUN + 16, GLYPH).is_ok());
    ASSERT_TRUE(memory.write_u64(RUN + 24, ADVANCE).is_ok());
    ASSERT_TRUE(memory.write_u64(RUN + 32, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(RUN + 40, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(RUN + 44, 0).is_ok());

    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = RUN;
    cpu.gpr.r8 = 0;   // transform
    cpu.gpr.r9 = 5;   // DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 0).is_ok());  // measuring
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, 2).is_ok());  // gridfit
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x38, 1).is_ok());  // GRAYSCALE
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x40, float_bits(0.0f)).is_ok());
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x48, float_bits(20.0f)).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x50, ANALYSIS_OUT).is_ok());
    manager.impl_CreateGlyphRunAnalysis2(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr analysis = memory.read_u64(ANALYSIS_OUT).value_or(0);
    ASSERT_NE(analysis, 0u);

    const auto read_bounds = [&](u32 texture_type) {
        for (u32 i = 0; i < 4; ++i) {
            EXPECT_TRUE(memory.write_u32(BOUNDS + i * 4, 0xCCCCCCCC).is_ok());
        }
        cpu.gpr.rcx = analysis;
        cpu.gpr.rdx = texture_type;
        cpu.gpr.r8 = BOUNDS;
        manager.impl_AnalysisGetAlphaTextureBounds(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        std::array<i32, 4> rect{};
        for (u32 i = 0; i < 4; ++i) {
            rect[i] = static_cast<i32>(memory.read_u32(BOUNDS + i * 4).value());
        }
        return rect;
    };

    const auto aliased = read_bounds(0);  // DWRITE_TEXTURE_ALIASED_1x1
    EXPECT_GT(aliased[2], aliased[0]) << "grayscale run must fill ALIASED_1x1";
    EXPECT_GT(aliased[3], aliased[1]);

    const auto cleartype = read_bounds(1);  // DWRITE_TEXTURE_CLEARTYPE_3x1
    EXPECT_EQ(cleartype[2] - cleartype[0], 0)
        << "a grayscale analysis has no ClearType texture";
    EXPECT_EQ(cleartype[3] - cleartype[1], 0);

    // One byte per pixel, and the stem has to be solidly covered.
    for (u32 i = 0; i < 4; ++i) {
        ASSERT_TRUE(memory.write_u32(
            BOUNDS + i * 4, static_cast<u32>(aliased[i])).is_ok());
    }
    const u32 texture_size = static_cast<u32>(
        (aliased[2] - aliased[0]) * (aliased[3] - aliased[1]));
    ASSERT_GT(texture_size, 0u);
    ASSERT_LT(texture_size, 0x800u);
    cpu.gpr.rcx = analysis;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = BOUNDS;
    cpu.gpr.r9 = TEXTURE;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, texture_size).is_ok());
    manager.impl_AnalysisCreateAlphaTexture(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    u8 max_coverage = 0;
    for (u32 index = 0; index < texture_size; ++index) {
        max_coverage =
            std::max(max_coverage, memory.read_u8(TEXTURE + index).value_or(0));
    }
    EXPECT_GE(max_coverage, 200)
        << "grayscale raster peaked at " << static_cast<u32>(max_coverage);
}
