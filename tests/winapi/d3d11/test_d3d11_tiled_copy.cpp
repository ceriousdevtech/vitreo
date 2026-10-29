// SPDX-License-Identifier: LGPL-2.1-only
//
// Tiled resources: tile copies/updates, ClearView, deferred contexts and pool references.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_tiled.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11TiledTest, CopyTilesAndUpdateTilesUseTheLinearTileLayout) {
    const GuestAddr p = pool(16);
    const void* object = nullptr;
    const GuestAddr t = tiled_texture(200, 200, 1, 1, &object);
    ASSERT_EQ(map_all(t, p, 0), d3d11::S_OK);
    GuestAddr linear = 0;
    ASSERT_EQ(make_buffer(4 * TILE, 0, 0, &linear), d3d11::S_OK);
    // A 2x1 box at (0,1): tiles (0,1) and (1,1), 64 KiB apart from the
    // offset; the pitch is a whole tile row (128 texels) even for the
    // 72-texel edge tile ("X2 CopyTiles edge tile (1,1) valid 72x72").
    coord(0, 0, 1, 0, 0);
    size(0, 2, 1, 2, 1, 1);
    backend.state.tile_copies.clear();
    ctx(136, {t, T_COORDS, T_SIZES, linear, 4, 4});
    ASSERT_EQ(backend.state.tile_copies.size(), 2u);
    const auto& first = backend.state.tile_copies[0];
    EXPECT_EQ(first.texture, object);
    EXPECT_EQ(first.x, 0u);
    EXPECT_EQ(first.y, 128u);
    EXPECT_EQ(first.width, 128u);
    EXPECT_EQ(first.height, 72u);
    EXPECT_EQ(first.offset, 4u) << "any byte offset (T8 offset 4)";
    EXPECT_EQ(first.row_pitch, 512u);
    EXPECT_FALSE(first.to_texture);
    const auto& second = backend.state.tile_copies[1];
    EXPECT_EQ(second.x, 128u);
    EXPECT_EQ(second.width, 72u);
    EXPECT_EQ(second.offset, 4u + TILE);
    EXPECT_EQ(second.row_pitch, 512u);
    // Flags: exactly one direction, nothing but NO_OVERWRITE besides;
    // an overrun of the linear buffer drops the copy (T8).
    for (const u32 flags : {0u, 6u, 0x80000002u}) {
        backend.state.tile_copies.clear();
        ctx(136, {t, T_COORDS, T_SIZES, linear, 0, flags});
        EXPECT_TRUE(backend.state.tile_copies.empty()) << flags;
    }
    backend.state.tile_copies.clear();
    ctx(136, {t, T_COORDS, T_SIZES, linear, 3ull * TILE + 4, 2});
    EXPECT_TRUE(backend.state.tile_copies.empty()) << "overrun";
    ctx(136, {t, T_COORDS, T_SIZES, linear, 2ull * TILE, 3});
    ASSERT_EQ(backend.state.tile_copies.size(), 2u) << "NO_OVERWRITE";
    EXPECT_TRUE(backend.state.tile_copies[0].to_texture);
    // UpdateTiles: flags 0, NO_OVERWRITE and LINEAR_BUFFER_TO_SWIZZLED
    // apply; SWIZZLED_TO_LINEAR and undefined bits drop it ("T8 UpdateTiles
    // flags ...").
    size(1, 1);
    for (const u32 flags : {0u, 1u, 2u, 4u, 0x80000000u}) {
        backend.state.tile_copies.clear();
        ctx(137, {t, T_COORDS, T_SIZES + 16, T_DATA, flags});
        EXPECT_EQ(backend.state.tile_copies.size(),
                  flags == 4u || flags == 0x80000000u ? 0u : 1u) << flags;
    }
    // Buffers: tile i of the region is bytes [64K i, 64K (i+1)).
    const GuestAddr b = tiled_buffer(3 * TILE);
    ASSERT_EQ(map_all(b, p, 8), d3d11::S_OK);
    backend.state.buffer_region_copies.clear();
    coord(2, 1, 0, 0, 0);
    size(2, 2);
    ctx(136, {b, T_COORDS + 32, T_SIZES + 32, linear, TILE, 4});
    ASSERT_EQ(backend.state.buffer_region_copies.size(), 2u);
    EXPECT_EQ(backend.state.buffer_region_copies[0].dst_offset, TILE);
    EXPECT_EQ(backend.state.buffer_region_copies[0].src_offset, TILE);
    EXPECT_EQ(backend.state.buffer_region_copies[1].dst_offset, 2u * TILE);
    EXPECT_EQ(backend.state.buffer_region_copies[1].src_offset, 2u * TILE);
    // 4x: a pixel is its four samples in index order, so the pitch of a
    // 64-texel RGBA8 tile row is 64 * 4 * 4 bytes (Learn, CopyTiles).
    GuestAddr msaa = 0;
    ASSERT_EQ(make_texture(256, 256, 1, 1, 28, 4, SRV | RTV, TILED, &msaa),
              d3d11::S_OK);
    ASSERT_EQ(map_all(msaa, p, 0), d3d11::S_OK);
    coord(3, 1, 0, 0, 0);
    size(3, 1);
    backend.state.tile_copies.clear();
    ctx(136, {msaa, T_COORDS + 48, T_SIZES + 48, linear, 0, 4});
    ASSERT_EQ(backend.state.tile_copies.size(), 1u);
    EXPECT_EQ(backend.state.tile_copies[0].x, 64u);
    EXPECT_EQ(backend.state.tile_copies[0].width, 64u);
    EXPECT_EQ(backend.state.tile_copies[0].row_pitch, 1024u);
    // TiledResourceBarrier orders everything through the backend.
    const unsigned barriers = backend.state.tiled_barriers;
    ctx(139, {0, 0});
    ctx(139, {t, b});
    EXPECT_EQ(backend.state.tiled_barriers, barriers + 2);
}

TEST_F(D3D11TiledTest, ClearViewClearsDepthRectanglesAsMeasured) {
    // Learn requires ClearView rectangles on depth-only views of devices with
    // tiled resources; OPTIONS1.ClearViewAlsoSupportsDepthOnlyFormats (WARP
    // 1). Measured (d3d11_tiled_probe [data] C): rectangles [l,r) x [t,b)
    // clamped, every slice of the view, D32_FLOAT the value as given (1.5
    // and -0.5 kept), D16_UNORM the saturated code.
    EXPECT_EQ(device_call(33, {10, DESC, 16}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(DESC + 8).value(), 1u);
    const auto dsv_of = [&](u32 format, u32 slices) {
        GuestAddr texture = 0;
        EXPECT_EQ(make_texture(8, 8, 1, slices, format, 1, 0x40, 0, &texture),
                  d3d11::S_OK);
        EXPECT_EQ(device_call(10, {texture, 0, OUT}), d3d11::S_OK);
        return memory.read_u64(OUT).value();
    };
    // clear_view writes the texels through fill_depth_stencil_texels: one
    // fill per rectangle over the view's slices, {depth bits, stencil, mip,
    // first slice, slices, x, y, width, height}.
    const auto clear = [&](GuestAddr view, float depth, u32 rects) {
        u32 bits = 0;
        std::memcpy(&bits, &depth, 4);
        write_words(T_OUT, {bits, 0, 0, 0});
        write_words(T_COORDS, {2, 1, 5, 4, 6, 6, 100, 100});
        backend.state.depth_fills.clear();
        ctx(132, {view, T_OUT, rects != 0 ? T_COORDS : 0, rects});
        return backend.state.depth_fills;
    };
    const auto slices_covered = [](const std::vector<std::array<u32, 9>>& f) {
        u32 n = 0;
        for (const auto& fill : f) n += fill[4];
        return n;
    };
    const GuestAddr d32 = dsv_of(40, 2);
    auto fills = clear(d32, 0.75f, 2);
    ASSERT_EQ(fills.size(), 2u) << "one fill per rectangle";
    EXPECT_EQ(slices_covered(fills), 4u) << "both rectangles on both slices";
    EXPECT_EQ(fills[0][0], 0x3F400000u);
    EXPECT_EQ(fills[0][3], 0u);
    EXPECT_EQ(fills[0][5], 2u);  // x
    EXPECT_EQ(fills[0][6], 1u);  // y
    EXPECT_EQ(fills[0][7], 3u);  // width
    EXPECT_EQ(fills[0][8], 3u);  // height
    EXPECT_EQ(fills[1][5], 6u);
    EXPECT_EQ(fills[1][7], 2u) << "clamped to the surface";
    EXPECT_EQ(fills[1][8], 2u);
    fills = clear(d32, -0.5f, 0);
    ASSERT_EQ(fills.size(), 1u) << "the whole view";
    EXPECT_EQ(slices_covered(fills), 2u);
    EXPECT_EQ(fills[0][7], 8u);
    EXPECT_EQ(fills[0][0], 0xBF000000u) << "D32_FLOAT keeps -0.5";
    const GuestAddr d16 = dsv_of(55, 1);
    fills = clear(d16, 1.5f, 1);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0][0] & 0xFFFFu, 0xFFFFu) << "D16_UNORM saturates";
    fills = clear(d16, 0.75f, 1);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0][0] & 0xFFFFu, 0xBFFFu);
}

TEST_F(D3D11TiledTest, DeferredContextsAndPredication) {
    const GuestAddr p = pool(4);
    const GuestAddr b = tiled_buffer(2 * TILE);
    ASSERT_EQ(map_all(b, p, 0), d3d11::S_OK);
    ASSERT_EQ(device_call(27, {0, OUT}), d3d11::S_OK);  // CreateDeferredContext
    const GuestAddr deferred = memory.read_u64(OUT).value();
    const auto dcall = [&](usize slot, std::initializer_list<u64> args) {
        cpu.gpr.rsp = STACK_BASE + 0x800;
        set_arg(0, deferred);
        usize i = 1;
        for (const u64 a : args) set_arg(i++, a);
        d3d11.impl_ContextMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    };
    // [data] D: UpdateTileMappings and CopyTileMappings refused; ResizeTilePool
    // S_OK, applied when the list executes.
    write_words(T_OFFSETS, {0});
    EXPECT_EQ(dcall(134, {b, 1, 0, 0, p, 1, 0, T_OFFSETS, 0, 0}),
              d3d11::E_INVALIDARG);
    coord(0, 0, 0, 0, 0);
    size(0, 1);
    EXPECT_EQ(dcall(135, {b, T_COORDS, b, T_COORDS, T_SIZES, 0}),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(dcall(138, {p, 8ull * TILE}), d3d11::S_OK);
    EXPECT_EQ(dcall(138, {p, 100000}), d3d11::E_INVALIDARG);
    set_arg(0, p);
    set_arg(1, OUT);
    d3d11.impl_BufferMethod(marshaler, 10);
    EXPECT_EQ(memory.read_u32(OUT).value(), 4u * TILE) << "not yet";
    const unsigned barriers = backend.state.tiled_barriers;
    EXPECT_EQ(dcall(139, {0, 0}), 0u);
    EXPECT_EQ(dcall(114, {0, OUT2}), d3d11::S_OK);  // FinishCommandList
    EXPECT_EQ(backend.state.tiled_barriers, barriers) << "recorded only";
    ctx(58, {memory.read_u64(OUT2).value(), 0});    // ExecuteCommandList
    set_arg(0, p);
    set_arg(1, OUT);
    d3d11.impl_BufferMethod(marshaler, 10);
    EXPECT_EQ(memory.read_u32(OUT).value(), 8u * TILE) << "executed";
    EXPECT_EQ(backend.state.tiled_barriers, barriers + 1);
    // SetPredication does not skip the tiled operations ([data] P: a
    // predicate that skips CopyResource leaves them applied).
    EXPECT_FALSE(predicated_context_slot(134));
    EXPECT_FALSE(predicated_context_slot(135));
    EXPECT_FALSE(predicated_context_slot(136));
    EXPECT_FALSE(predicated_context_slot(137));
    EXPECT_TRUE(predicated_context_slot(47));
}

TEST_F(D3D11TiledTest, ReleasingResourcesDropsPoolReferences) {
    const GuestAddr p = pool(4);
    const GuestAddr b = tiled_buffer(2 * TILE);
    ASSERT_EQ(map_all(b, p, 1), d3d11::S_OK);
    EXPECT_EQ(pool_refs(), (std::map<u64, u32>{{1, 1}, {2, 1}}));
    // The pool buffer released first: the mappings keep its memory
    // ("the mappings keep the pool alive", [data] X6).
    set_arg(0, p);
    d3d11.impl_BufferMethod(marshaler, 2);
    EXPECT_EQ(pool_refs(), (std::map<u64, u32>{{1, 1}, {2, 1}}));
    set_arg(0, b);
    d3d11.impl_BufferMethod(marshaler, 2);
    EXPECT_TRUE(pool_refs().empty());
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
