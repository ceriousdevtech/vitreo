/// @file test_user_paint.cpp
/// @brief user32 frame drawing and update regions measured on Windows 11
///        (tools/oracle_probe/s3gdi_userpaint_probe.expected, run in the
///        interactive session).

#include <gtest/gtest.h>
#include <winapi/graphics/gdi32/gdi32.hpp>
#include <winapi/user32/painting/frame_drawing.hpp>
#include <winapi/user32/painting/painting.hpp>
#include <core/memory/guest_memory.hpp>

#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kMem = 0x10000;
constexpr i32 kW = 48, kH = 32;
constexpr u32 kFill = 0x00123456;

class UserPaintTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(kMem, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(GdiManager::GDI_DIB_SCRATCH_BASE, GdiManager::GDI_DIB_SCRATCH_SIZE,
                                       MemoryProtection::ReadWrite).is_ok());
        gdi_.set_guest_memory(&memory_);
        const GuestAddr bmi = kMem + 0x100;
        (void)memory_.write_u32(bmi + 0, 40);
        (void)memory_.write_u32(bmi + 4, kW);
        (void)memory_.write_u32(bmi + 8, static_cast<u32>(-kH));
        (void)memory_.write_u16(bmi + 12, 1);
        (void)memory_.write_u16(bmi + 14, 32);
        const Handle dib = gdi_.create_dib_section(0, bmi, 0, kMem + 0x80, false);
        bits_ = memory_.read_u32(kMem + 0x80).value_or(0);
        dc_ = gdi_.create_compatible_dc(0);
        (void)gdi_.select_object(dc_, dib);
    }
    void reset() {
        for (i32 i = 0; i < kW * kH; ++i) (void)memory_.write_u32(bits_ + i * 4, kFill);
    }
    /// The probe's grid: '.' untouched, then a letter per distinct value,
    /// followed by the legend.
    std::string grid(i32 x0, i32 y0, i32 x1, i32 y1) {
        std::vector<u32> vals;
        std::string out;
        for (i32 y = y0; y < y1; ++y) {
            out += " ";
            for (i32 x = x0; x < x1; ++x) {
                const u32 v = memory_.read_u32(bits_ + (y * kW + x) * 4).value();
                if (v == kFill) {
                    out += ".";
                    continue;
                }
                usize k = 0;
                while (k < vals.size() && vals[k] != v) ++k;
                if (k == vals.size()) vals.push_back(v);
                out += static_cast<char>('a' + k);
            }
        }
        out += " legend:";
        char buf[16];
        for (usize k = 0; k < vals.size(); ++k) {
            std::snprintf(buf, sizeof(buf), " %c=%06x", static_cast<char>('a' + k), vals[k] & 0xFFFFFF);
            out += buf;
        }
        return out;
    }
    static std::string expected_grid(const std::string& label) {
        std::ifstream in(std::string(VITREO_ORACLE_PROBE_DIR) + "/s3gdi_userpaint_probe.expected");
        for (std::string line; std::getline(in, line);) {
            const std::string key = label + " grid:";
            if (line.rfind(key, 0) == 0) return line.substr(key.size());
        }
        ADD_FAILURE() << "no grid " << label;
        return {};
    }

    HandleTable handles_;
    GdiManager gdi_{handles_};
    GuestMemory memory_;
    Handle dc_{0};
    GuestAddr bits_{0};
};

TEST_F(UserPaintTest, FocusRectInvertsOddBorderPixels) {
    reset();
    Rect rc{4, 4, 20, 14};
    EXPECT_TRUE(user_draw::draw_focus_rect(gdi_, dc_, rc));
    EXPECT_EQ(grid(4, 4, 20, 14), expected_grid("drawfocusrect"));
    reset();
    rc = {5, 5, 12, 10};
    EXPECT_TRUE(user_draw::draw_focus_rect(gdi_, dc_, rc));
    EXPECT_EQ(grid(5, 5, 12, 10), expected_grid("drawfocusrect odd"));
    EXPECT_FALSE(user_draw::draw_focus_rect(gdi_, 0, rc));
}

TEST_F(UserPaintTest, InvertRectFlipsAllThirtyTwoBits) {
    // "invertrect box=4,4,20,14 ffedcba9*160".
    reset();
    const Rect rc{4, 4, 20, 14};
    EXPECT_TRUE(user_draw::invert_rect(gdi_, dc_, rc));
    EXPECT_EQ(memory_.read_u32(bits_ + (4 * kW + 4) * 4).value(), 0xFFEDCBA9u);
    EXPECT_EQ(memory_.read_u32(bits_ + (3 * kW + 4) * 4).value(), kFill);
    EXPECT_FALSE(user_draw::invert_rect(gdi_, 0, rc));
}

TEST_F(UserPaintTest, DrawEdgeRingsAndAdjust) {
    struct Case {
        u32 edge, flags;
        Rect adjusted;
        std::map<u32, i32> counts;
    };
    // s3gdi_userpaint_probe "drawedge ..." rows (BF_RECT | BF_ADJUST).
    const Case cases[] = {
        {5, 0x200F, {6, 6, 18, 12}, {{0xe3e3e3, 23}, {0x696969, 25}, {0xffffff, 19}, {0xa0a0a0, 21}}},
        {10, 0x200F, {6, 6, 18, 12}, {{0xa0a0a0, 23}, {0xffffff, 25}, {0x696969, 19}, {0xe3e3e3, 21}}},
        {6, 0x200F, {6, 6, 18, 12}, {{0xa0a0a0, 44}, {0xffffff, 44}}},
        {9, 0x200F, {6, 6, 18, 12}, {{0xe3e3e3, 44}, {0x696969, 44}}},
        {4, 0x200F, {5, 5, 19, 13}, {{0xffffff, 23}, {0xa0a0a0, 25}}},
        {5, 0x400F, {4, 4, 20, 14}, {{0xa0a0a0, 48}, {0xf0f0f0, 40}}},     // BF_FLAT
        {10, 0x000C, {4, 4, 20, 14}, {{0xe3e3e3, 23}, {0xffffff, 25}}},    // BF_BOTTOMRIGHT
    };
    for (const auto& c : cases) {
        reset();
        Rect rc{4, 4, 20, 14};
        EXPECT_TRUE(user_draw::draw_edge(gdi_, dc_, rc, c.edge, c.flags));
        if (c.flags & 0x2000) {
            EXPECT_EQ(rc.left, c.adjusted.left);
            EXPECT_EQ(rc.top, c.adjusted.top);
            EXPECT_EQ(rc.right, c.adjusted.right);
            EXPECT_EQ(rc.bottom, c.adjusted.bottom);
        }
        std::map<u32, i32> counts;
        for (i32 i = 0; i < kW * kH; ++i) {
            const u32 v = memory_.read_u32(bits_ + i * 4).value();
            if (v != kFill) ++counts[v & 0xFFFFFF];
        }
        EXPECT_EQ(counts, c.counts) << "edge " << c.edge << " flags " << c.flags;
    }
    Rect rc{4, 4, 20, 14};
    EXPECT_FALSE(user_draw::draw_edge(gdi_, 0, rc, 5, 15));
}

TEST_F(UserPaintTest, FrameControlsMatchWindowsPixels) {
    struct Case {
        u32 type, state;
        const char* label;
    };
    const Case cases[] = {
        {4, 0x10, "dfc push ret=1"},  {4, 0x0, "dfc check ret=1"}, {4, 0x400, "dfc checked ret=1"},
        {4, 0x4, "dfc radio ret=1"},  {1, 0x0, "dfc close ret=1"}, {3, 0x0, "dfc scrollup ret=1"},
        {2, 0x1, "dfc menucheck ret=1"},
    };
    for (const auto& c : cases) {
        reset();
        Rect rc{4, 4, 17, 17};
        EXPECT_TRUE(user_draw::draw_frame_control(gdi_, dc_, rc, c.type, c.state)) << c.label;
        EXPECT_EQ(grid(4, 4, 17, 17), expected_grid(c.label)) << c.label;
    }
}

TEST_F(UserPaintTest, UpdateRegionsAccumulateAndValidateInParts) {
    // "GetUpdateRect=1 rc=10,10,30,20", "union rc=10,5,60,20",
    // "GetUpdateRgn=3", BeginPaint's clip is the region.
    PaintingManager paint;
    paint.set_gdi_manager(&gdi_);
    const u64 hwnd = 0x100;
    const Rect a{10, 10, 30, 20}, b{50, 5, 60, 8};
    EXPECT_TRUE(paint.invalidate_rect(hwnd, &a, true));
    Rect box{};
    EXPECT_TRUE(paint.get_update_rect(hwnd, box, false));
    EXPECT_EQ(box.left, 10);
    EXPECT_EQ(box.bottom, 20);
    EXPECT_TRUE(paint.invalidate_rect(hwnd, &b, false));
    EXPECT_TRUE(paint.get_update_rect(hwnd, box, false));
    EXPECT_EQ(box.left, 10);
    EXPECT_EQ(box.top, 5);
    EXPECT_EQ(box.right, 60);
    EXPECT_EQ(box.bottom, 20);
    ClipRect region{};
    EXPECT_EQ(paint.get_update_region(hwnd, region, false), 3);
    // Validating one part leaves the other.
    EXPECT_TRUE(paint.validate_rect(hwnd, &b));
    EXPECT_EQ(paint.get_update_region(hwnd, region, false), 2);
    PaintStruct ps{};
    const u64 hdc = paint.begin_paint(hwnd, ps);
    EXPECT_EQ(ps.rcPaint.left, 10);
    EXPECT_EQ(ps.rcPaint.right, 30);
    i32 l = 0, t = 0, r = 0, bb = 0;
    EXPECT_EQ(gdi_.get_clip_box(static_cast<Handle>(hdc), l, t, r, bb), 2);
    EXPECT_EQ(l, 10);
    EXPECT_EQ(bb, 20);
    EXPECT_FALSE(paint.get_update_rect(hwnd, box, false));
    EXPECT_TRUE(paint.end_paint(hwnd, ps));
}

}  // namespace
