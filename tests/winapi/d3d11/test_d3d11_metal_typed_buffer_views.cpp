// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d11_metal_typed_buffer_views.cpp
/// @brief D3D11TypedBufferOffsetTest: several unaligned views in one draw,
///        view formats in and out of bounds, typed UAV writes.
///
/// Split from the former test_d3d11_metal.cpp; the fixture is
/// d3d11_test_metal_typed_buffer.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_metal_typed_buffer.hpp"
#include "../../graphics/typed_buffer_offset_shaders.h"

#include <array>
#include <cstring>
#include <vector>

namespace vitreo::winapi::d3d11_test {
namespace {

// Four views of one buffer at different FirstElements -- element biases of
// 2, 1, 0 and 3 against a 16-byte alignment -- read by one draw.
TEST_F(D3D11TypedBufferOffsetTest, FourUnalignedViewsInOneDraw) {
    const GuestAddr ps = shader(15, tbo_ps_read_four, sizeof(tbo_ps_read_four));
    const u32 bytes = kElements * 4;
    const auto a = ramp(100000.0f);
    const GuestAddr data = buffer(bytes, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE, 0, 0, a.data());
    const std::array<u32, 4> firsts{kSection, 2 * kSection + 1,
                                    3 * kSection + 2, 3};
    const GuestAddr v0 = srv(data, kR32Float, firsts[0], kWidth);
    const GuestAddr v1 = srv(data, kR32Float, firsts[1], kWidth);
    const GuestAddr v2 = srv(data, kR32Float, firsts[2], kWidth);
    const GuestAddr v3 = srv(data, kR32Float, firsts[3], kWidth);
    bind_target(ps);
    ps_views({v0, v1, v2, v3});
    columns(0, kWidth);
    draw();
    const auto texels = read_target();
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            for (u32 c = 0; c < 4; ++c) {
                EXPECT_EQ(texels[y * kWidth + x][c],
                          100000.0f + static_cast<float>(firsts[c] + x))
                    << "pixel " << x << "," << y << " view t" << c;
            }
        }
    }
}

// CSReadViews through two views of each format at FirstElements whose byte
// offsets are not 16-byte multiples (R32_FLOAT 270 and 271 -- 1084 is not a
// multiple of the format size x 4 -- R16_FLOAT 270 and 1, R8G8B8A8_UNORM 270
// and 3) and R32G32B32A32_FLOAT, whose every element is aligned. Threads 0-7
// read elements 0-7; threads 8-63 read past the 8-element views, and 64-67
// read 0xFFFFFFFF - k, which an added element bias would wrap into the
// elements in front of the view: Learn (ld) gives 0 in the format's
// components and the default (alpha 1) in the missing ones. GetDimensions
// is NumElements, 8.
TEST_F(D3D11TypedBufferOffsetTest, ViewFormatsReadInBoundsAndOutOfBounds) {
    const GuestAddr cs =
        shader(18, tbo_cs_read_views, sizeof(tbo_cs_read_views));
    ASSERT_NE(cs, 0u);
    constexpr u32 kThreads = 68;
    constexpr u32 kViewElements = 8;
    const u32 dump_bytes = kThreads * 48;
    const GuestAddr dump =
        buffer(dump_bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
               D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, nullptr);
    const GuestAddr dump_view =
        uav(dump, kR32Typeless, 0, dump_bytes / 4, d3d11::BUFFER_UAV_FLAG_RAW);

    struct Case {
        const char* name;
        u32 format;
        u32 element_bytes;
        std::array<u32, 2> firsts;
        bool has_alpha;
    };
    const std::array<Case, 4> cases{{
        {"R32_FLOAT", kR32Float, 4, {kSection, kSection + 1}, false},
        {"R16_FLOAT", kR16Float, 2, {kSection, 1}, false},
        {"R8G8B8A8_UNORM", kRgba8Unorm, 4, {kSection, 3}, true},
        {"R32G32B32A32_FLOAT", kRgba32Float, 16, {37, 200}, true},
    }};
    // 4352 bytes of a byte ramp: byte k = k & 0xFF, except R32_FLOAT /
    // R16_FLOAT, whose element e holds the value e (exact in both).
    constexpr u32 kBytes = kElements * 4;
    for (const Case& test : cases) {
        std::vector<u8> bytes(kBytes);
        for (u32 k = 0; k < kBytes; ++k) bytes[k] = static_cast<u8>(k);
        if (test.format == kR32Float || test.format == kRgba32Float) {
            for (u32 w = 0; w < kBytes / 4; ++w) {
                const float v = static_cast<float>(w);
                std::memcpy(&bytes[w * 4], &v, 4);
            }
        } else if (test.format == kR16Float) {
            // Half-precision integers e < 2048: exponent 15 + log2(e).
            for (u32 e = 0; e < kBytes / 2; ++e) {
                const u32 v = e % 1024;
                u16 half = 0;
                if (v != 0) {
                    u32 exp = 0;
                    while ((v >> (exp + 1)) != 0) ++exp;
                    const u32 mantissa = (v << (10 - exp)) & 0x3FFu;
                    half = static_cast<u16>(((15 + exp) << 10) | mantissa);
                }
                std::memcpy(&bytes[e * 2], &half, 2);
            }
        }
        const GuestAddr data =
            buffer(kBytes, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                   0, bytes.data());
        const GuestAddr t0 =
            srv(data, test.format, test.firsts[0], kViewElements);
        const GuestAddr t1 =
            srv(data, test.format, test.firsts[1], kViewElements);
        context_call(69, {cs, 0, 0});
        const std::array<u64, 2> views{t0, t1};
        context_call(67, {0, 2, put(views)});  // CSSetShaderResources
        const std::array<u64, 1> uavs{dump_view};
        context_call(68, {0, 1, put(uavs), 0});
        context_call(41, {kThreads, 1, 1});
        const auto words = read_buffer(dump, dump_bytes);

        const auto expected = [&](u32 first, u32 i) {
            std::array<float, 4> v{0, 0, 0, test.has_alpha ? 0.0f : 1.0f};
            if (i >= kViewElements) return v;
            const u32 e = first + i;
            if (test.format == kR32Float) {
                v[0] = static_cast<float>(e);
            } else if (test.format == kR16Float) {
                v[0] = static_cast<float>(e % 1024);
            } else if (test.format == kRgba8Unorm) {
                for (u32 c = 0; c < 4; ++c) {
                    v[c] = static_cast<float>((e * 4 + c) & 0xFF) / 255.0f;
                }
            } else {
                for (u32 c = 0; c < 4; ++c) {
                    v[c] = static_cast<float>(e * 4 + c);
                }
            }
            return v;
        };
        for (u32 t = 0; t < kThreads; ++t) {
            const u32 i = t < 64 ? t : 0xFFFFFFFFu - (t - 64);
            for (u32 view = 0; view < 2; ++view) {
                const auto want = expected(test.firsts[view], i);
                for (u32 c = 0; c < 4; ++c) {
                    float got = 0;
                    std::memcpy(&got, &words[t * 12 + view * 4 + c], 4);
                    if (test.format == kRgba8Unorm) {
                        EXPECT_NEAR(got, want[c], 1e-6f)
                            << test.name << " t" << view << " thread " << t
                            << " component " << c;
                    } else {
                        EXPECT_EQ(got, want[c])
                            << test.name << " t" << view << " thread " << t
                            << " component " << c;
                    }
                }
            }
            EXPECT_EQ(words[t * 12 + 8], kViewElements) << test.name;
            EXPECT_EQ(words[t * 12 + 9], kViewElements) << test.name;
            EXPECT_EQ(words[t * 12 + 10], 0xC0DEu) << test.name;
        }
        // The edges by name: ld at index NumElements and NumElements + bias
        // - 1, the bias the view's distance past a 16-byte boundary in
        // elements (Apple GPUs' linear-texture alignment; the loop above
        // covers every index whatever the alignment). Exactly 0 and the
        // default alpha -- never buffer contents.
        for (u32 view = 0; view < 2; ++view) {
            const u32 bias =
                (test.firsts[view] * test.element_bytes) % 16u /
                test.element_bytes;
            for (const u32 i : {kViewElements, kViewElements + bias - 1}) {
                if (i < kViewElements) continue;  // bias 0: one edge
                for (u32 c = 0; c < 4; ++c) {
                    float got = 0;
                    std::memcpy(&got, &words[i * 12 + view * 4 + c], 4);
                    EXPECT_EQ(got, c == 3 && !test.has_alpha ? 1.0f : 0.0f)
                        << test.name << " t" << view << " ld at index " << i
                        << " component " << c;
                }
            }
        }
    }
}

// RWBuffer<float> and RWBuffer<uint> UAVs at unaligned FirstElements (270 and
// 271): CSWriteViews reads u1[i], stores 1000 + t there, and adds 1 to u2[i]
// atomically. In-range threads see and change exactly elements first + i;
// the out-of-range ones (8-67, the last four wrapping) write nothing
// (Learn: store_uav_typed / imm_atomic_iadd past the view write nothing),
// so every element outside the views keeps its value. ld_uav_typed past the
// view reads 0 in red (Learn: as ld). GetDimensions is 8.
TEST_F(D3D11TypedBufferOffsetTest, UnalignedTypedUavsWriteOnlyTheirElements) {
    const GuestAddr cs =
        shader(18, tbo_cs_write_views, sizeof(tbo_cs_write_views));
    ASSERT_NE(cs, 0u);
    constexpr u32 kThreads = 68;
    constexpr u32 kViewElements = 8;
    const u32 dump_bytes = kThreads * 16;
    const GuestAddr dump =
        buffer(dump_bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
               D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, nullptr);
    const GuestAddr dump_view =
        uav(dump, kR32Typeless, 0, dump_bytes / 4, d3d11::BUFFER_UAV_FLAG_RAW);
    const u32 bytes = kElements * 4;
    const auto floats = ramp(100000.0f);
    std::vector<u32> uints(kElements);
    for (u32 e = 0; e < kElements; ++e) uints[e] = 7000u + e;
    const u32 bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    const GuestAddr fbuf =
        buffer(bytes, D3D11_USAGE_DEFAULT, bind, 0, 0, floats.data());
    const GuestAddr ubuf =
        buffer(bytes, D3D11_USAGE_DEFAULT, bind, 0, 0, uints.data());
    const GuestAddr fview = uav(fbuf, kR32Float, kSection, kViewElements);
    const GuestAddr uview = uav(ubuf, kR32Uint, kSection + 1, kViewElements);
    ASSERT_TRUE(fview != 0 && uview != 0);
    context_call(69, {cs, 0, 0});
    const std::array<u64, 3> uavs{dump_view, fview, uview};
    context_call(68, {0, 3, put(uavs), 0});
    context_call(41, {kThreads, 1, 1});
    const std::array<u64, 3> none{0, 0, 0};
    context_call(68, {0, 3, put(none), 0});

    const auto words = read_buffer(dump, dump_bytes);
    for (u32 t = 0; t < kThreads; ++t) {
        float before = 0;
        std::memcpy(&before, &words[t * 4], 4);
        if (t < kViewElements) {
            EXPECT_EQ(before, 100000.0f + static_cast<float>(kSection + t))
                << "thread " << t;
            EXPECT_EQ(words[t * 4 + 1], 7000u + kSection + 1 + t)
                << "thread " << t;
        } else {
            EXPECT_EQ(before, 0.0f) << "thread " << t << " read past the view";
            // No atomic ran past the view: imm_atomic_iadd returns 0.
            EXPECT_EQ(words[t * 4 + 1], 0u)
                << "thread " << t << " atomic past the view";
        }
        EXPECT_EQ(words[t * 4 + 2], kViewElements) << "thread " << t;
        EXPECT_EQ(words[t * 4 + 3], kViewElements) << "thread " << t;
    }
    const auto fwords = read_buffer(fbuf, bytes);
    const auto uwords = read_buffer(ubuf, bytes);
    for (u32 e = 0; e < kElements; ++e) {
        float f = 0;
        std::memcpy(&f, &fwords[e], 4);
        const bool in_f = e >= kSection && e < kSection + kViewElements;
        EXPECT_EQ(f, in_f ? 1000.0f + static_cast<float>(e - kSection)
                          : 100000.0f + static_cast<float>(e))
            << "float element " << e;
        const bool in_u = e >= kSection + 1 && e < kSection + 1 + kViewElements;
        EXPECT_EQ(uwords[e], 7000u + e + (in_u ? 1u : 0u))
            << "uint element " << e;
    }
    // The edges by name: index NumElements and NumElements + bias - 1, the
    // bias being the view's distance past a 16-byte boundary in elements
    // (Apple GPUs' linear-texture alignment: 2 for 270, 3 for 271; the loop
    // above covers every index whatever the alignment).
    for (const u32 i : {kViewElements, kViewElements + 2 - 1}) {
        float before = 0;
        std::memcpy(&before, &words[i * 4], 4);
        EXPECT_EQ(before, 0.0f) << "ld_uav_typed at index " << i;
        float f = 0;
        std::memcpy(&f, &fwords[kSection + i], 4);
        EXPECT_EQ(f, 100000.0f + static_cast<float>(kSection + i))
            << "store_uav_typed at index " << i;
    }
    for (const u32 i : {kViewElements, kViewElements + 3 - 1}) {
        EXPECT_EQ(words[i * 4 + 1], 0u) << "imm_atomic_iadd at index " << i;
        EXPECT_EQ(uwords[kSection + 1 + i], 7000u + kSection + 1 + i)
            << "imm_atomic_iadd at index " << i;
    }
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
