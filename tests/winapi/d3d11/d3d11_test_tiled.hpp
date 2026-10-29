// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file d3d11_test_tiled.hpp
/// @brief D3D11TiledTest: the tiled-resource fixture.

#include "d3d11_test_surface.hpp"

namespace vitreo::winapi::d3d11_test {

// -- Tiled resources (d3d11_tiled.cpp) ----------------------------------------
//
// Expected values: tools/oracle_probe/d3d11_tiled_probe64.expected (Windows
// WARP, FL 11_0; the same for x86) and Learn's tiled-resource pages. The fake
// backend lays textures out as the Metal backend does (standard shapes, a
// packed tail from the first mip no more than half a tile).

class D3D11TiledTest : public D3D11SurfaceTest {
protected:
    static constexpr GuestAddr T_BASE = 0x200000;
    static constexpr GuestAddr T_COORDS = T_BASE;          // 32 coordinates
    static constexpr GuestAddr T_SIZES = T_BASE + 0x200;   // 32 region sizes
    static constexpr GuestAddr T_FLAGS = T_BASE + 0x400;
    static constexpr GuestAddr T_OFFSETS = T_BASE + 0x500;
    static constexpr GuestAddr T_COUNTS = T_BASE + 0x600;
    static constexpr GuestAddr T_OUT = T_BASE + 0x700;
    static constexpr GuestAddr T_DESC = T_BASE + 0x800;
    static constexpr GuestAddr T_TILINGS = T_BASE + 0x900;  // 64 x 12 bytes
    static constexpr GuestAddr T_DATA = T_BASE + 0x10000;   // 4 tiles
    static constexpr u32 TILE = 65536;
    static constexpr u32 TILED = 0x40000;
    static constexpr u32 POOL = 0x20000;
    static constexpr u32 SRV = 0x8;
    static constexpr u32 RTV = 0x20;

    void SetUp() override {
        backend.state.tiled_resources = true;
        D3D11SurfaceTest::SetUp();
        ASSERT_TRUE(memory.map_region(T_BASE, 0x60000,
                                      MemoryProtection::ReadWrite).is_ok());
    }

    u32 ctx(usize slot, std::initializer_list<u64> args) {
        context_call(slot, args);
        return static_cast<u32>(cpu.gpr.rax);
    }
    u32 make_buffer(u32 bytes, u32 bind, u32 misc, GuestAddr* out = nullptr,
                    u32 usage = 0, u32 cpu_access = 0, GuestAddr init = 0) {
        write_words(T_DESC, {bytes, usage, bind, cpu_access, misc,
                             (misc & 0x40u) != 0 ? 16u : 0u});
        EXPECT_TRUE(memory.write_u64(T_OUT, 0).is_ok());
        const u32 hr = device_call(3, {T_DESC, init, T_OUT});
        if (out != nullptr) *out = memory.read_u64(T_OUT).value();
        return hr;
    }
    GuestAddr pool(u32 tiles) {
        GuestAddr p = 0;
        EXPECT_EQ(make_buffer(tiles * TILE, 0, POOL, &p), d3d11::S_OK);
        return p;
    }
    GuestAddr tiled_buffer(u32 bytes, const void** backend_object = nullptr) {
        GuestAddr b = 0;
        EXPECT_EQ(make_buffer(bytes, SRV, TILED, &b), d3d11::S_OK);
        if (backend_object != nullptr) {
            *backend_object = backend.state.last_tiled_object;
        }
        return b;
    }
    u32 make_texture(u32 w, u32 h, u32 mips, u32 arr, u32 fmt, u32 samples,
                     u32 bind, u32 misc, GuestAddr* out = nullptr,
                     u32 usage = 0, u32 cpu_access = 0) {
        write_words(T_DESC, {w, h, mips, arr, fmt, samples, 0, usage, bind,
                             cpu_access, misc});
        EXPECT_TRUE(memory.write_u64(T_OUT, 0).is_ok());
        const u32 hr = device_call(5, {T_DESC, 0, T_OUT});
        if (out != nullptr) *out = memory.read_u64(T_OUT).value();
        return hr;
    }
    GuestAddr tiled_texture(u32 w, u32 h, u32 mips, u32 arr,
                            const void** backend_object = nullptr,
                            u32 fmt = 28, u32 bind = SRV) {
        GuestAddr t = 0;
        EXPECT_EQ(make_texture(w, h, mips, arr, fmt, 1, bind, TILED, &t),
                  d3d11::S_OK);
        if (backend_object != nullptr) {
            *backend_object = backend.state.last_tiled_object;
        }
        return t;
    }
    void coord(u32 index, u32 x, u32 y, u32 z, u32 sub) {
        write_words(T_COORDS + index * 16, {x, y, z, sub});
    }
    void size(u32 index, u32 tiles, u32 box = 0, u32 w = 0, u32 h = 0,
              u32 d = 0) {
        write_words(T_SIZES + index * 16, {tiles, box, w, (d << 16) | h});
    }
    /// UpdateTileMappings, argument by argument (0 for a NULL array).
    u32 map(GuestAddr resource, u32 regions, GuestAddr coords,
            GuestAddr sizes, GuestAddr pool_buffer, u32 ranges,
            GuestAddr flags, GuestAddr offsets, GuestAddr counts,
            u32 mapping_flags = 0) {
        return ctx(134, {resource, regions, coords, sizes, pool_buffer, ranges,
                         flags, offsets, counts, mapping_flags});
    }
    /// Map a whole resource to pool tiles from @p first.
    u32 map_all(GuestAddr resource, GuestAddr pool_buffer, u32 first) {
        write_words(T_OFFSETS, {first});
        return map(resource, 1, 0, 0, pool_buffer, 1, 0, T_OFFSETS, 0);
    }
    const std::map<BackendState::TileKey, u64>& mappings(const void* object) {
        return backend.state.tile_mappings[object];
    }
    static BackendState::TileKey tile(u32 level, u32 slice, u32 x, u32 y,
                                      bool packed = false) {
        return {level, slice, x, y, packed};
    }
    /// Pool references held on the pool with the most references.
    std::map<u64, u32> pool_refs() {
        std::map<u64, u32> all;
        for (const auto& [object, refs] : backend.state.tile_pool_refs) {
            for (const auto& [t, n] : refs) all[t] += n;
        }
        return all;
    }
};

}  // namespace vitreo::winapi::d3d11_test
