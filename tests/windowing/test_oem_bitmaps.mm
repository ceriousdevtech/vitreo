// OBM_CHECKBOXES has Windows' shape (tools/oracle_probe/obm_bitmaps_probe:
// 52x39, 32 bpp, a 4x3 grid of 13x13 cells) but AppKit's checkbox and radio
// glyphs. Ids the host does not draw return 0 so user32 can fail them.

#import <Cocoa/Cocoa.h>
#include <gtest/gtest.h>
#include <windowing/cocoa/cocoa_bridge.hpp>

#include <cstdlib>
#include <fstream>

using namespace vitreo;
using namespace vitreo::windowing;

namespace {

/// 0xRRGGBB at (x, y) top-down in a bottom-up 32 bpp BMP file.
u32 pixel(const std::vector<u8>& bmp, int width, int height, int x, int y) {
    const u32 offset = bmp[10] | (bmp[11] << 8) | (bmp[12] << 16) | (bmp[13] << 24);
    const u8* p = bmp.data() + offset + (static_cast<size_t>(height - 1 - y) * width + x) * 4;
    return (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[1]) << 8) | p[0];
}

/// Number of pixels that differ between two 13x13 cells.
int cell_difference(const std::vector<u8>& bmp, int a_col, int a_row, int b_col, int b_row) {
    int differ = 0;
    for (int y = 0; y < 13; ++y) {
        for (int x = 0; x < 13; ++x) {
            if (pixel(bmp, 52, 39, a_col * 13 + x, a_row * 13 + y) !=
                pixel(bmp, 52, 39, b_col * 13 + x, b_row * 13 + y)) {
                ++differ;
            }
        }
    }
    return differ;
}

TEST(OemBitmapTest, CheckboxesHaveWindowsShapeAndAppKitStates) {
    CocoaBridge bridge;
    const u32 handle = bridge.register_oem_bitmap(32759);  // OBM_CHECKBOXES
    ASSERT_NE(handle, 0u);
    CocoaBridge::BitmapDims dims;
    ASSERT_TRUE(bridge.get_bitmap_dims(handle, dims));
    EXPECT_EQ(dims.width, 52);
    EXPECT_EQ(dims.height, 39);
    EXPECT_EQ(dims.bpp, 32);
    const auto* bmp = bridge.get_bitmap_bmp_data(handle);
    ASSERT_NE(bmp, nullptr);
    if (const char* out = std::getenv("VITREO_OEM_BITMAP_DUMP")) {
        std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(bmp->data()),
                                                   static_cast<std::streamsize>(bmp->size()));
    }
    // Each state is drawn: on differs from off, checkbox from radio, and the
    // indeterminate checkbox from both checkbox states.
    EXPECT_GT(cell_difference(*bmp, 0, 0, 1, 0), 4);
    EXPECT_GT(cell_difference(*bmp, 0, 1, 1, 1), 4);
    EXPECT_GT(cell_difference(*bmp, 1, 0, 1, 1), 4);
    EXPECT_GT(cell_difference(*bmp, 2, 2, 0, 0), 4);
    EXPECT_GT(cell_difference(*bmp, 2, 2, 1, 0), 4);
    // The empty cells stay the white background.
    for (int col : {1, 3}) {
        for (int y = 0; y < 13; ++y) {
            for (int x = 0; x < 13; ++x) {
                EXPECT_EQ(pixel(*bmp, 52, 39, col * 13 + x, 26 + y), 0xffffffu);
            }
        }
    }
}

// DrawFrameControl(DFC_BUTTON) composites these over the guest's own pixels:
// straight alpha, transparent around the glyph, one distinct image per state.
TEST(OemBitmapTest, ButtonGlyphsAreAlphaAndDistinctPerState) {
    CocoaBridge bridge;
    auto render = [&](bool radio, bool checked, bool mixed, bool enabled) {
        BitmapHost::ButtonGlyph g;
        g.radio = radio; g.checked = checked; g.mixed = mixed; g.enabled = enabled;
        std::vector<u8> rgba;
        EXPECT_TRUE(bridge.render_button_glyph(g, 16, 16, rgba));
        EXPECT_EQ(rgba.size(), 16u * 16u * 4u);
        return rgba;
    };
    const auto off = render(false, false, false, true);
    const auto on = render(false, true, false, true);
    const auto mixed = render(false, true, true, true);
    const auto radio_on = render(true, true, false, true);
    const auto disabled_on = render(false, true, false, false);
    EXPECT_NE(off, on);
    EXPECT_NE(on, mixed);
    EXPECT_NE(on, radio_on);
    EXPECT_NE(on, disabled_on);
    bool transparent = false, opaque = false;
    for (size_t i = 3; i < on.size(); i += 4) {
        transparent |= on[i] == 0;
        opaque |= on[i] == 255;
    }
    EXPECT_TRUE(transparent);
    EXPECT_TRUE(opaque);
    std::vector<u8> none;
    EXPECT_FALSE(bridge.render_button_glyph({}, 0, 16, none));
}

TEST(OemBitmapTest, UndrawnIdsReturnZero) {
    CocoaBridge bridge;
    EXPECT_EQ(bridge.register_oem_bitmap(32754), 0u);  // OBM_CLOSE: not drawn yet
    EXPECT_EQ(bridge.register_oem_bitmap(1), 0u);
}

}  // namespace
