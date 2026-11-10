/// @file test_static_image_layout.cpp
/// @brief An SS_BITMAP / SS_ICON static sizes itself to its image (MS Learn,
///        Static Control Styles): SS_CENTERIMAGE and SS_REALSIZECONTROL keep
///        the control's size, SS_RIGHTJUST keeps the lower-right corner.

#include <gtest/gtest.h>
#include <winapi/user32/messages/static_image_layout.hpp>

#include <array>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::windowing::Rect;

namespace {

constexpr u32 kSsBitmap = 0x0E;
constexpr u32 kWsChildVisible = 0x50000000;

void expect_rect(const Rect& r, i32 l, i32 t, i32 rr, i32 b) {
    EXPECT_EQ(r.left, l);
    EXPECT_EQ(r.top, t);
    EXPECT_EQ(r.right, rr);
    EXPECT_EQ(r.bottom, b);
}

TEST(StaticImageLayout, BitmapStaticTakesTheImageSizeAtItsTopLeft) {
    const Rect window{76, 74, 98, 95};  // 22x21 template placeholder
    expect_rect(static_image_window_rect(window, window,
                                         kWsChildVisible | kSsBitmap, 835, 400),
                76, 74, 76 + 835, 74 + 400);
    // A larger control shrinks to the image as well.
    const Rect big{0, 0, 300, 300};
    expect_rect(static_image_window_rect(big, big, kSsBitmap, 275, 50),
                0, 0, 275, 50);
}

TEST(StaticImageLayout, CenterImageAndRealSizeControlKeepTheControlSize) {
    const Rect window{10, 20, 232, 60};
    for (u32 extra : {kSsCenterImage, kSsRealSizeControl,
                      kSsCenterImage | kSsRealSizeControl,
                      kSsCenterImage | kSsRightJust}) {
        expect_rect(static_image_window_rect(window, window, kSsBitmap | extra,
                                             275, 50),
                    10, 20, 232, 60);
    }
}

TEST(StaticImageLayout, RightJustKeepsTheLowerRightCorner) {
    const Rect window{100, 100, 200, 150};
    expect_rect(static_image_window_rect(window, window,
                                         kSsBitmap | kSsRightJust, 32, 16),
                168, 134, 200, 150);
}

TEST(StaticImageLayout, BorderAroundTheClientAreaIsKept) {
    // WS_BORDER-style non-client: window 2px larger than client each axis.
    const Rect window{0, 0, 52, 52};
    const Rect client{0, 0, 50, 50};
    expect_rect(static_image_window_rect(window, client, kSsBitmap, 100, 40),
                0, 0, 102, 42);
}

TEST(StaticImageLayout, EmptyImageLeavesTheControlAlone) {
    const Rect window{5, 5, 27, 26};
    expect_rect(static_image_window_rect(window, window, kSsBitmap, 0, 0),
                5, 5, 27, 26);
}

TEST(StaticImageLayout, PackedDibSizeReadsCoreAndInfoHeaders) {
    // BITMAPINFOHEADER, bottom-up 835x400.
    std::array<u8, 40> info{};
    info[0] = 40;
    const i32 w = 835, h = 400;
    std::memcpy(&info[4], &w, 4);
    std::memcpy(&info[8], &h, 4);
    i32 ow = 0, oh = 0;
    ASSERT_TRUE(packed_dib_size(info.data(), info.size(), ow, oh));
    EXPECT_EQ(ow, 835);
    EXPECT_EQ(oh, 400);
    // Top-down DIB: negative biHeight.
    const i32 neg = -50;
    std::memcpy(&info[8], &neg, 4);
    ASSERT_TRUE(packed_dib_size(info.data(), info.size(), ow, oh));
    EXPECT_EQ(oh, 50);
    // BITMAPCOREHEADER: WORD width/height.
    std::array<u8, 12> core{};
    core[0] = 12;
    core[4] = 0x13; core[5] = 0x01;  // 275
    core[6] = 50;
    ASSERT_TRUE(packed_dib_size(core.data(), core.size(), ow, oh));
    EXPECT_EQ(ow, 275);
    EXPECT_EQ(oh, 50);
    EXPECT_FALSE(packed_dib_size(core.data(), 8, ow, oh));
}

}  // namespace
