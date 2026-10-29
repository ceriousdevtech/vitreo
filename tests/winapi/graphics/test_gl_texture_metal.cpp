/// @file test_gl_texture_metal.cpp
/// @brief OpenGL 1.1 texturing on the real Metal backend: the texture
///        environment per base format (Tables 3.18-3.19), the wrap modes, the
///        filters and mipmap selection (3.8.1-3.8.2), completeness, glTexGen
///        and the texture matrix on the GPU and CPU paths, the per-fragment
///        division by q, glCopyTexImage from the read buffer, and the order of
///        draws and image changes.
///
/// Kept out of vitreo_winapi_tests, which does not link the Metal backend.
/// Expected pixels come from the spec's formulas evaluated in double
/// precision here: a texel is its stored 8-bit value c / 255; NEAREST takes
/// texel floor(u), LINEAR weights the 2x2 square at floor(u - 1/2) by
/// frac(u - 1/2), CLAMP reads the border colour outside the image, the
/// mipmap filters select levels by lambda = log2(rho) (eq. 3.11), and the
/// texture functions are Tables 3.18-3.19. Results are 8-bit, so a channel
/// may differ from the exact value by the final conversion's rounding (1).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include <graphics/contracts/backend_device.hpp>
#include <graphics/metal/metal_device.hpp>

#include "winapi/graphics/opengl32/gl_backend_draw.hpp"
#include "winapi/graphics/opengl32/gl_commands.hpp"
#include "winapi/graphics/opengl32/gl_context.hpp"
#include "winapi/graphics/opengl32/gl_drawable.hpp"
#include "winapi/graphics/opengl32/gl_enums.hpp"
#include "winapi/graphics/opengl32/gl_texture.hpp"
#include "winapi/graphics/opengl32/opengl32.hpp"

#include <atomic>
#include <thread>
#include "gl_test_caller.hpp"

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::gl;
using test::Caller;
using cmd::GlClientPixels;
using cmd::GlResidency;

namespace {

constexpr i32 kDoubleAlpha = 13;   // double, alpha 8, depth 24, stencil 8 (gl_pixel_formats.cpp)
constexpr u64 kDc = 0x6000;
constexpr u64 kWindow = 0x200;
constexpr u64 kDc2 = 0x6004;      // a second window, for a second thread
constexpr u64 kWindow2 = 0x204;
constexpr u32 kW = 32, kH = 32;

using Color = std::array<double, 4>;

struct Rgba {
    u8 r, g, b, a;
};

/// Tolerance of the final 8-bit conversion of a value computed in float.
constexpr double kTol = 1.01;

class GlTextureMetalTest : public ::testing::TestWithParam<bool> {
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
        mgr_->set_pixel_format_query([](u64 dc) { return dc == kDc || dc == kDc2 ? kDoubleAlpha : 0; });
        mgr_->set_drawable_queries([](u64 dc) -> u64 { return dc == kDc ? kWindow : dc == kDc2 ? kWindow2 : 0; },
                                   [](u64 dc, i32& w, i32& h) {
                                       if (dc != kDc && dc != kDc2) return false;
                                       w = static_cast<i32>(kW);
                                       h = static_cast<i32>(kH);
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
        cmd::ClearColor(ctx(), 0, 0, 0, 0);
        cmd::Clear(ctx(), GL_COLOR_BUFFER_BIT);
        cmd::Enable(ctx(), GL_TEXTURE_2D);
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

    /// The back buffer, row 0 = the bottom row. Reading ends the pass.
    std::vector<Rgba> pixels() {
        std::vector<u8> bgra;
        EXPECT_TRUE(drawable_->read_color(GL_BACK, bgra));
        std::vector<Rgba> out(kW * kH);
        for (u32 gy = 0; gy < kH; ++gy) {
            const u32 row = kH - 1 - gy;
            for (u32 x = 0; x < kW; ++x) {
                const u8* p = &bgra[(static_cast<usize>(row) * kW + x) * 4u];
                out[gy * kW + x] = Rgba{p[2], p[1], p[0], p[3]};
            }
        }
        return out;
    }
    static Rgba at(const std::vector<Rgba>& px, u32 x, u32 y) { return px.at(y * kW + x); }

    static void expect_color(const Rgba& got, const Color& want, const std::string& where) {
        const double g[4] = {static_cast<double>(got.r), static_cast<double>(got.g), static_cast<double>(got.b),
                             static_cast<double>(got.a)};
        for (u32 k = 0; k < 4; ++k) {
            EXPECT_NEAR(g[k], std::clamp(want[k], 0.0, 1.0) * 255.0, kTol) << where << " channel " << k;
        }
    }

    void param(u32 pname, u32 value, u32 target = GL_TEXTURE_2D) {
        cmd::TexParameter(ctx(), target, pname, GlValues::scalar_value(value, true));
    }
    void filters(u32 min, u32 mag, u32 target = GL_TEXTURE_2D) {
        param(GL_TEXTURE_MIN_FILTER, min, target);
        param(GL_TEXTURE_MAG_FILTER, mag, target);
    }
    void wrap(u32 s, u32 t) {
        param(GL_TEXTURE_WRAP_S, s);
        param(GL_TEXTURE_WRAP_T, t);
    }
    void env(u32 mode) { cmd::TexEnv(ctx(), GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GlValues::scalar_value(mode, true)); }
    void image(u32 dims, i32 level, u32 internal_format, i32 w, i32 h, const std::vector<u8>& rgba) {
        GlClientPixels px;
        px.bytes = rgba;
        cmd::TexImage(ctx(), dims, dims == 1 ? GL_TEXTURE_1D : GL_TEXTURE_2D, level, static_cast<i32>(internal_format), w,
                      h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        ASSERT_EQ(error(), GL_NO_ERROR);
    }
    void color(const Color& c) {
        cmd::Color(ctx(), static_cast<float>(c[0]), static_cast<float>(c[1]), static_cast<float>(c[2]),
                   static_cast<float>(c[3]));
    }
    /// A quad over window pixels [x0, x1) x [y0, y1) with texture coordinates
    /// (s0, t0) at (x0, y0) and (s1, t1) at (x1, y1).
    void quad(double x0, double y0, double x1, double y1, double s0, double t0, double s1, double t1) {
        cmd::Begin(ctx(), GL_QUADS);
        const auto v = [&](double x, double y, double s, double t) {
            cmd::TexCoord(ctx(), static_cast<float>(s), static_cast<float>(t), 0, 1);
            cmd::Vertex(ctx(), static_cast<float>(x), static_cast<float>(y), 0, 1);
        };
        v(x0, y0, s0, t0);
        v(x1, y0, s1, t0);
        v(x1, y1, s1, t1);
        v(x0, y1, s0, t1);
        cmd::End(ctx());
    }

    GuestMemory memory_;
    std::unique_ptr<ThunkRegistry> registry_;
    std::unique_ptr<Opengl32Manager> mgr_;
    std::unique_ptr<Caller> main_;
    u64 rc_{0};
    GlContext* ctx_{nullptr};
    std::shared_ptr<GlDrawable> drawable_;
};

double c8(u32 v) { return v / 255.0; }

// ---- The texture environment (Tables 3.18-3.19) ------------------------------------

// Every mode on every base format: a 1x1 texture whose stored texel (Table
// 3.15 selection of R, G, B, A) meets a known fragment colour C_f and
// environment colour C_c; the pixel is the table's C_v, A_v. DECAL on ALPHA,
// LUMINANCE, LUMINANCE_ALPHA and INTENSITY is undefined: Vitreo passes the
// fragment through (C_v = C_f, A_v = A_f).
TEST_P(GlTextureMetalTest, TexEnvModesPerBaseFormat) {
    const std::array<u32, 4> src{204, 102, 51, 153};   // the pixel group (R, G, B, A)
    const Color cf{c8(128), c8(64), c8(255), c8(192)};
    const Color cc{c8(51), c8(230), c8(102), c8(77)};
    const u32 bases[] = {GL_ALPHA, GL_LUMINANCE, GL_LUMINANCE_ALPHA, GL_INTENSITY, GL_RGB, GL_RGBA};
    const u32 modes[] = {GL_REPLACE, GL_MODULATE, GL_DECAL, GL_BLEND};
    const std::vector<u8> texel{static_cast<u8>(src[0]), static_cast<u8>(src[1]), static_cast<u8>(src[2]),
                                static_cast<u8>(src[3])};
    cmd::TexEnv(ctx(), GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR,
                GlValues::vector(std::array<double, 4>{cc[0], cc[1], cc[2], cc[3]}, false));
    for (u32 b = 0; b < 6; ++b) {
        cmd::BindTexture(ctx(), GL_TEXTURE_2D, 10 + b);
        image(2, 0, bases[b], 1, 1, texel);
        filters(GL_NEAREST, GL_NEAREST);
    }
    struct Case {
        u32 base, mode, x, y;
    };
    std::vector<Case> cases;
    for (u32 b = 0; b < 6; ++b) {
        for (u32 m = 0; m < 4; ++m) {
            const u32 cell = b * 4 + m;
            cases.push_back({bases[b], modes[m], (cell % 8) * 4, (cell / 8) * 4});
            cmd::BindTexture(ctx(), GL_TEXTURE_2D, 10 + b);
            env(modes[m]);
            color(cf);
            quad(cases.back().x, cases.back().y, cases.back().x + 4, cases.back().y + 4, 0, 0, 1, 1);
        }
    }
    ASSERT_EQ(error(), GL_NO_ERROR);
    const std::vector<Rgba> px = pixels();
    for (const Case& c : cases) {
        // The filtered texture values: L_t = I_t = R, A_t = A (Table 3.15).
        const Color ct{c8(src[0]), c8(src[1]), c8(src[2]), c8(src[3])};
        const double lt = ct[0], at_ = ct[3];
        Color v = cf;
        const auto rgb = [&](const std::function<double(u32)>& f) {
            for (u32 k = 0; k < 3; ++k) v[k] = f(k);
        };
        switch (c.mode) {
        case GL_REPLACE:
            switch (c.base) {
            case GL_ALPHA: v[3] = at_; break;
            case GL_LUMINANCE: rgb([&](u32) { return lt; }); break;
            case GL_LUMINANCE_ALPHA: rgb([&](u32) { return lt; }); v[3] = at_; break;
            case GL_INTENSITY: rgb([&](u32) { return lt; }); v[3] = lt; break;
            case GL_RGB: rgb([&](u32 k) { return ct[k]; }); break;
            default: v = ct; break;
            }
            break;
        case GL_MODULATE:
            switch (c.base) {
            case GL_ALPHA: v[3] = cf[3] * at_; break;
            case GL_LUMINANCE: rgb([&](u32 k) { return cf[k] * lt; }); break;
            case GL_LUMINANCE_ALPHA: rgb([&](u32 k) { return cf[k] * lt; }); v[3] = cf[3] * at_; break;
            case GL_INTENSITY: rgb([&](u32 k) { return cf[k] * lt; }); v[3] = cf[3] * lt; break;
            case GL_RGB: rgb([&](u32 k) { return cf[k] * ct[k]; }); break;
            default: rgb([&](u32 k) { return cf[k] * ct[k]; }); v[3] = cf[3] * at_; break;
            }
            break;
        case GL_DECAL:
            if (c.base == GL_RGB) rgb([&](u32 k) { return ct[k]; });
            if (c.base == GL_RGBA) rgb([&](u32 k) { return cf[k] * (1 - at_) + ct[k] * at_; });
            break;
        case GL_BLEND:
            switch (c.base) {
            case GL_ALPHA: v[3] = cf[3] * at_; break;
            case GL_LUMINANCE: rgb([&](u32 k) { return cf[k] * (1 - lt) + cc[k] * lt; }); break;
            case GL_LUMINANCE_ALPHA:
                rgb([&](u32 k) { return cf[k] * (1 - lt) + cc[k] * lt; });
                v[3] = cf[3] * at_;
                break;
            case GL_INTENSITY:
                rgb([&](u32 k) { return cf[k] * (1 - lt) + cc[k] * lt; });
                v[3] = cf[3] * (1 - lt) + cc[3] * lt;
                break;
            case GL_RGB: rgb([&](u32 k) { return cf[k] * (1 - ct[k]) + cc[k] * ct[k]; }); break;
            default:
                rgb([&](u32 k) { return cf[k] * (1 - ct[k]) + cc[k] * ct[k]; });
                v[3] = cf[3] * at_;
                break;
            }
            break;
        }
        expect_color(at(px, c.x + 1, c.y + 1), v,
                     "base 0x" + std::to_string(c.base) + " mode 0x" + std::to_string(c.mode));
    }
}

// ---- Wrap modes and filters (3.8.1-3.8.2) ---------------------------------------------

/// The GL 1.1 sampling rules, independently: one level, (s, t) -> texel.
struct RefTexture {
    i32 w, h;
    std::vector<Color> texels;   // row 0 the bottom row
    Color border{0, 0, 0, 0};
    bool repeat_s = true, repeat_t = true;

    Color texel(i32 i, i32 j) const {
        if (i < 0 || j < 0 || i >= w || j >= h) return border;
        return texels[static_cast<usize>(j * w + i)];
    }
    static double wrap(double s, bool repeat) { return repeat ? s - std::floor(s) : std::clamp(s, 0.0, 1.0); }
    Color nearest(double s, double t) const {
        s = wrap(s, repeat_s);
        t = wrap(t, repeat_t);
        i32 i = s < 1 ? static_cast<i32>(std::floor(w * s)) : w - 1;
        i32 j = t < 1 ? static_cast<i32>(std::floor(h * t)) : h - 1;
        if (repeat_s) i = ((i % w) + w) % w;
        if (repeat_t) j = ((j % h) + h) % h;
        return texel(i, j);
    }
    Color linear(double s, double t) const {
        s = wrap(s, repeat_s);
        t = wrap(t, repeat_t);
        const double u = w * s - 0.5, v = h * t - 0.5;
        i32 i0 = static_cast<i32>(std::floor(u)), j0 = static_cast<i32>(std::floor(v));
        i32 i1 = i0 + 1, j1 = j0 + 1;
        const double a = u - std::floor(u), b = v - std::floor(v);
        if (repeat_s) {
            i0 = ((i0 % w) + w) % w;
            i1 = ((i1 % w) + w) % w;
        }
        if (repeat_t) {
            j0 = ((j0 % h) + h) % h;
            j1 = ((j1 % h) + h) % h;
        }
        Color out{};
        for (u32 k = 0; k < 4; ++k) {
            out[k] = (1 - a) * (1 - b) * texel(i0, j0)[k] + a * (1 - b) * texel(i1, j0)[k] +
                     (1 - a) * b * texel(i0, j1)[k] + a * b * texel(i1, j1)[k];
        }
        return out;
    }
};

std::vector<u8> pattern(i32 w, i32 h, RefTexture& ref) {
    std::vector<u8> rgba;
    ref.w = w;
    ref.h = h;
    ref.texels.clear();
    for (i32 j = 0; j < h; ++j) {
        for (i32 i = 0; i < w; ++i) {
            const std::array<u8, 4> c{static_cast<u8>(20 + i * 60), static_cast<u8>(30 + j * 50),
                                      static_cast<u8>(200 - i * 30 - j * 20), static_cast<u8>(255 - (i + j) * 15)};
            rgba.insert(rgba.end(), c.begin(), c.end());
            ref.texels.push_back({c8(c[0]), c8(c[1]), c8(c[2]), c8(c[3])});
        }
    }
    return rgba;
}

// REPEAT and CLAMP with NEAREST and LINEAR, magnified (rho <= 1). GL 1.1
// CLAMP clamps s to [0, 1]; LINEAR then blends the border colour into the
// edge texels (the 2x2 square reaches i = -1 or i = w), which is why the
// border colour shows at the edge (3.8.1). REPLACE shows the texel.
TEST_P(GlTextureMetalTest, WrapAndFilterParameters) {
    env(GL_REPLACE);
    RefTexture ref;
    cmd::BindTexture(ctx(), GL_TEXTURE_2D, 1);
    image(2, 0, GL_RGBA8, 4, 2, pattern(4, 2, ref));
    const Color border{c8(250), c8(10), c8(130), c8(60)};
    cmd::TexParameter(ctx(), GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR,
                      GlValues::vector(std::array<double, 4>{border[0], border[1], border[2], border[3]}, false));
    ref.border = border;
    struct Case {
        u32 filter, wrap_s, wrap_t;
        double s0, t0, s1, t1;
    };
    // 16x8-pixel quads; texture coordinates reach past [0, 1].
    const Case cases[] = {
        {GL_NEAREST, GL_REPEAT, GL_REPEAT, -0.75, -0.5, 2.25, 1.5},
        {GL_NEAREST, GL_CLAMP, GL_CLAMP, -0.5, -0.5, 1.5, 1.5},
        {GL_LINEAR, GL_REPEAT, GL_REPEAT, -0.25, 0.0, 1.25, 1.0},
        {GL_LINEAR, GL_CLAMP, GL_CLAMP, -0.25, -0.25, 1.25, 1.25},
        {GL_LINEAR, GL_CLAMP, GL_REPEAT, 0.0, 0.0, 1.0, 1.0},
    };
    for (const Case& c : cases) {
        filters(GL_NEAREST, c.filter);   // magnified: the mag filter decides
        if (c.filter == GL_LINEAR) filters(GL_LINEAR, GL_LINEAR);
        wrap(c.wrap_s, c.wrap_t);
        ref.repeat_s = c.wrap_s == GL_REPEAT;
        ref.repeat_t = c.wrap_t == GL_REPEAT;
        cmd::Clear(ctx(), GL_COLOR_BUFFER_BIT);
        quad(0, 0, 16, 8, c.s0, c.t0, c.s1, c.t1);
        ASSERT_EQ(error(), GL_NO_ERROR);
        const std::vector<Rgba> px = pixels();
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 16; ++x) {
                const double s = c.s0 + (x + 0.5) / 16.0 * (c.s1 - c.s0);
                const double t = c.t0 + (y + 0.5) / 8.0 * (c.t1 - c.t0);
                // Avoid texel boundaries for NEAREST (exact ties are a
                // matter of float rounding, not of the rule).
                if (c.filter == GL_NEAREST) {
                    const double u = RefTexture::wrap(s, ref.repeat_s) * 4, v = RefTexture::wrap(t, ref.repeat_t) * 2;
                    if (std::abs(u - std::round(u)) < 1e-3 || std::abs(v - std::round(v)) < 1e-3) continue;
                }
                const Color want = c.filter == GL_NEAREST ? ref.nearest(s, t) : ref.linear(s, t);
                expect_color(at(px, x, y), want,
                             "filter " + std::to_string(c.filter) + " wrap " + std::to_string(c.wrap_s) + "/" +
                                 std::to_string(c.wrap_t) + " at " + std::to_string(x) + "," + std::to_string(y));
            }
        }
    }
    // The border colour is interpreted to match the internal format (3.8.1,
    // Table 3.15): an INTENSITY texture's border is (R, R, R, R).
    cmd::BindTexture(ctx(), GL_TEXTURE_2D, 2);
    image(2, 0, GL_INTENSITY8, 1, 1, {255, 0, 0, 0});
    filters(GL_LINEAR, GL_LINEAR);
    wrap(GL_CLAMP, GL_CLAMP);
    cmd::TexParameter(ctx(), GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR,
                      GlValues::vector(std::array<double, 4>{0.2, 0.9, 0.9, 0.9}, false));
    cmd::Clear(ctx(), GL_COLOR_BUFFER_BIT);
    // s, t = 1/8 at pixel (0, 0) of a 4-pixel quad: u = 1/8 - 1/2 -> i0 = -1,
    // alpha = 0.625: 0.375 border + 0.625 texel in each direction.
    quad(0, 0, 4, 4, 0, 0, 1, 1);
    const std::vector<Rgba> px = pixels();
    const double w = 0.625 * 0.625;   // weight of the one real texel
    const double i = w * 1.0 + (1 - w) * 0.2;
    expect_color(at(px, 0, 0), {i, i, i, i}, "intensity border");
}

// Mipmap selection (3.8.2) by lambda = log2(rho), rho = the texels per pixel
// of level 0 (eq. 3.11), on an 8x8 mipmap whose levels are solid colours.
TEST_P(GlTextureMetalTest, MipmapFiltersSelectLevelsByLambda) {
    env(GL_REPLACE);
    const std::array<Color, 4> level_color{Color{c8(255), 0, 0, 1}, Color{0, c8(255), 0, 1}, Color{0, 0, c8(255), 1},
                                           Color{c8(255), c8(255), 0, 1}};
    cmd::BindTexture(ctx(), GL_TEXTURE_2D, 1);
    for (i32 level = 0; level < 4; ++level) {
        const i32 size = 8 >> level;
        std::vector<u8> rgba;
        for (i32 k = 0; k < size * size; ++k) {
            for (u32 c = 0; c < 4; ++c) rgba.push_back(static_cast<u8>(std::lround(level_color[level][c] * 255)));
        }
        image(2, level, GL_RGBA, size, size, rgba);
    }
    // A 16x16 quad spanning k repeats of the texture: rho = 8 k / 16.
    const auto draw_lambda = [&](double lambda) {
        const double k = std::exp2(lambda) * 2.0;
        cmd::Clear(ctx(), GL_COLOR_BUFFER_BIT);
        quad(0, 0, 16, 16, 0, 0, k, k);
        return at(pixels(), 7, 7);
    };
    const auto mix = [&](u32 a, u32 b, double f) {
        Color out{};
        for (u32 c = 0; c < 4; ++c) out[c] = (1 - f) * level_color[a][c] + f * level_color[b][c];
        return out;
    };
    struct Case {
        u32 min, mag;
        double lambda;
        Color want;
    };
    const Case cases[] = {
        // NEAREST_MIPMAP_NEAREST: d - 1/2 < lambda <= d + 1/2; level 0 up to 1/2.
        {GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST, 0.3, level_color[0]},
        {GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST, 1.3, level_color[1]},
        {GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST, 1.7, level_color[2]},
        {GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST, 4.5, level_color[3]},   // > p + 1/2: level p
        {GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR, 2.2, level_color[2]},
        // *_MIPMAP_LINEAR: levels floor(lambda) and +1 by frac(lambda).
        {GL_NEAREST_MIPMAP_LINEAR, GL_NEAREST, 1.25, mix(1, 2, 0.25)},
        {GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, 0.75, mix(0, 1, 0.75)},
        {GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, 3.5, level_color[3]},   // lambda >= p: level p
        // c = 1/2 for a LINEAR mag filter with NEAREST_MIPMAP_*: lambda =
        // 0.4 is magnification (level 0) ...
        {GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR, 0.4, level_color[0]},
        // ... while with a NEAREST mag filter c = 0 and it is minification.
        {GL_NEAREST_MIPMAP_LINEAR, GL_NEAREST, 0.4, mix(0, 1, 0.4)},
        // A non-mipmap min filter uses level 0 however far minified.
        {GL_LINEAR, GL_NEAREST, 2.5, level_color[0]},
    };
    for (const Case& c : cases) {
        filters(c.min, c.mag);
        expect_color(draw_lambda(c.lambda), c.want,
                     "min " + std::to_string(c.min) + " mag " + std::to_string(c.mag) + " lambda " +
                         std::to_string(c.lambda));
    }
    EXPECT_EQ(error(), GL_NO_ERROR);
}

// Spec 3.8.2 / 3.8.6: a mipmap filter on an incomplete set of arrays, or the
// null texture, is as if texturing were disabled: the fragment colour
// passes unchanged. With both enabled, 2D is used even when it is the
// incomplete one.
TEST_P(GlTextureMetalTest, MipmapCompletenessDisablesTexturing) {
    env(GL_MODULATE);
    const Color cf{c8(200), c8(100), c8(50), 1};
    const std::vector<u8> green4(4 * 4 * 4, 0);
    std::vector<u8> green = green4;
    for (usize k = 0; k < green.size(); k += 4) {
        green[k + 1] = 255;
        green[k + 3] = 255;
    }
    cmd::BindTexture(ctx(), GL_TEXTURE_2D, 1);
    image(2, 0, GL_RGBA, 4, 4, green);   // level 0 only; initial min filter NEAREST_MIPMAP_LINEAR
    const auto draw = [&]() {
        cmd::Clear(ctx(), GL_COLOR_BUFFER_BIT);
        color(cf);
        quad(0, 0, 8, 8, 0, 0, 1, 1);
        return at(pixels(), 3, 3);
    };
    const Color textured{0, cf[1], 0, 1};   // MODULATE: Cf * (0, 1, 0, 1)
    expect_color(draw(), cf, "incomplete mipmap");
    filters(GL_NEAREST, GL_NEAREST);
    expect_color(draw(), textured, "level 0 with a non-mipmap filter");
    filters(GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR);
    image(2, 1, GL_RGBA, 2, 2, std::vector<u8>(green.begin(), green.begin() + 16));
    expect_color(draw(), cf, "levels 0-1 of 0-2");
    image(2, 2, GL_RGBA, 1, 1, std::vector<u8>(green.begin(), green.begin() + 4));
    expect_color(draw(), textured, "complete");
    // A level of another internal format makes it incomplete again.
    image(2, 2, GL_RGBA8, 1, 1, std::vector<u8>(green.begin(), green.begin() + 4));
    expect_color(draw(), cf, "mixed internal formats");
    // 2D wins over 1D (3.8.6), even incomplete.
    cmd::Enable(ctx(), GL_TEXTURE_1D);
    cmd::BindTexture(ctx(), GL_TEXTURE_1D, 2);
    image(1, 0, GL_RGBA, 4, 1, std::vector<u8>(green.begin(), green.begin() + 16));
    filters(GL_NEAREST, GL_NEAREST, GL_TEXTURE_1D);
    expect_color(draw(), cf, "2D incomplete beside a complete 1D");
    cmd::Disable(ctx(), GL_TEXTURE_2D);
    expect_color(draw(), textured, "1D alone");
    // The null texture (zero width) at level 0.
    image(1, 0, GL_RGBA, 0, 1, {});
    expect_color(draw(), cf, "null texture");
    EXPECT_EQ(error(), GL_NO_ERROR);
}

// One-dimensional textures (3.8.1: height 1, t ignored): LINEAR with CLAMP
// blends the border at both ends; REPEAT wraps.
TEST_P(GlTextureMetalTest, OneDimensionalTexturesIgnoreT) {
    env(GL_REPLACE);
    cmd::Disable(ctx(), GL_TEXTURE_2D);
    cmd::Enable(ctx(), GL_TEXTURE_1D);
    cmd::BindTexture(ctx(), GL_TEXTURE_1D, 1);
    RefTexture ref;
    image(1, 0, GL_RGBA, 4, 1, pattern(4, 1, ref));
    filters(GL_LINEAR, GL_LINEAR, GL_TEXTURE_1D);
    param(GL_TEXTURE_WRAP_S, GL_CLAMP, GL_TEXTURE_1D);
    param(GL_TEXTURE_WRAP_T, GL_CLAMP, GL_TEXTURE_1D);   // irrelevant for 1D
    cmd::TexParameter(ctx(), GL_TEXTURE_1D, GL_TEXTURE_BORDER_COLOR,
                      GlValues::vector(std::array<double, 4>{1, 1, 1, 1}, false));
    ref.border = {1, 1, 1, 1};
    ref.repeat_s = false;
    cmd::Clear(ctx(), GL_COLOR_BUFFER_BIT);
    quad(0, 0, 16, 4, 0, -3, 1, 7);   // t far outside [0, 1]: ignored
    const std::vector<Rgba> px = pixels();
    for (u32 x = 0; x < 16; ++x) {
        const double s = (x + 0.5) / 16.0;
        // The 1D rule: (1 - alpha) tau_i0 + alpha tau_i1, j irrelevant.
        const double u = 4 * s - 0.5;
        const i32 i0 = static_cast<i32>(std::floor(u));
        const double a = u - std::floor(u);
        Color want{};
        for (u32 k = 0; k < 4; ++k) want[k] = (1 - a) * ref.texel(i0, 0)[k] + a * ref.texel(i0 + 1, 0)[k];
        for (u32 y = 0; y < 4; ++y) expect_color(at(px, x, y), want, "1D at " + std::to_string(x));
    }
}

// ---- Texture coordinates: generation, matrix, q --------------------------------------

/// GL 1.1 texgen (2.10.4) in double precision for the expectations.
struct RefTexgen {
    std::array<double, 4> s_plane{}, t_plane{};
    u32 mode = GL_OBJECT_LINEAR;
    GlMatrix modelview = gl_identity_matrix();

    static std::array<double, 4> mul(const GlMatrix& m, const std::array<double, 4>& p) {
        std::array<double, 4> r{};
        for (u32 row = 0; row < 4; ++row) {
            for (u32 k = 0; k < 4; ++k) r[row] += m[k * 4 + row] * p[k];
        }
        return r;
    }
    /// (s, t) of a vertex at object position @p obj with normal @p n.
    std::array<double, 2> st(const std::array<double, 4>& obj, const std::array<double, 3>& n) const {
        const auto dot = [](const std::array<double, 4>& a, const std::array<double, 4>& b) {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        };
        const std::array<double, 4> eye = mul(modelview, obj);
        if (mode == GL_OBJECT_LINEAR) return {dot(s_plane, obj), dot(t_plane, obj)};
        if (mode == GL_EYE_LINEAR) return {dot(s_plane, eye), dot(t_plane, eye)};
        // SPHERE_MAP: u = unit eye position, n' = n M^-1, r = u - 2 n'(n'.u).
        std::array<double, 3> u{eye[0] / eye[3], eye[1] / eye[3], eye[2] / eye[3]};
        const double ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        for (double& x : u) x /= ul;
        const auto inv = gl_invert(modelview);
        std::array<double, 3> np{};
        for (u32 c = 0; c < 3; ++c) np[c] = n[0] * (*inv)[c * 4 + 0] + n[1] * (*inv)[c * 4 + 1] + n[2] * (*inv)[c * 4 + 2];
        const double d = np[0] * u[0] + np[1] * u[1] + np[2] * u[2];
        const std::array<double, 3> r{u[0] - 2 * np[0] * d, u[1] - 2 * np[1] * d, u[2] - 2 * np[2] * d};
        const double m = 2 * std::sqrt(r[0] * r[0] + r[1] * r[1] + (r[2] + 1) * (r[2] + 1));
        return {r[0] / m + 0.5, r[1] / m + 0.5};
    }
};

// Generated coordinates (2.10.4) and the texture matrix (2.10.2), checked on
// both rasterization paths against one oracle: points (CPU-expanded, texgen
// evaluated on the CPU per vertex) and small filled triangles (texgen in the
// shader, interpolated, eq. 3.4). An 8x8 NEAREST texture whose texel (i, j)
// is unique shows which (s, t) each fragment got.
TEST_P(GlTextureMetalTest, TexgenAndTextureMatrixMatchOnCpuAndGpuPaths) {
    env(GL_REPLACE);
    RefTexture ref;
    cmd::BindTexture(ctx(), GL_TEXTURE_2D, 1);
    image(2, 0, GL_RGBA, 8, 8, pattern(8, 8, ref));
    filters(GL_NEAREST, GL_NEAREST);
    wrap(GL_REPEAT, GL_REPEAT);
    // A texture matrix: s' = 2 s + 0.125, t' = t - 0.25 (row-vector form).
    cmd::MatrixMode(ctx(), GL_TEXTURE);
    cmd::Translate(ctx(), 0.125, -0.25, 0);
    cmd::Scale(ctx(), 2, 1, 1);
    cmd::MatrixMode(ctx(), GL_MODELVIEW);
    const auto texmat = [](std::array<double, 2> st) { return std::array<double, 2>{2 * st[0] + 0.125, st[1] - 0.25}; };

    // Modelview at glTexGen time (EYE_PLANE is stored times its inverse) and
    // at draw time differ.
    GlMatrix m0 = gl_identity_matrix();
    m0[12] = 3;
    m0[13] = -1;
    GlMatrix m1 = gl_identity_matrix();
    m1[12] = 1.5;
    m1[13] = 2.0;

    struct Scenario {
        u32 mode;
        std::array<double, 4> s_plane, t_plane;
    };
    const Scenario scenarios[] = {
        {GL_OBJECT_LINEAR, {0.05, 0.01, 0, 0.1}, {-0.02, 0.04, 0, 0.3}},
        {GL_EYE_LINEAR, {0.03, 0.02, 0, 0.05}, {0.01, -0.05, 0, 0.7}},
        {GL_SPHERE_MAP, {}, {}},
    };
    for (const Scenario& sc : scenarios) {
        SCOPED_TRACE(sc.mode);
        cmd::LoadMatrix(ctx(), m0);
        for (u32 c = 0; c < 2; ++c) {
            const u32 coord = GL_S + c;
            cmd::TexGen(ctx(), coord, GL_TEXTURE_GEN_MODE, GlValues::scalar_value(sc.mode, true));
            const auto& p = c == 0 ? sc.s_plane : sc.t_plane;
            if (sc.mode != GL_SPHERE_MAP) {
                cmd::TexGen(ctx(), coord, sc.mode == GL_OBJECT_LINEAR ? GL_OBJECT_PLANE : GL_EYE_PLANE,
                            GlValues::vector(p, false));
            }
            cmd::Enable(ctx(), GL_TEXTURE_GEN_S + c);
        }
        cmd::LoadMatrix(ctx(), m1);
        RefTexgen gen;
        gen.mode = sc.mode;
        gen.modelview = m1;
        gen.s_plane = sc.s_plane;
        gen.t_plane = sc.t_plane;
        if (sc.mode == GL_EYE_LINEAR) {
            // p' = p M0^-1, as glTexGen stored it.
            const auto inv = *gl_invert(m0);
            for (auto* plane : {&gen.s_plane, &gen.t_plane}) {
                std::array<double, 4> r{};
                for (u32 col = 0; col < 4; ++col) {
                    for (u32 k = 0; k < 4; ++k) r[col] += (*plane)[k] * inv[col * 4 + k];
                }
                *plane = r;
            }
        }
        const auto normal_at = [](double x, double y) {
            const double nx = (x - 16) / 40.0, ny = (y - 16) / 40.0;
            return std::array<double, 3>{nx, ny, std::sqrt(1 - nx * nx - ny * ny)};
        };
        // Window position = object + (1.5, 2) under m1 and the ortho projection.
        u32 checked_points = 0, checked_triangles = 0;
        for (u32 py = 2; py < kH - 2; py += 5) {
            for (u32 px_ = 2; px_ < 16; px_ += 3) {
                const double ox = px_ + 0.5 - 1.5, oy = py + 0.5 - 2.0;
                const auto st = texmat(gen.st({ox, oy, 0, 1}, normal_at(ox, oy)));
                // The texel a coordinate selects, unless it is within 1/20
                // texel of a texel edge (float noise could tip NEAREST).
                const auto texel_of = [&](std::array<double, 2> c, Color& out) {
                    const double u = (c[0] - std::floor(c[0])) * 8, v = (c[1] - std::floor(c[1])) * 8;
                    if (std::abs(u - std::round(u)) < 0.05 || std::abs(v - std::round(v)) < 0.05) return false;
                    out = ref.nearest(c[0], c[1]);
                    return true;
                };
                // CPU path: a point at the pixel centre.
                Color want{};
                if (texel_of(st, want)) {
                    cmd::Clear(ctx(), GL_COLOR_BUFFER_BIT);
                    const auto n = normal_at(ox, oy);
                    cmd::Normal(ctx(), static_cast<float>(n[0]), static_cast<float>(n[1]), static_cast<float>(n[2]));
                    cmd::Begin(ctx(), GL_POINTS);
                    cmd::Vertex(ctx(), static_cast<float>(ox), static_cast<float>(oy), 0, 1);
                    cmd::End(ctx());
                    expect_color(at(pixels(), px_, py), want, "point " + std::to_string(px_) + "," + std::to_string(py));
                    ++checked_points;
                }
                // GPU path: a triangle around the pixel centre; the shader
                // generates at its vertices and the result is interpolated
                // (barycentric, w = 1) to the centre.
                const std::array<std::array<double, 2>, 3> tri{
                    {{ox - 1.0, oy - 1.0}, {ox + 1.5, oy - 0.5}, {ox - 0.25, oy + 1.25}}};
                std::array<double, 2> interp{};
                {
                    const auto area = [](const std::array<double, 2>& a, const std::array<double, 2>& b,
                                         const std::array<double, 2>& c) {
                        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
                    };
                    const std::array<double, 2> p{ox, oy};
                    const double total = area(tri[0], tri[1], tri[2]);
                    const double l0 = area(p, tri[1], tri[2]) / total, l1 = area(tri[0], p, tri[2]) / total;
                    const double l2 = 1 - l0 - l1;
                    for (u32 k = 0; k < 3; ++k) {
                        const auto g = texmat(gen.st({tri[k][0], tri[k][1], 0, 1}, normal_at(tri[k][0], tri[k][1])));
                        const double l = k == 0 ? l0 : k == 1 ? l1 : l2;
                        interp[0] += l * g[0];
                        interp[1] += l * g[1];
                    }
                }
                if (texel_of(interp, want)) {
                    cmd::Clear(ctx(), GL_COLOR_BUFFER_BIT);
                    cmd::Begin(ctx(), GL_TRIANGLES);
                    for (const auto& v : tri) {
                        const auto n = normal_at(v[0], v[1]);
                        cmd::Normal(ctx(), static_cast<float>(n[0]), static_cast<float>(n[1]), static_cast<float>(n[2]));
                        cmd::Vertex(ctx(), static_cast<float>(v[0]), static_cast<float>(v[1]), 0, 1);
                    }
                    cmd::End(ctx());
                    expect_color(at(pixels(), px_, py), want,
                                 "triangle " + std::to_string(px_) + "," + std::to_string(py));
                    ++checked_triangles;
                }
            }
        }
        EXPECT_GE(checked_points, 10u);
        EXPECT_GE(checked_triangles, 10u);
        cmd::Disable(ctx(), GL_TEXTURE_GEN_S);
        cmd::Disable(ctx(), GL_TEXTURE_GEN_T);
    }
    EXPECT_EQ(error(), GL_NO_ERROR);
}

// Texture coordinates are divided by q per fragment (3.8: s/q, t/q, after
// perspective-correct interpolation of s, t, q, here with w = 1, so linear).
TEST_P(GlTextureMetalTest, ProjectiveCoordinatesDivideByQPerFragment) {
    env(GL_REPLACE);
    RefTexture ref;
    cmd::BindTexture(ctx(), GL_TEXTURE_2D, 1);
    image(2, 0, GL_RGBA, 8, 1, pattern(8, 1, ref));
    filters(GL_NEAREST, GL_NEAREST);
    // s = 0 -> 2 and q = 1 -> 2 across 16 pixels: s/q runs 0 -> 1 non-linearly.
    cmd::Begin(ctx(), GL_QUADS);
    cmd::TexCoord(ctx(), 0, 0.5f, 0, 1);
    cmd::Vertex(ctx(), 0, 0, 0, 1);
    cmd::TexCoord(ctx(), 2, 1.0f, 0, 2);
    cmd::Vertex(ctx(), 16, 0, 0, 1);
    cmd::TexCoord(ctx(), 2, 1.0f, 0, 2);
    cmd::Vertex(ctx(), 16, 2, 0, 1);
    cmd::TexCoord(ctx(), 0, 0.5f, 0, 1);
    cmd::Vertex(ctx(), 0, 2, 0, 1);
    cmd::End(ctx());
    const std::vector<Rgba> px = pixels();
    u32 checked = 0;
    for (u32 x = 0; x < 16; ++x) {
        const double f = (x + 0.5) / 16.0;
        const double s = 2 * f / (1 + f);
        if (std::abs(s * 8 - std::round(s * 8)) < 0.05) continue;
        expect_color(at(px, x, 0), ref.nearest(s, 0.5), "x " + std::to_string(x));
        ++checked;
    }
    EXPECT_GE(checked, 12u);
}

// ---- Framebuffer copies and ordering ------------------------------------------------------

// glCopyTexImage2D / glCopyTexSubImage2D read the read buffer (3.8.2) as
// CopyPixels would, through pixel transfer; glGetTexImage returns them.
TEST_P(GlTextureMetalTest, CopyTexImageReadsTheReadBuffer) {
    cmd::Disable(ctx(), GL_TEXTURE_2D);
    // Back: a red rect at (2..6, 1..3), green at (6..10, 1..3); front: blue.
    color({1, 0, 0, 1});
    cmd::Rect(ctx(), 2, 1, 6, 3);
    color({0, 1, 0, 0.5});
    cmd::Rect(ctx(), 6, 1, 10, 3);
    cmd::DrawBuffer(ctx(), GL_FRONT);
    color({0, 0, 1, 1});
    cmd::Rect(ctx(), 0, 0, kW, kH);
    cmd::DrawBuffer(ctx(), GL_BACK);
    cmd::ReadBuffer(ctx(), GL_BACK);
    cmd::BindTexture(ctx(), GL_TEXTURE_2D, 1);
    // 8x2 at window (2, 1); the alpha scale is a pixel transfer operation.
    cmd::PixelTransfer(ctx(), GL_ALPHA_SCALE, 0.5, false);
    cmd::CopyTexImage(ctx(), 2, GL_TEXTURE_2D, 0, GL_RGBA8, 2, 1, 8, 2, 0);
    cmd::PixelTransfer(ctx(), GL_ALPHA_SCALE, 1.0, false);
    ASSERT_EQ(error(), GL_NO_ERROR);
    std::vector<u8> back;
    const auto get = [&]() {
        back.clear();
        return cmd::GetTexImage(
            ctx(), GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE,
            [](void* user, usize size) -> std::span<u8> {
                auto& b = *static_cast<std::vector<u8>*>(user);
                b.assign(size, 0);
                return b;
            },
            &back);
    };
    ASSERT_TRUE(get());
    ASSERT_EQ(back.size(), 8u * 2u * 4u);
    for (u32 j = 0; j < 2; ++j) {
        for (u32 i = 0; i < 8; ++i) {
            const u8* p = &back[(j * 8 + i) * 4];
            const bool red = i < 4;
            EXPECT_EQ(p[0], red ? 255 : 0) << i << "," << j;
            EXPECT_EQ(p[1], red ? 0 : 255) << i << "," << j;
            EXPECT_EQ(p[2], 0) << i << "," << j;
            EXPECT_NEAR(p[3], red ? 128 : 64, 1) << i << "," << j;   // A * 0.5
        }
    }
    // From the front buffer, into a sub-rectangle.
    cmd::ReadBuffer(ctx(), GL_FRONT);
    cmd::CopyTexSubImage(ctx(), 2, GL_TEXTURE_2D, 0, 6, 1, 0, 0, 2, 1);
    ASSERT_EQ(error(), GL_NO_ERROR);
    ASSERT_TRUE(get());
    for (u32 i = 0; i < 8; ++i) {
        const u8* p = &back[(8 + i) * 4];
        if (i >= 6) {
            EXPECT_EQ(p[2], 255) << i;
            EXPECT_EQ(p[0], 0) << i;
        } else {
            EXPECT_EQ(p[0], i < 4 ? 255 : 0) << i;
        }
    }
    // The copy draws like any image.
    cmd::Enable(ctx(), GL_TEXTURE_2D);
    filters(GL_NEAREST, GL_NEAREST);
    env(GL_REPLACE);
    cmd::Clear(ctx(), GL_COLOR_BUFFER_BIT);
    quad(16, 16, 24, 18, 0, 0, 1, 1);
    const std::vector<Rgba> px = pixels();
    expect_color(at(px, 16, 16), {1, 0, 0, 0.5}, "copied texel (0, 0)");
    expect_color(at(px, 23, 17), {0, 0, 1, 1}, "copied texel (7, 1)");
    // Errors: a proxy target, internal format 3, a bad size.
    cmd::CopyTexImage(ctx(), 2, GL_PROXY_TEXTURE_2D, 0, GL_RGBA, 0, 0, 4, 4, 0);
    EXPECT_EQ(error(), GL_INVALID_ENUM);
    cmd::CopyTexImage(ctx(), 2, GL_TEXTURE_2D, 0, 3, 0, 0, 4, 4, 0);
    EXPECT_EQ(error(), GL_INVALID_ENUM);
    cmd::CopyTexImage(ctx(), 2, GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 5, 4, 0);
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    cmd::CopyTexSubImage(ctx(), 2, GL_TEXTURE_2D, 0, 7, 0, 0, 0, 2, 1);
    EXPECT_EQ(error(), GL_INVALID_VALUE);
    cmd::BindTexture(ctx(), GL_TEXTURE_2D, 2);
    cmd::CopyTexSubImage(ctx(), 2, GL_TEXTURE_2D, 0, 0, 0, 0, 0, 1, 1);
    EXPECT_EQ(error(), GL_INVALID_VALUE);   // level 0 never specified: copytexsubimage2d-undefined-level
}

// Spec 2.5: commands take effect in order. A draw issued before an image
// change samples the old texels, even though both draws are still pending
// in the same pass when the image changes (the change goes to new backend
// storage; the pass is not ended). Residency follows the draw.
TEST_P(GlTextureMetalTest, DrawsBeforeAnImageChangeKeepTheirTexels) {
    env(GL_REPLACE);
    filters(GL_NEAREST, GL_NEAREST);
    cmd::BindTexture(ctx(), GL_TEXTURE_2D, 1);
    filters(GL_NEAREST, GL_NEAREST);
    image(2, 0, GL_RGBA, 1, 1, {255, 0, 0, 255});
    (void)pixels();
    const u32 passes = drawable_->passes_opened();
    quad(0, 0, 4, 4, 0, 0, 1, 1);
    GlClientPixels green;
    const std::vector<u8> g{0, 255, 0, 255};
    green.bytes = g;
    cmd::TexSubImage(ctx(), 2, GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, green);
    quad(4, 0, 8, 4, 0, 0, 1, 1);
    cmd::TexImage(ctx(), 2, GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, [&] {
        GlClientPixels blue;
        static const std::vector<u8> b{0, 0, 255, 255};
        blue.bytes = b;
        return blue;
    }());
    quad(8, 0, 12, 4, 0, 0, 1, 1);
    ASSERT_EQ(error(), GL_NO_ERROR);
    const std::vector<Rgba> px = pixels();
    EXPECT_EQ(drawable_->passes_opened() - passes, 1u);   // one pass throughout
    expect_color(at(px, 1, 1), {1, 0, 0, 1}, "before the sub-image");
    expect_color(at(px, 5, 1), {0, 1, 0, 1}, "between");
    expect_color(at(px, 9, 1), {0, 0, 1, 1}, "after the new image");
    std::vector<u8> residences;
    const std::array<u32, 1> one{1};
    EXPECT_TRUE(cmd::AreTexturesResident(ctx(), 1, one, residences).all_resident);
    GlQuery q;
    ASSERT_TRUE(cmd::GetTexParameter(ctx(), GL_TEXTURE_2D, GL_TEXTURE_RESIDENT, q));
    EXPECT_EQ(q.v[0], 1.0);
}

// Flat shading takes the provoking vertex's colour only (2.13.7): texture
// coordinates of a flat-shaded line are still interpolated (eq. 3.1).
TEST_P(GlTextureMetalTest, FlatShadedLinesInterpolateTextureCoordinates) {
    env(GL_REPLACE);
    RefTexture ref;
    cmd::BindTexture(ctx(), GL_TEXTURE_2D, 1);
    image(2, 0, GL_RGBA, 8, 1, pattern(8, 1, ref));
    filters(GL_NEAREST, GL_NEAREST);
    cmd::ShadeModel(ctx(), GL_FLAT);
    cmd::Begin(ctx(), GL_LINES);
    cmd::TexCoord(ctx(), 0, 0.5f, 0, 1);
    cmd::Vertex(ctx(), 0, 0.5f, 0, 1);
    cmd::TexCoord(ctx(), 1, 0.5f, 0, 1);
    cmd::Vertex(ctx(), 16, 0.5f, 0, 1);
    cmd::End(ctx());
    ASSERT_EQ(error(), GL_NO_ERROR);
    const std::vector<Rgba> px = pixels();
    // Fragment x: s = (x + 1/2) / 16, texel floor(8 s) = floor((x + 1/2) / 2).
    for (u32 x = 1; x < 15; ++x) {
        expect_color(at(px, x, 0), ref.nearest((x + 0.5) / 16.0, 0.5), "x " + std::to_string(x));
    }
}

/// The back buffer of @p d, row 0 = the bottom row.
std::vector<Rgba> read_back(GlDrawable& d) {
    std::vector<u8> bgra;
    EXPECT_TRUE(d.read_color(GL_BACK, bgra));
    std::vector<Rgba> out(kW * kH);
    for (u32 gy = 0; gy < kH; ++gy) {
        for (u32 x = 0; x < kW; ++x) {
            const u8* p = &bgra[(static_cast<usize>(kH - 1 - gy) * kW + x) * 4u];
            out[gy * kW + x] = Rgba{p[2], p[1], p[0], p[3]};
        }
    }
    return out;
}

// Two contexts sharing one texture, current on two threads with their own
// windows, draw it at the same time: recording a draw resolves the backend
// texture's shader view, which the backend memoises without a lock of its
// own (GlTextureStorage::record_mutex serialises it). Every flush records
// the draws, so both threads record the same texture concurrently.
TEST_P(GlTextureMetalTest, SharedTextureDrawsConcurrentlyOnTwoThreads) {
    const u64 rc2 = main_->create(kDc2);
    ASSERT_NE(rc2, 0u);
    ASSERT_EQ(main_->call("wglShareLists", {rc_, rc2}), 1u);
    cmd::BindTexture(ctx(), GL_TEXTURE_2D, 4);
    filters(GL_NEAREST, GL_NEAREST);
    env(GL_REPLACE);
    const std::vector<u8> texels{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
    image(2, 0, GL_RGBA, 2, 2, texels);
    const auto expect_texture = [&](const std::vector<Rgba>& px, const char* who) {
        for (u32 y = 0; y < kH; y += 5) {
            for (u32 x = 0; x < kW; x += 5) {
                const u8* t = &texels[((y / 16) * 2 + x / 16) * 4];
                expect_color(at(px, x, y), {c8(t[0]), c8(t[1]), c8(t[2]), c8(t[3])},
                             std::string(who) + " at " + std::to_string(x) + "," + std::to_string(y));
            }
        }
    };
    constexpr u32 kRounds = 300;
    std::atomic<u32> ready{0};
    const auto draw_rounds = [&](GlContext& c) {
        ready.fetch_add(1);
        while (ready.load() < 2) {}
        for (u32 k = 0; k < kRounds; ++k) {
            cmd::Begin(c, GL_QUADS);
            cmd::TexCoord(c, 0, 0, 0, 1);
            cmd::Vertex(c, 0, 0, 0, 1);
            cmd::TexCoord(c, 1, 0, 0, 1);
            cmd::Vertex(c, kW, 0, 0, 1);
            cmd::TexCoord(c, 1, 1, 0, 1);
            cmd::Vertex(c, kW, kH, 0, 1);
            cmd::TexCoord(c, 0, 1, 0, 1);
            cmd::Vertex(c, 0, kH, 0, 1);
            cmd::End(c);
            cmd::Flush(c);   // records the pending draw: the texture's view is resolved now
        }
    };
    std::vector<Rgba> second;
    u32 second_error = 0;
    std::thread other([&] {
        Caller b(memory_, *registry_, GetParam(), 1);
        if (b.make(kDc2, rc2) != 1u) {
            ready.fetch_add(1);
            ADD_FAILURE() << "second context not made current";
            return;
        }
        GlContext& c = *mgr_->context_for_testing(rc2);
        cmd::MatrixMode(c, GL_PROJECTION);
        cmd::Ortho(c, 0, kW, 0, kH, -1, 1);
        cmd::MatrixMode(c, GL_MODELVIEW);
        cmd::Enable(c, GL_TEXTURE_2D);
        cmd::TexEnv(c, GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GlValues::scalar_value(GL_REPLACE, true));
        cmd::BindTexture(c, GL_TEXTURE_2D, 4);
        draw_rounds(c);
        second_error = c.take_error();
        second = read_back(*c.drawable);
        b.make(0, 0);
    });
    draw_rounds(ctx());
    other.join();
    EXPECT_EQ(error(), GL_NO_ERROR);
    EXPECT_EQ(second_error, GL_NO_ERROR);
    expect_texture(pixels(), "first thread");
    ASSERT_EQ(second.size(), kW * kH);
    expect_texture(second, "second thread");
    EXPECT_EQ(main_->call("wglDeleteContext", {rc2}), 1u);
}

// The rename that keeps earlier draws' texels (DrawsBeforeAnImageChange...)
// is bounded: a 1-texel change of a 4 MiB texture between draws, 64 times in
// one frame, copies the levels forward on the GPU and uploads one texel per
// change, and once the share group holds kGlTextureRenameBudget bytes of
// renamed storage the next change ends the passes that captured it and
// updates in place instead. Every draw still shows the texel it was issued
// with.
TEST_P(GlTextureMetalTest, RenamesWithinAPassAreBounded) {
    env(GL_REPLACE);
    cmd::BindTexture(ctx(), GL_TEXTURE_2D, 1);
    filters(GL_NEAREST, GL_NEAREST);
    constexpr i32 kSize = 1024;
    constexpr usize kBytes = static_cast<usize>(kSize) * kSize * 4;
    image(2, 0, GL_RGBA8, kSize, kSize, std::vector<u8>(kBytes, 0));
    // A first draw creates the storage (one full upload) and captures it.
    quad(kW - 1, kH - 1, kW, kH, 0.5 / kSize, 0.5 / kSize, 0.5 / kSize, 0.5 / kSize);
    const GlTextureStats before = gl_texture_stats();
    constexpr u32 kUpdates = 64;
    const auto colour = [](u32 k) {
        return std::array<u8, 4>{static_cast<u8>(k * 4), static_cast<u8>(255 - k * 4), static_cast<u8>((k % 2) * 255),
                                 255};
    };
    for (u32 k = 0; k < kUpdates; ++k) {
        const std::array<u8, 4> c = colour(k);
        const std::vector<u8> texel(c.begin(), c.end());
        GlClientPixels px;
        px.bytes = texel;
        cmd::TexSubImage(ctx(), 2, GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        // A one-pixel quad sampling texel (0, 0).
        const double s = 0.5 / kSize;
        quad(k % kW, k / kW, k % kW + 1, k / kW + 1, s, s, s, s);
    }
    ASSERT_EQ(error(), GL_NO_ERROR);
    const std::vector<Rgba> px = pixels();
    for (u32 k = 0; k < kUpdates; ++k) {
        const std::array<u8, 4> c = colour(k);
        expect_color(at(px, k % kW, k / kW), {c8(c[0]), c8(c[1]), c8(c[2]), c8(c[3])}, "draw " + std::to_string(k));
    }
    const GlTextureStats after = gl_texture_stats();
    const u64 renames = after.renames - before.renames;
    const u64 flushes = after.budget_flushes - before.budget_flushes;
    const u64 per_budget = kGlTextureRenameBudget / kBytes;
    // Every change follows a draw that captured the storage: it renames or flushes.
    EXPECT_EQ(renames + flushes, kUpdates);
    EXPECT_GE(flushes, kUpdates / (per_budget + 1));
    // Never more than a budget's worth of renames between flushes.
    EXPECT_LE(renames, (flushes + 1) * per_budget);
    EXPECT_EQ(after.storages_created - before.storages_created, renames);
    // One texel uploaded per change; no full-level uploads.
    EXPECT_EQ(after.bytes_uploaded - before.bytes_uploaded, kUpdates * 4u);
}

// Texture objects shared by wglShareLists draw in either context.
TEST_P(GlTextureMetalTest, SharedTextureDrawsInTheSecondContext) {
    const u64 rc2 = main_->create(kDc);
    ASSERT_NE(rc2, 0u);
    ASSERT_EQ(main_->call("wglShareLists", {rc_, rc2}), 1u);
    cmd::BindTexture(ctx(), GL_TEXTURE_2D, 4);
    filters(GL_NEAREST, GL_NEAREST);
    image(2, 0, GL_RGBA, 1, 1, {10, 200, 30, 255});
    ASSERT_EQ(main_->make(kDc, rc2), 1u);
    GlContext& b = *mgr_->context_for_testing(rc2);
    cmd::MatrixMode(b, GL_PROJECTION);
    cmd::Ortho(b, 0, kW, 0, kH, -1, 1);
    cmd::MatrixMode(b, GL_MODELVIEW);
    cmd::Enable(b, GL_TEXTURE_2D);
    cmd::TexEnv(b, GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GlValues::scalar_value(GL_REPLACE, true));
    cmd::BindTexture(b, GL_TEXTURE_2D, 4);
    cmd::Begin(b, GL_QUADS);
    cmd::TexCoord(b, 0.5f, 0.5f, 0, 1);
    cmd::Vertex(b, 0, 0, 0, 1);
    cmd::Vertex(b, 4, 0, 0, 1);
    cmd::Vertex(b, 4, 4, 0, 1);
    cmd::Vertex(b, 0, 4, 0, 1);
    cmd::End(b);
    EXPECT_EQ(b.take_error(), GL_NO_ERROR);
    expect_color(at(pixels(), 1, 1), {c8(10), c8(200), c8(30), 1}, "shared texture");
    ASSERT_EQ(main_->make(kDc, rc_), 1u);
    EXPECT_EQ(main_->call("wglDeleteContext", {rc2}), 1u);
}

std::string bitness_name(const ::testing::TestParamInfo<bool>& info) { return info.param ? "x64" : "x86"; }
INSTANTIATE_TEST_SUITE_P(Bitness, GlTextureMetalTest, ::testing::Values(false, true), bitness_name);

}  // namespace
