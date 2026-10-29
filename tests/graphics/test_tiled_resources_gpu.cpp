// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_tiled_resources_gpu.cpp
/// @brief Tiled resources on the Metal backend (metal_tiled.hpp): placement
///        sparse textures and buffers mapped into tile-pool memory.
///
/// What the D3D11 contract needs from the backend, checked on the GPU:
///   * a tile is the D3D standard shape (D3D11.3 5.9.3.1; WARP's
///     GetResourceTiling in d3d11_tiled_probe64.expected);
///   * two tiled resources mapped to one pool tile show the same texels
///     (Learn, "Tile access limitations with duplicate mappings": compatible
///     resources share data), across a TiledResourceBarrier;
///   * two tiles of ONE resource on one pool tile stay ordered like accesses
///     to one resource (a write through one, then a read through the other);
///   * every mip round-trips through a mapped packed tail.
/// Everything is read back through the GPU into CPU-visible memory.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

using namespace vitreo::graphics;
using vitreo::u32;
using vitreo::u64;
using vitreo::u8;

namespace {

constexpr u32 kTile = 65536;

[[nodiscard]] MetalDevice* tiled_device() {
    auto& device = MetalDevice::instance();
    if (!device.available() || !device.supports_tiled_resources()) {
        return nullptr;
    }
    return &device;
}

[[nodiscard]] std::unique_ptr<BackendBuffer> host_buffer(
    GraphicsBackendDevice& device, u32 bytes, const void* data = nullptr) {
    BackendBufferDesc desc{};
    desc.byte_width = bytes;
    desc.cpu_writable = true;
    return device.create_buffer(desc, data);
}

[[nodiscard]] std::unique_ptr<BackendTexture2D> tiled_texture(
    GraphicsBackendDevice& device, unsigned width, unsigned height,
    unsigned mips) {
    BackendTexture2DDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.mip_levels = mips;
    desc.pixel_format = PixelFormat::RGBA8Unorm;
    desc.shader_resource = true;
    desc.render_target = true;
    desc.tiled = true;
    return device.create_texture_2d(desc, nullptr, 0);
}

std::vector<u32> pattern(u32 words, u32 seed) {
    std::vector<u32> out(words);
    for (u32 i = 0; i < words; ++i) out[i] = (i * 2654435761u) ^ seed;
    return out;
}

/// Point @p count tiles at pool tiles, referencing each as the frontend does.
void map(GraphicsBackendDevice& device, BackendTexture2D* texture,
         BackendTilePool& pool, std::vector<BackendTileMapping> mappings,
         bool self_aliased = false) {
    for (const auto& m : mappings) ASSERT_TRUE(pool.reference(m.pool_tile));
    ASSERT_TRUE(device.update_tile_mappings(texture, &pool, mappings.data(),
                                            mappings.size(), self_aliased));
}

}  // namespace

TEST(MetalTiledResources, TileShapesAreTheStandardOnes) {
    MetalDevice* device = tiled_device();
    if (device == nullptr) GTEST_SKIP() << "no placement sparse resources";
    struct Case {
        PixelFormat format;
        unsigned samples, w, h;
    };
    const Case cases[] = {
        {PixelFormat::R8Unorm, 1, 256, 256},
        {PixelFormat::RGBA8Unorm, 1, 128, 128},
        {PixelFormat::RGBA16Float, 1, 128, 64},
        {PixelFormat::RGBA32Float, 1, 64, 64},
        {PixelFormat::RGBA8Unorm, 4, 64, 64},
        {PixelFormat::R8Unorm, 4, 128, 128},
        {PixelFormat::RGBA32Float, 4, 32, 32},
        {PixelFormat::D32Float, 1, 128, 128},
    };
    for (const Case& c : cases) {
        unsigned w = 0;
        unsigned h = 0;
        ASSERT_TRUE(device->tiled_texture_tile_shape(c.format, c.samples, w, h))
            << static_cast<int>(c.format);
        EXPECT_EQ(w, c.w) << static_cast<int>(c.format) << " x" << c.samples;
        EXPECT_EQ(h, c.h) << static_cast<int>(c.format) << " x" << c.samples;
    }
}

TEST(MetalTiledResources, TwoTexturesOnOnePoolTileShareTexels) {
    MetalDevice* device = tiled_device();
    if (device == nullptr) GTEST_SKIP() << "no placement sparse resources";
    auto pool = device->create_tile_pool();
    ASSERT_NE(pool, nullptr);
    pool->set_size(8);
    auto a = tiled_texture(*device, 256, 256, 1);
    auto b = tiled_texture(*device, 256, 256, 1);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    // a: 2x2 tiles on pool 0..3; b on the same four.
    std::vector<BackendTileMapping> tiles;
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 2; ++x) {
            tiles.push_back({{0, 0, x, y, false}, y * 2u + x});
        }
    }
    map(*device, a.get(), *pool, tiles);
    map(*device, b.get(), *pool, tiles);
    const auto texels = pattern(256 * 256, 0x5a5a0000u);
    auto upload = host_buffer(*device, 256 * 256 * 4, texels.data());
    ASSERT_TRUE(device->copy_texture_buffer_region(
        a.get(), 0, 0, 0, 0, 256, 256, upload.get(), 0, 1024, true));
    ASSERT_TRUE(device->tiled_resource_barrier());
    auto readback = host_buffer(*device, 256 * 256 * 4);
    ASSERT_TRUE(device->copy_texture_buffer_region(
        b.get(), 0, 0, 0, 0, 256, 256, readback.get(), 0, 1024, false));
    ASSERT_TRUE(device->wait_idle());
    std::vector<u32> got(256 * 256);
    ASSERT_TRUE(readback->read(0, got.data(), got.size() * 4));
    EXPECT_EQ(got, texels);
    for (const auto& m : tiles) {
        pool->unreference(m.pool_tile);
        pool->unreference(m.pool_tile);
    }
}

TEST(MetalTiledResources, SelfAliasedBufferTilesStayOrdered) {
    MetalDevice* device = tiled_device();
    if (device == nullptr) GTEST_SKIP() << "no placement sparse resources";
    auto pool = device->create_tile_pool();
    ASSERT_NE(pool, nullptr);
    pool->set_size(4);
    BackendBufferDesc desc{};
    desc.byte_width = 4 * kTile;
    desc.shader_resource = true;
    desc.tiled = true;
    auto tiled = device->create_buffer(desc, nullptr);
    ASSERT_NE(tiled, nullptr);
    // D3D11_TILE_RANGE_REUSE_SINGLE_TILE: every tile on pool tile 2.
    std::vector<BackendTileMapping> tiles;
    for (u32 i = 0; i < 4; ++i) tiles.push_back({{0, 0, i, 0, false}, 2});
    for (const auto& m : tiles) ASSERT_TRUE(pool->reference(m.pool_tile));
    ASSERT_TRUE(device->update_tile_mappings(tiled.get(), pool.get(),
                                             tiles.data(), tiles.size(),
                                             true));
    // Write through tile 3, then read tiles 0..2 in the next blits: Metal
    // orders blits by byte range, so without the backend's fence the reads
    // ran first (measured: zeros).
    const auto words = pattern(kTile / 4, 0x33330000u);
    auto upload = host_buffer(*device, kTile, words.data());
    ASSERT_TRUE(device->copy_buffer_region(tiled.get(), 3ull * kTile,
                                           upload.get(), 0, kTile));
    auto readback = host_buffer(*device, 3 * kTile);
    for (u32 i = 0; i < 3; ++i) {
        ASSERT_TRUE(device->copy_buffer_region(readback.get(), 1ull * i * kTile,
                                               tiled.get(), 1ull * i * kTile,
                                               kTile));
    }
    ASSERT_TRUE(device->wait_idle());
    std::vector<u32> got(3 * kTile / 4);
    ASSERT_TRUE(readback->read(0, got.data(), got.size() * 4));
    for (u32 t = 0; t < 3; ++t) {
        EXPECT_TRUE(std::equal(words.begin(), words.end(),
                               got.begin() + t * (kTile / 4)))
            << "tile " << t;
    }
    for (const auto& m : tiles) pool->unreference(m.pool_tile);
}

TEST(MetalTiledResources, EveryMipRoundTripsThroughAMappedPackedTail) {
    MetalDevice* device = tiled_device();
    if (device == nullptr) GTEST_SKIP() << "no placement sparse resources";
    auto pool = device->create_tile_pool();
    ASSERT_NE(pool, nullptr);
    pool->set_size(32);
    auto texture = tiled_texture(*device, 256, 256, 9);
    ASSERT_NE(texture, nullptr);
    BackendTiledLayout layout{};
    ASSERT_TRUE(device->tiled_texture_layout(texture.get(), layout));
    EXPECT_EQ(layout.tile_width, 128u);
    EXPECT_EQ(layout.tile_height, 128u);
    ASSERT_LT(layout.first_packed_level, 9u) << "a 256x256 chain packs";
    ASSERT_GT(layout.packed_tiles, 0u);
    // Whole tiles are whole mips down to 128x128 (Tier 1: a mip that is an
    // integer multiple of the tile shape is never packed).
    EXPECT_GE(layout.first_packed_level, 2u);
    std::vector<BackendTileMapping> tiles;
    u64 next = 0;
    for (u32 mip = 0; mip < layout.first_packed_level; ++mip) {
        const u32 n = std::max(1u, (256u >> mip) / 128u);
        for (u32 y = 0; y < n; ++y) {
            for (u32 x = 0; x < n; ++x) {
                tiles.push_back({{mip, 0, x, y, false}, next++});
            }
        }
    }
    for (u32 k = 0; k < layout.packed_tiles; ++k) {
        tiles.push_back({{0, 0, k, 0, true}, next++});
    }
    map(*device, texture.get(), *pool, tiles);
    std::vector<std::vector<u32>> mips;
    for (u32 mip = 0; mip < 9; ++mip) {
        const u32 w = 256u >> mip;
        mips.push_back(pattern(w * w, 0x70000000u + mip));
        auto upload = host_buffer(*device, w * w * 4, mips.back().data());
        ASSERT_TRUE(device->copy_texture_buffer_region(
            texture.get(), mip, 0, 0, 0, w, w, upload.get(), 0, w * 4, true));
    }
    std::vector<std::unique_ptr<BackendBuffer>> readbacks;
    for (u32 mip = 0; mip < 9; ++mip) {
        const u32 w = 256u >> mip;
        readbacks.push_back(host_buffer(*device, w * w * 4));
        ASSERT_TRUE(device->copy_texture_buffer_region(
            texture.get(), mip, 0, 0, 0, w, w, readbacks.back().get(), 0,
            w * 4, false));
    }
    ASSERT_TRUE(device->wait_idle());
    for (u32 mip = 0; mip < 9; ++mip) {
        const u32 w = 256u >> mip;
        std::vector<u32> got(w * w);
        ASSERT_TRUE(readbacks[mip]->read(0, got.data(), got.size() * 4));
        EXPECT_EQ(got, mips[mip]) << "mip " << mip;
    }
    for (const auto& m : tiles) pool->unreference(m.pool_tile);
}

TEST(MetalTiledResources, MultisampledTilesRoundTripInSampleOrder) {
    MetalDevice* device = tiled_device();
    if (device == nullptr) GTEST_SKIP() << "no placement sparse resources";
    // CopyTiles / UpdateTiles of a 4x texture: every pixel's samples in
    // index order, rows at the full tile pitch (Learn, CopyTiles remarks).
    // A value per sample survives the write and the read only if all four
    // samples are stored and addressed alike both ways.
    for (const PixelFormat format :
         {PixelFormat::RGBA8Unorm, PixelFormat::D32Float}) {
        auto pool = device->create_tile_pool();
        ASSERT_NE(pool, nullptr);
        pool->set_size(8);
        BackendTexture2DDesc desc{};
        desc.width = 128;
        desc.height = 128;
        desc.pixel_format = format;
        desc.sample_count = 4;
        desc.tiled = true;
        if (format == PixelFormat::D32Float) {
            desc.depth_stencil = true;
        } else {
            desc.render_target = true;
            desc.shader_resource = true;
        }
        auto texture = device->create_texture_2d(desc, nullptr, 0);
        ASSERT_NE(texture, nullptr);
        std::vector<BackendTileMapping> tiles;
        for (u32 y = 0; y < 2; ++y) {
            for (u32 x = 0; x < 2; ++x) {
                tiles.push_back({{0, 0, x, y, false}, y * 2u + x});
            }
        }
        map(*device, texture.get(), *pool, tiles);
        // Tile (1,0): texels 64..127 x 0..63, 64 * 4 samples * 4 bytes a row.
        std::vector<u32> samples = pattern(64 * 64 * 4, 0x4d4d0000u);
        if (format == PixelFormat::D32Float) {
            for (u32 i = 0; i < samples.size(); ++i) {
                const float depth = static_cast<float>(i % 1021) / 1021.0f;
                std::memcpy(&samples[i], &depth, 4);
            }
        }
        auto upload = host_buffer(*device, kTile, samples.data());
        ASSERT_TRUE(device->copy_texture_buffer_region(
            texture.get(), 0, 0, 64, 0, 64, 64, upload.get(), 0, 1024, true));
        auto readback = host_buffer(*device, kTile + 4);
        ASSERT_TRUE(device->copy_texture_buffer_region(
            texture.get(), 0, 0, 64, 0, 64, 64, readback.get(), 4, 1024,
            false))
            << "an unaligned offset goes through scratch";
        ASSERT_TRUE(device->wait_idle());
        std::vector<u32> got(samples.size());
        ASSERT_TRUE(readback->read(4, got.data(), got.size() * 4));
        EXPECT_EQ(got, samples) << static_cast<int>(format);
        for (const auto& m : tiles) pool->unreference(m.pool_tile);
    }
}
