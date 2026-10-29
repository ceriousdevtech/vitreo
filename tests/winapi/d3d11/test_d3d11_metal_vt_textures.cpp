// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d11_metal_vt_textures.cpp
/// @brief VtFeedbackTest, textures: integer render targets with mips (the page
///        table) and partial updates of a large B8G8R8A8 pool.
///
/// Split from the former test_d3d11_metal.cpp; the fixture is
/// d3d11_test_metal_vt_feedback.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_metal_vt_feedback.hpp"
#include "vt_feedback_shaders.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace vitreo::winapi::d3d11_test {
namespace {

// -- 3. Integer render targets with mips (the page table) -------------------

// An R16_UINT / R32_UINT texture, 16 x 16 with 5 mips, rendered through an
// RTV per mip: mip k gets k * 16 + 1 (R32_UINT: with high bits set). An
// integer target stores the shader's value with no conversion, so `ld` at
// (x >> k, y >> k, k) and a STAGING copy of every mip read exactly it.
// Then a sub-rectangle of mip 1 is redrawn under a scissor and a sub-
// rectangle of mip 2 under a smaller viewport: texels outside them keep
// their values.
TEST_F(VtFeedbackTest, UintTargetMipsHoldExactValuesAndPartialRedraws) {
    const GuestAddr ps = shader(15, vtfb_ps_uint, sizeof(vtfb_ps_uint));
    const GuestAddr cs = shader(18, vtfb_cs_load_mips,
                                sizeof(vtfb_cs_load_mips));
    ASSERT_TRUE(ps != 0 && cs != 0);
    constexpr u32 kSide = 16, kMips = 5;
    for (const u32 format : {kR16Uint, kR32Uint}) {
        SCOPED_TRACE(format == kR16Uint ? "R16_UINT" : "R32_UINT");
        const u32 high = format == kR32Uint ? 0xA5A50000u : 0u;
        const auto value = [&](u32 k) { return high | (k * 16 + 1); };
        const GuestAddr table =
            texture(kSide, kSide, kMips, 1, format, D3D11_USAGE_DEFAULT,
                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0);
        std::array<GuestAddr, kMips> rtvs{};
        for (u32 k = 0; k < kMips; ++k) {
            rtvs[k] = render_target_view(table, format, k);
        }
        pipeline(ps, true);
        if (HasFatalFailure()) return;
        for (u32 k = 0; k < kMips; ++k) {
            const u32 side = kSide >> k;
            viewport(0, 0, static_cast<float>(side), static_cast<float>(side));
            scissor(0, 0, static_cast<i32>(side), static_cast<i32>(side));
            const std::array<u64, 1> list{rtvs[k]};
            context_call(33, {1, put(list), 0});
            constants({value(k), 0, 0, 0});
            draw();
        }
        context_call(33, {0, 0, 0});

        const auto expect_mips = [&](const char* what,
                                     const auto& want) {
            // `ld` from a compute shader.
            const GuestAddr srv = texture_srv(table, format, kMips);
            const GuestAddr out = buffer(kMips * kSide * kSide * 4,
                                         D3D11_USAGE_DEFAULT,
                                         D3D11_BIND_UNORDERED_ACCESS, 0,
                                         D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                                         4, nullptr);
            const std::array<u64, 1> srvs{srv};
            const std::array<u64, 1> uavs{buffer_uav(out, 0,
                                                     kMips * kSide * kSide)};
            context_call(67, {0, 1, put(srvs)});      // CSSetShaderResources
            context_call(68, {0, 1, put(uavs), 0});   // CSSetUnorderedAccessViews
            context_call(69, {cs, 0, 0});
            constants({kSide, kMips, 0, 0});
            context_call(41, {1, 1, 1});
            const std::array<u64, 1> none{0};
            context_call(67, {0, 1, put(none)});
            const auto words = read_buffer(out, kMips * kSide * kSide * 4);
            ASSERT_EQ(words.size(), kMips * kSide * kSide);
            for (u32 k = 0; k < kMips; ++k) {
                usize wrong = 0;
                u32 first_wrong = 0;
                for (u32 y = 0; y < kSide; ++y) {
                    for (u32 x = 0; x < kSide; ++x) {
                        const u32 got = words[k * kSide * kSide + y * kSide + x];
                        if (got != want(k, x >> k, y >> k) && wrong++ == 0) {
                            first_wrong = got;
                        }
                    }
                }
                EXPECT_EQ(wrong, 0u) << what << ": ld at mip " << k
                                     << ", first wrong 0x" << std::hex
                                     << first_wrong;
            }
            // And a STAGING copy of every mip.
            for (u32 k = 0; k < kMips; ++k) {
                const auto level =
                    read_uint_level(table, format, kSide, kSide, kMips, k);
                const u32 side = kSide >> k;
                ASSERT_EQ(level.size(), side * side);
                usize wrong = 0;
                for (u32 y = 0; y < side; ++y) {
                    for (u32 x = 0; x < side; ++x) {
                        wrong += level[y * side + x] != want(k, x, y);
                    }
                }
                EXPECT_EQ(wrong, 0u) << what << ": staging mip " << k;
            }
        };
        expect_mips("every mip", [&](u32 k, u32, u32) { return value(k); });

        // Mip 1 (8x8): scissor (2,1)-(6,5). Mip 2 (4x4): viewport (1,1)
        // 2x2, the scissor the whole mip.
        const u32 patch1 = high | 0xBEEFu, patch2 = high | 0x7E57u;
        viewport(0, 0, 8, 8);
        scissor(2, 1, 6, 5);
        std::array<u64, 1> list{rtvs[1]};
        context_call(33, {1, put(list), 0});
        constants({patch1, 0, 0, 0});
        draw();
        viewport(1, 1, 2, 2);
        scissor(0, 0, 4, 4);
        list = {rtvs[2]};
        context_call(33, {1, put(list), 0});
        constants({patch2, 0, 0, 0});
        draw();
        context_call(33, {0, 0, 0});
        expect_mips("after the partial redraws", [&](u32 k, u32 x, u32 y) {
            if (k == 1 && x >= 2 && x < 6 && y >= 1 && y < 5) return patch1;
            if (k == 2 && x >= 1 && x < 3 && y >= 1 && y < 3) return patch2;
            return value(k);
        });
    }
}

// ClearRenderTargetView of one mip of a UINT target clears that mip's whole
// extent and nothing else (D3D11.3 5.2.3.1: "The full extent of the resource
// view is always cleared"). The float colour becomes the integer as Windows
// converts it, measured on WARP for exactly R16_UINT and R32_UINT
// (tools/oracle_probe/d3d11_a06_resource_ops_probe.expected, "RTV R16_UINT"
// and "RTV R32_UINT" c0-c6): toward zero (300.7 -> 300, 0.5 -> 0; the
// spec's 3.2.2 round-to-nearest-even is NOT what Windows does), negative and
// NaN to 0, past the range to the largest value (70000 -> 0xFFFF,
// +INF and 1e20 -> 0xFFFFFFFF), and a large exact float exactly
// (4294967040 -> 0xFFFFFF00, 5.2.3.1's "the closest value is chosen").
TEST_F(VtFeedbackTest, UintClearOfOneMipFollowsTheIntegerRules) {
    constexpr u32 kSide = 16, kMips = 5;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    struct Case {
        float value;
        u32 r16;
        u32 r32;
    };
    const std::array<Case, 11> cases{{
        {17.0f, 17u, 17u},
        {3.5f, 3u, 3u},
        {300.7f, 300u, 300u},
        {0.5f, 0u, 0u},
        {70000.0f, 0xFFFFu, 70000u},
        {-5.5f, 0u, 0u},
        {nan, 0u, 0u},
        {3.0e9f, 0xFFFFu, 3000000000u},
        {4294967040.0f, 0xFFFFu, 0xFFFFFF00u},
        {1e20f, 0xFFFFu, 0xFFFFFFFFu},
        {inf, 0xFFFFu, 0xFFFFFFFFu},
    }};
    for (const u32 format : {kR16Uint, kR32Uint}) {
        SCOPED_TRACE(format == kR16Uint ? "R16_UINT" : "R32_UINT");
        const GuestAddr table =
            texture(kSide, kSide, kMips, 1, format, D3D11_USAGE_DEFAULT,
                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0);
        for (u32 k = 0; k < kMips; ++k) {
            const std::array<float, 4> c{static_cast<float>(100 + k), 0, 0, 0};
            context_call(50, {render_target_view(table, format, k), put(c)});
        }
        const GuestAddr mip2 = render_target_view(table, format, 2);
        for (const Case& c : cases) {
            SCOPED_TRACE(c.value);
            const std::array<float, 4> colour{c.value, 9, 9, 9};
            context_call(50, {mip2, put(colour)});
            for (u32 k = 0; k < kMips; ++k) {
                const auto level =
                    read_uint_level(table, format, kSide, kSide, kMips, k);
                const u32 want = k == 2 ? (format == kR16Uint ? c.r16 : c.r32)
                                        : 100 + k;
                usize wrong = 0;
                u32 first = 0;
                for (const u32 v : level) {
                    if (v != want && wrong++ == 0) first = v;
                }
                EXPECT_EQ(wrong, 0u) << "mip " << k << " want " << want
                                     << " first wrong " << first;
            }
        }
    }
}

// -- 4. Partial updates of a large B8G8R8A8 pool ----------------------------

// A 9288 x 9288 B8G8R8A8 texture (the landscape's physical pools): Learn
// UpdateSubresource with pDstBox writes only the box, its rows SrcRowPitch
// apart in pSrcData; CopySubresourceRegion writes the source box at
// (DstX, DstY). Boxes 37 and 13 texels wide (no multiple of 16), one at the
// level's far corner, source pitches padded past the row. Every texel of
// each box lands exactly, and the texels around each keep the clear colour.
TEST_F(VtFeedbackTest, LargePoolPartialUpdatesLandExactly) {
    constexpr u32 kPool = 9288;
    const GuestAddr pool =
        texture(kPool, kPool, 1, 1, kBgra8Unorm, D3D11_USAGE_DEFAULT,
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0);
    ASSERT_NE(pool, 0u);
    // R = 1, A = 1: B8G8R8A8 bytes 00 00 FF FF, the word 0xFFFF0000.
    constexpr u32 kClear = 0xFFFF0000u;
    const std::array<float, 4> red{1, 0, 0, 1};
    context_call(50, {render_target_view(pool, kBgra8Unorm, 0), put(red)});

    struct Region {
        u32 x, y, w, h;
        u32 tag;
    };
    const auto texel = [](u32 tag, u32 i, u32 j) {
        return tag | (j << 12) | i;
    };
    // UpdateSubresource boxes.
    const std::array<Region, 2> updates{{{9100, 9200, 37, 23, 0xA0000000u},
                                         {kPool - 13, kPool - 5, 13, 5,
                                          0xB0000000u}}};
    for (const Region& r : updates) {
        const u32 pitch = r.w * 4 + 20;
        std::vector<u8> rows(static_cast<usize>(pitch) * r.h, 0x5A);
        for (u32 j = 0; j < r.h; ++j) {
            for (u32 i = 0; i < r.w; ++i) {
                const u32 v = texel(r.tag, i, j);
                std::memcpy(rows.data() + j * pitch + i * 4, &v, 4);
            }
        }
        const std::array<u32, 6> box{r.x, r.y, 0, r.x + r.w, r.y + r.h, 1};
        context_call(48, {pool, 0, put(box), put(rows.data(), rows.size()),
                          pitch, pitch * r.h});
    }
    // CopySubresourceRegion from a 64 x 48 source's box (5,7)-(34,26).
    std::vector<u32> source(64 * 48);
    for (u32 y = 0; y < 48; ++y) {
        for (u32 x = 0; x < 64; ++x) source[y * 64 + x] = texel(0xC0000000u, x, y);
    }
    const GuestAddr src = texture(64, 48, 1, 1, kBgra8Unorm,
                                  D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE, 0,
                                  {{put(source.data(), source.size() * 4),
                                    64 * 4}});
    const Region copied{9250, 9100, 29, 19, 0xC0000000u};
    const std::array<u32, 6> src_box{5, 7, 0, 34, 26, 1};
    context_call(46, {pool, 0, copied.x, copied.y, 0, src, 0, put(src_box)});

    // Read each region with a one-texel border back through a STAGING copy
    // of that window.
    const auto check = [&](const Region& r, u32 si, u32 sj, const char* what) {
        const u32 x0 = r.x - 1;
        const u32 y0 = r.y - 1;
        const u32 x1 = std::min(kPool, r.x + r.w + 1);
        const u32 y1 = std::min(kPool, r.y + r.h + 1);
        const u32 w = x1 - x0, h = y1 - y0;
        const GuestAddr staging = texture(w, h, 1, 1, kBgra8Unorm,
                                          D3D11_USAGE_STAGING, 0,
                                          D3D11_CPU_ACCESS_READ);
        const std::array<u32, 6> box{x0, y0, 0, x1, y1, 1};
        context_call(46, {staging, 0, 0, 0, 0, pool, 0, put(box)});
        const Mapped m = map(staging, 0, D3D11_MAP_READ);
        ASSERT_EQ(m.hr, d3d11::S_OK) << what;
        usize wrong = 0;
        std::string first;
        for (u32 y = y0; y < y1; ++y) {
            const auto row = words_at(m.data + (y - y0) * m.row_pitch, w);
            for (u32 x = x0; x < x1; ++x) {
                const bool inside = x >= r.x && x < r.x + r.w && y >= r.y &&
                                    y < r.y + r.h;
                const u32 want = inside
                    ? texel(r.tag, x - r.x + si, y - r.y + sj) : kClear;
                if (row[x - x0] != want && wrong++ == 0) {
                    std::ostringstream s;
                    s << "(" << x << "," << y << ") = 0x" << std::hex
                      << row[x - x0] << " want 0x" << want;
                    first = s.str();
                }
            }
        }
        EXPECT_EQ(wrong, 0u) << what << ": first wrong " << first;
        unmap(staging, 0);
    };
    check(updates[0], 0, 0, "UpdateSubresource box");
    check(updates[1], 0, 0, "UpdateSubresource box at the far corner");
    check(copied, 5, 7, "CopySubresourceRegion box");
}


}  // namespace
}  // namespace vitreo::winapi::d3d11_test
