/// @file test_gl_extensions_metal.cpp
/// @brief GDI Generic's extensions on the real Metal backend: GL_EXT_bgra
///        through glDrawPixels / glReadPixels, a GL_EXT_paletted_texture
///        index texture drawn through its colour table, glDrawRangeElementsWIN
///        drawing, and GL_WIN_swap_hint: what SwapBuffers copies to the front
///        buffer with hint rectangles pending.
///
/// Kept out of vitreo_winapi_tests, which does not link the Metal backend.
/// Expected values: tools/oracle_probe/gl11_state_probe64.expected (ext-*)
/// and swap_hint_probe64.expected (hint-*), cited at each assertion.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <graphics/contracts/backend_device.hpp>
#include <graphics/metal/metal_device.hpp>

#include "winapi/graphics/opengl32/gl_backend_draw.hpp"
#include "winapi/graphics/opengl32/gl_commands.hpp"
#include "winapi/graphics/opengl32/gl_context.hpp"
#include "winapi/graphics/opengl32/gl_drawable.hpp"
#include "winapi/graphics/opengl32/gl_enums.hpp"
#include "winapi/graphics/opengl32/opengl32.hpp"

#include "gl_test_caller.hpp"

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::gl;
using test::Caller;
using cmd::GlClientPixels;

namespace {

constexpr i32 kDoubleAlpha = 13;   // double, alpha 8, depth 24, stencil 8 (gl_pixel_formats.cpp)
constexpr u64 kDc = 0x6000;
constexpr u64 kWindow = 0x200;
constexpr i32 kW = 320, kH = 200;   // the probes' client area

struct Rgb {
    u8 r, g, b;
    bool operator==(const Rgb&) const = default;
};

class GlExtensionsMetalTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        if (!graphics::MetalDevice::instance().available()) GTEST_SKIP() << "no Metal device";
        graphics::set_active_backend_device(&graphics::MetalDevice::instance());
        ASSERT_TRUE(memory_.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(Caller::kStackBase, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(OPENGL32_SCRATCH_BASE, OPENGL32_SCRATCH_SIZE,
                                       MemoryProtection::ReadWrite).is_ok());
        registry_ = std::make_unique<ThunkRegistry>(0x70000000);
        mgr_ = std::make_unique<Opengl32Manager>();
        mgr_->set_thunk_registry(registry_.get());
        ASSERT_TRUE(registry_->register_dll("opengl32.dll", create_opengl32_thunks(*mgr_)).is_ok());
        mgr_->set_pixel_format_query([](u64 dc) { return dc == kDc ? kDoubleAlpha : 0; });
        mgr_->set_drawable_queries([](u64 dc) -> u64 { return dc == kDc ? kWindow : 0; },
                                   [](u64 dc, i32& w, i32& h) {
                                       if (dc != kDc) return false;
                                       w = kW;
                                       h = kH;
                                       return true;
                                   });
        mgr_->set_swap_chain_factory([](u64, u32, u32) { return std::unique_ptr<graphics::BackendSwapChain>(); });
        main_ = std::make_unique<Caller>(memory_, *registry_, GetParam(), 0);
        rc_ = main_->create(kDc);
        ASSERT_NE(rc_, 0u);
        ASSERT_EQ(main_->make(kDc, rc_), 1u);
        ctx_ = mgr_->context_for_testing(rc_);
        ASSERT_NE(ctx_, nullptr);
        drawable_ = GlDrawable::for_window(kWindow);
        ASSERT_NE(drawable_, nullptr);
        cmd::Disable(ctx(), GL_DITHER);
        cmd::PixelStore(ctx(), GL_UNPACK_ALIGNMENT, 1, true);
        cmd::PixelStore(ctx(), GL_PACK_ALIGNMENT, 1, true);
    }

    void TearDown() override {
        drawable_.reset();
        if (main_) main_->make(0, 0);
        set_current_gl_context_for_thread(nullptr);
        GlDrawable::detach_all();
        graphics::set_active_backend_device(nullptr);
    }

    GlContext& ctx() { return *ctx_; }
    u32 error() { return ctx().take_error(); }

    void clear(float r, float g, float b) {
        cmd::ClearColor(ctx(), r, g, b, 1.0f);
        cmd::Clear(ctx(), GL_COLOR_BUFFER_BIT);
    }
    /// The colour at GL window (x, y) of @p buffer.
    Rgb at(u32 buffer, i32 x, i32 y) {
        std::vector<u8> bgra;
        EXPECT_TRUE(drawable_->read_color(buffer, bgra));
        const u8* p = &bgra[(static_cast<usize>(kH - 1 - y) * kW + static_cast<usize>(x)) * 4u];
        return Rgb{p[2], p[1], p[0]};
    }
    void swap() { EXPECT_TRUE(mgr_->swap_buffers(kDc)); }

    std::vector<u8> read(i32 x, i32 y, i32 w, i32 h, u32 format, u32 type, usize bytes) {
        struct Buf {
            std::vector<u8> b;
        } buf{std::vector<u8>(bytes, 0xCC)};
        const cmd::GlClientBuffer client = [](void* user, usize size) -> std::span<u8> {
            auto& b = static_cast<Buf*>(user)->b;
            if (b.size() < size) b.resize(size, 0xCC);
            return std::span<u8>(b.data(), size);
        };
        EXPECT_TRUE(cmd::ReadPixels(ctx(), x, y, w, h, format, type, client, &buf));
        return buf.b;
    }
    void identity() {
        cmd::MatrixMode(ctx(), GL_PROJECTION);
        cmd::LoadIdentity(ctx());
        cmd::MatrixMode(ctx(), GL_MODELVIEW);
        cmd::LoadIdentity(ctx());
    }

    GuestMemory memory_;
    std::unique_ptr<ThunkRegistry> registry_;
    std::unique_ptr<Opengl32Manager> mgr_;
    std::unique_ptr<Caller> main_;
    u64 rc_{0};
    GlContext* ctx_{nullptr};
    std::shared_ptr<GlDrawable> drawable_;
};

std::string bitness_name(const ::testing::TestParamInfo<bool>& info) { return info.param ? "x64" : "x86"; }

constexpr Rgb kRed{255, 0, 0}, kGreen{0, 255, 0}, kBlue{0, 0, 255}, kBlack{0, 0, 0}, kYellow{255, 255, 0},
    kMagenta{255, 0, 255}, kCyan{0, 255, 255};

// ---- GL_WIN_swap_hint --------------------------------------------------------------------

// swap_hint_probe64.expected: in = (60, 60) lies in the rectangle (50, 50, 30, 20),
// out = (200, 150) does not.
TEST_P(GlExtensionsMetalTest, SwapHintCopiesOnlyTheRectangles) {
    clear(1, 0, 0);
    swap();
    EXPECT_EQ(at(GL_FRONT, 60, 60), kRed);    // hint-baseline-red
    EXPECT_EQ(at(GL_FRONT, 200, 150), kRed);
    // One rectangle: only it reaches the front; the back keeps its contents
    // (hint-one-rect-green front-gl=00ff00/ff0000 back=00ff00/00ff00).
    clear(0, 1, 0);
    cmd::AddSwapHintRect(ctx(), 50, 50, 30, 20);
    swap();
    EXPECT_EQ(at(GL_FRONT, 60, 60), kGreen);
    EXPECT_EQ(at(GL_FRONT, 200, 150), kRed);
    EXPECT_EQ(at(GL_BACK, 200, 150), kGreen);
    // The swap cleared the hint: the next one is whole (hint-next-swap-blue).
    clear(0, 0, 1);
    swap();
    EXPECT_EQ(at(GL_FRONT, 60, 60), kBlue);
    EXPECT_EQ(at(GL_FRONT, 200, 150), kBlue);
    // Rectangles accumulate (hint-two-rects-yellow-over-black, -out-rect-magenta-over-black).
    clear(0, 0, 0);
    swap();
    clear(1, 1, 0);
    cmd::AddSwapHintRect(ctx(), 50, 50, 30, 20);
    cmd::AddSwapHintRect(ctx(), 190, 140, 20, 20);
    swap();
    EXPECT_EQ(at(GL_FRONT, 60, 60), kYellow);
    EXPECT_EQ(at(GL_FRONT, 200, 150), kYellow);
    clear(0, 0, 0);
    swap();
    clear(1, 0, 1);
    cmd::AddSwapHintRect(ctx(), 190, 140, 20, 20);
    swap();
    EXPECT_EQ(at(GL_FRONT, 60, 60), kBlack);
    EXPECT_EQ(at(GL_FRONT, 200, 150), kMagenta);
    // glFinish / glFlush in between keep the rectangle (hint-finish-*, -flush-*).
    clear(0, 0, 0);
    swap();
    clear(0, 1, 1);
    cmd::AddSwapHintRect(ctx(), 50, 50, 30, 20);
    cmd::Finish(ctx());
    cmd::Flush(ctx());
    swap();
    EXPECT_EQ(at(GL_FRONT, 60, 60), kCyan);
    EXPECT_EQ(at(GL_FRONT, 200, 150), kBlack);
    // A zero or negative size is ignored: the swap is whole (hint-zero-size-*,
    // hint-negative-width-*); with a real rectangle too, only that one counts
    // (hint-zero-plus-rect-yellow-over-black).
    clear(0, 0, 0);
    swap();
    clear(1, 0, 0);
    cmd::AddSwapHintRect(ctx(), 50, 50, 0, 0);
    cmd::AddSwapHintRect(ctx(), 50, 50, -5, 10);
    swap();
    EXPECT_EQ(at(GL_FRONT, 200, 150), kRed);
    clear(0, 0, 0);
    swap();
    clear(1, 1, 0);
    cmd::AddSwapHintRect(ctx(), 0, 0, 0, 0);
    cmd::AddSwapHintRect(ctx(), 50, 50, 30, 20);
    swap();
    EXPECT_EQ(at(GL_FRONT, 60, 60), kYellow);
    EXPECT_EQ(at(GL_FRONT, 200, 150), kBlack);
    // A rectangle larger than the window is clipped: everything (hint-huge-rect-blue-over-black).
    clear(0, 0, 0);
    swap();
    clear(0, 0, 1);
    cmd::AddSwapHintRect(ctx(), -100, -100, 10000, 10000);
    swap();
    EXPECT_EQ(at(GL_FRONT, 200, 150), kBlue);
    // Refused inside glBegin: no rectangle (hint-refused-hint-white-over-black).
    clear(0, 0, 0);
    swap();
    clear(1, 0, 0);
    cmd::Begin(ctx(), GL_POINTS);
    cmd::AddSwapHintRect(ctx(), 50, 50, 30, 20);
    cmd::End(ctx());
    EXPECT_EQ(error(), GL_INVALID_OPERATION);
    swap();
    EXPECT_EQ(at(GL_FRONT, 200, 150), kRed);
    // Kept across a release and re-binding of the context (hint-across-makecurrent-red-over-black).
    clear(0, 0, 0);
    swap();
    clear(1, 0, 0);
    cmd::AddSwapHintRect(ctx(), 50, 50, 30, 20);
    ASSERT_EQ(main_->make(0, 0), 1u);
    ASSERT_EQ(main_->make(kDc, rc_), 1u);
    swap();
    EXPECT_EQ(at(GL_FRONT, 60, 60), kRed);
    EXPECT_EQ(at(GL_FRONT, 200, 150), kBlack);
}

// Disjoint rectangles copy each rectangle, not their bounding rectangle: mid =
// (120, 100) lies between them and keeps the old front
// (hint-disjoint-rects-yellow-over-black front-gl=ffff00/000000/ffff00).
TEST_P(GlExtensionsMetalTest, SwapHintDisjointRectanglesAreNotMerged) {
    clear(0, 0, 0);
    swap();
    clear(1, 1, 0);
    cmd::AddSwapHintRect(ctx(), 50, 50, 30, 20);
    cmd::AddSwapHintRect(ctx(), 190, 140, 20, 20);
    swap();
    EXPECT_EQ(at(GL_FRONT, 60, 60), kYellow);
    EXPECT_EQ(at(GL_FRONT, 120, 100), kBlack);
    EXPECT_EQ(at(GL_FRONT, 200, 150), kYellow);
    EXPECT_EQ(at(GL_BACK, 120, 100), kYellow);
}

// The rectangles belong to the window: another context made current on it
// swaps them (hint-e3-hint-rc-swap-under-rcB-blue-over-black), and so does a
// swap with no context current (hint-e5-hint-swap-without-context-*).
TEST_P(GlExtensionsMetalTest, SwapHintBelongsToTheWindow) {
    const u64 other = main_->create(kDc);
    ASSERT_NE(other, 0u);
    clear(0, 0, 0);
    swap();
    clear(0, 0, 1);
    cmd::AddSwapHintRect(ctx(), 50, 50, 30, 20);
    ASSERT_EQ(main_->make(kDc, other), 1u);
    swap();
    EXPECT_EQ(at(GL_FRONT, 60, 60), kBlue);
    EXPECT_EQ(at(GL_FRONT, 200, 150), kBlack);

    ASSERT_EQ(main_->make(kDc, rc_), 1u);
    clear(0, 0, 0);
    swap();
    clear(1, 0, 1);
    cmd::AddSwapHintRect(ctx(), 50, 50, 30, 20);
    ASSERT_EQ(main_->make(0, 0), 1u);
    swap();
    EXPECT_EQ(at(GL_FRONT, 60, 60), kMagenta);
    EXPECT_EQ(at(GL_FRONT, 200, 150), kBlack);
    ASSERT_EQ(main_->make(kDc, rc_), 1u);
}

// ---- GL_EXT_bgra through glDrawPixels / glReadPixels --------------------------------------

TEST_P(GlExtensionsMetalTest, BgraDrawAndReadPixels) {
    identity();
    clear(0, 0, 0);
    cmd::RasterPos(ctx(), -1, -1, 0, 1);
    const std::vector<u8> bgra2{0x10, 0x80, 0xF0, 0xFF, 0xF0, 0x40, 0x20, 0x80};
    GlClientPixels px;
    px.bytes = bgra2;
    cmd::DrawPixels(ctx(), 2, 1, GL_BGRA_EXT, GL_UNSIGNED_BYTE, px);
    EXPECT_EQ(error(), GL_NO_ERROR);   // ext-bgra-drawpixels-BGRA
    // ext-bgra-drawpixels-BGRA-read-rgba (the format has alpha: 0x80 is kept).
    EXPECT_EQ(read(0, 0, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, 8),
              (std::vector<u8>{0xf0, 0x80, 0x10, 0xff, 0x20, 0x40, 0xf0, 0x80}));
    // ext-bgra-readpixels-BGRA / -BGR.
    EXPECT_EQ(read(0, 0, 2, 1, GL_BGRA_EXT, GL_UNSIGNED_BYTE, 8),
              (std::vector<u8>{0x10, 0x80, 0xf0, 0xff, 0xf0, 0x40, 0x20, 0x80}));
    EXPECT_EQ(read(0, 0, 2, 1, GL_BGR_EXT, GL_UNSIGNED_BYTE, 8),
              (std::vector<u8>{0x10, 0x80, 0xf0, 0xf0, 0x40, 0x20, 0xcc, 0xcc}));
    // ext-bgra-drawpixels-BGR-read-rgba.
    const std::vector<u8> bgr2{0x01, 0x02, 0xFE, 0x55, 0xAA, 0x33};
    px.bytes = bgr2;
    cmd::DrawPixels(ctx(), 2, 1, GL_BGR_EXT, GL_UNSIGNED_BYTE, px);
    EXPECT_EQ(read(0, 0, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, 8),
              (std::vector<u8>{0xfe, 0x02, 0x01, 0xff, 0x33, 0xaa, 0x55, 0xff}));
}

// ---- GL_EXT_paletted_texture drawn -------------------------------------------------------

// ext-pal-draw-index4-left-right: a 2x1 COLOR_INDEX4 texture of indices 1, 3
// with a 16-entry table, a full-viewport GL_REPLACE quad; then
// ext-pal-draw-after-subtable-left: entry 1 replaced, the next draw uses it.
TEST_P(GlExtensionsMetalTest, IndexTextureDrawsThroughTheTable) {
    identity();
    std::vector<u8> table(16 * 4, 0);
    const u8 first[16] = {0xFF, 0, 0, 0xFF, 0, 0xFF, 0, 0xFF, 0, 0, 0xFF, 0xFF, 0x80, 0x40, 0x20, 0xFF};
    std::memcpy(table.data(), first, 16);
    GlClientPixels tpx;
    tpx.bytes = table;
    cmd::BindTexture(ctx(), GL_TEXTURE_2D, 1);
    cmd::ColorTable(ctx(), GL_TEXTURE_2D, GL_RGBA, 16, GL_RGBA, GL_UNSIGNED_BYTE, tpx);
    const std::vector<u8> idx{1, 3};
    GlClientPixels ipx;
    ipx.bytes = idx;
    cmd::TexImage(ctx(), 2, GL_TEXTURE_2D, 0, GL_COLOR_INDEX4_EXT, 2, 1, 0, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, ipx);
    cmd::TexParameter(ctx(), GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GlValues::scalar_value(GL_NEAREST, true));
    cmd::TexParameter(ctx(), GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GlValues::scalar_value(GL_NEAREST, true));
    cmd::TexEnv(ctx(), GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GlValues::scalar_value(GL_REPLACE, true));
    cmd::Enable(ctx(), GL_TEXTURE_2D);
    auto quad = [&] {
        clear(0, 0, 0);
        cmd::Begin(ctx(), GL_QUADS);
        cmd::TexCoord(ctx(), 0, 0, 0, 1);
        cmd::Vertex(ctx(), -1, -1, 0, 1);
        cmd::TexCoord(ctx(), 1, 0, 0, 1);
        cmd::Vertex(ctx(), 1, -1, 0, 1);
        cmd::TexCoord(ctx(), 1, 1, 0, 1);
        cmd::Vertex(ctx(), 1, 1, 0, 1);
        cmd::TexCoord(ctx(), 0, 1, 0, 1);
        cmd::Vertex(ctx(), -1, 1, 0, 1);
        cmd::End(ctx());
    };
    quad();
    EXPECT_EQ(error(), GL_NO_ERROR);
    EXPECT_EQ(at(GL_BACK, 40, 100), (Rgb{0x00, 0xff, 0x00}));
    EXPECT_EQ(at(GL_BACK, 280, 100), (Rgb{0x80, 0x40, 0x20}));
    const std::vector<u8> entry{0x11, 0x22, 0x33, 0xFF};
    GlClientPixels spx;
    spx.bytes = entry;
    cmd::ColorSubTable(ctx(), GL_TEXTURE_2D, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, spx);
    quad();
    EXPECT_EQ(at(GL_BACK, 40, 100), (Rgb{0x11, 0x22, 0x33}));
    cmd::Disable(ctx(), GL_TEXTURE_2D);
}

INSTANTIATE_TEST_SUITE_P(Bitness, GlExtensionsMetalTest, ::testing::Values(false, true), bitness_name);

}  // namespace
