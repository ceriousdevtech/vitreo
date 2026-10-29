// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_resource_ops_gpu.cpp
/// @brief The Metal primitives behind D3D11's resource operations store the
///        bytes Windows stores.
///
/// Expected bytes are lines of
/// tools/oracle_probe/d3d11_a06_resource_ops_probe.expected (WARP): a UAV
/// clear writes its texel verbatim into a texture that is not a render target
/// (the old render-pass clear left those untouched); a copy between members
/// of one format family, or between BC1 and R32G32_UINT, moves bytes, not
/// converted colours; GenerateMips on a view leaves the levels outside it; a
/// GPU -> CPU readback recorded on the timeline returns the level.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"

#include <array>
#include <cstring>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

std::unique_ptr<BackendTexture2D> texture(PixelFormat format, unsigned width,
                                          unsigned height, bool rt, bool uav,
                                          unsigned mips = 1,
                                          const void* data = nullptr,
                                          unsigned pitch = 0) {
    BackendTexture2DDesc td{};
    td.width = width;
    td.height = height;
    td.mip_levels = mips;
    td.array_size = 1;
    td.pixel_format = format;
    td.shader_resource = true;
    td.render_target = rt;
    td.unordered_access = uav;
    return MetalDevice::instance().create_texture_2d(td, data, pitch);
}

std::vector<u8> read(BackendTexture2D& t, unsigned mip, unsigned bytes,
                     unsigned rows = 1) {
    std::vector<u8> out(static_cast<usize>(bytes) * rows, 0xEE);
    EXPECT_TRUE(t.readback_slice(mip, 0, out.data(), bytes));
    return out;
}

}  // namespace

TEST(ResourceOpsGpu, FillTexelsStoresTheTexelVerbatim) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    // a06: UAV UAV R10G10B10A2_UNORM Uint u0 -> 78c20bc0 (a UAV-only
    // texture: the compute path through an R32Uint view).
    auto uav = texture(PixelFormat::RGB10A2Unorm, 2, 1, false, true);
    ASSERT_TRUE(uav);
    const std::array<u8, 4> packed{0x78, 0xC2, 0x0B, 0xC0};
    ASSERT_TRUE(uav->fill_texels(packed.data(), 4, 0, 0, 1, 0, 0, 2, 1));
    EXPECT_EQ(read(*uav, 0, 8),
              (std::vector<u8>{0x78, 0xC2, 0x0B, 0xC0, 0x78, 0xC2, 0x0B,
                               0xC0}));
    // R11G11B10 Float f1 -> cb05c0ff; one texel of two.
    auto small = texture(PixelFormat::RG11B10Float, 2, 1, false, true);
    ASSERT_TRUE(small);
    const std::array<u8, 4> nan_blue{0xCB, 0x05, 0xC0, 0xFF};
    ASSERT_TRUE(small->fill_texels(nan_blue.data(), 4, 0, 0, 1, 1, 0, 1, 1));
    EXPECT_EQ(read(*small, 0, 8),
              (std::vector<u8>{0, 0, 0, 0, 0xCB, 0x05, 0xC0, 0xFF}));
    // A render target without shader write: the blit path (ClearView rect).
    auto rt = texture(PixelFormat::RGBA8Unorm, 4, 1, true, false);
    ASSERT_TRUE(rt);
    const std::array<u8, 4> red{0xFF, 0x80, 0x40, 0xFF};
    ASSERT_TRUE(rt->fill_texels(red.data(), 4, 0, 0, 1, 1, 0, 2, 1));
    EXPECT_EQ(read(*rt, 0, 16),
              (std::vector<u8>{0, 0, 0, 0, 0xFF, 0x80, 0x40, 0xFF, 0xFF,
                               0x80, 0x40, 0xFF, 0, 0, 0, 0}));
    // The texel must be exactly one storage texel.
    EXPECT_FALSE(rt->fill_texels(red.data(), 2, 0, 0, 1, 0, 0, 1, 1));
}

TEST(ResourceOpsGpu, VolumeFillCoversOnlyTheWWindow) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    // a06: UAV 3D view W 1..2 (4 slices) -> cc cc 07 07 07 07 cc cc.
    BackendTexture3DDesc vd{};
    vd.width = 2;
    vd.height = 1;
    vd.depth = 4;
    vd.pixel_format = PixelFormat::R32Uint;
    vd.unordered_access = true;
    std::vector<u8> cc(2 * 4 * 4, 0xCC);
    auto volume = device.create_texture_3d(vd, cc.data(), 8, 8);
    ASSERT_TRUE(volume);
    const u32 seven = 7;
    ASSERT_TRUE(volume->fill_texels(&seven, 4, 0, 0, 0, 1, 2, 1, 2));
    for (unsigned w = 0; w < 4; ++w) {
        std::array<u32, 2> row{};
        ASSERT_TRUE(volume->readback_w_slice(0, w, row.data(), 8));
        const u32 expected = (w == 1 || w == 2) ? 7u : 0xCCCCCCCCu;
        EXPECT_EQ(row[0], expected) << "W " << w;
        EXPECT_EQ(row[1], expected) << "W " << w;
    }
}

TEST(ResourceOpsGpu, ElementCopiesMoveBytesBetweenFormats) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    // a06: CPY family CopyResource RGBA8_SRGB -> RGBA8_UNORM keeps bytes.
    const std::array<u8, 8> bytes{0x05, 0x12, 0x1F, 0x2C,
                                  0x39, 0x46, 0x53, 0x60};
    auto srgb = texture(PixelFormat::RGBA8Unorm_sRGB, 2, 1, false, false, 1,
                        bytes.data(), 8);
    auto uint = texture(PixelFormat::RGBA8Uint, 2, 1, false, false);
    ASSERT_TRUE(srgb && uint);
    BackendTextureLocation to{};
    BackendTextureLocation from{};
    to.texture_2d = uint.get();
    from.texture_2d = srgb.get();
    ASSERT_TRUE(device.copy_texture_elements(to, from, 2, 1, 1));
    EXPECT_EQ(read(*uint, 0, 8), std::vector<u8>(bytes.begin(), bytes.end()));

    // a06: CPY region BC1 16x16 -> R32G32_UINT 4x4: one block, one texel.
    std::vector<u8> blocks(128);
    for (usize i = 0; i < blocks.size(); ++i) {
        blocks[i] = static_cast<u8>(0x05 + 13 * i);
    }
    auto bc1 = texture(PixelFormat::BC1_RGBA, 16, 16, false, false, 1,
                       blocks.data(), 32);
    auto rg32 = texture(PixelFormat::RG32Uint, 4, 4, false, false);
    ASSERT_TRUE(bc1 && rg32);
    to.texture_2d = rg32.get();
    from.texture_2d = bc1.get();
    ASSERT_TRUE(device.copy_texture_elements(to, from, 16, 16, 1));
    EXPECT_EQ(read(*rg32, 0, 32, 4), blocks);

    // One texture into itself, overlapping: as if read whole first.
    const std::array<u8, 16> row{1, 2, 3, 4, 5, 6, 7, 8,
                                 9, 10, 11, 12, 13, 14, 15, 16};
    auto self = texture(PixelFormat::RGBA8Unorm, 4, 1, false, false, 1,
                        row.data(), 16);
    ASSERT_TRUE(self);
    to.texture_2d = from.texture_2d = self.get();
    to.x = 1;
    ASSERT_TRUE(device.copy_texture_elements(to, from, 3, 1, 1));
    EXPECT_EQ(read(*self, 0, 16),
              (std::vector<u8>{1, 2, 3, 4, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                               11, 12}));
}

TEST(ResourceOpsGpu, MipWindowLeavesLevelsOutsideTheView) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    // a06: GEN view mips 0..1 -> mip1 regenerated, mip2 kept (22222222).
    BackendTexture2DDesc td{};
    td.width = td.height = 4;
    td.mip_levels = 3;
    td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.shader_resource = true;
    td.render_target = true;
    auto t = device.create_texture_2d(td, nullptr, 0);
    ASSERT_TRUE(t);
    std::vector<u8> m0(64);
    for (usize i = 0; i < 16; ++i) {
        m0[i * 4 + 0] = 200;
        m0[i * 4 + 1] = 100;
        m0[i * 4 + 2] = 50;
        m0[i * 4 + 3] = 255;
    }
    const std::vector<u8> m1(16, 0x11);
    const std::vector<u8> m2(4, 0x22);
    ASSERT_TRUE(t->update_slice(0, 0, m0.data(), 16));
    ASSERT_TRUE(t->update_slice(1, 0, m1.data(), 8));
    ASSERT_TRUE(t->update_slice(2, 0, m2.data(), 4));
    ASSERT_TRUE(device.generate_mips_window(t.get(), 0, 2, 0, 1));
    EXPECT_EQ(read(*t, 1, 8, 2),
              (std::vector<u8>{200, 100, 50, 255, 200, 100, 50, 255, 200,
                               100, 50, 255, 200, 100, 50, 255}));
    EXPECT_EQ(read(*t, 2, 4), m2);
}

TEST(ResourceOpsGpu, AsyncReadbackReturnsTheLevelOnceExecuted) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    const std::array<u8, 16> row{1, 2, 3, 4, 5, 6, 7, 8,
                                 9, 10, 11, 12, 13, 14, 15, 16};
    auto t = texture(PixelFormat::RGBA8Unorm, 4, 1, false, false, 1,
                     row.data(), 16);
    ASSERT_TRUE(t);
    auto readback = t->readback_slice_async(0, 0);
    ASSERT_TRUE(readback);
    ASSERT_TRUE(readback->wait());
    EXPECT_TRUE(readback->ready());
    std::array<u8, 16> out{};
    ASSERT_TRUE(readback->read(out.data(), 16));
    EXPECT_EQ(out, row);
}
