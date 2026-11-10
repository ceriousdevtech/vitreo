// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_format_422_gpu.cpp
/// @brief DXGI R8G8_B8G8_UNORM / G8R8_G8B8_UNORM held as decoded RGBA8
///        storage (graphics/contracts/format_422.hpp) on the Metal backend.
///
/// Learn DXGI_FORMAT: a 32-bit block per pixel pair, R and B shared, G per
/// pixel; components named from the lowest byte, so the R8G8_B8G8 block is
/// (R, G0, B, G1) and the G8R8_G8B8 block (G0, R, G1, B). Checked on the GPU:
///   * a buffer -> texture footprint copy decodes each block into two
///     storage texels (R, G, B, G'), the CPU readback showing the storage;
///   * texture -> buffer re-encodes the blocks byte for byte, including the
///     block of an odd-width level whose second pixel has no storage, and
///     the 2x1 level of a mipmapped texture (Metal's own 422 formats refuse
///     both: tools/oracle_probe/metal/metal_422_facts.m);
///   * a volume round-trips the same way.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/format_422.hpp"
#include "graphics/metal/metal_device.hpp"

#include <vector>

using namespace vitreo::graphics;
using vitreo::u32;
using vitreo::u8;

namespace {

[[nodiscard]] std::unique_ptr<BackendBuffer> host_buffer(GraphicsBackendDevice& device,
                                                         u32 bytes,
                                                         const void* data = nullptr) {
    BackendBufferDesc desc{};
    desc.byte_width = bytes;
    desc.cpu_writable = true;
    return device.create_buffer(desc, data);
}

[[nodiscard]] std::vector<u8> blocks(u32 count, u8 seed) {
    std::vector<u8> out(count * 4u);
    for (u32 i = 0; i < out.size(); ++i) out[i] = static_cast<u8>(seed + i * 7u);
    return out;
}

[[nodiscard]] MetalDevice* device_or_null() {
    auto& device = MetalDevice::instance();
    return device.available() ? &device : nullptr;
}

}  // namespace

TEST(Format422Storage, FootprintUploadDecodesAndReadbackEncodesEveryBlock) {
    MetalDevice* device = device_or_null();
    if (device == nullptr) GTEST_SKIP() << "no Metal device";
    for (const PixelFormat format : {PixelFormat::RGBG422Unorm, PixelFormat::GRGB422Unorm}) {
        // 5x2 with 2 levels: level 0 is 3 blocks a row (the last half
        // used), level 1 is 2x1, one block.
        BackendTexture2DDesc desc{};
        desc.width = 5;
        desc.height = 2;
        desc.mip_levels = 2;
        desc.pixel_format = format;
        desc.shader_resource = true;
        auto texture = device->create_texture_2d(desc, nullptr, 0);
        ASSERT_NE(texture, nullptr);
        const u32 pitch = 16;  // 12 bytes of blocks, padded
        std::vector<u8> level0(pitch * 2, 0xEE);
        const auto row0 = blocks(3, 0x10);
        const auto row1 = blocks(3, 0x80);
        std::copy(row0.begin(), row0.end(), level0.begin());
        std::copy(row1.begin(), row1.end(), level0.begin() + pitch);
        const auto level1 = blocks(1, 0x41);
        auto up0 = host_buffer(*device, static_cast<u32>(level0.size()), level0.data());
        auto up1 = host_buffer(*device, 4, level1.data());
        // A footprint of an odd level is whole blocks: width 6 for 5 pixels.
        ASSERT_TRUE(device->copy_texture_buffer_region(texture.get(), 0, 0, 0, 0, 6, 2,
                                                       up0.get(), 0, pitch, true));
        ASSERT_TRUE(device->copy_texture_buffer_region(texture.get(), 1, 0, 0, 0, 2, 1,
                                                       up1.get(), 0, 4, true));
        auto down0 = host_buffer(*device, pitch * 2);
        auto down1 = host_buffer(*device, 4);
        ASSERT_TRUE(device->copy_texture_buffer_region(texture.get(), 0, 0, 0, 0, 5, 2,
                                                       down0.get(), 0, pitch, false));
        ASSERT_TRUE(device->copy_texture_buffer_region(texture.get(), 1, 0, 0, 0, 2, 1,
                                                       down1.get(), 0, 4, false));
        ASSERT_TRUE(device->wait_idle());
        std::vector<u8> got0(pitch * 2);
        std::vector<u8> got1(4);
        ASSERT_TRUE(down0->read(0, got0.data(), got0.size()));
        ASSERT_TRUE(down1->read(0, got1.data(), got1.size()));
        for (u32 y = 0; y < 2; ++y) {
            for (u32 b = 0; b < 12; ++b) {
                EXPECT_EQ(got0[y * pitch + b], level0[y * pitch + b])
                    << static_cast<int>(format) << " row " << y << " byte " << b;
            }
        }
        EXPECT_EQ(got1, level1) << static_cast<int>(format);

        // The storage: pixel 2k = (R, G0, B, G1), pixel 2k+1 = (R, G1, B, G0).
        std::vector<u8> texels(5 * 4 * 2);
        ASSERT_TRUE(texture->readback_slice(0, 0, texels.data(), 20));
        const Block422Layout l = block_422_layout(format);
        for (u32 px = 0; px < 5; ++px) {
            const u8* block = row0.data() + (px / 2u) * 4u;
            const bool second = (px & 1u) != 0;
            EXPECT_EQ(texels[px * 4 + 0], block[l.r]) << px;
            EXPECT_EQ(texels[px * 4 + 1], block[second ? l.g1 : l.g0]) << px;
            EXPECT_EQ(texels[px * 4 + 2], block[l.b]) << px;
            EXPECT_EQ(texels[px * 4 + 3], block[second ? l.g0 : l.g1]) << px;
        }
    }
}

TEST(Format422Storage, VolumeFootprintRoundTrips) {
    MetalDevice* device = device_or_null();
    if (device == nullptr) GTEST_SKIP() << "no Metal device";
    BackendTexture3DDesc desc{};
    desc.width = 4;
    desc.height = 2;
    desc.depth = 2;
    desc.mip_levels = 1;
    desc.pixel_format = PixelFormat::RGBG422Unorm;
    desc.shader_resource = true;
    auto texture = device->create_texture_3d(desc, nullptr, 0, 0);
    ASSERT_NE(texture, nullptr);
    const u32 row = 8;
    const u32 slice = 32;  // rows padded apart
    std::vector<u8> data(slice * 2, 0);
    for (u32 z = 0; z < 2; ++z) {
        for (u32 y = 0; y < 2; ++y) {
            const auto b = blocks(2, static_cast<u8>(0x20 + z * 0x40 + y * 0x11));
            std::copy(b.begin(), b.end(), data.begin() + z * slice + y * row);
        }
    }
    auto up = host_buffer(*device, static_cast<u32>(data.size()), data.data());
    ASSERT_TRUE(device->copy_texture_3d_buffer_region(texture.get(), 0, 0, 0, 0, 4, 2, 2, up.get(),
                                                      0, row, slice, true));
    auto down = host_buffer(*device, static_cast<u32>(data.size()));
    ASSERT_TRUE(device->copy_texture_3d_buffer_region(texture.get(), 0, 0, 0, 0, 4, 2, 2,
                                                      down.get(), 0, row, slice, false));
    ASSERT_TRUE(device->wait_idle());
    std::vector<u8> got(data.size());
    ASSERT_TRUE(down->read(0, got.data(), got.size()));
    for (u32 z = 0; z < 2; ++z) {
        for (u32 y = 0; y < 2; ++y) {
            for (u32 b = 0; b < row; ++b) {
                const u32 at = z * slice + y * row + b;
                EXPECT_EQ(got[at], data[at]) << z << "," << y << "," << b;
            }
        }
    }
}
