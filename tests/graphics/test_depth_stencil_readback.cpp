// SPDX-License-Identifier: LGPL-2.1-only
//
// Reading the two planes of a real Depth32Float_Stencil8 texture, which is how
// every guest depth+stencil format is stored (format_converter.hpp). A plain
// blit of the combined format is not allowed; each plane needs its own blit
// option. The D3D11 frontend repacks the two into the guest's staging layout.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"

#include <cstring>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

TEST(DepthStencilPlaneReadback, ClearedValuesComeBackPerPlane) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    BackendTexture2DDesc desc{};
    desc.width = 5;  // odd, so the plane pitches are not accidentally aligned
    desc.height = 3;
    desc.mip_levels = 1;
    desc.array_size = 2;
    desc.pixel_format = PixelFormat::D24Unorm_S8Uint;
    desc.depth_stencil = true;
    auto texture = device.create_texture_2d(desc, nullptr, 0);
    ASSERT_TRUE(texture && texture->valid());
    ASSERT_TRUE(texture->clear_depth_stencil_aspects(0.25f, 0x5A, true, true,
                                                     0, 0, 1));
    ASSERT_TRUE(texture->clear_depth_stencil_aspects(0.75f, 0xC3, true, true,
                                                     0, 1, 1));

    for (const auto& [slice, depth, stencil] :
         {std::tuple{0u, 0.25f, u8{0x5A}}, std::tuple{1u, 0.75f, u8{0xC3}}}) {
        std::vector<float> depths(desc.width * desc.height, -1.0f);
        std::vector<u8> stencils(desc.width * desc.height, 0);
        ASSERT_TRUE(texture->readback_depth_stencil_plane(
            0, slice, false, depths.data(), desc.width * 4));
        ASSERT_TRUE(texture->readback_depth_stencil_plane(
            0, slice, true, stencils.data(), desc.width));
        for (usize i = 0; i < depths.size(); ++i) {
            EXPECT_EQ(depths[i], depth) << "slice " << slice << " texel " << i;
            EXPECT_EQ(stencils[i], stencil)
                << "slice " << slice << " texel " << i;
        }
    }
    // A colour texture has no planes to read.
    BackendTexture2DDesc colour{};
    colour.width = colour.height = 2;
    colour.pixel_format = PixelFormat::RGBA8Unorm;
    auto plain = device.create_texture_2d(colour, nullptr, 0);
    std::vector<float> scratch(4);
    EXPECT_FALSE(plain->readback_depth_stencil_plane(0, 0, false,
                                                     scratch.data(), 8));
}

TEST(DepthStencilPlaneReadback, DepthCubeFacesAreSeparateSlices) {
    // The point-light shadow map: a depth cube, one face cleared.
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    BackendTexture2DDesc desc{};
    desc.width = desc.height = 8;
    desc.mip_levels = 1;
    desc.array_size = 6;
    desc.cube = true;
    desc.pixel_format = PixelFormat::D24Unorm_S8Uint;
    desc.depth_stencil = true;
    auto cube = device.create_texture_2d(desc, nullptr, 0);
    ASSERT_TRUE(cube && cube->valid());
    ASSERT_TRUE(cube->clear_depth_stencil_aspects(1.0f, 0, true, true, 0, 0, 6));
    ASSERT_TRUE(cube->clear_depth_stencil_aspects(0.5f, 7, true, true, 0, 3, 1));
    for (u32 face = 0; face < 6; ++face) {
        std::vector<float> depths(64, -1.0f);
        std::vector<u8> stencils(64, 0xAA);
        ASSERT_TRUE(cube->readback_depth_stencil_plane(0, face, false,
                                                       depths.data(), 32));
        ASSERT_TRUE(cube->readback_depth_stencil_plane(0, face, true,
                                                       stencils.data(), 8));
        EXPECT_EQ(depths[0], face == 3 ? 0.5f : 1.0f) << "face " << face;
        EXPECT_EQ(stencils[63], face == 3 ? 7u : 0u) << "face " << face;
    }
}

// D3D11 ClearView on a depth-stencil view (ClearViewAlsoSupportsDepthOnlyFormats;
// tools/oracle_probe/d3d11_capsmall_probe.expected "clearview"): each plane
// takes the given bits over a rectangle -- including depths a render-pass
// clear could not store (1.5, NaN, -0) -- and nothing outside it.
TEST(DepthStencilPlaneReadback, FillDepthStencilTexelsStoresExactBitsInARect) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    const auto bits = [](float f) {
        u32 b = 0;
        std::memcpy(&b, &f, 4);
        return b;
    };

    BackendTexture2DDesc desc{};
    desc.width = 5;
    desc.height = 3;
    desc.mip_levels = 1;
    desc.array_size = 2;
    desc.pixel_format = PixelFormat::D32Float;
    desc.depth_stencil = true;
    auto d32 = device.create_texture_2d(desc, nullptr, 0);
    ASSERT_TRUE(d32 && d32->valid());
    ASSERT_TRUE(d32->clear_depth_stencil_aspects(0.25f, 0, true, false, 0, 0, 2));
    // Slice 1, x 1..3, y 0..1: 1.5; then slice 0 whole: -0 and NaN corners.
    ASSERT_TRUE(d32->fill_depth_stencil_texels(bits(1.5f), false, 0, 0, 1, 1,
                                               1, 0, 3, 2));
    ASSERT_TRUE(d32->fill_depth_stencil_texels(0x80000000u, false, 0, 0, 0, 1,
                                               0, 0, 1, 1));
    ASSERT_TRUE(d32->fill_depth_stencil_texels(0x7FC00000u, false, 0, 0, 0, 1,
                                               4, 2, 1, 1));
    std::vector<u32> texels(15, 0);
    ASSERT_TRUE(d32->readback_slice(0, 1, texels.data(), 20));
    for (u32 y = 0; y < 3; ++y) {
        for (u32 x = 0; x < 5; ++x) {
            const bool inside = x >= 1 && x < 4 && y < 2;
            EXPECT_EQ(texels[y * 5 + x], inside ? 0x3FC00000u : bits(0.25f))
                << x << "," << y;
        }
    }
    ASSERT_TRUE(d32->readback_slice(0, 0, texels.data(), 20));
    EXPECT_EQ(texels[0], 0x80000000u);
    EXPECT_EQ(texels[14], 0x7FC00000u);
    EXPECT_EQ(texels[7], bits(0.25f));

    // Depth16Unorm: the code as given.
    desc.pixel_format = PixelFormat::D16Unorm;
    desc.array_size = 1;
    auto d16 = device.create_texture_2d(desc, nullptr, 0);
    ASSERT_TRUE(d16 && d16->valid());
    ASSERT_TRUE(d16->clear_depth_stencil_aspects(0.0f, 0, true, false, 0, 0, 1));
    ASSERT_TRUE(d16->fill_depth_stencil_texels(0x4CCCu, false, 0, 0, 0, 1, 2,
                                               1, 2, 2));
    std::vector<u16> codes(15, 0xFFFF);
    ASSERT_TRUE(d16->readback_slice(0, 0, codes.data(), 10));
    EXPECT_EQ(codes[0], 0u);
    EXPECT_EQ(codes[1 * 5 + 2], 0x4CCCu);
    EXPECT_EQ(codes[2 * 5 + 3], 0x4CCCu);
    EXPECT_EQ(codes[1 * 5 + 4], 0u);

    // A combined format: depth and stencil planes, each through its option.
    desc.pixel_format = PixelFormat::D24Unorm_S8Uint;
    auto ds = device.create_texture_2d(desc, nullptr, 0);
    ASSERT_TRUE(ds && ds->valid());
    ASSERT_TRUE(ds->clear_depth_stencil_aspects(0.25f, 0x5A, true, true, 0, 0,
                                                1));
    ASSERT_TRUE(ds->fill_depth_stencil_texels(bits(0.75f), true, 8, 0, 0, 1, 1,
                                              0, 2, 1));
    std::vector<float> depths(15, -1.0f);
    std::vector<u8> stencils(15, 0);
    ASSERT_TRUE(ds->readback_depth_stencil_plane(0, 0, false, depths.data(),
                                                 20));
    ASSERT_TRUE(ds->readback_depth_stencil_plane(0, 0, true, stencils.data(),
                                                 5));
    EXPECT_EQ(depths[1], 0.75f);
    EXPECT_EQ(stencils[1], 8u);
    EXPECT_EQ(depths[2], 0.75f);
    EXPECT_EQ(stencils[2], 8u);
    EXPECT_EQ(depths[0], 0.25f);
    EXPECT_EQ(stencils[0], 0x5Au);
    EXPECT_EQ(depths[6], 0.25f);
    EXPECT_EQ(stencils[6], 0x5Au);

    // Blits cannot write a multisampled texture.
    desc.pixel_format = PixelFormat::D32Float;
    desc.sample_count = 4;
    auto msaa = device.create_texture_2d(desc, nullptr, 0);
    ASSERT_TRUE(msaa && msaa->valid());
    EXPECT_FALSE(msaa->fill_depth_stencil_texels(0, false, 0, 0, 0, 1, 0, 0, 1,
                                                 1));
}

// D3D event markers (MARKER_SUPPORT.Profile): debug groups mirrored onto
// every encoder they span and balanced there, signposts inside and between
// encoders, unbalanced pops ignored. Run under MTL_DEBUG_LAYER=1 this is
// what Metal validation checks; the work around the markers must still
// complete and land.
TEST(DepthStencilPlaneReadback, DebugMarkersSpanPassesAndStayBalanced) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    ASSERT_TRUE(device.supports_debug_markers());
    BackendTexture2DDesc desc{};
    desc.width = desc.height = 4;
    desc.mip_levels = 1;
    desc.array_size = 1;
    desc.pixel_format = PixelFormat::D24Unorm_S8Uint;
    desc.depth_stencil = true;
    auto texture = device.create_texture_2d(desc, nullptr, 0);
    ASSERT_TRUE(texture && texture->valid());

    device.pop_debug_group();                      // nothing open: ignored
    device.insert_debug_signpost("before");        // between encoders
    device.push_debug_group("frame");
    ASSERT_TRUE(texture->clear_depth_stencil_aspects(0.5f, 3, true, true, 0,
                                                     0, 1));  // a render pass
    device.push_debug_group("inner");
    ASSERT_TRUE(texture->fill_depth_stencil_texels(0x3F400000u, true, 9, 0, 0,
                                                   1, 0, 0, 2, 2));  // blits
    device.insert_debug_signpost("mark");
    device.pop_debug_group();
    device.pop_debug_group();
    device.pop_debug_group();                      // unbalanced
    device.push_debug_group("left open");          // never popped
    ASSERT_TRUE(device.wait_idle());

    std::vector<float> depths(16, -1.0f);
    std::vector<u8> stencils(16, 0);
    ASSERT_TRUE(texture->readback_depth_stencil_plane(0, 0, false,
                                                      depths.data(), 16));
    ASSERT_TRUE(texture->readback_depth_stencil_plane(0, 0, true,
                                                      stencils.data(), 4));
    EXPECT_EQ(depths[0], 0.75f);
    EXPECT_EQ(stencils[5], 9u);
    EXPECT_EQ(depths[15], 0.5f);
    EXPECT_EQ(stencils[15], 3u);
    device.pop_debug_group();
    ASSERT_TRUE(device.wait_idle());
}

}  // namespace
