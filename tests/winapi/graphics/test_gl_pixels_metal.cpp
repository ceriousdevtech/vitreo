/// @file test_gl_pixels_metal.cpp
/// @brief OpenGL 1.1 pixel rectangles on the real Metal backend: glBitmap's
///        fragments (spec 3.7) and their batching, glDrawPixels through the
///        pixel transfer operations, the zoomed rectangle rule (3.6.5) and the
///        per-fragment operations, DEPTH_COMPONENT and STENCIL_INDEX groups
///        (4.3.1), glReadPixels' pack pipeline (4.3.2) and glCopyPixels
///        (4.3.3), with an invalid raster position drawing nothing (2.12).
///
/// Kept out of vitreo_winapi_tests, which does not link the Metal backend.
/// Expected pixels come from the spec's rules evaluated here: the bitmap bit
/// of eq. 3.10's layout, the fragments whose centres lie in a group's zoomed
/// rectangle or on its left or bottom edge, scale and bias, the maps, Table
/// 4.7 / 4.6 for packing. Colours are 8-bit, so a channel may differ from the
/// exact value by the final conversion's rounding (1).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
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
#include "winapi/graphics/opengl32/gl_pixels.hpp"
#include "winapi/graphics/opengl32/gl_texture.hpp"
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
constexpr i32 kW = 32, kH = 32;
constexpr GuestAddr kGuestImage = 0x14000;
constexpr GuestAddr kGuestOut = 0x18000;

using Color = std::array<double, 4>;

struct Rgba {
    u8 r, g, b, a;
    bool operator==(const Rgba&) const = default;
};

u8 c8(double v) { return static_cast<u8>(std::floor(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5)); }

class GlPixelsMetalTest : public ::testing::TestWithParam<bool> {
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
        cmd::MatrixMode(ctx(), GL_PROJECTION);
        cmd::Ortho(ctx(), 0, kW, 0, kH, -1, 1);
        cmd::MatrixMode(ctx(), GL_MODELVIEW);
        clear(0, 0, 0, 0);
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

    void clear(float r, float g, float b, float a) {
        cmd::ClearColor(ctx(), r, g, b, a);
        cmd::ClearDepth(ctx(), 1.0);
        cmd::ClearStencil(ctx(), 0);
        cmd::Clear(ctx(), GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }

    /// A colour buffer, row 0 = the bottom row. Reading ends the pass.
    std::vector<Rgba> pixels(u32 buffer = GL_BACK) {
        std::vector<u8> bgra;
        EXPECT_TRUE(drawable_->read_color(buffer, bgra));
        std::vector<Rgba> out(kW * kH);
        for (i32 gy = 0; gy < kH; ++gy) {
            const i32 row = kH - 1 - gy;
            for (i32 x = 0; x < kW; ++x) {
                const u8* p = &bgra[(static_cast<usize>(row) * kW + x) * 4u];
                out[gy * kW + x] = Rgba{p[2], p[1], p[0], p[3]};
            }
        }
        return out;
    }
    std::vector<float> depth() {
        std::vector<float> d;
        EXPECT_TRUE(drawable_->read_depth(d));
        std::vector<float> out(kW * kH);
        for (i32 gy = 0; gy < kH; ++gy) {
            for (i32 x = 0; x < kW; ++x) out[gy * kW + x] = d[static_cast<usize>(kH - 1 - gy) * kW + x];
        }
        return out;
    }
    std::vector<u8> stencil() {
        std::vector<u8> s;
        EXPECT_TRUE(drawable_->read_stencil(s));
        std::vector<u8> out(kW * kH);
        for (i32 gy = 0; gy < kH; ++gy) {
            for (i32 x = 0; x < kW; ++x) out[gy * kW + x] = s[static_cast<usize>(kH - 1 - gy) * kW + x];
        }
        return out;
    }
    static Rgba at(const std::vector<Rgba>& px, i32 x, i32 y) { return px.at(static_cast<usize>(y * kW + x)); }

    static void expect_color(const Rgba& got, const Color& want, const std::string& where) {
        const double g[4] = {static_cast<double>(got.r), static_cast<double>(got.g), static_cast<double>(got.b),
                             static_cast<double>(got.a)};
        for (u32 k = 0; k < 4; ++k) {
            EXPECT_NEAR(g[k], std::clamp(want[k], 0.0, 1.0) * 255.0, 1.01) << where << " channel " << k;
        }
    }

    void raster(double x, double y, double z = 0.0) { cmd::RasterPos(ctx(), x, y, z, 1.0); }
    void draw(i32 w, i32 h, u32 format, u32 type, const std::vector<u8>& bytes) {
        GlClientPixels px;
        px.bytes = bytes;
        cmd::DrawPixels(ctx(), w, h, format, type, px);
    }
    void bitmap(i32 w, i32 h, float xo, float yo, float xm, float ym, const std::vector<u8>& bytes) {
        GlClientPixels px;
        px.bytes = bytes;
        cmd::Bitmap(ctx(), w, h, xo, yo, xm, ym, px);
    }
    void store(u32 pname, i32 value) { cmd::PixelStore(ctx(), pname, value, true); }
    void transfer(u32 pname, double value) { cmd::PixelTransfer(ctx(), pname, value, false); }

    /// glReadPixels into a buffer pre-filled with 0xAB.
    std::vector<u8> read(i32 x, i32 y, i32 w, i32 h, u32 format, u32 type, usize bytes) {
        struct Buf {
            std::vector<u8> b;
        } buf{std::vector<u8>(bytes, 0xAB)};
        const cmd::GlClientBuffer client = [](void* user, usize size) -> std::span<u8> {
            auto& b = static_cast<Buf*>(user)->b;
            if (b.size() < size) b.resize(size, 0xAB);
            return std::span<u8>(b.data(), size);
        };
        EXPECT_TRUE(cmd::ReadPixels(ctx(), x, y, w, h, format, type, client, &buf));
        EXPECT_EQ(error(), GL_NO_ERROR);
        return buf.b;
    }

    GuestMemory memory_;
    std::unique_ptr<ThunkRegistry> registry_;
    std::unique_ptr<Opengl32Manager> mgr_;
    std::unique_ptr<Caller> main_;
    u64 rc_{0};
    GlContext* ctx_{nullptr};
    std::shared_ptr<GlDrawable> drawable_;
};

// ---- References from the spec text -------------------------------------------------

/// Spec 3.6.4 BITMAP unpacking: element i of row j (after SKIP_ROWS rows of
/// k = a ceil(l / 8a) ubytes and SKIP_PIXELS bits), MSB first unless LSB_FIRST.
bool ref_bit(const std::vector<u8>& data, const GlPixelStoreModes& m, i32 width, i32 i, i32 j) {
    const i32 l = m.row_length > 0 ? m.row_length : width;
    const i32 a = m.alignment;
    const i32 k = a * ((l + 8 * a - 1) / (8 * a));
    const i32 bit = m.skip_pixels + i;
    const u8 byte = data.at(static_cast<usize>((m.skip_rows + j) * k + bit / 8));
    const i32 shift = m.lsb_first ? bit % 8 : 7 - bit % 8;
    return ((byte >> shift) & 1) != 0;
}

/// Spec 3.6.5: the group (n, m) whose zoomed rectangle produces the fragment
/// at window pixel (x, y) (its centre inside, or on the left or bottom edge),
/// or false.
bool ref_group(double xr, double yr, double zx, double zy, i32 w, i32 h, i32 x, i32 y, i32& gn, i32& gm) {
    const double cx = x + 0.5, cy = y + 0.5;
    for (i32 m = 0; m < h; ++m) {
        const double y0 = std::min(yr + zy * m, yr + zy * (m + 1)), y1 = std::max(yr + zy * m, yr + zy * (m + 1));
        if (!(y0 <= cy && cy < y1)) continue;
        for (i32 n = 0; n < w; ++n) {
            const double x0 = std::min(xr + zx * n, xr + zx * (n + 1)), x1 = std::max(xr + zx * n, xr + zx * (n + 1));
            if (x0 <= cx && cx < x1) {
                gn = n;
                gm = m;
                return true;
            }
        }
    }
    return false;
}

std::vector<u8> pattern_bytes(usize n, u32 seed) {
    std::vector<u8> out(n);
    u32 s = seed;
    for (u8& b : out) {
        s = s * 1103515245u + 12345u;
        b = static_cast<u8>(s >> 16);
    }
    return out;
}

// ---- Spec 2.12: an invalid raster position draws nothing --------------------------------

TEST_P(GlPixelsMetalTest, RasterPosTransformsAndClips) {
    clear(0.2f, 0.2f, 0.2f, 1.0f);
    const std::vector<Rgba> before = pixels();
    cmd::Color(ctx(), 1, 0, 0, 1);
    raster(100, 5);   // right of the clip volume: culled
    ASSERT_FALSE(ctx().state.current.raster_position_valid);
    bitmap(8, 8, 0, 0, 1, 1, std::vector<u8>(32, 0xFF));   // rows of 4 bytes (UNPACK_ALIGNMENT 4)
    draw(2, 2, GL_RGBA, GL_UNSIGNED_BYTE, std::vector<u8>(16, 0xFF));
    cmd::CopyPixels(ctx(), 0, 0, 4, 4, GL_COLOR);
    EXPECT_EQ(error(), GL_NO_ERROR);
    EXPECT_TRUE(pixels() == before) << "nothing is drawn with an invalid raster position";
    // A raster position transformed by the modelview and projection: the
    // translated (1, 2) lands at window (4, 6); the bitmap's lower left is
    // floor(x_r - x_o) = 4.
    cmd::LoadIdentity(ctx());
    cmd::Translate(ctx(), 3, 4, 0);
    raster(1, 2);
    cmd::LoadIdentity(ctx());
    ASSERT_TRUE(ctx().state.current.raster_position_valid);
    bitmap(1, 1, 0, 0, 0, 0, std::vector<u8>{0x80});
    const std::vector<Rgba> px = pixels();
    for (i32 y = 0; y < kH; ++y) {
        for (i32 x = 0; x < kW; ++x) {
            const Rgba want = x == 4 && y == 6 ? Rgba{255, 0, 0, 255} : Rgba{51, 51, 51, 255};
            EXPECT_EQ(at(px, x, y), want) << x << "," << y;
        }
    }
}

// ---- Spec 3.7: fragments for each 1 bit, then the raster position moves ------------------

TEST_P(GlPixelsMetalTest, BitmapAdvancesRasterPosAndColorsSetBits) {
    clear(0, 0, 0, 0);
    // The raster colour is the colour at glRasterPos, not a later glColor.
    cmd::Color(ctx(), 1, 0, 0, 1);
    raster(5.0, 6.0);
    cmd::Color(ctx(), 0, 1, 0, 1);
    // xll = floor(5 - 0.5) = 4, yll = floor(6 - 1.25) = 4.
    GlPixelStoreModes m{false, true, 16, 1, 3, 4};   // LSB_FIRST, ROW_LENGTH 16, SKIP_ROWS 1, SKIP_PIXELS 3, ALIGNMENT 4
    store(GL_UNPACK_LSB_FIRST, 1);
    store(GL_UNPACK_ROW_LENGTH, 16);
    store(GL_UNPACK_SKIP_ROWS, 1);
    store(GL_UNPACK_SKIP_PIXELS, 3);
    store(GL_UNPACK_ALIGNMENT, 4);
    const i32 w = 10, h = 3;
    const std::vector<u8> bits = pattern_bytes(4 * (h + 1), 7);
    bitmap(w, h, 0.5f, 1.25f, 12.5f, -2.0f, bits);
    EXPECT_EQ(error(), GL_NO_ERROR);
    EXPECT_FLOAT_EQ(ctx().state.current.raster_position[0], 17.5f);
    EXPECT_FLOAT_EQ(ctx().state.current.raster_position[1], 4.0f);
    // A second bitmap at the moved position, default unpack modes (MSB
    // first): xll = floor(17.5 - 0) = 17, yll = floor(4 - 0) = 4.
    store(GL_UNPACK_LSB_FIRST, 0);
    store(GL_UNPACK_ROW_LENGTH, 0);
    store(GL_UNPACK_SKIP_ROWS, 0);
    store(GL_UNPACK_SKIP_PIXELS, 0);
    store(GL_UNPACK_ALIGNMENT, 1);
    const GlPixelStoreModes plain{false, false, 0, 0, 0, 1};
    const std::vector<u8> bits2 = pattern_bytes(2 * 5, 11);
    bitmap(9, 5, 0, 0, 0, 0, bits2);
    const std::vector<Rgba> px = pixels();
    for (i32 y = 0; y < kH; ++y) {
        for (i32 x = 0; x < kW; ++x) {
            bool on = false;
            if (x >= 4 && x < 4 + w && y >= 4 && y < 4 + h) on = ref_bit(bits, m, w, x - 4, y - 4);
            if (x >= 17 && x < 17 + 9 && y >= 4 && y < 4 + 5) on = on || ref_bit(bits2, plain, 9, x - 17, y - 4);
            EXPECT_EQ(at(px, x, y), (on ? Rgba{255, 0, 0, 255} : Rgba{0, 0, 0, 0})) << x << "," << y;
        }
    }

    // Partly outside the window: only the fragments inside it exist.
    clear(0, 0, 0, 0);
    cmd::Color(ctx(), 1, 0, 0, 1);
    raster(1, 1);
    bitmap(8, 2, 4.0f, 1.0f, 0, 0, std::vector<u8>{0xFF, 0xFF});   // xll -3, yll 0
    const std::vector<Rgba> clipped = pixels();
    for (i32 y = 0; y < 4; ++y) {
        for (i32 x = 0; x < 8; ++x) {
            const bool on = x < 5 && y < 2;   // columns -3..4, rows 0..1
            EXPECT_EQ(at(clipped, x, y), (on ? Rgba{255, 0, 0, 255} : Rgba{0, 0, 0, 0})) << x << "," << y;
        }
    }

    // The fragments take the raster texture coordinates (s/q, t/q) and are
    // textured; blending applies to them.
    clear(0, 0, 0.5f, 1);
    u32 tex = 0;
    std::vector<u32> names;
    cmd::GenTextures(ctx(), 1, names);
    tex = names[0];
    cmd::BindTexture(ctx(), GL_TEXTURE_2D, tex);
    const std::vector<u8> texels{10, 20, 30, 255, 200, 100, 50, 255, 1, 2, 3, 255, 4, 5, 6, 255};
    GlClientPixels tp;
    tp.bytes = texels;
    cmd::TexImage(ctx(), 2, GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, tp);
    cmd::TexParameter(ctx(), GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GlValues::scalar_value(GL_NEAREST, true));
    cmd::TexParameter(ctx(), GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GlValues::scalar_value(GL_NEAREST, true));
    cmd::TexEnv(ctx(), GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GlValues::scalar_value(GL_REPLACE, true));
    cmd::Enable(ctx(), GL_TEXTURE_2D);
    cmd::TexCoord(ctx(), 1.5f, 0.5f, 0, 2);   // (0.75, 0.25): texel (1, 0)
    cmd::Color(ctx(), 1, 1, 1, 1);
    raster(2, 2);
    bitmap(1, 1, 0, 0, 0, 0, std::vector<u8>{0x80});
    cmd::Disable(ctx(), GL_TEXTURE_2D);
    cmd::Enable(ctx(), GL_BLEND);
    cmd::BlendFunc(ctx(), GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    cmd::Color(ctx(), 1, 1, 0, 0.25f);
    raster(6, 2);
    bitmap(1, 1, 0, 0, 0, 0, std::vector<u8>{0x80});
    cmd::Disable(ctx(), GL_BLEND);
    const std::vector<Rgba> tx = pixels();
    EXPECT_EQ(at(tx, 2, 2), (Rgba{200, 100, 50, 255}));
    expect_color(at(tx, 6, 2), {0.25, 0.25, 0.5 * 0.75, 0.25 * 0.25 + 0.75}, "blended bitmap");
}

// ---- Performance (ruling 7): glBitmap text batches into the open pass ------------------

TEST_P(GlPixelsMetalTest, ThousandBitmapsAreOnePass) {
    clear(0, 0, 0, 0);
    cmd::Color(ctx(), 1, 1, 1, 1);
    raster(0, 0);
    store(GL_UNPACK_ALIGNMENT, 1);
    // Flush everything so the counts below cover the bitmaps only.
    (void)pixels();
    const u32 passes = drawable_->passes_opened();
    const GlBatchStats before = gl_batch_stats(ctx());
    const GlTextureStats tex_before = gl_texture_stats();
    const std::vector<u8> glyph = pattern_bytes(13, 3);   // an 8 x 13 glyph
    for (i32 i = 0; i < 1000; ++i) bitmap(8, 13, 0, 0, 0.024f, 0.0f, glyph);
    EXPECT_EQ(error(), GL_NO_ERROR);
    const std::vector<Rgba> px = pixels();
    const GlBatchStats after = gl_batch_stats(ctx());
    const GlTextureStats tex_after = gl_texture_stats();
    EXPECT_LE(drawable_->passes_opened() - passes, 1u);
    EXPECT_LE(after.pipelines_created - before.pipelines_created, 1u);
    // One draw, split only by the pending draw's 65,536-vertex cap.
    EXPECT_LE(after.draws_recorded - before.draws_recorded, 4u);
    EXPECT_EQ(tex_after.storages_created, tex_before.storages_created);
    // The glyphs land at floor(0.024 i): x 0..23.
    for (i32 y = 0; y < 13; ++y) {
        for (i32 x = 0; x < kW; ++x) {
            bool on = false;
            for (i32 xll = 0; xll <= 23 && !on; ++xll) {
                if (x >= xll && x < xll + 8) on = ref_bit(glyph, GlPixelStoreModes{false, false, 0, 0, 0, 1}, 8, x - xll, y);
            }
            EXPECT_EQ(at(px, x, y), (on ? Rgba{255, 255, 255, 255} : Rgba{0, 0, 0, 0})) << x << "," << y;
        }
    }
}

// ---- Spec 3.6.4: the pixel transfer operations -------------------------------------------

TEST_P(GlPixelsMetalTest, DrawPixelsAppliesScaleBiasAndMaps) {
    clear(0, 0, 0, 0);
    // RGBA unsigned bytes: scale and bias, then MAP_COLOR's clamp and lookups
    // (address round(c (n - 1))).
    const std::vector<u8> img{0, 64, 128, 255, 255, 32, 192, 128, 100, 200, 50, 0};
    transfer(GL_RED_SCALE, 0.5);
    transfer(GL_GREEN_BIAS, 0.25);
    transfer(GL_ALPHA_SCALE, 2.0);
    const std::vector<float> r_map{0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f};
    const std::vector<float> g_map{1.0f, 0.5f};
    const std::vector<float> b_map{0.9f, 0.6f, 0.3f, 0.0f};
    const std::vector<float> a_map{0.0f, 1.0f};
    cmd::PixelMap(ctx(), GL_PIXEL_MAP_R_TO_R, 8, r_map);
    cmd::PixelMap(ctx(), GL_PIXEL_MAP_G_TO_G, 2, g_map);
    cmd::PixelMap(ctx(), GL_PIXEL_MAP_B_TO_B, 4, b_map);
    cmd::PixelMap(ctx(), GL_PIXEL_MAP_A_TO_A, 2, a_map);
    cmd::PixelTransfer(ctx(), GL_MAP_COLOR, 1, true);
    raster(2, 3);
    draw(3, 1, GL_RGBA, GL_UNSIGNED_BYTE, img);
    EXPECT_EQ(error(), GL_NO_ERROR);
    const auto lookup = [](const std::vector<float>& map, double v) {
        v = std::clamp(v, 0.0, 1.0);
        return static_cast<double>(map[static_cast<usize>(std::floor(v * (map.size() - 1) + 0.5))]);
    };
    std::vector<Rgba> px = pixels();
    for (i32 n = 0; n < 3; ++n) {
        const u8* e = &img[static_cast<usize>(n) * 4u];
        const Color want{lookup(r_map, e[0] / 255.0 * 0.5), lookup(g_map, e[1] / 255.0 + 0.25),
                         lookup(b_map, e[2] / 255.0), lookup(a_map, e[3] / 255.0 * 2.0)};
        expect_color(at(px, 2 + n, 3), want, "mapped group " + std::to_string(n));
    }
    cmd::PixelTransfer(ctx(), GL_MAP_COLOR, 0, true);
    transfer(GL_RED_SCALE, 1);
    transfer(GL_GREEN_BIAS, 0);
    transfer(GL_ALPHA_SCALE, 1);

    // COLOR_INDEX in RGBA mode: shift and offset, then I_TO_R/G/B/A at
    // round(i) & (n - 1); unsigned shorts.
    const std::vector<float> ir{0.0f, 0.25f, 0.5f, 1.0f}, ig{1.0f, 0.0f}, ib{0.5f}, ia{1.0f, 1.0f, 1.0f, 0.5f};
    cmd::PixelMap(ctx(), GL_PIXEL_MAP_I_TO_R, 4, ir);
    cmd::PixelMap(ctx(), GL_PIXEL_MAP_I_TO_G, 2, ig);
    cmd::PixelMap(ctx(), GL_PIXEL_MAP_I_TO_B, 1, ib);
    cmd::PixelMap(ctx(), GL_PIXEL_MAP_I_TO_A, 4, ia);
    cmd::PixelTransfer(ctx(), GL_INDEX_SHIFT, 1, true);
    cmd::PixelTransfer(ctx(), GL_INDEX_OFFSET, 1, true);
    const std::vector<u16> idx{0, 1, 5};   // -> 1, 3, 11
    std::vector<u8> idx_bytes(idx.size() * 2);
    std::memcpy(idx_bytes.data(), idx.data(), idx_bytes.size());
    raster(2, 5);
    draw(3, 1, GL_COLOR_INDEX, GL_UNSIGNED_SHORT, idx_bytes);
    cmd::PixelTransfer(ctx(), GL_INDEX_SHIFT, 0, true);
    cmd::PixelTransfer(ctx(), GL_INDEX_OFFSET, 0, true);
    // COLOR_INDEX as BITMAP: indices 0 / 1 through the same maps.
    raster(2, 7);
    draw(4, 1, GL_COLOR_INDEX, GL_BITMAP, std::vector<u8>{0xA0});
    // LUMINANCE_ALPHA floats: (L, L, L, A).
    const std::vector<float> la{0.25f, 0.75f, 1.5f, -1.0f};
    std::vector<u8> la_bytes(la.size() * 4);
    std::memcpy(la_bytes.data(), la.data(), la_bytes.size());
    raster(2, 9);
    draw(2, 1, GL_LUMINANCE_ALPHA, GL_FLOAT, la_bytes);
    // RGB under UNPACK_ROW_LENGTH 3, SKIP_PIXELS 1, SKIP_ROWS 1, ALIGNMENT 2:
    // rows of 9 bytes padded to 10; the 2 x 2 image starts at byte 10 + 3.
    std::vector<u8> rgb = pattern_bytes(40, 5);
    store(GL_UNPACK_ROW_LENGTH, 3);
    store(GL_UNPACK_SKIP_PIXELS, 1);
    store(GL_UNPACK_SKIP_ROWS, 1);
    store(GL_UNPACK_ALIGNMENT, 2);
    raster(10, 10);
    draw(2, 2, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    store(GL_UNPACK_ROW_LENGTH, 0);
    store(GL_UNPACK_SKIP_PIXELS, 0);
    store(GL_UNPACK_SKIP_ROWS, 0);
    store(GL_UNPACK_ALIGNMENT, 4);
    // ALPHA unsigned bytes: (0, 0, 0, A).
    raster(14, 10);
    draw(1, 1, GL_ALPHA, GL_UNSIGNED_BYTE, std::vector<u8>{77, 0, 0, 0});
    EXPECT_EQ(error(), GL_NO_ERROR);

    px = pixels();
    const i32 shifted[3] = {1, 3, 11};
    for (i32 n = 0; n < 3; ++n) {
        const i32 i = shifted[n];
        const Color want{ir[i & 3], ig[i & 1], ib[0], ia[i & 3]};
        expect_color(at(px, 2 + n, 5), want, "index group " + std::to_string(n));
    }
    for (i32 n = 0; n < 4; ++n) {
        const i32 i = (0xA0 >> (7 - n)) & 1;
        expect_color(at(px, 2 + n, 7), {ir[i], ig[i], ib[0], ia[i]}, "bitmap index " + std::to_string(n));
    }
    expect_color(at(px, 2, 9), {0.25, 0.25, 0.25, 0.75}, "luminance alpha 0");
    expect_color(at(px, 3, 9), {1, 1, 1, 0}, "luminance alpha 1 (clamped)");
    for (i32 m = 0; m < 2; ++m) {
        for (i32 n = 0; n < 2; ++n) {
            const usize first = static_cast<usize>(10 * (1 + m) + 3 * (1 + n));
            expect_color(at(px, 10 + n, 10 + m), {rgb[first] / 255.0, rgb[first + 1] / 255.0, rgb[first + 2] / 255.0, 1},
                         "rgb " + std::to_string(n) + "," + std::to_string(m));
        }
    }
    expect_color(at(px, 14, 10), {0, 0, 0, 77 / 255.0}, "alpha");
}

// ---- Spec 3.6.5: the zoomed rectangle rule ---------------------------------------------

TEST_P(GlPixelsMetalTest, DrawPixelsZoom) {
    const i32 w = 3, h = 2;
    std::vector<u8> img(static_cast<usize>(w * h) * 4u);
    for (i32 g = 0; g < w * h; ++g) {
        img[g * 4 + 0] = static_cast<u8>(40 * (g + 1));
        img[g * 4 + 1] = static_cast<u8>(255 - 30 * g);
        img[g * 4 + 2] = static_cast<u8>(17 * g);
        img[g * 4 + 3] = 255;
    }
    struct Case {
        double xr, yr, zx, zy;
    };
    const Case cases[] = {{10.3, 12.6, 2.0, 3.0},  {10.3, 12.6, -1.0, 1.0}, {4.5, 20.25, 1.5, -0.5},
                          {16.0, 8.0, -2.5, -1.5}, {3.7, 3.2, 0.5, 0.75},   {5.0, 5.0, 0.0, 2.0},
                          {28.0, 30.0, 3.0, 2.0}};
    for (const Case& c : cases) {
        clear(0, 0, 0, 0);
        raster(c.xr, c.yr);
        cmd::PixelZoom(ctx(), static_cast<float>(c.zx), static_cast<float>(c.zy));
        draw(w, h, GL_RGBA, GL_UNSIGNED_BYTE, img);
        EXPECT_EQ(error(), GL_NO_ERROR);
        const std::vector<Rgba> px = pixels();
        for (i32 y = 0; y < kH; ++y) {
            for (i32 x = 0; x < kW; ++x) {
                i32 n = 0, m = 0;
                Rgba want{0, 0, 0, 0};
                if (ref_group(static_cast<float>(c.xr), static_cast<float>(c.yr), static_cast<float>(c.zx),
                              static_cast<float>(c.zy), w, h, x, y, n, m)) {
                    const u8* e = &img[static_cast<usize>(m * w + n) * 4u];
                    want = {e[0], e[1], e[2], e[3]};
                }
                EXPECT_EQ(at(px, x, y), want) << "zoom " << c.zx << "," << c.zy << " at " << x << "," << y;
            }
        }
    }
    cmd::PixelZoom(ctx(), 1, 1);
}

// ---- Spec 3.6.5, 4.1, 4.3.1: per-fragment operations, depth and stencil groups -------------

TEST_P(GlPixelsMetalTest, DrawPixelsGoesThroughDepthAndBlend) {
    clear(0, 0, 0, 0);
    cmd::ClearDepth(ctx(), 0.5);
    cmd::Clear(ctx(), GL_DEPTH_BUFFER_BIT);
    cmd::Enable(ctx(), GL_DEPTH_TEST);
    cmd::DepthFunc(ctx(), GL_LESS);
    const std::vector<u8> red(16, 0);
    std::vector<u8> r = red, g = red;
    for (usize i = 0; i < 4; ++i) r[i * 4] = 255, r[i * 4 + 3] = 255, g[i * 4 + 1] = 255, g[i * 4 + 3] = 255;
    // glOrtho(.., -1, 1): z_w = (1 - z) / 2. z = 0.5 -> 0.25 (passes LESS
    // against 0.5, written); z = -0.5 -> 0.75 (fails).
    raster(2, 2, 0.5);
    draw(2, 2, GL_RGBA, GL_UNSIGNED_BYTE, r);
    raster(4, 2, -0.5);
    draw(2, 2, GL_RGBA, GL_UNSIGNED_BYTE, g);
    // Blending ONE, ONE over the red square (raster z 0.0 -> 0.5 fails LESS
    // against 0.25, so use LEQUAL with z 0.5 -> 0.25).
    cmd::DepthFunc(ctx(), GL_LEQUAL);
    cmd::Enable(ctx(), GL_BLEND);
    cmd::BlendFunc(ctx(), GL_ONE, GL_ONE);
    raster(3, 2, 0.5);
    draw(1, 1, GL_RGBA, GL_UNSIGNED_BYTE, std::vector<u8>{0, 0, 128, 0});
    cmd::Disable(ctx(), GL_BLEND);
    // Linear fog by the raster distance: glOrtho(0, 32, 0, 32, -10, 10) and
    // eye z -2: f = (end - |z|) / (end - start) = (4 - 2) / 4.
    cmd::MatrixMode(ctx(), GL_PROJECTION);
    cmd::LoadIdentity(ctx());
    cmd::Ortho(ctx(), 0, kW, 0, kH, -10, 10);
    cmd::MatrixMode(ctx(), GL_MODELVIEW);
    cmd::Disable(ctx(), GL_DEPTH_TEST);
    cmd::Enable(ctx(), GL_FOG);
    cmd::Fog(ctx(), GL_FOG_MODE, GlValues::scalar_value(GL_LINEAR, true));
    cmd::Fog(ctx(), GL_FOG_START, GlValues::scalar_value(0, false));
    cmd::Fog(ctx(), GL_FOG_END, GlValues::scalar_value(4, false));
    cmd::Fog(ctx(), GL_FOG_COLOR, GlValues::vector(std::array<double, 4>{1, 1, 1, 1}, false));
    raster(8, 2, -2);
    EXPECT_FLOAT_EQ(ctx().state.current.raster_distance, -2.0f);
    draw(1, 1, GL_RGBA, GL_UNSIGNED_BYTE, std::vector<u8>{0, 0, 0, 255});
    cmd::Disable(ctx(), GL_FOG);
    std::vector<Rgba> px = pixels();
    std::vector<float> z = depth();
    for (i32 y = 2; y < 4; ++y) {
        EXPECT_EQ(at(px, 2, y), (Rgba{255, 0, 0, 255}));
        EXPECT_EQ(at(px, 3, y), (y == 2 ? Rgba{255, 0, 128, 255} : Rgba{255, 0, 0, 255}));
        EXPECT_EQ(at(px, 4, y), (Rgba{0, 0, 0, 0})) << "behind the depth buffer";
        EXPECT_EQ(at(px, 5, y), (Rgba{0, 0, 0, 0}));
        EXPECT_FLOAT_EQ(z[y * kW + 2], 0.25f);
        EXPECT_FLOAT_EQ(z[y * kW + 4], 0.5f);
    }
    expect_color(at(px, 8, 2), {0.5, 0.5, 0.5, 1}, "fogged");

    // DEPTH_COMPONENT groups write their depth through the depth test and
    // mask; the fragments carry the raster colour.
    cmd::MatrixMode(ctx(), GL_PROJECTION);
    cmd::LoadIdentity(ctx());
    cmd::Ortho(ctx(), 0, kW, 0, kH, -1, 1);
    cmd::MatrixMode(ctx(), GL_MODELVIEW);
    clear(0, 0, 0, 0);
    cmd::ClearDepth(ctx(), 0.5);
    cmd::Clear(ctx(), GL_DEPTH_BUFFER_BIT);
    cmd::Enable(ctx(), GL_DEPTH_TEST);
    cmd::DepthFunc(ctx(), GL_LESS);
    const std::vector<float> dv{0.125f, 0.75f, 0.375f, 0.0625f};
    std::vector<u8> dbytes(dv.size() * 4);
    std::memcpy(dbytes.data(), dv.data(), dbytes.size());
    cmd::Color(ctx(), 0, 1, 1, 1);
    raster(10, 10);
    draw(2, 2, GL_DEPTH_COMPONENT, GL_FLOAT, dbytes);
    EXPECT_EQ(error(), GL_NO_ERROR);
    // Masked: nothing changes the depth buffer; scaled and biased.
    cmd::DepthMask(ctx(), false);
    transfer(GL_DEPTH_SCALE, 0.0);
    raster(14, 10);
    draw(2, 2, GL_DEPTH_COMPONENT, GL_FLOAT, dbytes);
    cmd::DepthMask(ctx(), true);
    transfer(GL_DEPTH_SCALE, 1.0);
    cmd::Disable(ctx(), GL_DEPTH_TEST);
    px = pixels();
    z = depth();
    for (i32 m = 0; m < 2; ++m) {
        for (i32 n = 0; n < 2; ++n) {
            const float d = dv[static_cast<usize>(m * 2 + n)];
            const bool pass = d < 0.5f;
            EXPECT_EQ(z[(10 + m) * kW + 10 + n], pass ? d : 0.5f) << n << "," << m;
            EXPECT_EQ(at(px, 10 + n, 10 + m), (pass ? Rgba{0, 255, 255, 255} : Rgba{0, 0, 0, 0}));
            EXPECT_EQ(z[(10 + m) * kW + 14 + n], 0.5f) << "depth mask";
            EXPECT_EQ(at(px, 14 + n, 10 + m), (Rgba{0, 255, 255, 255})) << "depth 0 passes, colour written";
        }
    }

    // STENCIL_INDEX groups: written under the stencil write mask, only the
    // scissor test applies, colour and depth untouched.
    clear(0.25f, 0.25f, 0.25f, 1);
    cmd::StencilMask(ctx(), 0x0F);
    cmd::Enable(ctx(), GL_SCISSOR_TEST);
    cmd::Scissor(ctx(), 0, 0, 21, 32);
    cmd::Enable(ctx(), GL_DEPTH_TEST);
    cmd::DepthFunc(ctx(), GL_NEVER);   // bypassed
    cmd::ColorMask(ctx(), true, true, true, true);
    const std::vector<u8> sv{0x13, 0x25, 0xFF, 0x00, 0x13, 0x13};
    raster(18, 4);
    store(GL_UNPACK_ALIGNMENT, 1);   // rows of 3 bytes
    draw(3, 2, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, sv);
    EXPECT_EQ(error(), GL_NO_ERROR);
    cmd::Disable(ctx(), GL_SCISSOR_TEST);
    cmd::Disable(ctx(), GL_DEPTH_TEST);
    cmd::StencilMask(ctx(), 0xFF);
    px = pixels();
    const std::vector<u8> st = stencil();
    z = depth();
    for (i32 m = 0; m < 2; ++m) {
        for (i32 n = 0; n < 3; ++n) {
            const i32 x = 18 + n, y = 4 + m;
            const u8 want = x < 21 ? static_cast<u8>(sv[static_cast<usize>(m * 3 + n)] & 0x0F) : 0;
            EXPECT_EQ(st[y * kW + x], want) << x << "," << y;
            EXPECT_EQ(at(px, x, y), (Rgba{64, 64, 64, 255}));
            EXPECT_EQ(z[y * kW + x], 1.0f);
        }
    }
}

// ---- Spec 4.3.2: glReadPixels' pack pipeline ---------------------------------------------

TEST_P(GlPixelsMetalTest, ReadPixelsPacksPerPixelStore) {
    clear(0, 0, 0, 0);
    // A known 4 x 3 image at (1, 2) (DrawPixels of exact 8-bit values).
    const i32 w = 4, h = 3;
    const std::vector<u8> img = pattern_bytes(static_cast<usize>(w * h) * 4u, 9);
    raster(1, 2);
    draw(w, h, GL_RGBA, GL_UNSIGNED_BYTE, img);
    const auto group = [&](i32 n, i32 m) { return &img[static_cast<usize>(m * w + n) * 4u]; };

    // RGBA bytes under PACK_ALIGNMENT 8, ROW_LENGTH 5, SKIP_PIXELS 1, SKIP_ROWS
    // 1: k = 8 ceil(20 / 8) = 24 bytes per row; bytes between groups stay.
    store(GL_PACK_ALIGNMENT, 8);
    store(GL_PACK_ROW_LENGTH, 5);
    store(GL_PACK_SKIP_PIXELS, 1);
    store(GL_PACK_SKIP_ROWS, 1);
    std::vector<u8> out = read(1, 2, w, h, GL_RGBA, GL_UNSIGNED_BYTE, 24 * 4);
    for (i32 m = 0; m < h; ++m) {
        for (i32 n = 0; n < w; ++n) {
            const usize at = static_cast<usize>(24 * (m + 1) + 4 * (n + 1));
            for (usize k = 0; k < 4; ++k) EXPECT_EQ(out[at + k], group(n, m)[k]) << n << "," << m << " " << k;
        }
    }
    EXPECT_EQ(out[0], 0xAB);
    EXPECT_EQ(out[24], 0xAB);   // SKIP_PIXELS group untouched
    store(GL_PACK_ALIGNMENT, 4);
    store(GL_PACK_ROW_LENGTH, 0);
    store(GL_PACK_SKIP_PIXELS, 0);
    store(GL_PACK_SKIP_ROWS, 0);

    // LUMINANCE floats: L = R + G + B, clamped; with RED_SCALE 0.5 first.
    transfer(GL_RED_SCALE, 0.5);
    out = read(1, 2, w, 1, GL_LUMINANCE, GL_FLOAT, 16);
    transfer(GL_RED_SCALE, 1.0);
    for (i32 n = 0; n < w; ++n) {
        float l;
        std::memcpy(&l, &out[static_cast<usize>(n) * 4u], 4);
        const double want = std::min(1.0, group(n, 0)[0] / 255.0 * 0.5 + group(n, 0)[1] / 255.0 + group(n, 0)[2] / 255.0);
        EXPECT_NEAR(l, want, 1e-6) << n;
    }
    // RGB and ALPHA unsigned shorts (Table 4.7: round(c 65535)), BGR order kept.
    out = read(1, 2, 2, 1, GL_RGB, GL_UNSIGNED_SHORT, 12);
    for (i32 n = 0; n < 2; ++n) {
        for (usize k = 0; k < 3; ++k) {
            u16 v;
            std::memcpy(&v, &out[static_cast<usize>(n) * 6u + k * 2], 2);
            EXPECT_EQ(v, group(n, 0)[k] * 257u);
        }
    }
    out = read(2, 3, 1, 1, GL_ALPHA, GL_BYTE, 4);
    EXPECT_EQ(static_cast<i8>(out[0]), static_cast<i8>(std::floor((255.0 * (group(1, 1)[3] / 255.0) - 1.0) / 2.0 + 0.5)));
    EXPECT_EQ(out[1], 0xAB);

    // Outside the window: those pixels' client bytes are untouched
    // (readpixels-outside-lower-left-2x2, -upper-right-2x2).
    out = read(-1, -1, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, 16);
    for (usize i = 0; i < 12; ++i) EXPECT_EQ(out[i], 0xAB) << i;
    EXPECT_EQ((std::vector<u8>(out.begin() + 12, out.end())), (std::vector<u8>{0, 0, 0, 0}));
    out = read(kW - 1, kH - 1, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, 16);
    EXPECT_EQ((std::vector<u8>(out.begin(), out.begin() + 4)), (std::vector<u8>{0, 0, 0, 0}));
    for (usize i = 4; i < 16; ++i) EXPECT_EQ(out[i], 0xAB) << i;

    // DEPTH_COMPONENT and STENCIL_INDEX from the depth/stencil attachment.
    cmd::ClearDepth(ctx(), 0.25);
    cmd::ClearStencil(ctx(), 0x5A);
    cmd::Clear(ctx(), GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    out = read(5, 5, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, 4);
    float dz;
    std::memcpy(&dz, out.data(), 4);
    EXPECT_EQ(dz, 0.25f);
    transfer(GL_DEPTH_SCALE, 2.0);
    transfer(GL_DEPTH_BIAS, -0.25);
    out = read(5, 5, 1, 1, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 4);
    u16 dus;
    std::memcpy(&dus, out.data(), 2);
    EXPECT_EQ(dus, static_cast<u16>(std::floor(0.25 * 65535.0 + 0.5)));
    transfer(GL_DEPTH_SCALE, 1.0);
    transfer(GL_DEPTH_BIAS, 0.0);
    // readpixels-stencil-*: 0x5A; shift 2, offset 3 -> 0x16B masked (Table 4.6).
    out = read(5, 5, 1, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, 4);
    EXPECT_EQ(out[0], 0x5A);
    cmd::PixelTransfer(ctx(), GL_INDEX_SHIFT, 2, true);
    cmd::PixelTransfer(ctx(), GL_INDEX_OFFSET, 3, true);
    out = read(5, 5, 1, 1, GL_STENCIL_INDEX, GL_BYTE, 4);
    EXPECT_EQ(out[0], 0x6B);
    cmd::PixelTransfer(ctx(), GL_INDEX_SHIFT, 0, true);
    cmd::PixelTransfer(ctx(), GL_INDEX_OFFSET, 0, true);
    out = read(5, 5, 8, 1, GL_STENCIL_INDEX, GL_BITMAP, 4);
    EXPECT_EQ(out[0], 0x00);   // readpixels-stencil-bitmap-8: 0x5A & 1
    out = read(-1, 5, 2, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, 4);
    EXPECT_EQ((std::vector<u8>(out.begin(), out.begin() + 3)), (std::vector<u8>{0xAB, 0x5A, 0xAB}));

    // The exports, both bitnesses: glDrawPixels and glReadPixels with guest
    // memory (readpixels-inside-rgba-ubyte-2x1 shape).
    const std::vector<u8> two{11, 22, 33, 44, 55, 66, 77, 88};
    ASSERT_TRUE(memory_.write_bytes(kGuestImage, two.data(), two.size()).is_ok());
    std::vector<u8> sentinel(12, 0xAB);
    ASSERT_TRUE(memory_.write_bytes(kGuestOut, sentinel.data(), sentinel.size()).is_ok());
    raster(20, 20);
    main_->call("glDrawPixels", {2, 1, GL_RGBA, GL_UNSIGNED_BYTE, kGuestImage});
    main_->call("glReadPixels", {20, 20, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, kGuestOut});
    EXPECT_EQ(main_->call("glGetError"), 0u);
    std::vector<u8> back(12);
    ASSERT_TRUE(memory_.read_bytes(kGuestOut, back.data(), back.size()).is_ok());
    EXPECT_EQ(back, (std::vector<u8>{11, 22, 33, 44, 55, 66, 77, 88, 0xAB, 0xAB, 0xAB, 0xAB}));
}

// ---- Spec 4.3.3: glCopyPixels -----------------------------------------------------------

TEST_P(GlPixelsMetalTest, CopyPixelsFromFrontToBack) {
    clear(0, 0, 0, 0);
    cmd::DrawBuffer(ctx(), GL_FRONT);
    clear(0, 0, 0, 0);
    const i32 w = 3, h = 2;
    const std::vector<u8> img = pattern_bytes(static_cast<usize>(w * h) * 4u, 21);
    raster(1, 1);
    draw(w, h, GL_RGBA, GL_UNSIGNED_BYTE, img);
    const std::vector<Rgba> front_before = pixels(GL_FRONT);
    // Read the front, draw into the back, zoom (2, 1), RED_SCALE 0.5.
    cmd::ReadBuffer(ctx(), GL_FRONT);
    cmd::DrawBuffer(ctx(), GL_BACK);
    raster(10, 12);
    cmd::PixelZoom(ctx(), 2, 1);
    transfer(GL_RED_SCALE, 0.5);
    cmd::CopyPixels(ctx(), 1, 1, w, h, GL_COLOR);
    EXPECT_EQ(error(), GL_NO_ERROR);
    transfer(GL_RED_SCALE, 1);
    cmd::PixelZoom(ctx(), 1, 1);
    const std::vector<Rgba> back = pixels(GL_BACK);
    for (i32 y = 0; y < kH; ++y) {
        for (i32 x = 0; x < kW; ++x) {
            i32 n = 0, m = 0;
            if (ref_group(10, 12, 2, 1, w, h, x, y, n, m)) {
                const u8* e = &img[static_cast<usize>(m * w + n) * 4u];
                expect_color(at(back, x, y), {e[0] / 255.0 * 0.5, e[1] / 255.0, e[2] / 255.0, e[3] / 255.0},
                             "copy " + std::to_string(x) + "," + std::to_string(y));
            } else {
                EXPECT_EQ(at(back, x, y), (Rgba{0, 0, 0, 0})) << x << "," << y;
            }
        }
    }
    EXPECT_TRUE(pixels(GL_FRONT) == front_before) << "the source is not changed";

    // Overlapping source and destination behave as if the source were read
    // first: a 4 x 1 row copied one pixel right.
    cmd::ReadBuffer(ctx(), GL_BACK);
    clear(0, 0, 0, 0);
    const std::vector<u8> row = pattern_bytes(16, 4);
    raster(0, 20);
    draw(4, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
    raster(1, 20);
    cmd::CopyPixels(ctx(), 0, 20, 4, 1, GL_COLOR);
    const std::vector<Rgba> shifted = pixels();
    EXPECT_EQ(at(shifted, 0, 20), (Rgba{row[0], row[1], row[2], row[3]}));
    for (i32 n = 0; n < 4; ++n) {
        const u8* e = &row[static_cast<usize>(n) * 4u];
        EXPECT_EQ(at(shifted, 1 + n, 20), (Rgba{e[0], e[1], e[2], e[3]})) << n;
    }

    // DEPTH and STENCIL copies, through the transfer paths.
    cmd::ClearDepth(ctx(), 0.75);
    cmd::ClearStencil(ctx(), 0x21);
    cmd::Clear(ctx(), GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    cmd::Enable(ctx(), GL_DEPTH_TEST);
    cmd::DepthFunc(ctx(), GL_ALWAYS);
    transfer(GL_DEPTH_BIAS, -0.5);
    cmd::PixelTransfer(ctx(), GL_INDEX_OFFSET, 2, true);
    raster(6, 6);
    cmd::CopyPixels(ctx(), 0, 0, 2, 1, GL_DEPTH);
    cmd::CopyPixels(ctx(), 0, 0, 2, 1, GL_STENCIL);
    transfer(GL_DEPTH_BIAS, 0);
    cmd::PixelTransfer(ctx(), GL_INDEX_OFFSET, 0, true);
    cmd::Disable(ctx(), GL_DEPTH_TEST);
    const std::vector<float> z = depth();
    const std::vector<u8> st = stencil();
    EXPECT_EQ(z[6 * kW + 6], 0.25f);
    EXPECT_EQ(z[6 * kW + 7], 0.25f);
    EXPECT_EQ(z[6 * kW + 8], 0.75f);
    EXPECT_EQ(st[6 * kW + 6], 0x23);
    EXPECT_EQ(st[6 * kW + 7], 0x23);
    EXPECT_EQ(st[6 * kW + 8], 0x21);
}

// ---- Spec 3.6.5 / 3.8: colour pixel rectangles are textured at the raster position ------------

// The fragments of glDrawPixels and glCopyPixels colour groups carry the
// group colour as C_f and the raster position's texture coordinates (s/q,
// t/q); with TEXTURE_2D enabled the texture environment (Table 3.18/3.19,
// RGBA base: REPLACE C = C_t, A = A_t; MODULATE C = C_f C_t, A = A_f A_t)
// applies the texel there. The coordinates are the same for every fragment,
// so the magnification filter samples (LINEAR at u = 2^n s blends texels
// floor(u - 1/2) and the next by frac(u - 1/2)).
TEST_P(GlPixelsMetalTest, DrawAndCopyPixelsAreTexturedAtTheRasterCoordinates) {
    clear(0, 0, 0, 0);
    const u8 tex[16] = {200, 100, 50, 128, 20, 40, 60, 255, 1, 2, 3, 4, 5, 6, 7, 8};   // t00 t10 / t01 t11
    const std::vector<u8> img{255, 128, 64, 255, 100, 200, 50, 102};                 // c0 c1
    std::vector<u32> names;
    cmd::GenTextures(ctx(), 1, names);
    cmd::BindTexture(ctx(), GL_TEXTURE_2D, names[0]);
    GlClientPixels tp;
    tp.bytes = std::span<const u8>(tex, 16);
    cmd::TexImage(ctx(), 2, GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, tp);
    const auto filters = [&](u32 min, u32 mag) {
        cmd::TexParameter(ctx(), GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GlValues::scalar_value(min, true));
        cmd::TexParameter(ctx(), GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GlValues::scalar_value(mag, true));
    };
    const auto env = [&](u32 mode) {
        cmd::TexEnv(ctx(), GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GlValues::scalar_value(mode, true));
    };
    filters(GL_NEAREST, GL_NEAREST);
    const auto t = [&](i32 i) { return Color{tex[i * 4] / 255.0, tex[i * 4 + 1] / 255.0, tex[i * 4 + 2] / 255.0, tex[i * 4 + 3] / 255.0}; };
    const auto c = [&](i32 i) { return Color{img[i * 4] / 255.0, img[i * 4 + 1] / 255.0, img[i * 4 + 2] / 255.0, img[i * 4 + 3] / 255.0}; };
    const auto mul = [](const Color& a, const Color& b) { return Color{a[0] * b[0], a[1] * b[1], a[2] * b[2], a[3] * b[3]}; };

    // Untextured reference copy source at (2, 2).
    raster(2, 2);
    draw(2, 1, GL_RGBA, GL_UNSIGNED_BYTE, img);
    cmd::Enable(ctx(), GL_TEXTURE_2D);
    // The raster coordinates (1.5, 0.5, 0, 2) -> (0.75, 0.25): texel (1, 0).
    cmd::TexCoord(ctx(), 1.5f, 0.5f, 0, 2);
    env(GL_REPLACE);
    raster(2, 4);
    cmd::TexCoord(ctx(), 0, 0, 0, 1);   // a later glTexCoord does not change the raster position's
    draw(2, 1, GL_RGBA, GL_UNSIGNED_BYTE, img);
    env(GL_MODULATE);
    cmd::TexCoord(ctx(), 1.5f, 0.5f, 0, 2);
    raster(2, 6);
    draw(2, 1, GL_RGBA, GL_UNSIGNED_BYTE, img);
    // glCopyPixels COLOR of the untextured copy at (2, 2), MODULATE.
    raster(10, 6);
    cmd::CopyPixels(ctx(), 2, 2, 2, 1, GL_COLOR);
    env(GL_REPLACE);
    raster(10, 4);
    cmd::CopyPixels(ctx(), 2, 2, 2, 1, GL_COLOR);
    // Magnification: LINEAR at (0.5, 0.25) blends t00 and t10 equally
    // (a minification filter, NEAREST here, would take one texel).
    filters(GL_NEAREST, GL_LINEAR);
    env(GL_MODULATE);
    cmd::TexCoord(ctx(), 0.5f, 0.25f, 0, 1);
    raster(2, 8);
    draw(2, 1, GL_RGBA, GL_UNSIGNED_BYTE, img);
    cmd::Disable(ctx(), GL_TEXTURE_2D);
    EXPECT_EQ(error(), GL_NO_ERROR);

    const std::vector<Rgba> px = pixels();
    const Color avg{(tex[0] + tex[4]) / 510.0, (tex[1] + tex[5]) / 510.0, (tex[2] + tex[6]) / 510.0, (tex[3] + tex[7]) / 510.0};
    for (i32 n = 0; n < 2; ++n) {
        const std::string g = " group " + std::to_string(n);
        expect_color(at(px, 2 + n, 2), c(n), "untextured" + g);
        expect_color(at(px, 2 + n, 4), t(1), "DrawPixels REPLACE" + g);
        expect_color(at(px, 2 + n, 6), mul(c(n), t(1)), "DrawPixels MODULATE" + g);
        expect_color(at(px, 10 + n, 6), mul(c(n), t(1)), "CopyPixels MODULATE" + g);
        expect_color(at(px, 10 + n, 4), t(1), "CopyPixels REPLACE" + g);
        expect_color(at(px, 2 + n, 8), mul(c(n), avg), "DrawPixels LINEAR magnification" + g);
    }
}

// The group colour reaches blending at full precision (no 8-bit rounding
// before the per-fragment operations): R = 2.6 / 255 blended (SRC_ALPHA,
// ZERO) with A = 0.5 is 1.3 / 255 -> 1, where a group first rounded to
// 3 / 255 would give 1.5 / 255 -> 2.
TEST_P(GlPixelsMetalTest, ColourGroupsKeepFullPrecisionIntoBlending) {
    clear(0, 0, 0, 0);
    const float v[4] = {static_cast<float>(2.6 / 255.0), 0, 0, 0.5f};
    std::vector<u8> bytes(16);
    std::memcpy(bytes.data(), v, 16);
    cmd::Enable(ctx(), GL_BLEND);
    cmd::BlendFunc(ctx(), GL_SRC_ALPHA, GL_ZERO);
    raster(3, 3);
    draw(1, 1, GL_RGBA, GL_FLOAT, bytes);
    cmd::Disable(ctx(), GL_BLEND);
    const Rgba p = at(pixels(), 3, 3);
    EXPECT_EQ(p.r, 1u);
}

// ---- Client memory (final review Findings 1 and 3) -------------------------------------

// The final review's values (ALIGNMENT 4, ROW_LENGTH 2^31 - 1, SKIP_ROWS 2^30,
// SKIP_PIXELS 2^30 - 100) wrapped the RGBA FLOAT layout of a 101 x 1 image to
// first_byte -1600 and a 16-byte span: glDrawPixels drew 1600 bytes of host
// heap and glReadPixels wrote 1.6 KB before its buffer. Through the exports,
// both bitnesses: nothing is read, drawn or written; no error.
TEST_P(GlPixelsMetalTest, WrappingPixelStoreTouchesNoMemory) {
    clear(0, 0, 0, 0);
    std::vector<u8> fill(0x800, 0x3F);
    ASSERT_TRUE(memory_.write_bytes(kGuestImage, fill.data(), fill.size()).is_ok());
    for (const bool pack : {false, true}) {
        main_->call("glPixelStorei", {pack ? GL_PACK_ALIGNMENT : GL_UNPACK_ALIGNMENT, 4});
        main_->call("glPixelStorei", {pack ? GL_PACK_ROW_LENGTH : GL_UNPACK_ROW_LENGTH, 0x7FFFFFFF});
        main_->call("glPixelStorei", {pack ? GL_PACK_SKIP_ROWS : GL_UNPACK_SKIP_ROWS, 1u << 30});
        main_->call("glPixelStorei", {pack ? GL_PACK_SKIP_PIXELS : GL_UNPACK_SKIP_PIXELS, (1u << 30) - 100});
    }
    raster(0, 0);
    for (const u64 w : {101u, 100u}) {
        main_->call("glDrawPixels", {w, 1, GL_RGBA, GL_FLOAT, kGuestImage});
        EXPECT_EQ(main_->call("glGetError"), 0u) << w;
    }
    // glBitmap: SKIP_ROWS (2^30) * k (2^28 bytes) is past any user address
    // space; the raster position still moves.
    main_->call("glBitmap", {8, 8, 0, 0, 0x40A00000u /*5.0f*/, 0, kGuestImage});
    EXPECT_EQ(main_->call("glGetError"), 0u);
    EXPECT_FLOAT_EQ(ctx().state.current.raster_position[0], 5.0f);
    const std::vector<Rgba> px = pixels();
    for (usize i = 0; i < px.size(); ++i) ASSERT_EQ(px[i], (Rgba{0, 0, 0, 0})) << i;

    std::vector<u8> sentinel(0x800, 0xAB);
    ASSERT_TRUE(memory_.write_bytes(kGuestOut, sentinel.data(), sentinel.size()).is_ok());
    main_->call("glReadPixels", {0, 0, 101, 1, GL_RGBA, GL_FLOAT, kGuestOut});
    main_->call("glReadPixels", {0, 0, 101, 1, GL_STENCIL_INDEX, GL_FLOAT, kGuestOut});
    EXPECT_EQ(main_->call("glGetError"), 0u);
    std::vector<u8> back(sentinel.size());
    ASSERT_TRUE(memory_.read_bytes(kGuestOut, back.data(), back.size()).is_ok());
    EXPECT_EQ(back, sentinel);
}

// Finding 3: off-window pixels are undefined but legal (spec 4.3.2), so a
// glReadPixels rectangle far larger than the window only writes the window's
// part of the client's buffer; nothing of width x height is allocated. A
// 100000 x 100000 rectangle names 40 GB of client memory that is not mapped:
// nothing is written and the call returns (before the fix it allocated about
// 90 GB of host memory first).
TEST_P(GlPixelsMetalTest, ReadPixelsWorkIsBoundedByTheWindow) {
    clear(0.0f, 1.0f, 0.0f, 1.0f);
    constexpr GuestAddr kWide = 0x200000;
    constexpr usize kWideBytes = 0x50000;   // 320 KiB: the row, then the 200000-byte column
    ASSERT_TRUE(memory_.map_region(kWide, kWideBytes, MemoryProtection::ReadWrite).is_ok());
    std::vector<u8> sentinel(kWideBytes, 0xAB);
    ASSERT_TRUE(memory_.write_bytes(kWide, sentinel.data(), sentinel.size()).is_ok());
    main_->call("glPixelStorei", {GL_PACK_ALIGNMENT, 1});
    // One row of 100000 GREEN bytes: the window's kW are written, the rest stay.
    main_->call("glReadPixels", {0, 0, 100000, 1, GL_GREEN, GL_UNSIGNED_BYTE, kWide});
    // One column of 100000 rows, two bytes each, after it.
    main_->call("glReadPixels", {0, 0, 1, 100000, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, kWide + 100000});
    EXPECT_EQ(main_->call("glGetError"), 0u);
    std::vector<u8> back(kWideBytes);
    ASSERT_TRUE(memory_.read_bytes(kWide, back.data(), back.size()).is_ok());
    for (usize i = 0; i < 100000; ++i) ASSERT_EQ(back[i], i < static_cast<usize>(kW) ? 0xFF : 0xAB) << i;
    // The column: L = R + G + B = 1 and A = 1 for the window's kH rows.
    for (usize j = 0; j < 200000; ++j) {
        const u8 want = j < static_cast<usize>(kH) * 2 ? 0xFF : 0xAB;
        ASSERT_EQ(back[100000 + j], want) << j;
    }
    main_->call("glReadPixels", {0, 0, 100000, 100000, GL_RGBA, GL_UNSIGNED_BYTE, kWide});
    EXPECT_EQ(main_->call("glGetError"), 0u);
    ASSERT_TRUE(memory_.read_bytes(kWide, back.data(), 16).is_ok());
    EXPECT_EQ(back[kW], 0xAB);   // unchanged: the 40 GB span is not client memory
}

std::string bitness_name(const ::testing::TestParamInfo<bool>& info) { return info.param ? "x64" : "x86"; }

INSTANTIATE_TEST_SUITE_P(Bitness, GlPixelsMetalTest, ::testing::Values(false, true), bitness_name);

}  // namespace
