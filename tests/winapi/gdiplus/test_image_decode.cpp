/// @file test_image_decode.cpp
/// @brief GDI+ image decoding.
///
/// The fixture is generated, not captured, so the expected pixels are exact:
/// a 2x2 RGBA PNG whose corners are opaque red, opaque green, opaque blue and
/// 50%-alpha white.

#include <gtest/gtest.h>

#include <winapi/graphics/gdiplus/image_decode.hpp>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi::gdiplus_decode;

namespace {

std::vector<u8> read_fixture(const std::string& name) {
    // Tests run from the build directory; the fixture lives with the source.
    for (const char* prefix : {"", "../", "../../"}) {
        const std::string path =
            std::string(prefix) + "tests/winapi/gdiplus/" + name;
        std::ifstream f(path, std::ios::binary);
        if (!f) continue;
        return std::vector<u8>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    }
    return {};
}

} // namespace

TEST(GdiplusDecodeTest, DecodesPngToTopDownBgra) {
    auto bytes = read_fixture("rgba_2x2.png");
    ASSERT_FALSE(bytes.empty()) << "fixture not found";

    auto image = decode(std::span<const u8>(bytes.data(), bytes.size()));
    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(image->width, 2u);
    EXPECT_EQ(image->height, 2u);
    ASSERT_EQ(image->bgra.size(), 2u * 2u * 4u);

    // Row 0 must be the TOP row: CoreGraphics draws bottom-up, so a missing
    // flip silently returns the image upside down.
    const auto px = [&](usize i) {
        return std::vector<u8>(image->bgra.begin() + static_cast<long>(i * 4),
                               image->bgra.begin() + static_cast<long>(i * 4 + 4));
    };
    EXPECT_EQ(px(0), (std::vector<u8>{0, 0, 255, 255}));   // red   -> B,G,R,A
    EXPECT_EQ(px(1), (std::vector<u8>{0, 255, 0, 255}));   // green
    EXPECT_EQ(px(2), (std::vector<u8>{255, 0, 0, 255}));   // blue
}

TEST(GdiplusDecodeTest, ReturnsStraightAlphaNotPremultiplied) {
    auto bytes = read_fixture("rgba_2x2.png");
    ASSERT_FALSE(bytes.empty());
    auto image = decode(std::span<const u8>(bytes.data(), bytes.size()));
    ASSERT_TRUE(image.has_value());

    // The last pixel is white at 50% alpha. Premultiplied it would come back
    // near 128; GDI+ reports straight alpha, so the colour stays at full white
    // and only the alpha channel carries the transparency.
    const usize i = 3 * 4;
    EXPECT_EQ(image->bgra[i + 3], 128);
    EXPECT_GE(image->bgra[i + 0], 250);
    EXPECT_GE(image->bgra[i + 1], 250);
    EXPECT_GE(image->bgra[i + 2], 250);
}

TEST(GdiplusDecodeTest, RejectsNonImageBytes) {
    const std::vector<u8> junk(64, 0xAB);
    EXPECT_FALSE(decode(std::span<const u8>(junk.data(), junk.size())).has_value());
    EXPECT_FALSE(decode(std::span<const u8>()).has_value());
}
