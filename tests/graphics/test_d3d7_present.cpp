// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_d3d7_present.cpp
/// @brief A Direct3D 7 render target is guest-visible DirectDraw surface
///        memory, so a GPU result must land back in it to exist at all.

#include <gtest/gtest.h>

#include "winapi/graphics/ddraw/d3d7_present.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi::d3d7;

TEST(D3D7Present, Bgra8RowsCopyVerbatimAt32Bpp) {
    // Source pitch deliberately exceeds width*4: a DirectDraw surface and a
    // readback buffer are allocated with independent alignment.
    constexpr u32 kWidth = 2, kHeight = 2;
    const std::vector<u8> src = {
        10, 20, 30, 40, 11, 21, 31, 41, 0xEE, 0xEE,  // row 0 + padding
        12, 22, 32, 42, 13, 23, 33, 43, 0xEE, 0xEE,  // row 1 + padding
    };
    std::vector<u8> dst(kHeight * 12u, 0);
    ASSERT_TRUE(copy_bgra8_to_surface(src.data(), 10, kWidth, kHeight,
                                      dst.data(), 12, 32));
    EXPECT_EQ(dst[0], 10u);
    EXPECT_EQ(dst[3], 40u);
    EXPECT_EQ(dst[12], 12u);  // row 1 starts at the destination pitch
    EXPECT_EQ(dst[15], 42u);
    EXPECT_EQ(dst[8], 0u);    // padding untouched, not copied from source
}

TEST(D3D7Present, Rgb565TruncatesHighBitsPerChannel) {
    // One opaque white and one pure blue pixel, in B,G,R,A order.
    const std::vector<u8> src = {255, 255, 255, 255, 255, 0, 0, 255};
    std::vector<u8> dst(4, 0);
    ASSERT_TRUE(copy_bgra8_to_surface(src.data(), 8, 2, 1, dst.data(), 4, 16));

    u16 white = 0, blue = 0;
    std::memcpy(&white, dst.data(), sizeof(white));
    std::memcpy(&blue, dst.data() + 2, sizeof(blue));
    EXPECT_EQ(white, 0xFFFFu);  // saturated stays saturated
    EXPECT_EQ(blue, 0x001Fu);   // blue occupies the low five bits
}

TEST(D3D7Present, RejectsUndefinedDepthWithoutWriting) {
    const std::vector<u8> src(16, 0x7F);
    std::vector<u8> dst(16, 0xAB);
    // An 8bpp indexed target has no defined mapping from a rendered frame.
    EXPECT_FALSE(copy_bgra8_to_surface(src.data(), 8, 2, 2, dst.data(), 8, 8));
    for (u8 byte : dst) EXPECT_EQ(byte, 0xABu);
}

TEST(D3D7Present, RejectsPitchSmallerThanRow) {
    const std::vector<u8> src(32, 0);
    std::vector<u8> dst(32, 0);
    EXPECT_FALSE(copy_bgra8_to_surface(src.data(), 4, 4, 1, dst.data(), 16, 32));
    EXPECT_FALSE(copy_bgra8_to_surface(src.data(), 16, 4, 1, dst.data(), 4, 32));
}
