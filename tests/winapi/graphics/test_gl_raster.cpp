/// @file test_gl_raster.cpp
/// @brief OpenGL 1.1 rasterization and per-fragment operations on the real
///        Metal backend (Task 9): blending, depth, stencil, culling and front
///        face, polygon modes, polygon offset, wide aliased lines, points,
///        line stipple, alpha test, fog, scissor, colour mask, polygon
///        stipple, logic op on two buffers, user clip planes, and the GL
///        lighting / material / light-model mapping.
///
/// Every expected value is computed here from the spec, never from Vitreo:
/// - point sampling of polygons (3.5.1) with edge functions at (x + .5,
///   y + .5);
/// - the diamond-exit rule (3.4.1) evaluated per fragment by brute force,
///   the wide-line column rule and the stipple counter (3.4.2);
/// - the aliased point square (3.3);
/// - Tables 4.1 / 4.2 blending, 4.1.4 stencil, 4.1.5 depth, 4.1.3 alpha;
/// - eq. 3.16-3.18 fog with z the eye distance; the 2.13.1 lighting sum;
/// - 3.5.5's o = m * factor + r * units with r of the actual depth format.
/// Colours are 8-bit values; computed colours compare within 1.5 / 255.
/// Window coordinates are GL's: (0, 0) is the bottom-left pixel. With
/// glOrtho(0, w, 0, h, -1, 1) and the initial viewport, object x/y are
/// window x/y and window z = (1 - z_object) / 2.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <vector>

#include <graphics/contracts/backend_device.hpp>
#include <graphics/metal/metal_device.hpp>

#include "winapi/graphics/opengl32/gl_backend_draw.hpp"
#include "winapi/graphics/opengl32/gl_commands.hpp"
#include "winapi/graphics/opengl32/gl_context.hpp"
#include "winapi/graphics/opengl32/gl_drawable.hpp"
#include "winapi/graphics/opengl32/gl_enums.hpp"
#include "winapi/graphics/opengl32/gl_raster_state.hpp"
#include "winapi/graphics/opengl32/opengl32.hpp"
#include "gl_test_caller.hpp"

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::gl;
using test::Caller;

namespace {

constexpr i32 kFormatD24S8 = 13;   // double, alpha 8, depth 24 (D32Float_S8Uint), stencil 8
constexpr i32 kFormatD16 = 15;     // double, alpha 8, depth 16 (D16Unorm), no stencil
constexpr i32 kFormatNoAlpha = 5;  // double, no alpha, depth 24, stencil 8
constexpr u64 kDc = 0x6000, kDc16 = 0x6100, kDcNoAlpha = 0x6200;
constexpr u64 kWindow = 0x200, kWindow16 = 0x300, kWindowNoAlpha = 0x400;
constexpr i32 kW = 32, kH = 24;

struct Rgba {
    u8 r, g, b, a;
    bool operator==(const Rgba&) const = default;
};
std::ostream& operator<<(std::ostream& os, const Rgba& c) {
    return os << "(" << int(c.r) << "," << int(c.g) << "," << int(c.b) << "," << int(c.a) << ")";
}
constexpr Rgba kBlack{0, 0, 0, 255};
constexpr Rgba kRed{255, 0, 0, 255};
constexpr Rgba kGreen{0, 255, 0, 255};
constexpr Rgba kBlue{0, 0, 255, 255};

using Pixel = std::pair<i32, i32>;
using PixelSet = std::set<Pixel>;

// ---- Spec oracles ---------------------------------------------------------------

/// Spec 3.4.1, by brute force: fragment f is produced when the segment
/// a -> b intersects R_f = {|x - x_f| + |y - y_f| < 1/2} and b is not in R_f.
/// The L1 distance from the centre along the segment is convex and piecewise
/// linear, so its minimum is at an end or where x = x_f or y = y_f.
std::vector<Pixel> diamond_exit(double xa, double ya, double xb, double yb) {
    std::vector<Pixel> out;
    const auto l1 = [](double x, double y, double xf, double yf) { return std::abs(x - xf) + std::abs(y - yf); };
    const i32 x_lo = static_cast<i32>(std::floor(std::min(xa, xb))) - 2, x_hi = static_cast<i32>(std::ceil(std::max(xa, xb))) + 2;
    const i32 y_lo = static_cast<i32>(std::floor(std::min(ya, yb))) - 2, y_hi = static_cast<i32>(std::ceil(std::max(ya, yb))) + 2;
    for (i32 y = y_lo; y <= y_hi; ++y) {
        for (i32 x = x_lo; x <= x_hi; ++x) {
            const double xf = x + 0.5, yf = y + 0.5;
            if (l1(xb, yb, xf, yf) < 0.5) continue;
            std::vector<double> ts{0.0, 1.0};
            if (xb != xa) ts.push_back((xf - xa) / (xb - xa));
            if (yb != ya) ts.push_back((yf - ya) / (yb - ya));
            double best = 1e9;
            for (double t : ts) {
                if (t < 0.0 || t > 1.0) continue;
                best = std::min(best, l1(xa + t * (xb - xa), ya + t * (yb - ya), xf, yf));
            }
            if (best < 0.5) out.push_back({x, y});
        }
    }
    return out;
}

/// Spec 3.4.2: a width-w aliased segment: the segment offset by -(w-1)/2 in
/// the minor direction, each of its fragments replicated w times in +minor.
/// Fragments are listed in rasterization order from the start (for the
/// stipple counter), each column's w fragments together.
std::vector<std::vector<Pixel>> wide_line(double xa, double ya, double xb, double yb, float width) {
    i32 w = static_cast<i32>(std::lround(width));
    if (w < 1) w = 1;
    const bool x_major = std::abs(xb - xa) >= std::abs(yb - ya);
    const double shift = (w - 1) / 2.0;
    std::vector<Pixel> thin = x_major ? diamond_exit(xa, ya - shift, xb, yb - shift)
                                      : diamond_exit(xa - shift, ya, xb - shift, yb);
    // Order from the start along the major axis.
    const bool forward = x_major ? xb > xa : yb > ya;
    std::sort(thin.begin(), thin.end(), [&](const Pixel& p, const Pixel& q) {
        const i32 a = x_major ? p.first : p.second, b = x_major ? q.first : q.second;
        return forward ? a < b : a > b;
    });
    std::vector<std::vector<Pixel>> columns;
    for (const Pixel& p : thin) {
        std::vector<Pixel> column;
        for (i32 k = 0; k < w; ++k) column.push_back(x_major ? Pixel{p.first, p.second + k} : Pixel{p.first + k, p.second});
        columns.push_back(column);
    }
    return columns;
}

/// Spec 3.3: the aliased point square.
PixelSet point_square(double xw, double yw, float size) {
    i32 w = static_cast<i32>(std::lround(size));
    if (w < 1) w = 1;
    const double cx = (w % 2) ? std::floor(xw) + 0.5 : std::floor(xw + 0.5);
    const double cy = (w % 2) ? std::floor(yw) + 0.5 : std::floor(yw + 0.5);
    PixelSet out;
    for (i32 j = 0; j < w; ++j) {
        for (i32 i = 0; i < w; ++i) {
            out.insert({static_cast<i32>(std::floor(cx - w / 2.0 + i)), static_cast<i32>(std::floor(cy - w / 2.0 + j))});
        }
    }
    return out;
}

/// Spec 3.5.1 point sampling of a convex polygon (either winding).
PixelSet polygon_cover(const std::vector<std::array<double, 2>>& p) {
    PixelSet out;
    for (i32 y = 0; y < kH; ++y) {
        for (i32 x = 0; x < kW; ++x) {
            const double px = x + 0.5, py = y + 0.5;
            bool pos = true, neg = true;
            for (usize i = 0; i < p.size(); ++i) {
                const auto& a = p[i];
                const auto& b = p[(i + 1) % p.size()];
                const double e = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
                // Keep test geometry away from ties the backend's sub-pixel
                // snapping (1/256) may resolve either way.
                const double length = std::hypot(b[0] - a[0], b[1] - a[1]);
                const double t = ((px - a[0]) * (b[0] - a[0]) + (py - a[1]) * (b[1] - a[1])) / (length * length);
                if (t >= 0 && t <= 1 && std::abs(e) / length < 1.0 / 64.0) ADD_FAILURE() << "pixel centre " << px << "," << py << " is on an edge";
                pos = pos && e > 0;
                neg = neg && e < 0;
            }
            if (pos || neg) out.insert({x, y});
        }
    }
    return out;
}

PixelSet in_window(const PixelSet& s) {
    PixelSet out;
    for (const Pixel& p : s) {
        if (p.first >= 0 && p.first < kW && p.second >= 0 && p.second < kH) out.insert(p);
    }
    return out;
}

u8 to8(double c) { return static_cast<u8>(std::lround(std::clamp(c, 0.0, 1.0) * 255.0)); }

// ---- Fixture -------------------------------------------------------------------------

class GlRasterTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        if (!graphics::MetalDevice::instance().available()) GTEST_SKIP() << "no Metal device";
        graphics::set_active_backend_device(&graphics::MetalDevice::instance());
        ASSERT_TRUE(memory_.map_region(Caller::kStackBase, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(OPENGL32_SCRATCH_BASE, OPENGL32_SCRATCH_SIZE,
                                       MemoryProtection::ReadWrite).is_ok());
        registry_ = std::make_unique<ThunkRegistry>(0x70000000);
        mgr_ = std::make_unique<Opengl32Manager>();
        mgr_->set_thunk_registry(registry_.get());
        ASSERT_TRUE(registry_->register_dll("opengl32.dll", create_opengl32_thunks(*mgr_)).is_ok());
        mgr_->set_pixel_format_query([](u64 dc) { return dc == kDc ? kFormatD24S8 : dc == kDc16 ? kFormatD16 : dc == kDcNoAlpha ? kFormatNoAlpha : 0; });
        mgr_->set_drawable_queries(
            [](u64 dc) -> u64 { return dc == kDc ? kWindow : dc == kDc16 ? kWindow16 : dc == kDcNoAlpha ? kWindowNoAlpha : 0; },
            [](u64 dc, i32& w, i32& h) {
                if (dc != kDc && dc != kDc16 && dc != kDcNoAlpha) return false;
                w = kW;
                h = kH;
                return true;
            });
        mgr_->set_swap_chain_factory([](u64, u32, u32) { return std::unique_ptr<graphics::BackendSwapChain>(); });
        main_ = std::make_unique<Caller>(memory_, *registry_, GetParam(), 0);
        use(kDc, kWindow);
    }

    /// Makes a context current on @p dc and sets the window-coordinate
    /// projection.
    void use(u64 dc, u64 window) {
        const u64 rc = main_->create(dc);
        ASSERT_NE(rc, 0u);
        ASSERT_EQ(main_->make(dc, rc), 1u);
        ctx_ = mgr_->context_for_testing(rc);
        ASSERT_NE(ctx_, nullptr);
        drawable_ = GlDrawable::for_window(window);
        ASSERT_NE(drawable_, nullptr);
        cmd::MatrixMode(*ctx_, GL_PROJECTION);
        cmd::LoadIdentity(*ctx_);
        cmd::Ortho(*ctx_, 0, kW, 0, kH, -1, 1);
        cmd::MatrixMode(*ctx_, GL_MODELVIEW);
        cmd::ClearColor(*ctx_, 0, 0, 0, 1);
        clear();
    }

    void TearDown() override {
        drawable_.reset();
        if (main_) main_->make(0, 0);
        set_current_gl_context_for_thread(nullptr);
        GlDrawable::detach_all();
        graphics::set_active_backend_device(nullptr);
    }

    GlContext& ctx() { return *ctx_; }
    void clear() { cmd::Clear(ctx(), GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT); }

    /// A colour buffer as RGBA, row 0 = the bottom row. Reading ends the pass.
    std::vector<Rgba> pixels(u32 buffer = GL_BACK) {
        std::vector<u8> bgra;
        EXPECT_TRUE(drawable_->read_color(buffer, bgra));
        std::vector<Rgba> out(kW * kH);
        for (i32 gy = 0; gy < kH; ++gy) {
            for (i32 x = 0; x < kW; ++x) {
                const u8* p = &bgra[(static_cast<usize>(kH - 1 - gy) * kW + x) * 4u];
                out[gy * kW + x] = Rgba{p[2], p[1], p[0], p[3]};
            }
        }
        return out;
    }
    std::vector<float> depths() {
        std::vector<float> raw, out(kW * kH);
        EXPECT_TRUE(drawable_->read_depth(raw));
        for (i32 gy = 0; gy < kH; ++gy) {
            for (i32 x = 0; x < kW; ++x) out[gy * kW + x] = raw[static_cast<usize>(kH - 1 - gy) * kW + x];
        }
        return out;
    }
    std::vector<u8> stencils() {
        std::vector<u8> raw, out(kW * kH);
        EXPECT_TRUE(drawable_->read_stencil(raw));
        for (i32 gy = 0; gy < kH; ++gy) {
            for (i32 x = 0; x < kW; ++x) out[gy * kW + x] = raw[static_cast<usize>(kH - 1 - gy) * kW + x];
        }
        return out;
    }
    static Rgba at(const std::vector<Rgba>& px, i32 x, i32 y) { return px.at(y * kW + x); }

    void color(double r, double g, double b, double a = 1.0) {
        cmd::Color(ctx(), static_cast<float>(r), static_cast<float>(g), static_cast<float>(b), static_cast<float>(a));
    }
    void color(const Rgba& c) { color(c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0); }
    void vertex(double x, double y, double z = 0) {
        cmd::Vertex(ctx(), static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), 1);
    }
    /// A counter-clockwise rectangle at depth z (object).
    void quad(double x0, double y0, double x1, double y1, double z = 0) {
        cmd::Begin(ctx(), GL_QUADS);
        vertex(x0, y0, z);
        vertex(x1, y0, z);
        vertex(x1, y1, z);
        vertex(x0, y1, z);
        cmd::End(ctx());
    }
    void pixel_quad(i32 x, i32 y, double z = 0) { quad(x, y, x + 1, y + 1, z); }
    void line(double xa, double ya, double xb, double yb) {
        cmd::Begin(ctx(), GL_LINES);
        vertex(xa, ya);
        vertex(xb, yb);
        cmd::End(ctx());
    }
    static GlValues values(std::initializer_list<double> v) {
        const std::vector<double> d(v);
        return GlValues::vector(d, false);
    }
    static GlValues scalar(double v) { return GlValues::scalar_value(v, false); }

    /// Every pixel is @p on inside @p set and @p off outside it.
    void expect_coverage(const std::vector<Rgba>& px, const PixelSet& set, const Rgba& on, const Rgba& off,
                         const std::string& what) {
        u32 wrong = 0;
        for (i32 y = 0; y < kH; ++y) {
            for (i32 x = 0; x < kW; ++x) {
                const Rgba want = set.count({x, y}) ? on : off;
                if (at(px, x, y) != want && ++wrong <= 6) {
                    ADD_FAILURE() << what << ": pixel " << x << "," << y << " is " << at(px, x, y) << ", want " << want;
                }
            }
        }
        EXPECT_EQ(wrong, 0u) << what;
    }
    static void expect_near(const Rgba& got, const std::array<double, 4>& want, const std::string& what) {
        const double tol = 1.5;
        std::ostringstream w;
        w << what << ": got " << got << ", want (" << want[0] * 255 << "," << want[1] * 255 << "," << want[2] * 255
          << "," << want[3] * 255 << ")";
        EXPECT_NEAR(got.r, want[0] * 255.0, tol) << w.str();
        EXPECT_NEAR(got.g, want[1] * 255.0, tol) << w.str();
        EXPECT_NEAR(got.b, want[2] * 255.0, tol) << w.str();
        EXPECT_NEAR(got.a, want[3] * 255.0, tol) << w.str();
    }

    GuestMemory memory_;
    std::unique_ptr<ThunkRegistry> registry_;
    std::unique_ptr<Opengl32Manager> mgr_;
    std::unique_ptr<Caller> main_;
    GlContext* ctx_{nullptr};
    std::shared_ptr<GlDrawable> drawable_;
};

// ---- Blending (4.1.6) -------------------------------------------------------------

constexpr u32 kSourceFactors[] = {GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA,
                                  GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA_SATURATE};
constexpr u32 kDestinationFactors[] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA,
                                       GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};

/// Tables 4.1 and 4.2: the factor quadruplet for source s and destination d.
std::array<double, 4> blend_factor(u32 f, const std::array<double, 4>& s, const std::array<double, 4>& d) {
    switch (f) {
    case GL_ZERO: return {0, 0, 0, 0};
    case GL_ONE: return {1, 1, 1, 1};
    case GL_DST_COLOR: return d;
    case GL_ONE_MINUS_DST_COLOR: return {1 - d[0], 1 - d[1], 1 - d[2], 1 - d[3]};
    case GL_SRC_COLOR: return s;
    case GL_ONE_MINUS_SRC_COLOR: return {1 - s[0], 1 - s[1], 1 - s[2], 1 - s[3]};
    case GL_SRC_ALPHA: return {s[3], s[3], s[3], s[3]};
    case GL_ONE_MINUS_SRC_ALPHA: return {1 - s[3], 1 - s[3], 1 - s[3], 1 - s[3]};
    case GL_DST_ALPHA: return {d[3], d[3], d[3], d[3]};
    case GL_ONE_MINUS_DST_ALPHA: return {1 - d[3], 1 - d[3], 1 - d[3], 1 - d[3]};
    case GL_SRC_ALPHA_SATURATE: {
        const double f2 = std::min(s[3], 1 - d[3]);
        return {f2, f2, f2, 1};
    }
    default: return {0, 0, 0, 0};
    }
}

TEST_P(GlRasterTest, BlendFuncsMapExactly) {
    using F = graphics::BackendBlendFactor;
    // Table 4.1 / 4.2 tokens to the backend's factors; the other side's
    // tokens are not factors there.
    const std::map<u32, F> both{{GL_ZERO, F::Zero},
                                {GL_ONE, F::One},
                                {GL_SRC_ALPHA, F::SourceAlpha},
                                {GL_ONE_MINUS_SRC_ALPHA, F::OneMinusSourceAlpha},
                                {GL_DST_ALPHA, F::DestinationAlpha},
                                {GL_ONE_MINUS_DST_ALPHA, F::OneMinusDestinationAlpha}};
    for (const auto& [token, factor] : both) {
        EXPECT_EQ(gl_blend_factor(token, true), factor) << token;
        EXPECT_EQ(gl_blend_factor(token, false), factor) << token;
    }
    EXPECT_EQ(gl_blend_factor(GL_DST_COLOR, true), F::DestinationColor);
    EXPECT_EQ(gl_blend_factor(GL_ONE_MINUS_DST_COLOR, true), F::OneMinusDestinationColor);
    EXPECT_EQ(gl_blend_factor(GL_SRC_ALPHA_SATURATE, true), F::SourceAlphaSaturated);
    EXPECT_EQ(gl_blend_factor(GL_SRC_COLOR, false), F::SourceColor);
    EXPECT_EQ(gl_blend_factor(GL_ONE_MINUS_SRC_COLOR, false), F::OneMinusSourceColor);
    for (u32 t : {GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE}) EXPECT_FALSE(gl_blend_factor(t, false));
    for (u32 t : {GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR}) EXPECT_FALSE(gl_blend_factor(t, true));

    // Every pair on the GPU: C = Cs S + Cd D, clamped (4.1.6).
    const std::array<double, 4> d{51 / 255.0, 102 / 255.0, 153 / 255.0, 204 / 255.0};
    const std::array<double, 4> s{204 / 255.0, 153 / 255.0, 102 / 255.0, 153 / 255.0};
    cmd::ClearColor(ctx(), static_cast<float>(d[0]), static_cast<float>(d[1]), static_cast<float>(d[2]),
                    static_cast<float>(d[3]));
    clear();
    cmd::Enable(ctx(), GL_BLEND);
    color(s[0], s[1], s[2], s[3]);
    i32 cell = 0;
    for (u32 sf : kSourceFactors) {
        for (u32 df : kDestinationFactors) {
            cmd::BlendFunc(ctx(), sf, df);
            pixel_quad(cell % kW, cell / kW);
            ++cell;
        }
    }
    EXPECT_EQ(cmd::GetError(ctx()), GL_NO_ERROR);
    const auto px = pixels();
    cell = 0;
    for (u32 sf : kSourceFactors) {
        for (u32 df : kDestinationFactors) {
            const auto S = blend_factor(sf, s, d), D = blend_factor(df, s, d);
            std::array<double, 4> want{};
            for (usize k = 0; k < 4; ++k) want[k] = std::clamp(s[k] * S[k] + d[k] * D[k], 0.0, 1.0);
            expect_near(at(px, cell % kW, cell / kW), want, "glBlendFunc(" + std::to_string(sf) + ", " + std::to_string(df) + ")");
            ++cell;
        }
    }
}

// ---- Depth (4.1.5) ------------------------------------------------------------------

TEST_P(GlRasterTest, DepthFuncsAndMask) {
    constexpr u32 kFuncs[] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    const auto passes = [](u32 f, double z, double stored) {
        switch (f) {
        case GL_NEVER: return false;
        case GL_LESS: return z < stored;
        case GL_EQUAL: return z == stored;
        case GL_LEQUAL: return z <= stored;
        case GL_GREATER: return z > stored;
        case GL_NOTEQUAL: return z != stored;
        case GL_GEQUAL: return z >= stored;
        default: return true;
        }
    };
    cmd::ClearDepth(ctx(), 0.5);
    clear();
    const double objects[3] = {0.5, 0.0, -0.5};   // window z 0.25, 0.5, 0.75
    color(kRed);
    cmd::Enable(ctx(), GL_DEPTH_TEST);
    i32 row = 0;
    for (bool mask : {true, false}) {
        cmd::DepthMask(ctx(), mask);
        for (u32 f : kFuncs) {
            cmd::DepthFunc(ctx(), f);
            for (i32 k = 0; k < 3; ++k) pixel_quad(k, row, objects[k]);
            ++row;
        }
    }
    // Disabled: drawn, and the depth buffer is not updated, whatever the mask.
    cmd::Disable(ctx(), GL_DEPTH_TEST);
    cmd::DepthMask(ctx(), true);
    pixel_quad(5, 0, 0.5);
    // GL_NONE: no colour buffer, the depth test and update still happen.
    cmd::Enable(ctx(), GL_DEPTH_TEST);
    cmd::DepthFunc(ctx(), GL_LESS);
    cmd::DrawBuffer(ctx(), GL_NONE);
    pixel_quad(6, 0, 0.5);
    cmd::DrawBuffer(ctx(), GL_BACK);
    EXPECT_EQ(cmd::GetError(ctx()), GL_NO_ERROR);

    const auto px = pixels();
    const auto z = depths();
    row = 0;
    for (bool mask : {true, false}) {
        for (u32 f : kFuncs) {
            for (i32 k = 0; k < 3; ++k) {
                const double zw = (1.0 - objects[k]) / 2.0;
                const bool pass = passes(f, zw, 0.5);
                EXPECT_EQ(at(px, k, row), pass ? kRed : kBlack) << "func " << f << " z " << zw << " mask " << mask;
                EXPECT_EQ(z[row * kW + k], static_cast<float>(pass && mask ? zw : 0.5))
                    << "func " << f << " z " << zw << " mask " << mask;
            }
            ++row;
        }
    }
    EXPECT_EQ(at(px, 5, 0), kRed);
    EXPECT_EQ(z[5], 0.5f);
    EXPECT_EQ(at(px, 6, 0), kBlack);
    EXPECT_EQ(z[6], 0.25f);
}

// ---- Stencil (4.1.4) ----------------------------------------------------------------

TEST_P(GlRasterTest, StencilFuncOpsAndMasks) {
    struct Case {
        u32 initial;
        u32 func;
        i32 ref;
        u32 mask;
        u32 sfail, zfail, zpass;
        u32 writemask;
        bool depth_fails;   // a GL_NEVER depth test
    };
    const std::vector<Case> cases = {
        {0x5A, GL_EQUAL, 0x5A, 0xFF, GL_ZERO, GL_ZERO, GL_INCR, 0xFF, false},
        {0x5A, GL_EQUAL, 0x0A, 0x0F, GL_ZERO, GL_ZERO, GL_DECR, 0xFF, false},     // masked compare passes
        {0x5A, GL_EQUAL, 0x0B, 0x0F, GL_INVERT, GL_ZERO, GL_KEEP, 0xFF, false},  // fails: sfail
        {0xFF, GL_ALWAYS, 0, 0xFF, GL_KEEP, GL_KEEP, GL_INCR, 0xFF, false},      // INCR clamps at 255
        {0x00, GL_ALWAYS, 0, 0xFF, GL_KEEP, GL_KEEP, GL_DECR, 0xFF, false},      // DECR clamps at 0
        {0x5A, GL_ALWAYS, 0x33, 0xFF, GL_KEEP, GL_KEEP, GL_REPLACE, 0x0F, false},  // write mask
        {0x5A, GL_ALWAYS, 300, 0xFF, GL_KEEP, GL_KEEP, GL_REPLACE, 0xFF, false},   // ref clamped to 255
        {0x5A, GL_ALWAYS, -5, 0xFF, GL_KEEP, GL_KEEP, GL_REPLACE, 0xFF, false},    // ref clamped to 0
        {0x5A, GL_ALWAYS, 0x11, 0xFF, GL_KEEP, GL_REPLACE, GL_ZERO, 0xFF, true},   // depth fails: zfail
        {0x20, GL_LESS, 0x10, 0xFF, GL_INVERT, GL_KEEP, GL_INCR, 0xFF, false},     // 0x10 < 0x20 passes
        {0x10, GL_LESS, 0x10, 0xFF, GL_INVERT, GL_KEEP, GL_INCR, 0xFF, false},
        {0x10, GL_LEQUAL, 0x10, 0xFF, GL_INVERT, GL_KEEP, GL_INCR, 0xFF, false},
        {0x10, GL_GREATER, 0x20, 0xFF, GL_INVERT, GL_KEEP, GL_INCR, 0xFF, false},
        {0x10, GL_GEQUAL, 0x05, 0xFF, GL_INVERT, GL_KEEP, GL_INCR, 0xFF, false},
        {0x10, GL_NOTEQUAL, 0x10, 0xFF, GL_INVERT, GL_KEEP, GL_INCR, 0xFF, false},
        {0x10, GL_NEVER, 0x10, 0xFF, GL_REPLACE, GL_KEEP, GL_INCR, 0xFF, false},
        {0x10, GL_ALWAYS, 0x10, 0xF0, GL_KEEP, GL_KEEP, GL_INVERT, 0xFF, false},
    };
    const auto compare = [](u32 f, u32 ref, u32 stored) {
        switch (f) {
        case GL_NEVER: return false;
        case GL_LESS: return ref < stored;
        case GL_EQUAL: return ref == stored;
        case GL_LEQUAL: return ref <= stored;
        case GL_GREATER: return ref > stored;
        case GL_NOTEQUAL: return ref != stored;
        case GL_GEQUAL: return ref >= stored;
        default: return true;
        }
    };
    const auto apply_op = [](u32 op, u32 old, u32 ref) -> u32 {
        switch (op) {
        case GL_ZERO: return 0;
        case GL_REPLACE: return ref;
        case GL_INCR: return std::min(old + 1, 255u);
        case GL_DECR: return old == 0 ? 0 : old - 1;
        case GL_INVERT: return ~old & 0xFF;
        default: return old;
        }
    };
    cmd::Enable(ctx(), GL_STENCIL_TEST);
    color(kRed);
    for (usize i = 0; i < cases.size(); ++i) {
        const Case& c = cases[i];
        const i32 x = static_cast<i32>(i % kW), y = static_cast<i32>(i / kW);
        // Seed the stencil value: ALWAYS + REPLACE, colour masked off.
        cmd::ColorMask(ctx(), false, false, false, false);
        cmd::Disable(ctx(), GL_DEPTH_TEST);
        cmd::StencilMask(ctx(), 0xFF);
        cmd::StencilFunc(ctx(), GL_ALWAYS, static_cast<i32>(c.initial), 0xFF);
        cmd::StencilOp(ctx(), GL_KEEP, GL_KEEP, GL_REPLACE);
        pixel_quad(x, y);
        cmd::ColorMask(ctx(), true, true, true, true);
        if (c.depth_fails) {
            cmd::Enable(ctx(), GL_DEPTH_TEST);
            cmd::DepthFunc(ctx(), GL_NEVER);
        }
        cmd::StencilMask(ctx(), c.writemask);
        cmd::StencilFunc(ctx(), c.func, c.ref, c.mask);
        cmd::StencilOp(ctx(), c.sfail, c.zfail, c.zpass);
        pixel_quad(x, y);
    }
    EXPECT_EQ(cmd::GetError(ctx()), GL_NO_ERROR);
    const auto px = pixels();
    const auto st = stencils();
    for (usize i = 0; i < cases.size(); ++i) {
        const Case& c = cases[i];
        const i32 x = static_cast<i32>(i % kW), y = static_cast<i32>(i / kW);
        const u32 ref = static_cast<u32>(std::clamp(c.ref, 0, 255));
        const bool pass = compare(c.func, ref & c.mask, c.initial & c.mask);
        const u32 op = !pass ? c.sfail : c.depth_fails ? c.zfail : c.zpass;
        const u32 want = (c.initial & ~c.writemask) | (apply_op(op, c.initial, ref) & c.writemask);
        EXPECT_EQ(st[y * kW + x], want & 0xFF) << "case " << i;
        EXPECT_EQ(at(px, x, y), pass && !c.depth_fails ? kRed : kBlack) << "case " << i;
    }
}

// ---- Culling, front face, two-sided lighting (3.5.1, 2.13.1) --------------------------

TEST_P(GlRasterTest, CullFaceAndFrontFace) {
    // A counter-clockwise triangle (positive window area, eq. 2.6) on the
    // left, a clockwise one on the right.
    const auto draw = [&]() {
        cmd::Begin(ctx(), GL_TRIANGLES);
        vertex(1.2, 1.3);
        vertex(12.7, 2.1);
        vertex(5.4, 11.6);
        vertex(17.2, 1.3);
        vertex(21.4, 11.6);
        vertex(28.7, 2.1);
        cmd::End(ctx());
    };
    const Pixel ccw_probe{6, 4}, cw_probe{22, 4};
    struct Mode {
        bool enabled;
        u32 mode;
    };
    for (u32 front_face : {GL_CCW, GL_CW}) {
        for (const Mode m : {Mode{false, GL_BACK}, Mode{true, GL_BACK}, Mode{true, GL_FRONT}, Mode{true, GL_FRONT_AND_BACK}}) {
            clear();
            cmd::FrontFace(ctx(), front_face);
            cmd::CullFace(ctx(), m.mode);
            if (m.enabled) cmd::Enable(ctx(), GL_CULL_FACE);
            else cmd::Disable(ctx(), GL_CULL_FACE);
            color(kRed);
            draw();
            const auto px = pixels();
            // 3.5.1: front facing when the sign (reversed for CW) is positive.
            const bool ccw_front = front_face == GL_CCW;
            const auto visible = [&](bool front) {
                if (!m.enabled) return true;
                if (m.mode == GL_FRONT_AND_BACK) return false;
                return m.mode == GL_BACK ? front : !front;
            };
            const std::string what = std::string(front_face == GL_CCW ? "CCW" : "CW") + " cull " +
                                     (m.enabled ? std::to_string(m.mode) : "off");
            EXPECT_EQ(at(px, ccw_probe.first, ccw_probe.second), visible(ccw_front) ? kRed : kBlack) << what;
            EXPECT_EQ(at(px, cw_probe.first, cw_probe.second), visible(!ccw_front) ? kRed : kBlack) << what;
        }
    }
    cmd::Disable(ctx(), GL_CULL_FACE);

    // Two-sided lighting: no lights, zero ambient: the colour is the face's
    // emission (2.13.1). Front red, back blue; lines and points are front.
    cmd::Enable(ctx(), GL_LIGHTING);
    cmd::LightModel(ctx(), GL_LIGHT_MODEL_TWO_SIDE, scalar(1));
    cmd::LightModel(ctx(), GL_LIGHT_MODEL_AMBIENT, values({0, 0, 0, 1}));
    cmd::Material(ctx(), GL_FRONT_AND_BACK, GL_AMBIENT, values({0, 0, 0, 1}));
    cmd::Material(ctx(), GL_FRONT, GL_EMISSION, values({1, 0, 0, 1}));
    cmd::Material(ctx(), GL_BACK, GL_EMISSION, values({0, 0, 1, 1}));
    for (u32 front_face : {GL_CCW, GL_CW}) {
        clear();
        cmd::FrontFace(ctx(), front_face);
        draw();
        line(1.2, 15.5, 30.7, 15.5);
        cmd::Begin(ctx(), GL_POINTS);
        vertex(3.5, 20.5);
        cmd::End(ctx());
        const auto px = pixels();
        const std::string what = front_face == GL_CCW ? "two-sided CCW" : "two-sided CW";
        EXPECT_EQ(at(px, ccw_probe.first, ccw_probe.second), front_face == GL_CCW ? kRed : kBlue) << what;
        EXPECT_EQ(at(px, cw_probe.first, cw_probe.second), front_face == GL_CCW ? kBlue : kRed) << what;
        EXPECT_EQ(at(px, 10, 15), kRed) << what << " line";
        EXPECT_EQ(at(px, 3, 20), kRed) << what << " point";
    }
}

// ---- Polygon modes (3.5.4) ----------------------------------------------------------

TEST_P(GlRasterTest, PolygonModeLineAndPointPerFace) {
    // A counter-clockwise ("front" under GL_CCW) and a clockwise ("back")
    // quadrilateral.
    // Every pixel centre is at least 1/4 pixel from their edges (no ties).
    const std::vector<std::array<double, 2>> front{{2.82, 2.07}, {13.25, 2.2}, {13.15, 9.97}, {2.91, 9.91}};
    const std::vector<std::array<double, 2>> back{{17.13, 2.09}, {17.08, 9.75}, {28.87, 9.93}, {28.91, 2.06}};
    const auto draw = [&](i32 hidden_edge) {
        cmd::Begin(ctx(), GL_QUADS);
        for (i32 i = 0; i < 4; ++i) {
            cmd::EdgeFlag(ctx(), i != hidden_edge);
            vertex(front[i][0], front[i][1]);
        }
        cmd::EdgeFlag(ctx(), true);
        for (const auto& p : back) vertex(p[0], p[1]);
        cmd::End(ctx());
    };
    const auto edges = [&](const std::vector<std::array<double, 2>>& p, i32 hidden_edge) {
        PixelSet out;
        for (i32 i = 0; i < 4; ++i) {
            if (i == hidden_edge) continue;
            for (const Pixel& f : diamond_exit(p[i][0], p[i][1], p[(i + 1) % 4][0], p[(i + 1) % 4][1])) out.insert(f);
        }
        return out;
    };
    const auto points = [&](const std::vector<std::array<double, 2>>& p, i32 hidden_edge) {
        PixelSet out;
        for (i32 i = 0; i < 4; ++i) {
            if (i != hidden_edge) out.insert({static_cast<i32>(std::floor(p[i][0])), static_cast<i32>(std::floor(p[i][1]))});
        }
        return out;
    };
    struct Step {
        u32 front_mode, back_mode;
        i32 hidden_edge;   // the counter-clockwise quad's vertex whose edge flag is FALSE (-1: none)
    };
    // Ruling 2a: both windings. Under GL_CW the clockwise quad is the front.
    for (u32 front_face : {GL_CCW, GL_CW}) {
        const std::string ff = front_face == GL_CCW ? "GL_CCW" : "GL_CW";
        const bool ccw_front = front_face == GL_CCW;
        cmd::FrontFace(ctx(), front_face);
        color(kGreen);
        for (const Step st : {Step{GL_LINE, GL_POINT, -1}, Step{GL_LINE, GL_POINT, 1}, Step{GL_POINT, GL_FILL, 2},
                              Step{GL_FILL, GL_LINE, -1}}) {
            clear();
            cmd::PolygonMode(ctx(), GL_FRONT, st.front_mode);
            cmd::PolygonMode(ctx(), GL_BACK, st.back_mode);
            draw(st.hidden_edge);
            PixelSet want;
            const auto add = [&](u32 mode, const std::vector<std::array<double, 2>>& p, i32 hidden) {
                const PixelSet s = mode == GL_LINE ? edges(p, hidden) : mode == GL_POINT ? points(p, hidden) : polygon_cover(p);
                want.insert(s.begin(), s.end());
            };
            add(ccw_front ? st.front_mode : st.back_mode, front, st.hidden_edge);
            add(ccw_front ? st.back_mode : st.front_mode, back, -1);
            expect_coverage(pixels(), want, kGreen, kBlack,
                            ff + " front " + std::to_string(st.front_mode) + " back " + std::to_string(st.back_mode) +
                                " hidden " + std::to_string(st.hidden_edge));
        }

        // GL_FRONT_AND_BACK + culling: the culled face is not drawn in any mode.
        clear();
        cmd::PolygonMode(ctx(), GL_FRONT_AND_BACK, GL_LINE);
        cmd::Enable(ctx(), GL_CULL_FACE);
        cmd::CullFace(ctx(), GL_BACK);
        draw(-1);
        expect_coverage(pixels(), edges(ccw_front ? front : back, -1), kGreen, kBlack, ff + " LINE, back culled");
        cmd::Disable(ctx(), GL_CULL_FACE);

        // Two-sided lighting: edges and vertices take their polygon's face
        // colour; the expanded quads are wound so the backend agrees.
        cmd::Enable(ctx(), GL_LIGHTING);
        cmd::LightModel(ctx(), GL_LIGHT_MODEL_TWO_SIDE, scalar(1));
        cmd::LightModel(ctx(), GL_LIGHT_MODEL_AMBIENT, values({0, 0, 0, 1}));
        cmd::Material(ctx(), GL_FRONT_AND_BACK, GL_AMBIENT, values({0, 0, 0, 1}));
        cmd::Material(ctx(), GL_FRONT, GL_EMISSION, values({1, 0, 0, 1}));
        cmd::Material(ctx(), GL_BACK, GL_EMISSION, values({0, 0, 1, 1}));
        for (u32 mode : {GL_LINE, GL_POINT}) {
            clear();
            cmd::PolygonMode(ctx(), GL_FRONT_AND_BACK, mode);
            draw(-1);
            const auto px = pixels();
            const PixelSet a = mode == GL_LINE ? edges(front, -1) : points(front, -1);
            const PixelSet c = mode == GL_LINE ? edges(back, -1) : points(back, -1);
            for (const Pixel& p : a) EXPECT_EQ(at(px, p.first, p.second), ccw_front ? kRed : kBlue) << ff << " ccw quad " << mode;
            for (const Pixel& p : c) EXPECT_EQ(at(px, p.first, p.second), ccw_front ? kBlue : kRed) << ff << " cw quad " << mode;
        }
        cmd::Disable(ctx(), GL_LIGHTING);
    }
    cmd::FrontFace(ctx(), GL_CCW);
    color(kGreen);

    // Line stipple over polygon edges: reset at the polygon's first edge,
    // continuing across its edges (3.5.4).
    clear();
    cmd::PolygonMode(ctx(), GL_FRONT_AND_BACK, GL_LINE);
    cmd::Enable(ctx(), GL_LINE_STIPPLE);
    cmd::LineStipple(ctx(), 2, 0x3C5A);
    cmd::Begin(ctx(), GL_POLYGON);
    for (const auto& p : front) vertex(p[0], p[1]);
    cmd::End(ctx());
    PixelSet want;
    u32 s = 0;
    for (i32 i = 0; i < 4; ++i) {
        for (const auto& column : wide_line(front[i][0], front[i][1], front[(i + 1) % 4][0], front[(i + 1) % 4][1], 1)) {
            if ((0x3C5A >> ((s / 2) % 16)) & 1u) want.insert(column.begin(), column.end());
            ++s;
        }
    }
    expect_coverage(pixels(), want, kGreen, kBlack, "stippled polygon edges");
}

// ---- Polygon offset (3.5.5, Review Focus 4) ----------------------------------------------

TEST_P(GlRasterTest, PolygonOffsetUnitsMatchFormatResolution) {
    // r of D32Float (the 24-bit formats): 2^-24, the spacing of floats in
    // [0.5, 1). A coplanar polygon offset by one unit is resolvable.
    const double r24 = std::ldexp(1.0, -24);
    EXPECT_EQ(gl_depth_resolution(graphics::PixelFormat::D32Float_S8Uint), r24);
    EXPECT_EQ(gl_depth_resolution(graphics::PixelFormat::D16Unorm), 1.0 / 65535.0);

    cmd::Enable(ctx(), GL_DEPTH_TEST);
    cmd::DepthFunc(ctx(), GL_LESS);
    // Depth only: window z 0.75, then the same polygon with offsets of +1,
    // -1 and -2 units.
    cmd::ColorMask(ctx(), false, false, false, false);
    quad(0, 0, 4, 4, -0.5);
    quad(16, 0, 20, 4, -0.5);
    cmd::Enable(ctx(), GL_POLYGON_OFFSET_FILL);
    cmd::DepthFunc(ctx(), GL_ALWAYS);
    for (const auto& [x, units] : std::vector<std::pair<i32, float>>{{4, 1.0f}, {8, -1.0f}, {12, -2.0f}}) {
        cmd::PolygonOffset(ctx(), 0, units);
        quad(x, 0, x + 4, 4, -0.5);
    }
    cmd::ColorMask(ctx(), true, true, true, true);
    // Coplanar separation: without offset GL_LESS fails on the equal depth;
    // with -1 unit it passes, and the unoffset polygon then fails behind it.
    cmd::DepthFunc(ctx(), GL_LESS);
    cmd::Disable(ctx(), GL_POLYGON_OFFSET_FILL);
    color(kRed);
    quad(0, 0, 4, 4, -0.5);
    cmd::Enable(ctx(), GL_POLYGON_OFFSET_FILL);
    cmd::PolygonOffset(ctx(), 0, -1);
    color(kGreen);
    quad(16, 0, 20, 4, -0.5);
    cmd::Disable(ctx(), GL_POLYGON_OFFSET_FILL);
    color(kBlue);
    quad(16, 0, 20, 4, -0.5);
    // Slope: window z = 0.25 + x / 64 (object z = 0.5 - x / 32), factor 2:
    // o = 2 m = 2 / 64.
    cmd::Enable(ctx(), GL_POLYGON_OFFSET_FILL);
    cmd::DepthFunc(ctx(), GL_ALWAYS);
    cmd::PolygonOffset(ctx(), 2, 0);
    cmd::Begin(ctx(), GL_QUADS);
    vertex(0, 8, 0.5);
    vertex(32, 8, 0.5 - 1.0);
    vertex(32, 12, 0.5 - 1.0);
    vertex(0, 12, 0.5);
    cmd::End(ctx());
    // GL_LINE mode with GL_POLYGON_OFFSET_LINE: m is the polygon's slope.
    cmd::Disable(ctx(), GL_POLYGON_OFFSET_FILL);
    cmd::Enable(ctx(), GL_POLYGON_OFFSET_LINE);
    cmd::PolygonOffset(ctx(), 2, 30);
    cmd::PolygonMode(ctx(), GL_FRONT_AND_BACK, GL_LINE);
    // Its first three vertices are collinear: m comes from the polygon's
    // plane all the same (3.5.5).
    cmd::Begin(ctx(), GL_POLYGON);
    vertex(0.2, 14.5, 0.5);
    vertex(16.0, 14.5, 0.5 - 15.8 / 32.0);
    vertex(31.8, 14.5, 0.5 - 31.6 / 32.0);
    vertex(31.8, 20.5, 0.5 - 31.6 / 32.0);
    vertex(0.2, 20.5, 0.5);
    cmd::End(ctx());
    // GL_POINT mode with GL_POLYGON_OFFSET_POINT: each vertex's depth plus
    // the polygon's offset (window z = 0.25 + x / 64, m = 1 / 64).
    cmd::Disable(ctx(), GL_POLYGON_OFFSET_LINE);
    cmd::Enable(ctx(), GL_POLYGON_OFFSET_POINT);
    cmd::PolygonMode(ctx(), GL_FRONT_AND_BACK, GL_POINT);
    const std::array<std::array<double, 2>, 4> corners{{{2.5, 21.5}, {29.5, 21.5}, {29.5, 23.5}, {2.5, 23.5}}};
    cmd::Begin(ctx(), GL_QUADS);
    for (const auto& c : corners) vertex(c[0], c[1], 0.5 - c[0] / 32.0);
    cmd::End(ctx());
    cmd::Disable(ctx(), GL_POLYGON_OFFSET_POINT);
    EXPECT_EQ(cmd::GetError(ctx()), GL_NO_ERROR);

    const auto z = depths();
    const auto px = pixels();
    for (i32 x = 0; x < 16; ++x) {
        const double want = x < 4 ? 0.75 : x < 8 ? 0.75 + r24 : x < 12 ? 0.75 - r24 : 0.75 - 2 * r24;
        EXPECT_EQ(z[1 * kW + x], static_cast<float>(want)) << "x " << x;
    }
    EXPECT_EQ(at(px, 1, 1), kBlack) << "coplanar without offset fails GL_LESS";
    EXPECT_EQ(at(px, 17, 1), kGreen) << "coplanar with -1 unit passes GL_LESS";
    for (i32 x = 0; x < kW; ++x) {
        const double plane = 0.25 + (x + 0.5) / 64.0;
        EXPECT_NEAR(z[9 * kW + x], plane + 2.0 / 64.0, 1e-6) << "slope x " << x;
    }
    // The bottom edge (y 14.5) produces row 14; its depth is the edge's at
    // the fragment's projection (window z = 0.25 + (x - 0.2) / 64), plus
    // 2 m + 30 r with m the polygon's slope, 1 / 64.
    for (i32 x = 1; x < 31; ++x) {
        const double edge = 0.25 + (x + 0.5 - 0.2) / 64.0;
        EXPECT_NEAR(z[14 * kW + x], edge + 2.0 / 64.0 + 30 * r24, 2e-7) << "line-mode edge x " << x;
    }
    for (const auto& c : corners) {
        const i32 x = static_cast<i32>(c[0]), y = static_cast<i32>(c[1]);
        EXPECT_NEAR(z[y * kW + x], 0.25 + c[0] / 64.0 + 2.0 / 64.0 + 30 * r24, 2e-7) << "point-mode vertex " << x << "," << y;
    }

    // D16Unorm: r is one step, 1 / 65535.
    cmd::Disable(ctx(), GL_POLYGON_OFFSET_LINE);
    use(kDc16, kWindow16);
    cmd::Enable(ctx(), GL_DEPTH_TEST);
    cmd::DepthFunc(ctx(), GL_ALWAYS);
    const double z16 = 13107.0 / 65535.0;   // window z on a step
    cmd::Enable(ctx(), GL_POLYGON_OFFSET_FILL);
    for (const auto& [x, units] : std::vector<std::pair<i32, float>>{{0, 0.0f}, {4, 1.0f}, {8, -1.0f}, {12, 3.0f}}) {
        cmd::PolygonOffset(ctx(), 0, units);
        quad(x, 0, x + 4, 4, 1.0 - 2.0 * z16);
    }
    const auto z2 = depths();
    for (i32 x = 0; x < 16; ++x) {
        const double steps = x < 4 ? 0 : x < 8 ? 1 : x < 12 ? -1 : 3;
        EXPECT_EQ(std::lround(z2[1 * kW + x] * 65535.0), 13107 + std::lround(steps)) << "D16 x " << x;
    }
}

// ---- Lines (3.4) ----------------------------------------------------------------------

TEST_P(GlRasterTest, WideAliasedLineCoversSpecPixels) {
    struct Segment {
        double xa, ya, xb, yb;
    };
    const std::vector<Segment> segments = {
        {2.3, 3.6, 27.8, 10.1},    // x-major, left to right
        {27.8, 14.2, 3.1, 20.7},   // x-major, right to left, rising
        {6.4, 1.2, 11.7, 21.9},    // y-major, upwards
        {24.6, 22.3, 18.2, 2.6},   // y-major, downwards
        {4.25, 8.75, 28.6, 8.4},   // nearly horizontal
        {15.3, 4.6, 16.9, 18.2},   // steep
    };
    color(kGreen);
    for (float width : {1.0f, 2.0f, 3.0f, 4.0f, 0.4f, 2.6f, 5.0f}) {
        cmd::LineWidth(ctx(), width);
        for (const Segment& s : segments) {
            clear();
            line(s.xa, s.ya, s.xb, s.yb);
            PixelSet want;
            for (const auto& column : wide_line(s.xa, s.ya, s.xb, s.yb, width)) want.insert(column.begin(), column.end());
            expect_coverage(pixels(), in_window(want), kGreen, kBlack,
                            "width " + std::to_string(width) + " from " + std::to_string(s.xa) + "," + std::to_string(s.ya));
        }
    }

    // A strip shares its inner endpoints: additive blending counts each
    // fragment once per segment that produces it (3.4.1 rule 4 for
    // continuing x-major segments; the diamond-exit rule exactly).
    cmd::LineWidth(ctx(), 1);
    clear();
    cmd::Enable(ctx(), GL_BLEND);
    cmd::BlendFunc(ctx(), GL_ONE, GL_ONE);
    color(0, 20 / 255.0, 0, 0);
    const std::vector<std::array<double, 2>> strip{{1.3, 2.2}, {9.7, 6.4}, {20.2, 7.9}, {29.6, 15.3}, {12.4, 21.6}};
    cmd::Begin(ctx(), GL_LINE_STRIP);
    for (const auto& p : strip) vertex(p[0], p[1]);
    cmd::End(ctx());
    std::map<Pixel, u32> count;
    for (usize i = 0; i + 1 < strip.size(); ++i) {
        for (const Pixel& p : diamond_exit(strip[i][0], strip[i][1], strip[i + 1][0], strip[i + 1][1])) ++count[p];
    }
    const auto px = pixels();
    for (i32 y = 0; y < kH; ++y) {
        for (i32 x = 0; x < kW; ++x) {
            const u32 n = count.count({x, y}) ? count[{x, y}] : 0;
            EXPECT_EQ(at(px, x, y).g, n * 20) << x << "," << y;
        }
    }
    cmd::Disable(ctx(), GL_BLEND);

    // Smooth shading along a wide, sloped line: eq. 3.1, t is the projection
    // of the fragment centre onto the segment, f = (1 - t) f_a + t f_b (all
    // w = 1, so eq. 3.2 is linear), for every fragment of every column.
    for (float width : {1.0f, 3.0f, 4.0f}) {
        clear();
        cmd::LineWidth(ctx(), width);
        const std::array<double, 4> ca{0.9, 0.1, 0.2, 1.0}, cb{0.1, 0.8, 1.0, 1.0};
        const double xa = 1.6, ya = 3.3, xb = 29.7, yb = 17.8;
        cmd::Begin(ctx(), GL_LINES);
        color(ca[0], ca[1], ca[2], ca[3]);
        vertex(xa, ya);
        color(cb[0], cb[1], cb[2], cb[3]);
        vertex(xb, yb);
        cmd::End(ctx());
        const auto shaded = pixels();
        for (const auto& column : wide_line(xa, ya, xb, yb, width)) {
            for (const Pixel& p : column) {
                const double t = ((p.first + 0.5 - xa) * (xb - xa) + (p.second + 0.5 - ya) * (yb - ya)) /
                                 ((xb - xa) * (xb - xa) + (yb - ya) * (yb - ya));
                std::array<double, 4> want{};
                for (usize k = 0; k < 4; ++k) want[k] = (1 - t) * ca[k] + t * cb[k];
                expect_near(at(shaded, p.first, p.second), want,
                            "smooth width " + std::to_string(width) + " at " + std::to_string(p.first) + "," +
                                std::to_string(p.second));
            }
        }
    }

    // GL_LINE_SMOOTH is not implemented: drawn aliased (after a one-time
    // UNIMPLEMENTED).
    clear();
    color(kGreen);
    cmd::Enable(ctx(), GL_LINE_SMOOTH);
    cmd::LineWidth(ctx(), 3);
    line(2.3, 3.6, 27.8, 10.1);
    PixelSet want;
    for (const auto& column : wide_line(2.3, 3.6, 27.8, 10.1, 3)) want.insert(column.begin(), column.end());
    expect_coverage(pixels(), want, kGreen, kBlack, "LINE_SMOOTH draws aliased");
}

TEST_P(GlRasterTest, LineStippleCounterResetsPerSegmentAndBegin) {
    cmd::Enable(ctx(), GL_LINE_STIPPLE);
    color(kGreen);
    struct Seg {
        double xa, ya, xb, yb;
    };
    const auto check = [&](const std::vector<std::vector<Seg>>& begins, u32 mode, i32 factor, u16 pattern, float width,
                           const std::string& what) {
        clear();
        cmd::LineStipple(ctx(), factor, pattern);
        cmd::LineWidth(ctx(), width);
        PixelSet want;
        for (const auto& segs : begins) {
            cmd::Begin(ctx(), mode);
            if (mode == GL_LINES) {
                for (const Seg& s : segs) {
                    vertex(s.xa, s.ya);
                    vertex(s.xb, s.yb);
                }
            } else {
                vertex(segs[0].xa, segs[0].ya);
                for (const Seg& s : segs) vertex(s.xb, s.yb);
            }
            cmd::End(ctx());
            // 3.4.2: s = 0 at glBegin and before each GL_LINES segment;
            // it advances by one per fragment (column) produced.
            u32 s = 0;
            for (const Seg& seg : segs) {
                if (mode == GL_LINES) s = 0;
                for (const auto& column : wide_line(seg.xa, seg.ya, seg.xb, seg.yb, width)) {
                    if ((pattern >> ((s / static_cast<u32>(factor)) % 16)) & 1u) want.insert(column.begin(), column.end());
                    ++s;
                }
            }
        }
        expect_coverage(pixels(), in_window(want), kGreen, kBlack, what);
    };
    const std::vector<Seg> two{{1.2, 2.5, 20.7, 2.5}, {1.2, 5.5, 25.3, 9.1}};
    check({two}, GL_LINES, 1, 0x00FF, 1, "GL_LINES reset per segment");
    check({{{1.2, 2.5, 13.7, 2.5}, {13.7, 2.5, 29.4, 8.6}}}, GL_LINE_STRIP, 1, 0x0F0F, 1, "strip continues");
    check({{{1.2, 2.5, 13.7, 2.5}}, {{13.7, 2.5, 29.4, 8.6}}}, GL_LINE_STRIP, 1, 0x0F0F, 1, "glBegin resets");
    check({{{29.6, 4.5, 2.2, 4.5}}}, GL_LINES, 3, 0x1C7, 1, "reversed segment counts from its first vertex");
    check({{{3.4, 2.2, 7.1, 21.8}}}, GL_LINES, 2, 0x3333, 1, "y-major");
    check({{{1.2, 6.5, 30.7, 12.3}}}, GL_LINES, 2, 0x0F0F, 3, "wide stippled line drops whole columns");
    // Disabled: as if the default pattern 0xFFFF.
    cmd::Disable(ctx(), GL_LINE_STIPPLE);
    clear();
    cmd::LineWidth(ctx(), 1);
    line(1.2, 2.5, 20.7, 2.5);
    PixelSet want;
    for (const Pixel& p : diamond_exit(1.2, 2.5, 20.7, 2.5)) want.insert(p);
    expect_coverage(pixels(), want, kGreen, kBlack, "stipple disabled");
}

// ---- Points (3.3) ---------------------------------------------------------------------

TEST_P(GlRasterTest, PointSizeCoversSquare) {
    color(kGreen);
    const std::vector<std::array<double, 2>> where{{5.3, 6.8}, {12.7, 3.2}, {20.1, 15.9}, {0.2, 11.6}, {31.9, 22.4}};
    for (float size : {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 0.3f, 2.4f, 6.0f}) {
        clear();
        cmd::PointSize(ctx(), size);
        cmd::Begin(ctx(), GL_POINTS);
        for (const auto& p : where) vertex(p[0], p[1]);
        vertex(-0.2, 5.0);   // outside the clip volume: discarded although its square reaches in
        vertex(16.0, 24.3);
        cmd::End(ctx());
        PixelSet want;
        for (const auto& p : where) {
            const PixelSet s = point_square(p[0], p[1], size);
            want.insert(s.begin(), s.end());
        }
        expect_coverage(pixels(), in_window(want), kGreen, kBlack, "point size " + std::to_string(size));
    }
    // Smooth points are drawn aliased (one-time UNIMPLEMENTED).
    clear();
    cmd::Enable(ctx(), GL_POINT_SMOOTH);
    cmd::PointSize(ctx(), 4);
    cmd::Begin(ctx(), GL_POINTS);
    vertex(12.7, 3.2);
    cmd::End(ctx());
    expect_coverage(pixels(), point_square(12.7, 3.2, 4), kGreen, kBlack, "POINT_SMOOTH draws aliased");
}

// ---- Alpha test (4.1.3) and fog (3.9) ---------------------------------------------------------

TEST_P(GlRasterTest, AlphaTestAndFogApply) {
    constexpr u32 kFuncs[] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    const double alphas[3] = {0.25, 0.5, 0.75};
    cmd::Enable(ctx(), GL_ALPHA_TEST);
    for (usize f = 0; f < 8; ++f) {
        cmd::AlphaFunc(ctx(), kFuncs[f], 0.5f);
        for (i32 k = 0; k < 3; ++k) {
            color(1, 0, 0, alphas[k]);
            pixel_quad(k, static_cast<i32>(f));
        }
    }
    // The reference is clamped to [0, 1] (4.1.3).
    cmd::AlphaFunc(ctx(), GL_LESS, 2.0f);
    color(1, 0, 0, 1.0);
    pixel_quad(4, 0);
    cmd::Disable(ctx(), GL_ALPHA_TEST);
    const auto px = pixels();
    for (usize f = 0; f < 8; ++f) {
        for (i32 k = 0; k < 3; ++k) {
            const double a = alphas[k], ref = 0.5;
            const bool pass = kFuncs[f] == GL_NEVER ? false : kFuncs[f] == GL_LESS ? a < ref : kFuncs[f] == GL_EQUAL ? a == ref
                              : kFuncs[f] == GL_LEQUAL ? a <= ref : kFuncs[f] == GL_GREATER ? a > ref
                              : kFuncs[f] == GL_NOTEQUAL ? a != ref : kFuncs[f] == GL_GEQUAL ? a >= ref : true;
            EXPECT_EQ(at(px, k, static_cast<i32>(f)).r, pass ? 255 : 0) << "alpha func " << kFuncs[f] << " a " << a;
        }
    }
    EXPECT_EQ(at(px, 4, 0).r, 0) << "1.0 < clamp(2.0) fails";

    // Fog: a point per pixel (a 1x1 viewport each) at eye position e, fog
    // factor from eq. 3.16-3.18 with z = |e|, the eye distance; C = f Cr +
    // (1 - f) Cf for R, G, B (alpha unchanged).
    clear();
    cmd::MatrixMode(ctx(), GL_PROJECTION);
    cmd::LoadIdentity(ctx());
    cmd::Ortho(ctx(), -1, 1, -1, 1, 0.5, 100);
    cmd::MatrixMode(ctx(), GL_MODELVIEW);
    cmd::Enable(ctx(), GL_FOG);
    const std::array<double, 4> cr{1.0, 0.6, 0.0, 0.8}, cf{0.0, 0.2, 1.0, 1.0};
    cmd::Fog(ctx(), GL_FOG_COLOR, values({cf[0], cf[1], cf[2], cf[3]}));
    struct FogCase {
        u32 mode;
        double start, end, density;
    };
    const std::vector<FogCase> fogs{{GL_LINEAR, 1.0, 9.0, 1.0}, {GL_EXP, 0, 1, 0.3}, {GL_EXP2, 0, 1, 0.25}};
    const std::vector<std::array<double, 3>> eyes{{0, 0, -1}, {0, 0, -2.5}, {0, 0, -4}, {0, 0, -7}, {0.6, 0.8, -2}};
    for (usize f = 0; f < fogs.size(); ++f) {
        cmd::Fog(ctx(), GL_FOG_MODE, scalar(fogs[f].mode));
        cmd::Fog(ctx(), GL_FOG_START, scalar(fogs[f].start));
        cmd::Fog(ctx(), GL_FOG_END, scalar(fogs[f].end));
        cmd::Fog(ctx(), GL_FOG_DENSITY, scalar(fogs[f].density));
        for (usize e = 0; e < eyes.size(); ++e) {
            cmd::Viewport(ctx(), static_cast<i32>(e), static_cast<i32>(f), 1, 1);
            color(cr[0], cr[1], cr[2], cr[3]);
            cmd::Begin(ctx(), GL_POINTS);
            vertex(eyes[e][0], eyes[e][1], eyes[e][2]);
            cmd::End(ctx());
        }
    }
    const auto fp = pixels();
    for (usize f = 0; f < fogs.size(); ++f) {
        for (usize e = 0; e < eyes.size(); ++e) {
            const double z = std::sqrt(eyes[e][0] * eyes[e][0] + eyes[e][1] * eyes[e][1] + eyes[e][2] * eyes[e][2]);
            const FogCase& c = fogs[f];
            double factor = c.mode == GL_LINEAR ? (c.end - z) / (c.end - c.start)
                            : c.mode == GL_EXP  ? std::exp(-c.density * z)
                                                : std::exp(-(c.density * z) * (c.density * z));
            factor = std::clamp(factor, 0.0, 1.0);
            std::array<double, 4> want{};
            for (usize k = 0; k < 3; ++k) want[k] = factor * cr[k] + (1 - factor) * cf[k];
            want[3] = cr[3];
            expect_near(at(fp, static_cast<i32>(e), static_cast<i32>(f)), want,
                        "fog mode " + std::to_string(c.mode) + " eye " + std::to_string(e));
        }
    }
}

// ---- Scissor (4.1.2), colour mask (4.2.2), polygon stipple (3.5.2) ---------------------

TEST_P(GlRasterTest, ScissorClipsDraws) {
    cmd::Enable(ctx(), GL_SCISSOR_TEST);
    cmd::Scissor(ctx(), 3, 2, 10, 5);
    color(kRed);
    quad(0, 0, kW, kH);
    line(0.3, 4.5, 31.7, 4.5);
    cmd::PointSize(ctx(), 3);
    cmd::Begin(ctx(), GL_POINTS);
    vertex(20.5, 4.5);   // outside the box
    cmd::End(ctx());
    PixelSet want;
    for (i32 y = 2; y < 7; ++y) {
        for (i32 x = 3; x < 13; ++x) want.insert({x, y});
    }
    expect_coverage(pixels(), want, kRed, kBlack, "scissor box (3, 2, 10, 5)");
    clear();   // the clear honours the scissor too (4.2.3)
    cmd::Scissor(ctx(), 5, 5, 0, 4);
    quad(0, 0, kW, kH);
    cmd::Disable(ctx(), GL_SCISSOR_TEST);
    expect_coverage(pixels(), {}, kRed, kBlack, "an empty scissor box");

    // glColorMask: masked channels keep the buffer's values.
    cmd::ClearColor(ctx(), 0.2f, 0.4f, 0.6f, 0.8f);
    clear();
    cmd::ColorMask(ctx(), true, false, true, false);
    color(1, 1, 0, 0);
    quad(0, 0, kW, kH);
    cmd::ColorMask(ctx(), true, true, true, true);
    const Rgba masked = at(pixels(), 4, 4);
    EXPECT_EQ(masked, (Rgba{255, to8(0.4), 0, to8(0.8)}));

    // Polygon stipple in window coordinates: bit (x mod 32) of row (y mod 32).
    cmd::ClearColor(ctx(), 0, 0, 0, 1);
    clear();
    std::array<u32, 32> rows{};
    for (u32 y = 0; y < 32; ++y) {
        for (u32 x = 0; x < 32; ++x) rows[y] |= ((x * 7 + y * 3) % 5 == 0 ? 1u : 0u) << x;
    }
    cmd::PolygonStipple(ctx(), rows);
    cmd::Enable(ctx(), GL_POLYGON_STIPPLE);
    color(kRed);
    quad(0, 0, kW, kH);
    line(0.3, 20.5, 31.7, 20.5);   // lines are not polygon-stippled
    cmd::Disable(ctx(), GL_POLYGON_STIPPLE);
    PixelSet stippled;
    for (i32 y = 0; y < kH; ++y) {
        for (i32 x = 0; x < kW; ++x) {
            if ((rows[y % 32] >> (x % 32)) & 1u) stippled.insert({x, y});
        }
    }
    for (const Pixel& p : diamond_exit(0.3, 20.5, 31.7, 20.5)) stippled.insert(p);
    expect_coverage(pixels(), stippled, kRed, kBlack, "polygon stipple");
}

// ---- Logic op into both buffers (4.1.8, 4.2.1) -------------------------------------------------

TEST_P(GlRasterTest, LogicOpWritesEachBufferWithItsOwnDestination) {
    if (!graphics::MetalDevice::instance().supports_framebuffer_fetch()) GTEST_SKIP() << "no framebuffer fetch";
    cmd::DrawBuffer(ctx(), GL_FRONT);
    cmd::ClearColor(ctx(), 0x30 / 255.0f, 0xC0 / 255.0f, 0x0F / 255.0f, 1);
    cmd::Clear(ctx(), GL_COLOR_BUFFER_BIT);
    cmd::DrawBuffer(ctx(), GL_BACK);
    cmd::ClearColor(ctx(), 0xA5 / 255.0f, 0x5A / 255.0f, 0xFF / 255.0f, 1);
    cmd::Clear(ctx(), GL_COLOR_BUFFER_BIT);
    cmd::DrawBuffer(ctx(), GL_FRONT_AND_BACK);
    cmd::Enable(ctx(), GL_COLOR_LOGIC_OP);
    cmd::LogicOp(ctx(), GL_XOR);
    cmd::Enable(ctx(), GL_BLEND);   // bypassed while the logic op is on
    cmd::BlendFunc(ctx(), GL_ZERO, GL_ZERO);
    color(0xFF / 255.0, 0x0F / 255.0, 0x3C / 255.0, 0);
    quad(2, 2, 6, 6);
    cmd::Disable(ctx(), GL_COLOR_LOGIC_OP);
    cmd::DrawBuffer(ctx(), GL_BACK);
    const Rgba front = at(pixels(GL_FRONT), 3, 3), back = at(pixels(GL_BACK), 3, 3);
    EXPECT_EQ(front, (Rgba{0x30 ^ 0xFF, 0xC0 ^ 0x0F, 0x0F ^ 0x3C, 0xFF ^ 0x00}));
    EXPECT_EQ(back, (Rgba{0xA5 ^ 0xFF, 0x5A ^ 0x0F, 0xFF ^ 0x3C, 0xFF ^ 0x00}));
}

// ---- User clip planes (2.11) -------------------------------------------------------------------

TEST_P(GlRasterTest, UserClipPlanesClipEveryPrimitive) {
    // Keep x_e <= 10: (-1, 0, 0, 10) . (x, y, z, 1) >= 0.
    cmd::ClipPlane(ctx(), GL_CLIP_PLANE0, {-1, 0, 0, 10});
    cmd::Enable(ctx(), GL_CLIP_PLANE0);
    color(kRed);
    quad(0, 0, kW, 8);
    line(2.0, 12.5, 20.0, 12.5);
    cmd::PointSize(ctx(), 3);
    cmd::Begin(ctx(), GL_POINTS);
    vertex(12.5, 18.5);
    vertex(6.5, 18.5);
    cmd::End(ctx());
    cmd::Disable(ctx(), GL_CLIP_PLANE0);
    PixelSet want;
    for (i32 y = 0; y < 8; ++y) {
        for (i32 x = 0; x < 10; ++x) want.insert({x, y});
    }
    for (const Pixel& p : diamond_exit(2.0, 12.5, 10.0, 12.5)) want.insert(p);
    const PixelSet pt = point_square(6.5, 18.5, 3);
    want.insert(pt.begin(), pt.end());
    expect_coverage(pixels(), want, kRed, kBlack, "clip plane x <= 10");
}

// ---- Lighting (2.13) ---------------------------------------------------------------------------

struct SpecLight {
    std::array<double, 4> ambient, diffuse, specular, position;   // position in eye coordinates
    std::array<double, 3> spot_direction;
    double spot_exponent, spot_cutoff, k0, k1, k2;
};
struct SpecMaterial {
    std::array<double, 4> ambient, diffuse, specular, emission;
    double shininess;
};

std::array<double, 3> normalize3(std::array<double, 3> v) {
    const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return {v[0] / l, v[1] / l, v[2] / l};
}
double dot3(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/// Spec 2.13.1: c = e_cm + a_cm a_cs + sum att spot (a_cm a_cl + (n (.) VP)
/// d_cm d_cl + f (n (.) h)^s s_cm s_cl), clamped; alpha d_cm's.
std::array<double, 4> spec_lighting(const std::array<double, 3>& p, const std::array<double, 3>& n,
                                    const SpecMaterial& m, const std::array<double, 4>& scene_ambient,
                                    const std::vector<SpecLight>& lights, bool local_viewer) {
    std::array<double, 4> c{};
    for (usize k = 0; k < 3; ++k) c[k] = m.emission[k] + m.ambient[k] * scene_ambient[k];
    for (const SpecLight& l : lights) {
        std::array<double, 3> vp;
        double att = 1.0;
        if (l.position[3] != 0.0) {
            const std::array<double, 3> lp{l.position[0] / l.position[3], l.position[1] / l.position[3],
                                           l.position[2] / l.position[3]};
            const std::array<double, 3> d{lp[0] - p[0], lp[1] - p[1], lp[2] - p[2]};
            const double dist = std::sqrt(dot3(d, d));
            att = 1.0 / (l.k0 + l.k1 * dist + l.k2 * dist * dist);
            vp = normalize3(d);
        } else {
            vp = normalize3({l.position[0], l.position[1], l.position[2]});
        }
        double spot = 1.0;
        if (l.spot_cutoff != 180.0) {
            const double pvs = std::max(dot3({-vp[0], -vp[1], -vp[2]}, normalize3(l.spot_direction)), 0.0);
            spot = pvs >= std::cos(l.spot_cutoff * M_PI / 180.0) ? std::pow(pvs, l.spot_exponent) : 0.0;
        }
        const std::array<double, 3> ve = local_viewer ? normalize3({-p[0], -p[1], -p[2]}) : std::array<double, 3>{0, 0, 1};
        const std::array<double, 3> h = normalize3({vp[0] + ve[0], vp[1] + ve[1], vp[2] + ve[2]});
        const double ndvp = std::max(dot3(n, vp), 0.0);
        const double f = ndvp != 0.0 ? 1.0 : 0.0;
        const double ndh = std::max(dot3(n, h), 0.0);
        const double spec = m.shininess == 0.0 ? 1.0 : std::pow(ndh, m.shininess);
        for (usize k = 0; k < 3; ++k) {
            c[k] += att * spot *
                    (m.ambient[k] * l.ambient[k] + ndvp * m.diffuse[k] * l.diffuse[k] + f * spec * m.specular[k] * l.specular[k]);
        }
    }
    for (usize k = 0; k < 3; ++k) c[k] = std::clamp(c[k], 0.0, 1.0);
    c[3] = std::clamp(m.diffuse[3], 0.0, 1.0);
    return c;
}

TEST_P(GlRasterTest, LightingMaterialAndLightModelFollowSpec) {
    // Lights, set under a translated modelview: glLight stores the position
    // and spot direction in eye coordinates (2.13.2).
    std::vector<SpecLight> lights{
        {{0.1, 0.05, 0.0, 1}, {0.9, 0.8, 0.6, 1}, {1.0, 1.0, 0.9, 1}, {10, 5, 8, 1}, {0, 0, -1}, 3.0, 70.0, 1.0, 0.02, 0.003},
        {{0.0, 0.1, 0.1, 1}, {0.3, 0.5, 0.7, 1}, {0.6, 0.4, 0.2, 1}, {-1, 1, 1, 0}, {0, 0, -1}, 0.0, 180.0, 1, 0, 0},
    };
    const SpecMaterial mat{{0.2, 0.1, 0.1, 1}, {0.6, 0.5, 0.3, 0.8}, {0.5, 0.5, 0.5, 1}, {0.05, 0.02, 0.0, 1}, 10.0};
    const std::array<double, 4> scene{0.1, 0.1, 0.15, 1};
    cmd::PushMatrix(ctx());
    cmd::Translate(ctx(), 2, 0, 0);
    for (u32 i = 0; i < lights.size(); ++i) {
        const SpecLight& l = lights[i];
        const u32 id = GL_LIGHT0 + i;
        cmd::Light(ctx(), id, GL_AMBIENT, values({l.ambient[0], l.ambient[1], l.ambient[2], l.ambient[3]}));
        cmd::Light(ctx(), id, GL_DIFFUSE, values({l.diffuse[0], l.diffuse[1], l.diffuse[2], l.diffuse[3]}));
        cmd::Light(ctx(), id, GL_SPECULAR, values({l.specular[0], l.specular[1], l.specular[2], l.specular[3]}));
        // Object coordinates; the modelview translation by +2 in x applies
        // to points (w != 0) only.
        const double ox = l.position[3] != 0 ? l.position[0] - 2 * l.position[3] : l.position[0];
        cmd::Light(ctx(), id, GL_POSITION, values({ox, l.position[1], l.position[2], l.position[3]}));
        cmd::Light(ctx(), id, GL_SPOT_DIRECTION, values({l.spot_direction[0], l.spot_direction[1], l.spot_direction[2]}));
        cmd::Light(ctx(), id, GL_SPOT_EXPONENT, scalar(l.spot_exponent));
        cmd::Light(ctx(), id, GL_SPOT_CUTOFF, scalar(l.spot_cutoff));
        cmd::Light(ctx(), id, GL_CONSTANT_ATTENUATION, scalar(l.k0));
        cmd::Light(ctx(), id, GL_LINEAR_ATTENUATION, scalar(l.k1));
        cmd::Light(ctx(), id, GL_QUADRATIC_ATTENUATION, scalar(l.k2));
        cmd::Enable(ctx(), id);
    }
    cmd::PopMatrix(ctx());
    cmd::Material(ctx(), GL_FRONT, GL_AMBIENT, values({mat.ambient[0], mat.ambient[1], mat.ambient[2], mat.ambient[3]}));
    cmd::Material(ctx(), GL_FRONT, GL_DIFFUSE, values({mat.diffuse[0], mat.diffuse[1], mat.diffuse[2], mat.diffuse[3]}));
    cmd::Material(ctx(), GL_FRONT, GL_SPECULAR, values({mat.specular[0], mat.specular[1], mat.specular[2], mat.specular[3]}));
    cmd::Material(ctx(), GL_FRONT, GL_EMISSION, values({mat.emission[0], mat.emission[1], mat.emission[2], mat.emission[3]}));
    cmd::Material(ctx(), GL_FRONT, GL_SHININESS, scalar(mat.shininess));
    cmd::LightModel(ctx(), GL_LIGHT_MODEL_AMBIENT, values({scene[0], scene[1], scene[2], scene[3]}));
    cmd::Enable(ctx(), GL_LIGHTING);
    cmd::Enable(ctx(), GL_NORMALIZE);

    // Points in eye coordinates (identity modelview): one per pixel.
    struct Probe {
        std::array<double, 3> n;
        std::array<double, 4> color;   // the current colour (colour material)
    };
    std::vector<Probe> probes;
    for (i32 i = 0; i < 12; ++i) {
        const double a = i * 0.45;
        probes.push_back({normalize3({std::cos(a) * 0.7, std::sin(a) * 0.7, 0.6 + 0.03 * i}),
                          {0.1 + 0.07 * i, 0.9 - 0.06 * i, 0.4, 0.5 + 0.03 * i}});
    }
    struct Run {
        bool local_viewer;
        bool color_material;
    };
    const std::vector<Run> runs{{false, false}, {true, false}, {false, true}};
    for (usize r = 0; r < runs.size(); ++r) {
        cmd::LightModel(ctx(), GL_LIGHT_MODEL_LOCAL_VIEWER, scalar(runs[r].local_viewer ? 1 : 0));
        if (runs[r].color_material) {
            cmd::ColorMaterial(ctx(), GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
            cmd::Enable(ctx(), GL_COLOR_MATERIAL);
        }
        cmd::Begin(ctx(), GL_POINTS);
        for (usize i = 0; i < probes.size(); ++i) {
            const Probe& p = probes[i];
            color(p.color[0], p.color[1], p.color[2], p.color[3]);
            cmd::Normal(ctx(), static_cast<float>(p.n[0] * 2), static_cast<float>(p.n[1] * 2), static_cast<float>(p.n[2] * 2));
            vertex(2 * i + 0.5, 3 * r + 0.5, 0.25);
        }
        cmd::End(ctx());
        cmd::Disable(ctx(), GL_COLOR_MATERIAL);
        // glColorMaterial changed the front material: restore it.
        cmd::Material(ctx(), GL_FRONT, GL_AMBIENT, values({mat.ambient[0], mat.ambient[1], mat.ambient[2], mat.ambient[3]}));
        cmd::Material(ctx(), GL_FRONT, GL_DIFFUSE, values({mat.diffuse[0], mat.diffuse[1], mat.diffuse[2], mat.diffuse[3]}));
    }
    EXPECT_EQ(cmd::GetError(ctx()), GL_NO_ERROR);
    const auto px = pixels();
    for (usize r = 0; r < runs.size(); ++r) {
        for (usize i = 0; i < probes.size(); ++i) {
            SpecMaterial m = mat;
            if (runs[r].color_material) m.ambient = m.diffuse = probes[i].color;
            const std::array<double, 3> pos{2.0 * i + 0.5, 3.0 * r + 0.5, 0.25};
            const auto want = spec_lighting(pos, probes[i].n, m, scene, lights, runs[r].local_viewer);
            expect_near(at(px, static_cast<i32>(2 * i), static_cast<i32>(3 * r)), want,
                        "run " + std::to_string(r) + " probe " + std::to_string(i));
        }
    }
}

// Spec 2.13 lights each vertex; 3.4.1 interpolates the lit colours along the
// segment (eq. 3.1, all w = 1 here). Widths 1 and 3, a gradient from two
// different normals, one positional light with specular.
TEST_P(GlRasterTest, LitLinesInterpolateTheVerticesLitColours) {
    const std::vector<SpecLight> lights{
        {{0.1, 0.1, 0.1, 1}, {0.6, 0.4, 0.3, 1}, {0.3, 0.3, 0.3, 1}, {16, 12, 6, 1}, {0, 0, -1}, 0.0, 180.0, 1.0, 0.01, 0.0},
    };
    const SpecMaterial mat{{0.2, 0.2, 0.2, 1}, {0.6, 0.5, 0.8, 1}, {0.6, 0.6, 0.6, 1}, {0, 0, 0, 1}, 8.0};
    const std::array<double, 4> scene{0.1, 0.1, 0.1, 1};
    const SpecLight& l = lights[0];
    cmd::Light(ctx(), GL_LIGHT0, GL_AMBIENT, values({l.ambient[0], l.ambient[1], l.ambient[2], l.ambient[3]}));
    cmd::Light(ctx(), GL_LIGHT0, GL_DIFFUSE, values({l.diffuse[0], l.diffuse[1], l.diffuse[2], l.diffuse[3]}));
    cmd::Light(ctx(), GL_LIGHT0, GL_SPECULAR, values({l.specular[0], l.specular[1], l.specular[2], l.specular[3]}));
    cmd::Light(ctx(), GL_LIGHT0, GL_POSITION, values({l.position[0], l.position[1], l.position[2], l.position[3]}));
    cmd::Light(ctx(), GL_LIGHT0, GL_LINEAR_ATTENUATION, scalar(l.k1));
    cmd::Enable(ctx(), GL_LIGHT0);
    cmd::Material(ctx(), GL_FRONT, GL_AMBIENT, values({mat.ambient[0], mat.ambient[1], mat.ambient[2], 1}));
    cmd::Material(ctx(), GL_FRONT, GL_DIFFUSE, values({mat.diffuse[0], mat.diffuse[1], mat.diffuse[2], 1}));
    cmd::Material(ctx(), GL_FRONT, GL_SPECULAR, values({mat.specular[0], mat.specular[1], mat.specular[2], 1}));
    cmd::Material(ctx(), GL_FRONT, GL_SHININESS, scalar(mat.shininess));
    cmd::LightModel(ctx(), GL_LIGHT_MODEL_AMBIENT, values({scene[0], scene[1], scene[2], scene[3]}));
    cmd::Enable(ctx(), GL_LIGHTING);
    const double xa = 1.6, ya = 3.3, xb = 29.7, yb = 17.8, z = 0.2;
    const std::array<double, 3> na = normalize3({0.6, 0.3, 0.7}), nb = normalize3({0.2, 0.1, 0.97});
    const auto ca = spec_lighting({xa, ya, z}, na, mat, scene, lights, false);
    const auto cb = spec_lighting({xb, yb, z}, nb, mat, scene, lights, false);
    double spread = 0;
    for (usize k = 0; k < 3; ++k) spread = std::max(spread, std::abs(ca[k] - cb[k]));
    ASSERT_GT(spread, 0.15) << "the endpoints' lit colours differ visibly";
    for (float width : {1.0f, 3.0f}) {
        clear();
        cmd::LineWidth(ctx(), width);
        cmd::Begin(ctx(), GL_LINES);
        cmd::Normal(ctx(), static_cast<float>(na[0]), static_cast<float>(na[1]), static_cast<float>(na[2]));
        vertex(xa, ya, z);
        cmd::Normal(ctx(), static_cast<float>(nb[0]), static_cast<float>(nb[1]), static_cast<float>(nb[2]));
        vertex(xb, yb, z);
        cmd::End(ctx());
        const auto px = pixels();
        for (const auto& column : wide_line(xa, ya, xb, yb, width)) {
            for (const Pixel& p : column) {
                const double t = ((p.first + 0.5 - xa) * (xb - xa) + (p.second + 0.5 - ya) * (yb - ya)) /
                                 ((xb - xa) * (xb - xa) + (yb - ya) * (yb - ya));
                std::array<double, 4> want{};
                for (usize k = 0; k < 4; ++k) want[k] = std::clamp((1 - t) * ca[k] + t * cb[k], 0.0, 1.0);
                expect_near(at(px, p.first, p.second), want,
                            "lit width " + std::to_string(width) + " at " + std::to_string(p.first) + "," + std::to_string(p.second));
            }
        }
    }
}

// Spec 2.11 / 3.3: a point is clipped by its centre only, a line as a
// segment; the fragments of a wide point's square or a wide line's columns
// outside the viewport but inside the window are produced.
TEST_P(GlRasterTest, WidePrimitivesExtendPastTheViewport) {
    cmd::Viewport(ctx(), 8, 6, 12, 10);
    cmd::MatrixMode(ctx(), GL_PROJECTION);
    cmd::LoadIdentity(ctx());
    cmd::Ortho(ctx(), 0, 12, 0, 10, -1, 1);   // object = window - (8, 6)
    cmd::MatrixMode(ctx(), GL_MODELVIEW);
    color(kGreen);
    cmd::PointSize(ctx(), 5);
    cmd::Begin(ctx(), GL_POINTS);
    vertex(0.4, 5.3);    // window (8.4, 11.3): the square spans x 6..10
    vertex(-0.3, 2.2);   // centre left of the viewport: discarded
    cmd::End(ctx());
    cmd::LineWidth(ctx(), 5);
    line(2.2, 0.3, 10.6, 0.3);   // window y 6.3: the columns span rows 4..8
    PixelSet want = point_square(8.4, 11.3, 5);
    for (const auto& column : wide_line(10.2, 6.3, 18.6, 6.3, 5)) want.insert(column.begin(), column.end());
    ASSERT_TRUE(want.count({6, 11}) && want.count({12, 4})) << "the expected set leaves the viewport";
    expect_coverage(pixels(), in_window(want), kGreen, kBlack, "viewport (8, 6, 12, 10)");
}

// Spec 4.1.6: "If a color buffer has no A value, then it is as if the
// destination A value is 1."
TEST_P(GlRasterTest, DestinationAlphaIsOneWithoutAnAlphaBuffer) {
    use(kDcNoAlpha, kWindowNoAlpha);
    cmd::ClearColor(ctx(), 0.2f, 0.4f, 0.6f, 0.0f);
    clear();
    cmd::Enable(ctx(), GL_BLEND);
    color(1, 1, 1, 0.5);
    cmd::BlendFunc(ctx(), GL_DST_ALPHA, GL_ZERO);   // C = Cs * 1
    pixel_quad(1, 1);
    cmd::BlendFunc(ctx(), GL_ONE_MINUS_DST_ALPHA, GL_ONE);   // C = Cs * 0 + Cd
    pixel_quad(2, 1);
    cmd::BlendFunc(ctx(), GL_SRC_ALPHA_SATURATE, GL_ZERO);   // f = min(As, 1 - 1) = 0
    pixel_quad(3, 1);
    cmd::Disable(ctx(), GL_BLEND);
    const auto px = pixels();
    EXPECT_EQ(at(px, 1, 1), (Rgba{255, 255, 255, 255}));
    EXPECT_EQ(at(px, 2, 1), (Rgba{to8(0.2), to8(0.4), to8(0.6), 255}));
    EXPECT_EQ(at(px, 3, 1), (Rgba{0, 0, 0, 255}));
}

// Spec 3.5.5: "Fragment depth values are always limited to the range [0,1]"
// after the offset, also when glDepthRange is narrower: an offset polygon at
// the far value f = 0.75 lands beyond f.
TEST_P(GlRasterTest, PolygonOffsetDepthIsLimitedToZeroOneNotTheDepthRange) {
    cmd::DepthRange(ctx(), 0.25, 0.75);
    cmd::Enable(ctx(), GL_DEPTH_TEST);
    cmd::DepthFunc(ctx(), GL_ALWAYS);
    cmd::Enable(ctx(), GL_POLYGON_OFFSET_FILL);
    // Object z -0.9: z_d 0.9, window 0.25 + 0.5 * 1.9 / 2 = 0.725.
    cmd::PolygonOffset(ctx(), 0, static_cast<float>(1 << 20));   // + 2^20 * 2^-24 = 1/16
    quad(0, 0, 4, 4, -0.9);
    cmd::PolygonOffset(ctx(), 0, static_cast<float>(1 << 24));   // + 1: clamped to 1
    quad(4, 0, 8, 4, -0.9);
    const auto z = depths();
    EXPECT_NEAR(z[1 * kW + 1], 0.725 + 1.0 / 16.0, 1e-6);
    EXPECT_EQ(z[1 * kW + 5], 1.0f);
}

/// Column-major 4x4 helpers for the modelview cases (test-side math).
using M4 = std::array<double, 16>;
M4 mul4(const M4& a, const M4& b) {
    M4 r{};
    for (usize c = 0; c < 4; ++c) {
        for (usize rr = 0; rr < 4; ++rr) {
            for (usize k = 0; k < 4; ++k) r[c * 4 + rr] += a[k * 4 + rr] * b[c * 4 + k];
        }
    }
    return r;
}
M4 inverse4(const M4& m) {
    // Gauss-Jordan on the row-major form A[r][c] = m[c * 4 + r].
    double a[4][8]{};
    for (usize r = 0; r < 4; ++r) {
        for (usize c = 0; c < 4; ++c) a[r][c] = m[c * 4 + r];
        a[r][4 + r] = 1;
    }
    for (usize c = 0; c < 4; ++c) {
        usize p = c;
        for (usize r = c + 1; r < 4; ++r) {
            if (std::abs(a[r][c]) > std::abs(a[p][c])) p = r;
        }
        for (usize k = 0; k < 8; ++k) std::swap(a[c][k], a[p][k]);
        const double d = a[c][c];
        for (usize k = 0; k < 8; ++k) a[c][k] /= d;
        for (usize r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = a[r][c];
            for (usize k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
        }
    }
    M4 out{};
    for (usize r = 0; r < 4; ++r) {
        for (usize c = 0; c < 4; ++c) out[c * 4 + r] = a[r][4 + c];
    }
    return out;
}

// One oracle (the 2.13.1 sum, spec_lighting) for both lighting paths: the
// same probes drawn as points / polygon-mode vertices (lit per vertex on the
// CPU) and as small filled triangles (lit per vertex by the generated
// shader, the fragment at the pixel centre their barycentric combination,
// eq. 3.4 with w = 1). Point, spot (cutoff 30, and 90 with exponent 2 so
// "behind" is 0) and directional lights with attenuation; infinite and
// local viewer; identity and a non-identity modelview (n' = n M^-1, 2.10.3)
// with and without GL_NORMALIZE; colour material for FRONT, BACK and
// FRONT_AND_BACK; two-sided lighting with real lights on back faces (-n and
// the back material).
TEST_P(GlRasterTest, LightingCpuAndGpuPathsMatchOneOracle) {
    const std::vector<SpecLight> lights{
        {{0.05, 0.04, 0.03, 1}, {0.25, 0.22, 0.2, 1}, {0.2, 0.2, 0.15, 1}, {24, 6, 1.5, 1}, {0, 0, -1}, 0.0, 180.0, 0.5, 0.05, 0.004},
        {{0.03, 0.03, 0.05, 1}, {0.3, 0.25, 0.2, 1}, {0.25, 0.2, 0.2, 1}, {8, 20, 2, 1}, {0, -16, -2}, 5.0, 30.0, 1.0, 0.0, 0.0},
        {{0.02, 0.05, 0.02, 1}, {0.2, 0.3, 0.25, 1}, {0.15, 0.2, 0.2, 1}, {16, 12, 0.6, 1}, {1, 0, 0}, 2.0, 90.0, 1.0, 0.02, 0.0},
        {{0.04, 0.02, 0.02, 1}, {0.2, 0.2, 0.3, 1}, {0.2, 0.15, 0.25, 1}, {-0.3, 0.5, 1, 0}, {0, 0, -1}, 0.0, 180.0, 1, 0, 0},
    };
    const SpecMaterial front_mat{{0.2, 0.15, 0.1, 1}, {0.5, 0.6, 0.4, 0.9}, {0.4, 0.4, 0.5, 1}, {0.02, 0.0, 0.03, 1}, 12.0};
    const SpecMaterial back_mat{{0.1, 0.2, 0.2, 1}, {0.3, 0.4, 0.7, 0.7}, {0.2, 0.3, 0.2, 1}, {0.0, 0.02, 0.0, 1}, 5.0};
    const std::array<double, 4> scene{0.1, 0.08, 0.12, 1};

    // Lights in eye coordinates (identity modelview at glLight time).
    for (u32 i = 0; i < lights.size(); ++i) {
        const SpecLight& l = lights[i];
        const u32 id = GL_LIGHT0 + i;
        cmd::Light(ctx(), id, GL_AMBIENT, values({l.ambient[0], l.ambient[1], l.ambient[2], l.ambient[3]}));
        cmd::Light(ctx(), id, GL_DIFFUSE, values({l.diffuse[0], l.diffuse[1], l.diffuse[2], l.diffuse[3]}));
        cmd::Light(ctx(), id, GL_SPECULAR, values({l.specular[0], l.specular[1], l.specular[2], l.specular[3]}));
        cmd::Light(ctx(), id, GL_POSITION, values({l.position[0], l.position[1], l.position[2], l.position[3]}));
        cmd::Light(ctx(), id, GL_SPOT_DIRECTION, values({l.spot_direction[0], l.spot_direction[1], l.spot_direction[2]}));
        cmd::Light(ctx(), id, GL_SPOT_EXPONENT, scalar(l.spot_exponent));
        cmd::Light(ctx(), id, GL_SPOT_CUTOFF, scalar(l.spot_cutoff));
        cmd::Light(ctx(), id, GL_CONSTANT_ATTENUATION, scalar(l.k0));
        cmd::Light(ctx(), id, GL_LINEAR_ATTENUATION, scalar(l.k1));
        cmd::Light(ctx(), id, GL_QUADRATIC_ATTENUATION, scalar(l.k2));
        cmd::Enable(ctx(), id);
    }
    cmd::LightModel(ctx(), GL_LIGHT_MODEL_AMBIENT, values({scene[0], scene[1], scene[2], scene[3]}));
    cmd::Enable(ctx(), GL_LIGHTING);
    const auto set_material = [&](u32 face, const SpecMaterial& m) {
        cmd::Material(ctx(), face, GL_AMBIENT, values({m.ambient[0], m.ambient[1], m.ambient[2], m.ambient[3]}));
        cmd::Material(ctx(), face, GL_DIFFUSE, values({m.diffuse[0], m.diffuse[1], m.diffuse[2], m.diffuse[3]}));
        cmd::Material(ctx(), face, GL_SPECULAR, values({m.specular[0], m.specular[1], m.specular[2], m.specular[3]}));
        cmd::Material(ctx(), face, GL_EMISSION, values({m.emission[0], m.emission[1], m.emission[2], m.emission[3]}));
        cmd::Material(ctx(), face, GL_SHININESS, scalar(m.shininess));
    };

    // A rotation, non-uniform scale and translation: normals need n M^-1.
    const double a = 20 * M_PI / 180, b = 15 * M_PI / 180;
    const M4 rz{std::cos(a), std::sin(a), 0, 0, -std::sin(a), std::cos(a), 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const M4 rx{1, 0, 0, 0, 0, std::cos(b), std::sin(b), 0, 0, -std::sin(b), std::cos(b), 0, 0, 0, 0, 1};
    const M4 sc{1.3, 0, 0, 0, 0, 0.8, 0, 0, 0, 0, 1.1, 0, 0, 0, 0, 1};
    const M4 tr{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 2, 0, 1};
    const M4 model = mul4(tr, mul4(rz, mul4(rx, sc)));
    const M4 identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    struct Scenario {
        bool use_model, normalize, local_viewer, two_side, back;
        u32 cm_face, cm_param;   // cm_face 0: no colour material
    };
    const std::vector<Scenario> scenarios{
        {false, true, false, false, false, 0, 0},
        {false, true, true, false, false, 0, 0},
        {true, true, true, false, false, 0, 0},
        {true, false, false, false, false, 0, 0},
        {false, true, false, false, false, GL_FRONT, GL_AMBIENT_AND_DIFFUSE},
        {false, true, false, true, true, GL_BACK, GL_DIFFUSE},
        {true, true, true, true, true, GL_FRONT_AND_BACK, GL_SPECULAR},
        {true, true, false, true, false, GL_FRONT_AND_BACK, GL_EMISSION},
    };
    constexpr i32 kProbes = 12;
    const auto probe_color = [](i32 i) {
        return std::array<double, 4>{0.2 + 0.05 * i, 0.8 - 0.04 * i, 0.5, 0.6 + 0.03 * i};
    };
    const auto probe_normal = [](i32 i) {
        const double t = i * 0.5, len = 1.0 + 0.1 * i;   // not unit: GL_NORMALIZE matters
        return std::array<double, 3>{std::cos(t) * 0.6 * len, std::sin(t) * 0.6 * len, 0.7 * len};
    };
    // Spec 2.10.3 and the oracle for one vertex at eye position e.
    const auto expected = [&](const Scenario& sc_, const M4& mv, const std::array<double, 3>& e, i32 i) {
        const M4 inv = inverse4(mv);
        const auto no = probe_normal(i);
        std::array<double, 3> n{};
        for (usize j = 0; j < 3; ++j) n[j] = no[0] * inv[j * 4 + 0] + no[1] * inv[j * 4 + 1] + no[2] * inv[j * 4 + 2];
        if (sc_.normalize) n = normalize3(n);
        SpecMaterial m = sc_.back ? back_mat : front_mat;
        if (sc_.back) n = {-n[0], -n[1], -n[2]};
        const u32 face = sc_.back ? GL_BACK : GL_FRONT;
        if (sc_.cm_face == GL_FRONT_AND_BACK || sc_.cm_face == face) {
            const auto c = probe_color(i);
            if (sc_.cm_param == GL_AMBIENT_AND_DIFFUSE) m.ambient = m.diffuse = c;
            if (sc_.cm_param == GL_DIFFUSE) m.diffuse = c;
            if (sc_.cm_param == GL_SPECULAR) m.specular = c;
            if (sc_.cm_param == GL_EMISSION) m.emission = c;
        }
        // Without two-sided lighting a back face takes the front colour.
        return spec_lighting(e, n, m, scene, lights, sc_.local_viewer);
    };

    for (usize si = 0; si < scenarios.size(); ++si) {
        const Scenario& sc_ = scenarios[si];
        const M4& mv = sc_.use_model ? model : identity;
        const M4 inv = inverse4(mv);
        const auto object = [&](const std::array<double, 3>& e) {
            std::array<double, 3> o{};
            for (usize r = 0; r < 3; ++r) o[r] = inv[0 * 4 + r] * e[0] + inv[1 * 4 + r] * e[1] + inv[2 * 4 + r] * e[2] + inv[3 * 4 + r];
            return o;
        };
        clear();
        set_material(GL_FRONT, front_mat);
        set_material(GL_BACK, back_mat);
        cmd::LightModel(ctx(), GL_LIGHT_MODEL_LOCAL_VIEWER, scalar(sc_.local_viewer ? 1 : 0));
        cmd::LightModel(ctx(), GL_LIGHT_MODEL_TWO_SIDE, scalar(sc_.two_side ? 1 : 0));
        if (sc_.normalize) cmd::Enable(ctx(), GL_NORMALIZE);
        else cmd::Disable(ctx(), GL_NORMALIZE);
        if (sc_.cm_face != 0) {
            cmd::ColorMaterial(ctx(), sc_.cm_face, sc_.cm_param);
            cmd::Enable(ctx(), GL_COLOR_MATERIAL);
        }
        GlMatrix gm{};
        std::copy(mv.begin(), mv.end(), gm.begin());
        cmd::LoadMatrix(ctx(), gm);
        const auto emit = [&](const std::array<double, 3>& e, i32 i) {
            const auto c = probe_color(i);
            const auto n = probe_normal(i);
            color(c[0], c[1], c[2], c[3]);
            cmd::Normal(ctx(), static_cast<float>(n[0]), static_cast<float>(n[1]), static_cast<float>(n[2]));
            const auto o = object(e);
            vertex(o[0], o[1], o[2]);
        };
        // A probe's eye position (the pixel centre of column 2i, row 2 si).
        const auto eye_at = [&](i32 i, i32 row, double dx, double dy) {
            return std::array<double, 3>{2.0 * i + 0.5 + dx, row + 0.5 + dy, 0.25 + 0.02 * i};
        };
        const i32 cpu_row = static_cast<i32>(2 * si), gpu_row = cpu_row + 1;
        // CPU path: front probes as GL_POINTS; back probes as the visible
        // vertex of a back-facing triangle in GL_POINT mode (its other two
        // vertices lie outside the clip volume and are discarded).
        if (!sc_.back) {
            cmd::Begin(ctx(), GL_POINTS);
            for (i32 i = 0; i < kProbes; ++i) emit(eye_at(i, cpu_row, 0, 0), i);
            cmd::End(ctx());
        } else {
            cmd::PolygonMode(ctx(), GL_FRONT_AND_BACK, GL_POINT);
            cmd::Begin(ctx(), GL_TRIANGLES);
            for (i32 i = 0; i < kProbes; ++i) {
                emit(eye_at(i, cpu_row, 0, 0), i);
                emit(eye_at(i, cpu_row, 0, 100), i);   // clockwise: up, then right
                emit(eye_at(i, cpu_row, 100, 0), i);
            }
            cmd::End(ctx());
            cmd::PolygonMode(ctx(), GL_FRONT_AND_BACK, GL_FILL);
        }
        // GPU path: a small filled triangle around the pixel centre,
        // counter-clockwise (front) or clockwise (back).
        const std::array<std::array<double, 2>, 3> tri{{{-0.4, -0.3}, {0.4, -0.3}, {0.0, 0.4}}};
        cmd::Begin(ctx(), GL_TRIANGLES);
        for (i32 i = 0; i < kProbes; ++i) {
            for (i32 k = 0; k < 3; ++k) {
                const auto& d = tri[sc_.back ? (3 - k) % 3 : k];
                emit(eye_at(i, gpu_row, d[0], d[1]), i);
            }
        }
        cmd::End(ctx());
        cmd::Disable(ctx(), GL_COLOR_MATERIAL);
        cmd::LoadIdentity(ctx());
        EXPECT_EQ(cmd::GetError(ctx()), GL_NO_ERROR);

        const auto px = pixels();
        // Barycentric weights of the centre (0, 0) in the triangle.
        const double x0 = tri[0][0], y0 = tri[0][1], x1 = tri[1][0], y1 = tri[1][1], x2 = tri[2][0], y2 = tri[2][1];
        const double area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        const std::array<double, 3> w{((x1) * (y2) - (x2) * (y1)) / area, ((x2) * (y0) - (x0) * (y2)) / area,
                                      ((x0) * (y1) - (x1) * (y0)) / area};
        for (i32 i = 0; i < kProbes; ++i) {
            const std::string what = "scenario " + std::to_string(si) + " probe " + std::to_string(i);
            expect_near(at(px, 2 * i, cpu_row), expected(sc_, mv, eye_at(i, cpu_row, 0, 0), i), what + " (CPU)");
            std::array<double, 4> want{};
            for (i32 k = 0; k < 3; ++k) {
                const auto c = expected(sc_, mv, eye_at(i, gpu_row, tri[k][0], tri[k][1]), i);
                for (usize j = 0; j < 4; ++j) want[j] += w[k] * c[j];
            }
            expect_near(at(px, 2 * i, gpu_row), want, what + " (GPU)");
        }
    }
}

std::string bitness_name(const ::testing::TestParamInfo<bool>& info) { return info.param ? "x64" : "x86"; }
INSTANTIATE_TEST_SUITE_P(Bitness, GlRasterTest, ::testing::Values(false, true), bitness_name);

}  // namespace
