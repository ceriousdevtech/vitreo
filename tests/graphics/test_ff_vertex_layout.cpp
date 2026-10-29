/// @file test_ff_vertex_layout.cpp
/// @brief FVF -> backend vertex layout.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <graphics/shaders/ff_vertex_layout.hpp>
#include <graphics/shaders/ff_emulation.hpp>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

// D3DFVF_TEXCOORDSIZEn(i) as MSDN defines them, so the tests exercise the
// documented encoding rather than this implementation's reading of it.
constexpr u32 texcoordsize1(u32 i) { return 3u << (i * 2 + 16); }
constexpr u32 texcoordsize3(u32 i) { return 1u << (i * 2 + 16); }
constexpr u32 texcoordsize4(u32 i) { return 2u << (i * 2 + 16); }

} // namespace

TEST(FFVertexLayout, TexCoordSizeEncodingIsNotOrdinal) {
    // SIZE2 is the zero case; a set that names no size is two floats.
    EXPECT_EQ(ff_fvf_texcoord_components(0, 0), 2u);
    EXPECT_EQ(ff_fvf_texcoord_components(texcoordsize1(0), 0), 1u);
    EXPECT_EQ(ff_fvf_texcoord_components(texcoordsize3(0), 0), 3u);
    EXPECT_EQ(ff_fvf_texcoord_components(texcoordsize4(0), 0), 4u);
    // Per-set, not global.
    const u32 fvf = texcoordsize4(1);
    EXPECT_EQ(ff_fvf_texcoord_components(fvf, 0), 2u);
    EXPECT_EQ(ff_fvf_texcoord_components(fvf, 1), 4u);
}

TEST(FFVertexLayout, PositionNormalDiffuseTexHasFvfMemoryOrder) {
    const u32 fvf = fvf::XYZ | fvf::NORMAL | fvf::DIFFUSE | (1u << fvf::TEXCOUNT_SHIFT);
    std::vector<FFVertexElement> elements;
    u32 stride = 0;
    const char* rejection = nullptr;
    ASSERT_TRUE(ff_enumerate_fvf_elements(fvf, elements, stride, rejection))
        << (rejection ? rejection : "");
    ASSERT_EQ(elements.size(), 4u);

    EXPECT_EQ(elements[0].usage, ff_usage::POSITION);
    EXPECT_EQ(elements[0].offset, 0u);
    EXPECT_EQ(elements[1].usage, ff_usage::NORMAL);
    EXPECT_EQ(elements[1].offset, 12u);
    EXPECT_EQ(elements[2].usage, ff_usage::COLOR);
    // D3DCOLOR is BGRA in memory; a plain RGBA format would swap red and blue.
    EXPECT_EQ(elements[2].format, BackendVertexFormat::UByte4NormalizedBGRA);
    EXPECT_EQ(elements[2].offset, 24u);
    EXPECT_EQ(elements[3].usage, ff_usage::TEXCOORD);
    EXPECT_EQ(elements[3].offset, 28u);
    EXPECT_EQ(stride, 36u);  // 12 + 12 + 4 + 8
}

TEST(FFVertexLayout, SpecularFollowsDiffuse) {
    const u32 fvf = fvf::XYZ | fvf::DIFFUSE | fvf::SPECULAR;
    std::vector<FFVertexElement> elements;
    u32 stride = 0;
    const char* rejection = nullptr;
    ASSERT_TRUE(ff_enumerate_fvf_elements(fvf, elements, stride, rejection));
    ASSERT_EQ(elements.size(), 3u);
    EXPECT_EQ(elements[1].usage_index, 0u);
    EXPECT_EQ(elements[2].usage_index, 1u);
    EXPECT_EQ(stride, 20u);
}

TEST(FFVertexLayout, TransformedPositionUsesPositionT) {
    std::vector<FFVertexElement> elements;
    u32 stride = 0;
    const char* rejection = nullptr;
    ASSERT_TRUE(ff_enumerate_fvf_elements(fvf::XYZRHW, elements, stride, rejection));
    ASSERT_EQ(elements.size(), 1u);
    EXPECT_EQ(elements[0].usage, ff_usage::POSITIONT);
    EXPECT_EQ(stride, 16u);
}

TEST(FFVertexLayout, TransformedPositionRejectsNormal) {
    // Direct3D refuses to light a screen-space vertex rather than ignoring
    // the normal.
    std::vector<FFVertexElement> elements;
    u32 stride = 0;
    const char* rejection = nullptr;
    EXPECT_FALSE(ff_enumerate_fvf_elements(fvf::XYZRHW | fvf::NORMAL, elements,
                                           stride, rejection));
    EXPECT_NE(rejection, nullptr);
    EXPECT_EQ(stride, 0u);
    EXPECT_TRUE(elements.empty());
}

TEST(FFVertexLayout, BlendWeightsAndLastBetaIndices) {
    // XYZB4 with LASTBETA_UBYTE4: three float weights then packed indices.
    const u32 fvf = fvf::XYZB4 | fvf::LASTBETA_UBYTE4;
    std::vector<FFVertexElement> elements;
    u32 stride = 0;
    const char* rejection = nullptr;
    ASSERT_TRUE(ff_enumerate_fvf_elements(fvf, elements, stride, rejection));
    ASSERT_EQ(elements.size(), 5u);
    EXPECT_EQ(elements[1].usage, ff_usage::BLENDWEIGHT);
    EXPECT_EQ(elements[3].usage, ff_usage::BLENDWEIGHT);
    EXPECT_EQ(elements[4].usage, ff_usage::BLENDINDICES);
    EXPECT_EQ(elements[4].format, BackendVertexFormat::UByte4);
    EXPECT_EQ(stride, 28u);  // 12 + 3*4 + 4
}

TEST(FFVertexLayout, ShaderLocationsFollowFvfOrder) {
    // generate_ff_shaders() numbers [[attribute(n)]] in FVF memory order.
    const u32 fvf = fvf::XYZ | fvf::NORMAL | fvf::DIFFUSE;
    std::vector<BackendVertexAttribute> attributes;
    u32 stride = 0;
    const char* rejection = nullptr;
    ASSERT_TRUE(ff_build_vertex_layout(fvf, attributes, stride, rejection));
    ASSERT_EQ(attributes.size(), 3u);
    for (unsigned i = 0; i < attributes.size(); ++i) {
        EXPECT_EQ(attributes[i].shader_location, i);
        EXPECT_EQ(attributes[i].buffer_index, 0u);
    }
    EXPECT_EQ(attributes[2].offset, 24u);
    EXPECT_EQ(stride, 28u);
}

TEST(FFVertexLayout, RejectsUnsupportedPositionMask) {
    std::vector<BackendVertexAttribute> attributes;
    u32 stride = 0;
    const char* rejection = nullptr;
    EXPECT_FALSE(ff_build_vertex_layout(0, attributes, stride, rejection));
    EXPECT_NE(rejection, nullptr);
    EXPECT_TRUE(attributes.empty());
}
