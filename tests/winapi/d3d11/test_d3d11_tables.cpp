// SPDX-License-Identifier: LGPL-2.1-only
//
// Pure table contracts: vertex-format mapping/packing, the query-type enum numbering and the standard tile shapes.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_common.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST(D3D11VertexFormatTest, MapsEveryNativeDxgiVertexFormatExactly) {
    struct Expected {
        u32 dxgi;
        graphics::BackendVertexFormat backend;
        u8 bytes;
        d3d11::VertexComponentKind kind;
    };
    constexpr std::array expected{
        Expected{2, graphics::BackendVertexFormat::Float4, 16, d3d11::VertexComponentKind::Float},
        Expected{3, graphics::BackendVertexFormat::UInt4, 16, d3d11::VertexComponentKind::UInt},
        Expected{4, graphics::BackendVertexFormat::SInt4, 16, d3d11::VertexComponentKind::SInt},
        Expected{6, graphics::BackendVertexFormat::Float3, 12, d3d11::VertexComponentKind::Float},
        Expected{7, graphics::BackendVertexFormat::UInt3, 12, d3d11::VertexComponentKind::UInt},
        Expected{8, graphics::BackendVertexFormat::SInt3, 12, d3d11::VertexComponentKind::SInt},
        Expected{10, graphics::BackendVertexFormat::Half4, 8, d3d11::VertexComponentKind::Float},
        Expected{11, graphics::BackendVertexFormat::UShort4Normalized, 8, d3d11::VertexComponentKind::UNorm},
        Expected{12, graphics::BackendVertexFormat::UShort4, 8, d3d11::VertexComponentKind::UInt},
        Expected{13, graphics::BackendVertexFormat::Short4Normalized, 8, d3d11::VertexComponentKind::SNorm},
        Expected{14, graphics::BackendVertexFormat::Short4, 8, d3d11::VertexComponentKind::SInt},
        Expected{16, graphics::BackendVertexFormat::Float2, 8, d3d11::VertexComponentKind::Float},
        Expected{17, graphics::BackendVertexFormat::UInt2, 8, d3d11::VertexComponentKind::UInt},
        Expected{18, graphics::BackendVertexFormat::SInt2, 8, d3d11::VertexComponentKind::SInt},
        Expected{24, graphics::BackendVertexFormat::UInt1010102Normalized, 4, d3d11::VertexComponentKind::UNorm},
        // IA-required at FL11 (Windows: ia_support=1, fetches
        // 1023 1023 1023 3 from all-ones, d3d11_a07_ia_probe `C fmt 25`).
        Expected{25, graphics::BackendVertexFormat::UInt1010102, 4, d3d11::VertexComponentKind::UInt},
        Expected{26, graphics::BackendVertexFormat::FloatRG11B10, 4, d3d11::VertexComponentKind::Float},
        Expected{28, graphics::BackendVertexFormat::UByte4Normalized, 4, d3d11::VertexComponentKind::UNorm},
        Expected{30, graphics::BackendVertexFormat::UByte4, 4, d3d11::VertexComponentKind::UInt},
        Expected{31, graphics::BackendVertexFormat::SByte4Normalized, 4, d3d11::VertexComponentKind::SNorm},
        Expected{32, graphics::BackendVertexFormat::SByte4, 4, d3d11::VertexComponentKind::SInt},
        Expected{34, graphics::BackendVertexFormat::Half2, 4, d3d11::VertexComponentKind::Float},
        Expected{35, graphics::BackendVertexFormat::UShort2Normalized, 4, d3d11::VertexComponentKind::UNorm},
        Expected{36, graphics::BackendVertexFormat::UShort2, 4, d3d11::VertexComponentKind::UInt},
        Expected{37, graphics::BackendVertexFormat::Short2Normalized, 4, d3d11::VertexComponentKind::SNorm},
        Expected{38, graphics::BackendVertexFormat::Short2, 4, d3d11::VertexComponentKind::SInt},
        Expected{41, graphics::BackendVertexFormat::Float1, 4, d3d11::VertexComponentKind::Float},
        Expected{42, graphics::BackendVertexFormat::UInt1, 4, d3d11::VertexComponentKind::UInt},
        Expected{43, graphics::BackendVertexFormat::SInt1, 4, d3d11::VertexComponentKind::SInt},
        Expected{49, graphics::BackendVertexFormat::UByte2Normalized, 2, d3d11::VertexComponentKind::UNorm},
        Expected{50, graphics::BackendVertexFormat::UByte2, 2, d3d11::VertexComponentKind::UInt},
        Expected{51, graphics::BackendVertexFormat::SByte2Normalized, 2, d3d11::VertexComponentKind::SNorm},
        Expected{52, graphics::BackendVertexFormat::SByte2, 2, d3d11::VertexComponentKind::SInt},
        Expected{54, graphics::BackendVertexFormat::Half1, 2, d3d11::VertexComponentKind::Float},
        // dxgiformat.h: R16_UNORM 56, R16_UINT 57, R16_SNORM 58, R16_SINT 59
        // (55 is D16_UNORM). This table was one low, like the one it tests.
        Expected{56, graphics::BackendVertexFormat::UShort1Normalized, 2, d3d11::VertexComponentKind::UNorm},
        Expected{57, graphics::BackendVertexFormat::UShort1, 2, d3d11::VertexComponentKind::UInt},
        Expected{58, graphics::BackendVertexFormat::Short1Normalized, 2, d3d11::VertexComponentKind::SNorm},
        Expected{59, graphics::BackendVertexFormat::Short1, 2, d3d11::VertexComponentKind::SInt},
        Expected{61, graphics::BackendVertexFormat::UByte1Normalized, 1, d3d11::VertexComponentKind::UNorm},
        Expected{62, graphics::BackendVertexFormat::UByte1, 1, d3d11::VertexComponentKind::UInt},
        Expected{63, graphics::BackendVertexFormat::SByte1Normalized, 1, d3d11::VertexComponentKind::SNorm},
        Expected{64, graphics::BackendVertexFormat::SByte1, 1, d3d11::VertexComponentKind::SInt},
        Expected{87, graphics::BackendVertexFormat::UByte4NormalizedBGRA, 4, d3d11::VertexComponentKind::UNorm},
    };
    static_assert(expected.size() == d3d11::kVertexFormats.size());
    for (const auto& value : expected) {
        const auto* info = d3d11::vertex_format_info(value.dxgi);
        ASSERT_NE(info, nullptr) << value.dxgi;
        EXPECT_EQ(info->backend_format, value.backend) << value.dxgi;
        EXPECT_EQ(info->byte_size, value.bytes) << value.dxgi;
        EXPECT_EQ(info->component_kind, value.kind) << value.dxgi;
        EXPECT_EQ(d3d11::vertex_format_from_dxgi(value.dxgi), value.backend);
        EXPECT_EQ(d3d11::vertex_format_stride(value.backend), value.bytes);
        EXPECT_EQ(d3d11::vertex_format_stride_from_dxgi(value.dxgi),
                  value.bytes);
        const auto storage_type = value.kind == d3d11::VertexComponentKind::UInt
            ? graphics::BackendVertexComponentType::UInt
            : value.kind == d3d11::VertexComponentKind::SInt
                ? graphics::BackendVertexComponentType::SInt
                : graphics::BackendVertexComponentType::Float;
        EXPECT_EQ(d3d11::vertex_storage_component_type(value.dxgi),
                  storage_type) << value.dxgi;
    }
}

TEST(D3D11VertexFormatTest, RejectsFormatsWithoutExactMetalVertexFetch) {
    // 55 D16_UNORM and 67 R9G9B9E5_SHAREDEXP have no IA vertex-buffer support
    // in Learn's FL11 format-support table. (25 R10G10B10A2_UINT does: it is
    // mapped, measured on Windows in d3d11_a07_ia_probe `C fmt 25`.)
    constexpr std::array unsupported{0u, 1u, 23u, 29u, 40u, 45u,
                                     55u, 67u, 88u, 89u, 99u};
    for (u32 dxgi : unsupported) {
        EXPECT_EQ(d3d11::vertex_format_info(dxgi), nullptr) << dxgi;
        EXPECT_EQ(d3d11::vertex_format_from_dxgi(dxgi),
                  graphics::BackendVertexFormat::Invalid) << dxgi;
        EXPECT_EQ(d3d11::vertex_format_stride_from_dxgi(dxgi), 0u) << dxgi;
    }
}

TEST(D3D11VertexFormatTest, PacksAppendOffsetsAndExtentsPerInputSlot) {
    d3d11::VertexLayoutPackingState packing{};
    u32 offset = 0;
    ASSERT_TRUE(d3d11::resolve_vertex_element_offset(0, 0, 16, packing,
                                                     offset));
    EXPECT_EQ(offset, 0u);
    ASSERT_TRUE(d3d11::resolve_vertex_element_offset(
        1, 4, 4, packing, offset));
    EXPECT_EQ(offset, 4u);
    ASSERT_TRUE(d3d11::resolve_vertex_element_offset(
        0, d3d11::kAppendAlignedElement, 4, packing, offset));
    EXPECT_EQ(offset, 16u);
    ASSERT_TRUE(d3d11::resolve_vertex_element_offset(
        1, d3d11::kAppendAlignedElement, 8, packing, offset));
    EXPECT_EQ(offset, 8u);
    ASSERT_TRUE(d3d11::resolve_vertex_element_offset(0, 4, 2, packing,
                                                     offset));
    ASSERT_TRUE(d3d11::resolve_vertex_element_offset(
        0, d3d11::kAppendAlignedElement, 2, packing, offset));
    EXPECT_EQ(offset, 6u);
    EXPECT_EQ(packing.slot_extent[0], 20u);
    EXPECT_EQ(packing.slot_extent[1], 16u);
    EXPECT_FALSE(d3d11::resolve_vertex_element_offset(
        32, 0, 4, packing, offset));
    EXPECT_FALSE(d3d11::resolve_vertex_element_offset(
        0, std::numeric_limits<u32>::max() - 1, 4, packing, offset));
}

// ---------------------------------------------------------------------------
// Query types
// ---------------------------------------------------------------------------

TEST(D3D11Query, EnumNumberingAndDataSizesMatchTheSdkAndWindows) {
    using namespace vitreo::winapi;
    // d3d11.h (mingw-w64 and Windows Kits 10.0.26100): the per-stream values
    // interleave, 8 SO_STATISTICS_STREAM0, 9 SO_OVERFLOW_PREDICATE_STREAM0,
    // 10 SO_STATISTICS_STREAM1 ... 15 SO_OVERFLOW_PREDICATE_STREAM3. Vitreo
    // used to read 8..11 as statistics streams 0..3 and 12..15 as overflow
    // predicates, so SO_OVERFLOW_PREDICATE_STREAM0 was a 16-byte statistics
    // object (tools/audit_d3d11_query_constants.py checks the constants).
    EXPECT_EQ(D3D11_QUERY_SO_STATISTICS_STREAM0, 8u);
    EXPECT_EQ(D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0, 9u);
    EXPECT_EQ(D3D11_QUERY_SO_STATISTICS_STREAM3, 14u);
    EXPECT_EQ(D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3, 15u);
    const std::array<int, 16> stream{-2, -2, -2, -2, -2, -2, -1, -1,
                                     0,  0,  1,  1,  2,  2,  3,  3};
    // d3d11_a10_queries_probe [create]: GetDataSize per type, and which types
    // answer QueryInterface(ID3D11Predicate) (made by CreateQuery).
    const std::array<u32, 16> size{4, 8, 8, 16, 88, 4, 16, 4,
                                   16, 4, 16, 4, 16, 4, 16, 4};
    const std::array<bool, 16> predicate{false, false, false, false,
                                         false, true,  false, true,
                                         false, true,  false, true,
                                         false, true,  false, true};
    // d3d11_a10_query_rules_probe [create1]: CreateQuery1 with a COMPUTE,
    // COPY or VIDEO context is refused from PIPELINE_STATISTICS (4) on.
    for (u32 type = 0; type < 16; ++type) {
        EXPECT_TRUE(query_type_valid(type)) << type;
        EXPECT_EQ(stream_out_query_stream(type), stream[type]) << type;
        EXPECT_EQ(query_data_size(type), size[type]) << type;
        EXPECT_EQ(query_type_is_predicate(type), predicate[type]) << type;
        EXPECT_EQ(query_type_needs_3d_context(type), type >= 4) << type;
    }
    EXPECT_FALSE(query_type_valid(16));
    EXPECT_FALSE(query_type_valid(0xFFFFFFFFu));
    EXPECT_EQ(query_data_size(16), 0u);
}

TEST(D3D11FormatCaps, StandardTileShapesAreTheMeasuredOnes) {
    // "shape=" of d3d11_tiled_probe64.expected [tiling] (WARP): 8, 16, 32,
    // 64, 128 bits per texel, BC 64- and 128-bit blocks, and 4x.
    struct Case {
        u32 format, samples, w, h;
    };
    const Case cases[] = {
        {61, 1, 256, 256},  // r8
        {56, 1, 256, 128},  // r16
        {28, 1, 128, 128},  // rgba8
        {16, 1, 128, 64},   // rg32f
        {10, 1, 128, 64},   // rgba16f
        {2, 1, 64, 64},     // rgba32f
        {71, 1, 512, 256},  // bc1
        {80, 1, 512, 256},  // bc4
        {77, 1, 256, 256},  // bc3
        {98, 1, 256, 256},  // bc7
        {40, 1, 128, 128},  // d32f
        {55, 1, 256, 128},  // d16
        {28, 4, 64, 64},    // rgba8 msaa4
        {61, 4, 128, 128},  // r8 msaa4
        {10, 4, 64, 32},    // rgba16f msaa4
        {40, 4, 64, 64},    // d32f msaa4
    };
    for (const Case& c : cases) {
        u32 w = 0, h = 0;
        EXPECT_TRUE(d3d11::standard_tile_shape(c.format, c.samples, w, h))
            << c.format;
        EXPECT_EQ(w, c.w) << c.format << " x" << c.samples;
        EXPECT_EQ(h, c.h) << c.format << " x" << c.samples;
    }
    u32 w = 0, h = 0;
    EXPECT_FALSE(d3d11::standard_tile_shape(28, 2, w, h)) << "Tier 1: 1x, 4x";
    EXPECT_FALSE(d3d11::standard_tile_shape(71, 4, w, h));
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
