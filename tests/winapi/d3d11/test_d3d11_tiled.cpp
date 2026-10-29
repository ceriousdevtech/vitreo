// SPDX-License-Identifier: LGPL-2.1-only
//
// Tiled resources: caps, creation, tiling description, mappings.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_tiled.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11TiledTest, ReportsTier1FormatsAndTiledSampleCounts) {
    // "OPTIONS1 ... TiledResourcesTier=1", "OPTIONS2 ... TiledResourcesTier=1"
    // at FL 11_0 (d3d11_tiled_probe64.expected [caps]).
    ASSERT_TRUE(memory.write_u32(OUT, 0xCD).is_ok());
    for (u32 i = 0; i < 32; i += 4) {
        ASSERT_TRUE(memory.write_u32(DESC + i, 0xCDCDCDCDu).is_ok());
    }
    EXPECT_EQ(device_call(33, {10, DESC, 16}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(DESC).value(), 1u) << "OPTIONS1 tier";
    EXPECT_EQ(device_call(33, {14, DESC, 32}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(DESC + 16).value(), 1u) << "OPTIONS2 tier";
    // FORMAT_SUPPORT2_TILED on R8G8B8A8_UNORM (28), R9G9B9E5 (67), BC1 (71),
    // D32_FLOAT (40); not on D24_UNORM_S8_UINT (45) or R32G32B32_FLOAT (6).
    for (const u32 format : {28u, 67u, 71u, 40u, 55u, 115u}) {
        write_words(DESC, {format, 0});
        EXPECT_EQ(device_call(33, {3, DESC, 8}), d3d11::S_OK) << format;
        EXPECT_NE(memory.read_u32(DESC + 4).value() & 0x200u, 0u) << format;
    }
    for (const u32 format : {45u, 6u, 20u}) {
        write_words(DESC, {format, 0});
        (void)device_call(33, {3, DESC, 8});
        EXPECT_EQ(memory.read_u32(DESC + 4).value() & 0x200u, 0u) << format;
    }
    // CheckMultisampleQualityLevels1(TILED): "msaa1 f28: x1=1/1 x2=0/1
    // x4=1/1 ...", "f45: x1=0/1 ...", "f71: x1=1/1 x2=0/0 x4=0/0".
    const auto levels = [&](u32 format, u32 samples) {
        EXPECT_TRUE(memory.write_u32(OUT, 0xCD).is_ok());
        EXPECT_EQ(device_call(53, {format, samples, 1, OUT}), d3d11::S_OK);
        return memory.read_u32(OUT).value();
    };
    EXPECT_EQ(levels(28, 1), 1u);
    EXPECT_EQ(levels(28, 2), 0u);
    EXPECT_EQ(levels(28, 4), 1u);
    EXPECT_EQ(levels(28, 8), 0u);
    EXPECT_EQ(levels(40, 4), 1u);
    EXPECT_EQ(levels(45, 1), 0u);
    EXPECT_EQ(levels(45, 4), 0u);
    EXPECT_EQ(levels(71, 1), 1u);
    EXPECT_EQ(levels(71, 4), 0u);
}

TEST_F(D3D11TiledTest, CreationFollowsTheMeasuredRules) {
    GuestAddr b = 0;
    // Tile pools ([create] "buf pool ...").
    EXPECT_EQ(make_buffer(TILE, 0, POOL), d3d11::S_OK);
    EXPECT_EQ(make_buffer(0, 0, POOL), d3d11::S_OK) << "pool 0";
    EXPECT_EQ(make_buffer(100000, 0, POOL), d3d11::E_INVALIDARG);
    EXPECT_EQ(make_buffer(TILE, SRV, POOL), d3d11::E_INVALIDARG);
    EXPECT_EQ(make_buffer(TILE, 0, POOL, nullptr, 2, 0x10000),
              d3d11::E_INVALIDARG) << "DYNAMIC";
    EXPECT_EQ(make_buffer(TILE, 0, POOL | TILED), d3d11::E_INVALIDARG);
    EXPECT_EQ(make_buffer(TILE, 0, POOL | 0x8000), d3d11::E_INVALIDARG)
        << "GUARDED";
    EXPECT_EQ(make_buffer(TILE, 0, POOL, nullptr, 0, 0, T_DATA),
              d3d11::E_INVALIDARG) << "initial data";
    EXPECT_EQ(make_buffer(0x80000000u, 0, POOL, &b), d3d11::S_OK)
        << "pool 2GB (x64)";
    // GetDesc: ByteWidth and MiscFlags as created.
    set_arg(0, b);
    set_arg(1, OUT);
    d3d11.impl_BufferMethod(marshaler, 10);
    EXPECT_EQ(memory.read_u32(OUT).value(), 0x80000000u);
    EXPECT_EQ(memory.read_u32(OUT + 16).value(), POOL);
    // Tiled buffers ([create] "buf tiled ...").
    EXPECT_EQ(make_buffer(100, SRV, TILED), d3d11::S_OK);
    EXPECT_EQ(make_buffer(TILE, 0, TILED), d3d11::S_OK) << "bind 0";
    EXPECT_EQ(make_buffer(TILE, RTV, TILED), d3d11::S_OK);
    EXPECT_EQ(make_buffer(TILE, 0x1, TILED), d3d11::E_INVALIDARG) << "VB";
    EXPECT_EQ(make_buffer(TILE, 0x4, TILED), d3d11::E_INVALIDARG) << "CB";
    EXPECT_EQ(make_buffer(TILE, 0x10, TILED), d3d11::E_INVALIDARG) << "SO";
    EXPECT_EQ(make_buffer(TILE, SRV, TILED | 0x40), d3d11::S_OK)
        << "STRUCTURED";
    EXPECT_EQ(make_buffer(TILE, SRV, TILED | 0x80), d3d11::E_INVALIDARG)
        << "RESOURCE_CLAMP";
    EXPECT_EQ(make_buffer(TILE, SRV, TILED | 0x2), d3d11::E_INVALIDARG)
        << "SHARED";
    EXPECT_EQ(make_buffer(TILE, SRV, TILED, nullptr, 0, 0x20000),
              d3d11::E_INVALIDARG) << "CPU read";
    EXPECT_EQ(make_buffer(0, SRV, TILED), d3d11::E_INVALIDARG) << "0 bytes";
    EXPECT_EQ(make_buffer(0xFFFF0000u, SRV, TILED), d3d11::S_OK)
        << "no 2 GiB limit";
    // Tiled textures ([create] "t2d ...").
    EXPECT_EQ(make_texture(256, 256, 2, 2, 28, 1, SRV, TILED), d3d11::S_OK)
        << "array of whole-tile mips";
    EXPECT_EQ(make_texture(256, 256, 3, 2, 28, 1, SRV, TILED),
              d3d11::E_INVALIDARG) << "array with a 64x64 mip";
    EXPECT_EQ(make_texture(200, 200, 1, 2, 28, 1, SRV, TILED),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(make_texture(256, 256, 1, 6, 28, 1, SRV, TILED | 0x4),
              d3d11::S_OK) << "cube of one mip";
    EXPECT_EQ(make_texture(256, 256, 0, 6, 28, 1, SRV, TILED | 0x4),
              d3d11::E_INVALIDARG) << "cube full chain";
    EXPECT_EQ(make_texture(256, 256, 1, 1, 28, 2, SRV | RTV, TILED),
              d3d11::E_INVALIDARG) << "msaa2";
    EXPECT_EQ(make_texture(256, 256, 1, 1, 28, 4, SRV | RTV, TILED),
              d3d11::S_OK) << "msaa4";
    EXPECT_EQ(make_texture(256, 256, 1, 1, 28, 8, SRV | RTV, TILED),
              d3d11::E_INVALIDARG) << "msaa8";
    EXPECT_EQ(make_texture(256, 256, 1, 1, 45, 1, 0x40, TILED),
              d3d11::E_INVALIDARG) << "d24s8";
    EXPECT_EQ(make_texture(256, 256, 1, 1, 40, 1, 0x40, TILED), d3d11::S_OK)
        << "d32f";
    EXPECT_EQ(make_texture(256, 256, 1, 1, 6, 1, SRV, TILED),
              d3d11::E_INVALIDARG) << "r32g32b32";
    EXPECT_EQ(make_texture(256, 256, 1, 1, 28, 1, SRV | RTV, TILED | 0x2),
              d3d11::E_INVALIDARG) << "SHARED";
    EXPECT_EQ(make_texture(256, 256, 1, 1, 28, 1, SRV, TILED | 0x8000),
              d3d11::E_INVALIDARG) << "GUARDED";
    EXPECT_EQ(make_texture(256, 256, 1, 1, 28, 1, SRV, TILED | 0x80),
              d3d11::S_OK) << "RESOURCE_CLAMP";
    EXPECT_EQ(make_texture(256, 256, 1, 1, 28, 1, SRV, TILED, nullptr, 2,
                           0x10000),
              d3d11::E_INVALIDARG) << "DYNAMIC";
    // Texture1D / Texture3D: "t1d tiled create=0x80070057", "t3d ...".
    write_words(T_DESC, {256, 1, 1, 28, 0, SRV, 0, TILED});
    EXPECT_EQ(device_call(4, {T_DESC, 0, T_OUT}), d3d11::E_INVALIDARG);
    write_words(T_DESC, {64, 64, 64, 1, 28, 0, SRV, 0, TILED});
    EXPECT_EQ(device_call(6, {T_DESC, 0, T_OUT}), d3d11::E_INVALIDARG);
}

TEST_F(D3D11TiledTest, TiledResourcesNeedFeatureLevel11) {
    // WARP at FL 10_1: TiledResourcesTier 0 and no TILED format; so TILED
    // creation is refused there. A device of FL 10_1 alone:
    write_words(LEVELS, {0xA100});
    prepare_create();
    set_arg(4, LEVELS);
    set_arg(5, 1);
    set_arg(7, OUT);
    set_arg(9, 0);
    Marshaler create(memory);
    create.attach_cpu(cpu);
    d3d11.impl_D3D11CreateDevice(create);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr device101 = memory.read_u64(OUT).value();
    cpu.gpr.rsp = STACK_BASE + 0x800;
    set_arg(0, device101);
    set_arg(1, 10);
    set_arg(2, DESC);
    set_arg(3, 16);
    d3d11.impl_DeviceMethod(marshaler, 33);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(DESC).value(), 0u) << "tier 0 at 10_1";
    write_words(T_DESC, {TILE, 0, 0, 0, POOL, 0});
    set_arg(0, device101);
    set_arg(1, T_DESC);
    set_arg(2, 0);
    set_arg(3, T_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "no tile pool at 10_1";
}

TEST_F(D3D11TiledTest, GetResourceTilingDescribesTheLayout) {
    const auto tiling = [&](GuestAddr resource, u32 requested, u32 first) {
        for (u32 i = 0; i < 0x40; i += 4) {
            EXPECT_TRUE(memory.write_u32(OUT + i, 0xCDCDCDCDu).is_ok());
        }
        for (u32 i = 0; i < 64 * 12; i += 4) {
            EXPECT_TRUE(memory.write_u32(T_TILINGS + i, 0xCDCDCDCDu).is_ok());
        }
        EXPECT_TRUE(memory.write_u32(OUT + 28, requested).is_ok());
        device_call(52, {resource, OUT, OUT + 4, OUT + 16, OUT + 28, first,
                         T_TILINGS});
    };
    // "tiling buffer 65540: total=2 packed std=1 packed=0 tiles=0 start=0
    // shape=65536x1x1 nsub=1", "sub0: 2x1x1 start=0".
    tiling(tiled_buffer(TILE + 4), 4, 0);
    EXPECT_EQ(memory.read_u32(OUT).value(), 2u);
    EXPECT_EQ(memory.read_u32(OUT + 4).value(), 1u) << "1 standard, 0 packed";
    EXPECT_EQ(memory.read_u32(OUT + 8).value(), 0u);
    EXPECT_EQ(memory.read_u32(OUT + 16).value(), 65536u);
    EXPECT_EQ(memory.read_u32(OUT + 20).value(), 1u);
    EXPECT_EQ(memory.read_u32(OUT + 24).value(), 1u);
    EXPECT_EQ(memory.read_u32(OUT + 28).value(), 1u);
    EXPECT_EQ(memory.read_u32(T_TILINGS).value(), 2u);
    EXPECT_EQ(memory.read_u32(T_TILINGS + 4).value(), 1u | (1u << 16));
    EXPECT_EQ(memory.read_u32(T_TILINGS + 8).value(), 0u);
    // "tiling rgba8 256 a3 m2: total=15 ... shape=128x128x1 nsub=6":
    // sub0 2x2 @0, sub1 1x1 @4, sub2 2x2 @5, ... sub5 1x1 @14.
    tiling(tiled_texture(256, 256, 2, 3), 64, 0);
    EXPECT_EQ(memory.read_u32(OUT).value(), 15u);
    EXPECT_EQ(memory.read_u32(OUT + 4).value(), 2u);
    EXPECT_EQ(memory.read_u32(OUT + 16).value(), 128u);
    EXPECT_EQ(memory.read_u32(OUT + 20).value(), 128u);
    EXPECT_EQ(memory.read_u32(OUT + 28).value(), 6u);
    const u32 starts[6] = {0, 4, 5, 9, 10, 14};
    for (u32 i = 0; i < 6; ++i) {
        EXPECT_EQ(memory.read_u32(T_TILINGS + i * 12 + 8).value(), starts[i])
            << "sub" << i;
        EXPECT_EQ(memory.read_u32(T_TILINGS + i * 12).value(),
                  i % 2 == 0 ? 2u : 1u);
    }
    // Packed mips (D3D11_PACKED_MIP_DESC; D3D11_PACKED_TILE for a packed
    // subresource): a 256x256 chain here packs from 64x64 into 2 tiles.
    const GuestAddr chain = tiled_texture(256, 256, 0, 1);
    tiling(chain, 64, 0);
    EXPECT_EQ(memory.read_u32(OUT).value(), 4u + 1u + 2u);
    EXPECT_EQ(memory.read_u8(OUT + 4).value(), 2u) << "NumStandardMips";
    EXPECT_EQ(memory.read_u8(OUT + 5).value(), 7u) << "NumPackedMips";
    EXPECT_EQ(memory.read_u32(OUT + 8).value(), 2u) << "NumTilesForPackedMips";
    EXPECT_EQ(memory.read_u32(OUT + 12).value(), 5u) << "StartTileIndex";
    EXPECT_EQ(memory.read_u32(OUT + 28).value(), 9u);
    EXPECT_EQ(memory.read_u32(T_TILINGS + 2 * 12).value(), 0u);
    EXPECT_EQ(memory.read_u32(T_TILINGS + 2 * 12 + 8).value(), 0xFFFFFFFFu);
    // Parameter rules ([tiling] "param ..."): FirstSubresourceTilingToGet 5
    // of 9 with 100 requested -> 4; past the end the count stays; 0
    // requested writes nothing; a NULL count writes no tiling.
    tiling(chain, 100, 5);
    EXPECT_EQ(memory.read_u32(OUT + 28).value(), 4u);
    tiling(chain, 3, 20);
    EXPECT_EQ(memory.read_u32(OUT + 28).value(), 3u);
    tiling(chain, 0, 0);
    EXPECT_EQ(memory.read_u32(OUT + 28).value(), 0u);
    EXPECT_EQ(memory.read_u32(T_TILINGS).value(), 0xCDCDCDCDu);
    for (u32 i = 0; i < 12; i += 4) {
        ASSERT_TRUE(memory.write_u32(T_TILINGS + i, 0xCDCDCDCDu).is_ok());
    }
    device_call(52, {chain, OUT, 0, 0, 0, 0, T_TILINGS});
    EXPECT_EQ(memory.read_u32(OUT).value(), 7u);
    EXPECT_EQ(memory.read_u32(T_TILINGS).value(), 0xCDCDCDCDu);
    // A tile pool is not tiled: "tiling pool (not tiled): total=0 ... nsub=0".
    tiling(pool(2), 4, 0);
    EXPECT_EQ(memory.read_u32(OUT).value(), 0u);
    EXPECT_EQ(memory.read_u32(OUT + 16).value(), 0u);
    EXPECT_EQ(memory.read_u32(OUT + 28).value(), 0u);
}

TEST_F(D3D11TiledTest, UpdateTileMappingsValidatesAsWindows) {
    const GuestAddr p = pool(8);
    const GuestAddr b = tiled_buffer(4 * TILE);
    GuestAddr plain = 0;
    ASSERT_EQ(make_buffer(TILE, SRV, 0, &plain), d3d11::S_OK);
    const GuestAddr other = tiled_buffer(4 * TILE);
    coord(0, 0, 0, 0, 0);
    size(0, 1);
    write_words(T_OFFSETS, {0, 1});
    // [val] lines, in order.
    EXPECT_EQ(map(b, 1, T_COORDS, T_SIZES, 0, 1, 0, T_OFFSETS, 0),
              d3d11::E_INVALIDARG) << "fresh res, pool NULL, seq mapping";
    write_words(T_FLAGS, {1});
    EXPECT_EQ(map(b, 1, T_COORDS, T_SIZES, 0, 1, T_FLAGS, 0, 0), d3d11::S_OK)
        << "fresh res, pool NULL, NULL mapping";
    write_words(T_FLAGS, {2});
    EXPECT_EQ(map(b, 1, T_COORDS, T_SIZES, 0, 1, T_FLAGS, 0, 0),
              d3d11::E_INVALIDARG) << "fresh res, pool NULL, SKIP mapping";
    EXPECT_EQ(map(b, 1, T_COORDS, T_SIZES, plain, 1, 0, T_OFFSETS, 0),
              d3d11::E_INVALIDARG) << "plain buffer as pool";
    EXPECT_EQ(map(b, 1, T_COORDS, T_SIZES, other, 1, 0, T_OFFSETS, 0),
              d3d11::E_INVALIDARG) << "tiled buffer as pool";
    EXPECT_EQ(map(b, 1, T_COORDS, T_SIZES, p, 1, 0, T_OFFSETS, 0),
              d3d11::S_OK);
    EXPECT_EQ(map(b, 1, T_COORDS, T_SIZES, 0, 1, 0, T_OFFSETS, 0),
              d3d11::S_OK) << "after mapping, pool NULL + seq";
    write_words(T_COUNTS, {2});
    EXPECT_EQ(map(b, 1, T_COORDS, T_SIZES, p, 1, 0, T_OFFSETS, T_COUNTS),
              d3d11::E_INVALIDARG) << "range count 2 vs region 1";
    coord(1, 0, 1, 0, 0);
    EXPECT_EQ(map(b, 1, T_COORDS + 16, T_SIZES, p, 1, 0, T_OFFSETS, 0),
              d3d11::S_OK) << "buffer coord Y=1";
    coord(1, 0, 0, 0, 1);
    EXPECT_EQ(map(b, 1, T_COORDS + 16, T_SIZES, p, 1, 0, T_OFFSETS, 0),
              d3d11::E_INVALIDARG) << "buffer coord Subresource=1";
    write_words(T_OFFSETS + 8, {8});
    EXPECT_EQ(map(b, 1, T_COORDS, T_SIZES, p, 1, 0, T_OFFSETS + 8, 0),
              d3d11::E_INVALIDARG) << "pool offset 8 of 8";
    for (const u32 flag : {3u, 5u, 6u, 8u, 0x80000000u}) {
        write_words(T_FLAGS, {flag});
        EXPECT_EQ(map(b, 1, T_COORDS, T_SIZES, p, 1, T_FLAGS, T_OFFSETS, 0),
                  d3d11::E_INVALIDARG) << "range flag " << flag;
    }
    EXPECT_EQ(map(b, 1, T_COORDS, T_SIZES, p, 1, 0, T_OFFSETS, 0, 1),
              d3d11::S_OK) << "Flags=NO_OVERWRITE";
    EXPECT_EQ(map(b, 1, T_COORDS, T_SIZES, p, 1, 0, T_OFFSETS, 0, 2),
              d3d11::E_INVALIDARG) << "Flags=2";
    EXPECT_EQ(map(b, 0, T_COORDS, T_SIZES, p, 1, 0, T_OFFSETS, 0),
              d3d11::E_INVALIDARG) << "NumTiledResourceRegions 0";
    EXPECT_EQ(map(b, 1, T_COORDS, T_SIZES, p, 0, 0, T_OFFSETS, 0),
              d3d11::E_INVALIDARG) << "NumRanges 0";
    EXPECT_EQ(map(b, 1, T_COORDS, T_SIZES, p, 1, 0, 0, 0),
              d3d11::E_INVALIDARG) << "offsets NULL";
    // pRangeTileCounts NULL: several ranges are one tile each.
    size(1, 2);
    EXPECT_EQ(map(b, 1, T_COORDS, T_SIZES + 16, p, 2, 0, T_OFFSETS, 0),
              d3d11::S_OK) << "counts NULL, 2 ranges, 2 tiles";
    size(1, 4);
    EXPECT_EQ(map(b, 1, T_COORDS, T_SIZES + 16, p, 2, 0, T_OFFSETS, 0),
              d3d11::E_INVALIDARG) << "counts NULL, 2 ranges, 4 tiles";
    size(1, 0);
    EXPECT_EQ(map(b, 1, T_COORDS, T_SIZES + 16, p, 1, 0, T_OFFSETS, 0),
              d3d11::E_INVALIDARG) << "region NumTiles 0";
    EXPECT_EQ(map(b, 2, 0, 0, p, 1, 0, T_OFFSETS, 0), d3d11::E_INVALIDARG)
        << "2 regions, coords NULL";
    write_words(T_OFFSETS + 8, {5});
    EXPECT_EQ(map(b, 1, 0, 0, p, 1, 0, T_OFFSETS + 8, 0),
              d3d11::E_INVALIDARG) << "whole buffer from pool 5 (overflows)";
    // Out of the resource: WARP removes the device; refused here.
    coord(1, 4, 0, 0, 0);
    EXPECT_EQ(map(b, 1, T_COORDS + 16, T_SIZES, p, 1, 0, T_OFFSETS, 0),
              d3d11::E_INVALIDARG) << "buffer coord X=4 of 4";
    // Texture boxes: NumTiles must be Width*Height*Depth, none zero; a box
    // past the mip's tiles is S_OK ("tex box 3x2 (past width)").
    const GuestAddr t = tiled_texture(256, 256, 1, 1);
    size(2, 4, 1, 2, 2, 1);
    EXPECT_EQ(map(t, 1, T_COORDS, T_SIZES + 32, p, 1, 0, T_OFFSETS, 0),
              d3d11::S_OK);
    size(2, 3, 1, 2, 2, 1);
    EXPECT_EQ(map(t, 1, T_COORDS, T_SIZES + 32, p, 1, 0, T_OFFSETS, 0),
              d3d11::E_INVALIDARG) << "tex box 2x2 NumTiles 3";
    size(2, 0, 1, 2, 2, 0);
    EXPECT_EQ(map(t, 1, T_COORDS, T_SIZES + 32, p, 1, 0, T_OFFSETS, 0),
              d3d11::E_INVALIDARG) << "tex box depth 0";
    size(2, 6, 1, 3, 2, 1);
    EXPECT_EQ(map(t, 1, T_COORDS, T_SIZES + 32, p, 1, 0, T_OFFSETS, 0),
              d3d11::S_OK) << "tex box 3x2 (past width)";
    coord(2, 0, 0, 0, 1);
    EXPECT_EQ(map(t, 1, T_COORDS + 32, T_SIZES, p, 1, 0, T_OFFSETS, 0),
              d3d11::E_INVALIDARG) << "tex coord Subresource past the end";
    // ResizeTilePool ([val] "Resize ..."): 64 KiB multiples up to a UINT;
    // GetDesc follows.
    EXPECT_EQ(ctx(138, {p, 9ull * TILE}), d3d11::S_OK);
    set_arg(0, p);
    set_arg(1, OUT);
    d3d11.impl_BufferMethod(marshaler, 10);
    EXPECT_EQ(memory.read_u32(OUT).value(), 9u * TILE);
    EXPECT_EQ(ctx(138, {p, 100000}), d3d11::E_INVALIDARG);
    EXPECT_EQ(ctx(138, {p, 0x100000000ull}), d3d11::E_INVALIDARG);
    EXPECT_EQ(ctx(138, {b, 2ull * TILE}), d3d11::E_INVALIDARG);
    EXPECT_EQ(ctx(138, {plain, 2ull * TILE}), d3d11::E_INVALIDARG);
    EXPECT_EQ(ctx(138, {p, 0}), d3d11::S_OK);
    EXPECT_EQ(ctx(138, {p, 0xFFFF0000ull}), d3d11::S_OK);
}

TEST_F(D3D11TiledTest, MappingsWalkRegionsAgainstRanges) {
    const GuestAddr p = pool(16);
    const void* object = nullptr;
    // RGBA8 256x256, 2 mips, 2 slices: 5 tiles a slice, nothing packed.
    const GuestAddr t = tiled_texture(256, 256, 2, 2, &object);
    const auto expect_map = [&](std::map<BackendState::TileKey, u64> want) {
        EXPECT_EQ(mappings(object), want);
    };
    // "data X9 linear 10 from sub0 ... map: s0[0,1,2,3] s1[4] s2[5,6,7,8]
    // s3[9]".
    coord(0, 0, 0, 0, 0);
    size(0, 10);
    write_words(T_OFFSETS, {0});
    ASSERT_EQ(map(t, 1, T_COORDS, T_SIZES, p, 1, 0, T_OFFSETS, 0),
              d3d11::S_OK);
    expect_map({{tile(0, 0, 0, 0), 0}, {tile(0, 0, 1, 0), 1},
                {tile(0, 0, 0, 1), 2}, {tile(0, 0, 1, 1), 3},
                {tile(1, 0, 0, 0), 4}, {tile(0, 1, 0, 0), 5},
                {tile(0, 1, 1, 0), 6}, {tile(0, 1, 0, 1), 7},
                {tile(0, 1, 1, 1), 8}, {tile(1, 1, 0, 0), 9}});
    // "linear 4 from sub0 (1,1)": s0 tile 3 -> 10, s1 -> 11, s2 (0,0),(1,0)
    // -> 12, 13.
    coord(0, 1, 1, 0, 0);
    size(0, 4);
    write_words(T_OFFSETS, {10});
    ASSERT_EQ(map(t, 1, T_COORDS, T_SIZES, p, 1, 0, T_OFFSETS, 0),
              d3d11::S_OK);
    EXPECT_EQ(mappings(object).at(tile(0, 0, 1, 1)), 10u);
    EXPECT_EQ(mappings(object).at(tile(1, 0, 0, 0)), 11u);
    EXPECT_EQ(mappings(object).at(tile(0, 1, 0, 0)), 12u);
    EXPECT_EQ(mappings(object).at(tile(0, 1, 1, 0)), 13u);
    // "box 1x2 at (1,0) sub0": (1,0),(1,1) -> 14, 15.
    coord(0, 1, 0, 0, 0);
    size(0, 2, 1, 1, 2, 1);
    write_words(T_OFFSETS, {14});
    ASSERT_EQ(map(t, 1, T_COORDS, T_SIZES, p, 1, 0, T_OFFSETS, 0),
              d3d11::S_OK);
    EXPECT_EQ(mappings(object).at(tile(0, 0, 1, 0)), 14u);
    EXPECT_EQ(mappings(object).at(tile(0, 0, 1, 1)), 15u);
    // Depth on an array steps to the next slice of the same mip (Learn,
    // D3D11_TILE_REGION_SIZE): a 1x1x2 box at sub0 maps both slices' (0,0).
    size(0, 2, 1, 1, 1, 2);
    coord(0, 0, 0, 0, 0);
    write_words(T_FLAGS, {4});  // REUSE_SINGLE_TILE
    write_words(T_OFFSETS, {7});
    ASSERT_EQ(map(t, 1, T_COORDS, T_SIZES, p, 1, T_FLAGS, T_OFFSETS, 0),
              d3d11::S_OK);
    EXPECT_EQ(mappings(object).at(tile(0, 0, 0, 0)), 7u);
    EXPECT_EQ(mappings(object).at(tile(0, 1, 0, 0)), 7u);
    EXPECT_TRUE(backend.state.last_self_aliased)
        << "two tiles on pool tile 7";
    // SKIP leaves, NULL unmaps: "T3 b1 = [skip, pool2, pool3, pool4]".
    const void* buffer_object = nullptr;
    const GuestAddr b = tiled_buffer(4 * TILE, &buffer_object);
    ASSERT_EQ(map_all(b, p, 4), d3d11::S_OK);
    write_words(T_FLAGS, {2, 4, 0});
    write_words(T_OFFSETS, {0xFFFFFFFFu, 2, 3});
    write_words(T_COUNTS, {1, 1, 2});
    ASSERT_EQ(map(b, 1, 0, 0, p, 3, T_FLAGS, T_OFFSETS, T_COUNTS),
              d3d11::S_OK);
    EXPECT_EQ(mappings(buffer_object),
              (std::map<BackendState::TileKey, u64>{{tile(0, 0, 0, 0), 4},
                                                    {tile(0, 0, 1, 0), 2},
                                                    {tile(0, 0, 2, 0), 3},
                                                    {tile(0, 0, 3, 0), 4}}));
    EXPECT_TRUE(backend.state.last_self_aliased) << "tiles 0 and 3 on 4";
    write_words(T_FLAGS, {1});
    coord(0, 1, 0, 0, 0);
    size(0, 1);
    ASSERT_EQ(map(b, 1, T_COORDS, T_SIZES, 0, 1, T_FLAGS, 0, 0), d3d11::S_OK);
    EXPECT_EQ(mappings(buffer_object).count(tile(0, 0, 1, 0)), 0u);
    // A buffer box is its Width, Y ignored: "T9 buffer box 2x2 at (1,1) seq
    // from pool4 ... tiles: 0 4 5 3" (tile 0 on 0, 3 on 3 beforehand).
    ASSERT_EQ(map_all(b, p, 0), d3d11::S_OK);
    coord(0, 1, 1, 0, 0);
    size(0, 4, 1, 2, 2, 1);
    write_words(T_OFFSETS, {4});
    ASSERT_EQ(map(b, 1, T_COORDS, T_SIZES, p, 1, 0, T_OFFSETS, 0),
              d3d11::S_OK);
    EXPECT_EQ(mappings(buffer_object),
              (std::map<BackendState::TileKey, u64>{{tile(0, 0, 0, 0), 0},
                                                    {tile(0, 0, 1, 0), 4},
                                                    {tile(0, 0, 2, 0), 5},
                                                    {tile(0, 0, 3, 0), 3}}));
    EXPECT_FALSE(backend.state.last_self_aliased);
    // Pool references follow the mappings: pool tile 7 shows three texture
    // tiles, pool tile 4 the buffer's tile 1 only.
    const auto refs = pool_refs();
    EXPECT_EQ(refs.at(7), 3u);
    EXPECT_EQ(refs.at(4), 1u);
}

TEST_F(D3D11TiledTest, PoolSwitchCopyTileMappingsAndResize) {
    const GuestAddr pa = pool(4);
    const GuestAddr pb = pool(4);
    const void* src_object = nullptr;
    const void* dst_object = nullptr;
    const GuestAddr src = tiled_buffer(2 * TILE, &src_object);
    const GuestAddr dst = tiled_buffer(2 * TILE, &dst_object);
    ASSERT_EQ(map_all(src, pa, 2), d3d11::S_OK);
    ASSERT_EQ(map_all(dst, pb, 0), d3d11::S_OK);
    // "T10 CopyTileMappings onto other-pool dest ... dst tile0=3 ...
    // tile1 ... (0 words=16384)": dest moves to pool A, tile 1 cleared.
    coord(0, 0, 0, 0, 0);
    coord(1, 1, 0, 0, 0);
    size(0, 1);
    EXPECT_EQ(ctx(135, {dst, T_COORDS, src, T_COORDS + 16, T_SIZES, 0}),
              d3d11::S_OK);
    EXPECT_EQ(mappings(dst_object),
              (std::map<BackendState::TileKey, u64>{{tile(0, 0, 0, 0), 3}}));
    // "then pool NULL map tile1 -> 0 ... tile1=0 (pool A tile 0 = 0)".
    write_words(T_OFFSETS, {0});
    EXPECT_EQ(map(dst, 1, T_COORDS + 16, T_SIZES, 0, 1, 0, T_OFFSETS, 0),
              d3d11::S_OK);
    EXPECT_EQ(mappings(dst_object).at(tile(0, 0, 1, 0)), 0u);
    // [val]: Flags other than NO_OVERWRITE, 0 tiles, past the end: refused.
    EXPECT_EQ(ctx(135, {dst, T_COORDS, src, T_COORDS, T_SIZES, 2}),
              d3d11::E_INVALIDARG);
    size(1, 0);
    EXPECT_EQ(ctx(135, {dst, T_COORDS, src, T_COORDS, T_SIZES + 16, 0}),
              d3d11::E_INVALIDARG);
    size(1, 2);
    EXPECT_EQ(ctx(135, {dst, T_COORDS + 16, src, T_COORDS, T_SIZES + 16, 0}),
              d3d11::E_INVALIDARG);
    GuestAddr plain = 0;
    ASSERT_EQ(make_buffer(TILE, SRV, 0, &plain), d3d11::S_OK);
    EXPECT_EQ(ctx(135, {plain, T_COORDS, src, T_COORDS, T_SIZES, 0}),
              d3d11::E_INVALIDARG) << "not tiled";
    // Overlap within one resource: "T4 shifted: t0==old0 t1==old0 t2==old1
    // t3==old2" -- as if through a temporary.
    const void* object = nullptr;
    const GuestAddr four = tiled_buffer(4 * TILE, &object);
    write_words(T_OFFSETS, {0});
    ASSERT_EQ(map_all(four, pa, 0), d3d11::S_OK);
    coord(2, 1, 0, 0, 0);
    size(2, 3);
    ASSERT_EQ(ctx(135, {four, T_COORDS + 32, four, T_COORDS, T_SIZES + 32, 0}),
              d3d11::S_OK);
    EXPECT_EQ(mappings(object),
              (std::map<BackendState::TileKey, u64>{{tile(0, 0, 0, 0), 0},
                                                    {tile(0, 0, 1, 0), 0},
                                                    {tile(0, 0, 2, 0), 1},
                                                    {tile(0, 0, 3, 0), 2}}));
    // Resize: "T7 shrink to 1 ... new mapping past shrunk end hr=0x80070057,
    // inside S_OK"; existing mappings past the end stay.
    ASSERT_EQ(ctx(138, {pa, TILE}), d3d11::S_OK);
    EXPECT_EQ(mappings(object).size(), 4u);
    write_words(T_OFFSETS, {1});
    coord(3, 0, 0, 0, 0);
    size(3, 1);
    EXPECT_EQ(map(four, 1, T_COORDS + 48, T_SIZES + 48, pa, 1, 0, T_OFFSETS,
                  0),
              d3d11::E_INVALIDARG);
    write_words(T_OFFSETS, {0});
    EXPECT_EQ(map(four, 1, T_COORDS + 48, T_SIZES + 48, pa, 1, 0, T_OFFSETS,
                  0),
              d3d11::S_OK);
    // Out of pool memory: E_OUTOFMEMORY, the mappings unchanged
    // (UpdateTileMappings return values).
    ASSERT_EQ(ctx(138, {pa, 4ull * TILE}), d3d11::S_OK);
    backend.state.tile_pool_out_of_memory = true;
    write_words(T_OFFSETS, {3});
    EXPECT_EQ(map(four, 1, T_COORDS + 48, T_SIZES + 48, pa, 1, 0, T_OFFSETS,
                  0),
              0x8007000Eu);
    EXPECT_EQ(mappings(object).at(tile(0, 0, 0, 0)), 0u);
    backend.state.tile_pool_out_of_memory = false;
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
