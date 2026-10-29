/// @file test_gl_draw.cpp
/// @brief OpenGL 1.1 drawing on the real Metal backend: immediate mode, glRect,
///        the client arrays, flat shading's provoking vertex, polygon
///        rasterization's pixel coverage and the batching of primitives into
///        one render pass (Design 1, Review Focus 1).
///
/// Kept out of vitreo_winapi_tests, which does not link the Metal backend.
/// Expected pixels come from the spec: a polygon covers the pixels whose
/// centres (x + 0.5, y + 0.5) lie inside it (spec 3.5.1 point sampling), and
/// a flat-shaded primitive takes the colour of its provoking vertex (2.13.7,
/// Table 2.9). Colours are 8-bit values c / 255, which the 8-bit buffers store
/// exactly. Window coordinates are GL's: (0, 0) is the bottom-left pixel.
/// With glOrtho(0, w, 0, h, -1, 1) and the initial viewport, object x/y are
/// window x/y.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
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

namespace {

constexpr i32 kDoubleAlpha = 13;   // double, alpha 8, depth 24, stencil 8 (gl_pixel_formats.cpp)
constexpr u64 kDc = 0x6000;
constexpr u64 kWindow = 0x200;
constexpr u32 kW = 16, kH = 8;
constexpr GuestAddr kMem = 0x10000;            // 0x10000 bytes, read/write
constexpr GuestAddr kMemEnd = kMem + 0x10000;  // first unmapped byte

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
constexpr Rgba kYellow{255, 255, 0, 255};
constexpr Rgba kCyan{0, 255, 255, 255};
constexpr Rgba kMagenta{255, 0, 255, 255};

/// The 2D edge function of the spec's point-sampling test: positive left of a->b.
double edge(const std::array<double, 2>& a, const std::array<double, 2>& b, double x, double y) {
    return (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
}

class GlDrawTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        if (!graphics::MetalDevice::instance().available()) GTEST_SKIP() << "no Metal device";
        graphics::set_active_backend_device(&graphics::MetalDevice::instance());
        ASSERT_TRUE(memory_.map_region(kMem, 0x10000, MemoryProtection::ReadWrite).is_ok());
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
                                       w = static_cast<i32>(kW);
                                       h = static_cast<i32>(kH);
                                       return true;
                                   });
        // No presentation: rendering into the drawable is all these tests read.
        mgr_->set_swap_chain_factory([](u64, u32, u32) { return std::unique_ptr<graphics::BackendSwapChain>(); });
        main_ = std::make_unique<Caller>(memory_, *registry_, GetParam(), 0);
        const u64 rc = gl().create(kDc);
        ASSERT_NE(rc, 0u);
        ASSERT_EQ(gl().make(kDc, rc), 1u);
        ctx_ = mgr_->context_for_testing(rc);
        ASSERT_NE(ctx_, nullptr);
        drawable_ = GlDrawable::for_window(kWindow);
        ASSERT_NE(drawable_, nullptr);
        // Object coordinates = window coordinates.
        cmd::MatrixMode(*ctx_, GL_PROJECTION);
        cmd::Ortho(*ctx_, 0, kW, 0, kH, -1, 1);
        cmd::MatrixMode(*ctx_, GL_MODELVIEW);
        cmd::ClearColor(*ctx_, 0, 0, 0, 1);
        cmd::Clear(*ctx_, GL_COLOR_BUFFER_BIT);
    }

    void TearDown() override {
        drawable_.reset();
        if (main_) main_->make(0, 0);
        set_current_gl_context_for_thread(nullptr);
        GlDrawable::detach_all();
        graphics::set_active_backend_device(nullptr);
    }

    Caller& gl() { return *main_; }
    GlContext& ctx() { return *ctx_; }
    u32 error() { return static_cast<u32>(gl().call("glGetError")); }

    /// The back buffer as RGBA, row 0 = the bottom row. Reading ends the pass.
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

    void color(const Rgba& c) { cmd::Color(ctx(), c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f); }
    void vertex(double x, double y) { cmd::Vertex(ctx(), static_cast<float>(x), static_cast<float>(y), 0, 1); }

    void put_floats(GuestAddr at, std::initializer_list<float> v) {
        u32 k = 0;
        for (float x : v) {
            u32 b;
            std::memcpy(&b, &x, 4);
            (void)memory_.write_u32(at + 4 * k++, b);
        }
    }
    void put_bytes(GuestAddr at, std::initializer_list<u8> v) {
        u32 k = 0;
        for (u8 x : v) (void)memory_.write_u8(at + k++, x);
    }

    GuestMemory memory_;
    std::unique_ptr<ThunkRegistry> registry_;
    std::unique_ptr<Opengl32Manager> mgr_;
    std::unique_ptr<Caller> main_;
    GlContext* ctx_{nullptr};
    std::shared_ptr<GlDrawable> drawable_;
};

// Review Focus 1: 1,000 glBegin(GL_QUADS)/glEnd pairs with unchanged state
// append to one batch: one render pass, one pipeline creation, one draw. Each
// quad covers one pixel (centre inside); the last quad at a pixel wins.
TEST_P(GlDrawTest, ThousandQuadsAreOneRenderPass) {
    (void)pixels();   // settle the clear
    const u32 passes = drawable_->passes_opened();
    const GlBatchStats before = gl_batch_stats(ctx());
    for (u32 k = 0; k < 1000; ++k) {
        const u32 x = k % kW, y = (k / kW) % kH;
        gl().call("glBegin", {GL_QUADS});
        gl().call("glColor3ub", {k % 256, (k / 256) * 60, 200});
        gl().call("glVertex2i", {x, y});
        gl().call("glVertex2i", {x + 1, y});
        gl().call("glVertex2i", {x + 1, y + 1});
        gl().call("glVertex2i", {x, y + 1});
        gl().call("glEnd");
    }
    EXPECT_EQ(error(), GL_NO_ERROR);
    const std::vector<Rgba> px = pixels();
    const GlBatchStats after = gl_batch_stats(ctx());
    EXPECT_EQ(drawable_->passes_opened() - passes, 1u);
    EXPECT_LE(after.pipelines_created - before.pipelines_created, 1u);
    EXPECT_EQ(after.draws_recorded - before.draws_recorded, 1u);
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const u32 p = y * kW + x;
            const u32 k = p + ((999 - p) / 128) * 128;   // the last quad at this pixel
            EXPECT_EQ(at(px, x, y), (Rgba{static_cast<u8>(k % 256), static_cast<u8>((k / 256) * 60), 200, 255}))
                << x << "," << y;
        }
    }
}

// Design 1: a state change between primitives flushes the open draw into the
// same pass (it does not end the pass); a change that leaves the pipeline as it
// was reuses the cached pipeline.
TEST_P(GlDrawTest, StateChangesFlushIntoTheSamePass) {
    (void)pixels();
    const u32 passes = drawable_->passes_opened();
    const GlBatchStats before = gl_batch_stats(ctx());
    auto quad = [&](double x0, double y0) {
        cmd::Begin(ctx(), GL_QUADS);
        vertex(x0, y0);
        vertex(x0 + 2, y0);
        vertex(x0 + 2, y0 + 2);
        vertex(x0, y0 + 2);
        cmd::End(ctx());
    };
    color(kRed);
    quad(0, 0);
    color(kGreen);   // a current value: same draw
    quad(2, 0);
    cmd::Translate(ctx(), 4, 0, 0);   // a uniform change: new draw, same pipeline
    color(kBlue);
    quad(0, 0);
    cmd::ShadeModel(ctx(), GL_FLAT);  // a pipeline change: new draw, new pipeline
    color(kYellow);
    quad(2, 0);
    EXPECT_EQ(error(), GL_NO_ERROR);
    const std::vector<Rgba> px = pixels();
    const GlBatchStats after = gl_batch_stats(ctx());
    EXPECT_EQ(drawable_->passes_opened() - passes, 1u);
    EXPECT_EQ(after.draws_recorded - before.draws_recorded, 3u);
    EXPECT_EQ(after.pipelines_created - before.pipelines_created, 2u);
    EXPECT_EQ(at(px, 1, 1), kRed);
    EXPECT_EQ(at(px, 3, 1), kGreen);
    EXPECT_EQ(at(px, 5, 1), kBlue);
    EXPECT_EQ(at(px, 7, 1), kYellow);
    EXPECT_EQ(at(px, 9, 1), kBlack);
}

// A pending draw is recorded before anything that touches the buffers after
// it: glClear (spec 4.2.3 clears what was drawn before it) and the draw-buffer
// change.
TEST_P(GlDrawTest, PendingDrawsKeepTheirOrderWithClears) {
    color(kRed);
    gl().call("glRecti", {0, 0, 4, 4});
    cmd::ClearColor(ctx(), 0, 0, 1, 1);
    cmd::Clear(ctx(), GL_COLOR_BUFFER_BIT);
    color(kGreen);
    gl().call("glRecti", {4, 0, 8, 4});
    const std::vector<Rgba> px = pixels();
    EXPECT_EQ(at(px, 1, 1), kBlue);   // drawn, then cleared
    EXPECT_EQ(at(px, 5, 1), kGreen);
    // GL_FRONT_AND_BACK after a pending draw to GL_BACK only.
    color(kMagenta);
    gl().call("glRecti", {0, 4, 4, 8});
    cmd::DrawBuffer(ctx(), GL_FRONT);
    color(kCyan);
    gl().call("glRecti", {4, 4, 8, 8});
    cmd::DrawBuffer(ctx(), GL_BACK);
    const std::vector<Rgba> back = pixels();
    EXPECT_EQ(at(back, 1, 5), kMagenta);
    EXPECT_EQ(at(back, 5, 5), kBlue);
    std::vector<u8> front;
    ASSERT_TRUE(drawable_->read_color(GL_FRONT, front));
    const u8* p = &front[(static_cast<usize>(kH - 1 - 5) * kW + 5) * 4u];
    EXPECT_EQ((Rgba{p[2], p[1], p[0], p[3]}), kCyan);
    // Spec 4.2.1: GL_FRONT_AND_BACK writes every fragment to both buffers.
    cmd::DrawBuffer(ctx(), GL_FRONT_AND_BACK);
    color(kYellow);
    gl().call("glRecti", {8, 0, 12, 4});
    cmd::DrawBuffer(ctx(), GL_BACK);
    EXPECT_EQ(at(pixels(), 9, 1), kYellow);
    ASSERT_TRUE(drawable_->read_color(GL_FRONT, front));
    p = &front[(static_cast<usize>(kH - 1 - 1) * kW + 9) * 4u];
    EXPECT_EQ((Rgba{p[2], p[1], p[0], p[3]}), kYellow);
    EXPECT_EQ(error(), GL_NO_ERROR);
}

// Spec 3.5.1: a polygon produces the fragments whose pixel centres lie inside
// it; flat shading gives every fragment the provoking vertex's colour (the
// last vertex of an independent triangle, the first of a polygon). The
// vertices are on the 1/8-pixel grid, so GL's 3 subpixel bits
// (get-GL_SUBPIXEL_BITS) and Metal's snapping place them exactly.
TEST_P(GlDrawTest, FlatTriangleCoversPixelCentersPerSpec) {
    const std::array<std::array<double, 2>, 3> tri{{{0.625, 0.375}, {15.375, 2.875}, {4.125, 7.875}}};
    std::vector<bool> inside(kW * kH);
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const double cx = x + 0.5, cy = y + 0.5;
            const double e0 = edge(tri[0], tri[1], cx, cy), e1 = edge(tri[1], tri[2], cx, cy),
                         e2 = edge(tri[2], tri[0], cx, cy);
            // No centre on an edge, so the tie rule never decides a pixel.
            ASSERT_TRUE(e0 != 0 && e1 != 0 && e2 != 0) << x << "," << y;
            inside[y * kW + x] = (e0 > 0 && e1 > 0 && e2 > 0) || (e0 < 0 && e1 < 0 && e2 < 0);
        }
    }
    cmd::ShadeModel(ctx(), GL_FLAT);
    for (u32 mode : {GL_TRIANGLES, GL_POLYGON}) {
        cmd::Clear(ctx(), GL_COLOR_BUFFER_BIT);
        cmd::Begin(ctx(), mode);
        color(kRed);
        vertex(tri[0][0], tri[0][1]);
        color(kGreen);
        vertex(tri[1][0], tri[1][1]);
        color(kBlue);
        vertex(tri[2][0], tri[2][1]);
        cmd::End(ctx());
        const Rgba want = mode == GL_POLYGON ? kRed : kBlue;
        const std::vector<Rgba> px = pixels();
        u32 covered = 0;
        for (u32 y = 0; y < kH; ++y) {
            for (u32 x = 0; x < kW; ++x) {
                covered += inside[y * kW + x] ? 1 : 0;
                EXPECT_EQ(at(px, x, y), inside[y * kW + x] ? want : kBlack) << "mode " << mode << " " << x << "," << y;
            }
        }
        EXPECT_GT(covered, 30u);
    }
    // Clockwise: coverage does not depend on the winding.
    cmd::Clear(ctx(), GL_COLOR_BUFFER_BIT);
    cmd::Begin(ctx(), GL_TRIANGLES);
    color(kGreen);
    vertex(tri[2][0], tri[2][1]);
    vertex(tri[1][0], tri[1][1]);
    vertex(tri[0][0], tri[0][1]);
    cmd::End(ctx());
    const std::vector<Rgba> px = pixels();
    for (u32 i = 0; i < kW * kH; ++i) EXPECT_EQ(px[i], inside[i] ? kGreen : kBlack) << i;
    EXPECT_EQ(error(), GL_NO_ERROR);
}

// Table 2.9, every primitive mode: with GL_FLAT each primitive is the colour of
// its provoking vertex. Sample pixels lie well inside one primitive.
TEST_P(GlDrawTest, FlatShadingUsesGlProvokingVertex) {
    cmd::ShadeModel(ctx(), GL_FLAT);
    const std::array<Rgba, 6> c{kRed, kGreen, kBlue, kYellow, kCyan, kMagenta};
    struct V {
        double x, y;
    };
    struct Sample {
        u32 x, y;
        Rgba want;
    };
    struct Case {
        u32 mode;
        std::vector<V> v;
        std::vector<Sample> samples;
    };
    const std::vector<Case> cases = {
        {GL_POINTS, {{3.5, 3.5}, {9.5, 5.5}}, {{3, 3, c[0]}, {9, 5, c[1]}}},
        {GL_LINES, {{0.5, 1.5}, {15.5, 1.5}, {0.5, 5.5}, {15.5, 5.5}}, {{8, 1, c[1]}, {8, 5, c[3]}}},
        {GL_LINE_STRIP, {{0.5, 1.5}, {12.5, 1.5}, {12.5, 7.5}}, {{6, 1, c[1]}, {12, 4, c[2]}}},
        {GL_LINE_LOOP,
         {{1.5, 1.5}, {13.5, 1.5}, {13.5, 6.5}, {1.5, 6.5}},
         {{7, 1, c[1]}, {13, 4, c[2]}, {7, 6, c[3]}, {1, 4, c[0]}}},
        {GL_TRIANGLES, {{0, 0}, {16, 0}, {0, 8}, {16, 0}, {16, 8}, {0, 8}}, {{2, 2, c[2]}, {13, 6, c[5]}}},
        {GL_TRIANGLE_STRIP, {{0, 0}, {0, 8}, {8, 0}, {8, 8}}, {{1, 1, c[2]}, {6, 6, c[3]}}},
        {GL_TRIANGLE_FAN, {{8, 4}, {0, 0}, {16, 0}, {16, 8}}, {{8, 1, c[2]}, {14, 4, c[3]}}},
        {GL_QUADS, {{2, 2}, {10, 2}, {10, 6}, {2, 6}}, {{3, 3, c[3]}, {9, 5, c[3]}, {3, 5, c[3]}, {9, 3, c[3]}}},
        {GL_QUAD_STRIP,
         {{0, 0}, {0, 8}, {8, 0}, {8, 8}, {16, 0}, {16, 8}},
         {{1, 1, c[3]}, {6, 6, c[3]}, {9, 1, c[5]}, {14, 6, c[5]}}},
        {GL_POLYGON, {{1, 1}, {15, 1}, {15, 7}, {8, 7.5}, {1, 7}}, {{2, 2, c[0]}, {14, 6, c[0]}, {8, 6, c[0]}}},
    };
    for (const Case& k : cases) {
        cmd::Clear(ctx(), GL_COLOR_BUFFER_BIT);
        cmd::Begin(ctx(), k.mode);
        for (usize i = 0; i < k.v.size(); ++i) {
            color(c[i]);
            vertex(k.v[i].x, k.v[i].y);
        }
        cmd::End(ctx());
        const std::vector<Rgba> px = pixels();
        for (const Sample& s : k.samples) {
            EXPECT_EQ(at(px, s.x, s.y), s.want) << "mode " << k.mode << " at " << s.x << "," << s.y;
        }
    }
    EXPECT_EQ(error(), GL_NO_ERROR);
}

// Smooth shading interpolates (spec 3.5.1 barycentric interpolation); here a
// horizontal ramp across a rectangle stays monotonic and spans its colours.
TEST_P(GlDrawTest, SmoothShadingInterpolatesAcrossThePrimitive) {
    cmd::Begin(ctx(), GL_QUADS);
    color(kBlack);
    vertex(0, 0);
    color(kRed);
    vertex(16, 0);
    vertex(16, 8);
    color(kBlack);
    vertex(0, 8);
    cmd::End(ctx());
    const std::vector<Rgba> px = pixels();
    for (u32 x = 0; x < kW; ++x) {
        // r = 255 * (x + 0.5) / 16 at the pixel centre, within one unit.
        const double want = 255.0 * (x + 0.5) / 16.0;
        EXPECT_NEAR(at(px, x, 4).r, want, 1.01) << x;
        EXPECT_EQ(at(px, x, 4).g, 0);
    }
}

// Spec 2.9: glRect(x1, y1, x2, y2) is the polygon (x1,y1) (x2,y1) (x2,y2)
// (x1,y2); its pixels are those with centres inside.
TEST_P(GlDrawTest, RectCoversItsPixels) {
    color(kGreen);
    gl().call("glRecti", {2, 1, 7, 4});
    put_floats(kMem + 0x100, {12, 7, 10, 5});   // glRectfv corners in any order
    color(kRed);
    gl().call("glRectfv", {kMem + 0x100, kMem + 0x108});
    const std::vector<Rgba> px = pixels();
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            Rgba want = kBlack;
            if (x >= 2 && x < 7 && y >= 1 && y < 4) want = kGreen;
            if (x >= 10 && x < 12 && y >= 5 && y < 7) want = kRed;
            EXPECT_EQ(at(px, x, y), want) << x << "," << y;
        }
    }
    EXPECT_EQ(error(), GL_NO_ERROR);
}

// Spec 2.10: (x, y, z, w) object coordinates are homogeneous; a vertex with
// w = 2 at (2x, 2y) is the same point as (x, y).
TEST_P(GlDrawTest, HomogeneousVerticesDivideByW) {
    color(kBlue);
    cmd::Begin(ctx(), GL_QUADS);
    cmd::Vertex(ctx(), 4, 2, 0, 2);
    cmd::Vertex(ctx(), 12, 2, 0, 2);
    cmd::Vertex(ctx(), 12, 6, 0, 2);
    cmd::Vertex(ctx(), 4, 6, 0, 2);
    cmd::End(ctx());
    const std::vector<Rgba> px = pixels();
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const bool in = x >= 2 && x < 6 && y >= 1 && y < 3;
            EXPECT_EQ(at(px, x, y), in ? kBlue : kBlack) << x << "," << y;
        }
    }
}

// GL's side of the generator's matrix-convention coupling (ff_emulation.hpp;
// FFEmulationGl.MatrixApplicationConventionIsPinned is the generator's side):
// spec 2.10.2 glTranslate multiplies the modelview by T, so object (x, y)
// lands at window (x + 5, y + 2). Transposing the matrices in to_ff, or the
// generator changing its application form alone, moves or loses the quad.
TEST_P(GlDrawTest, ModelviewTranslateLandsWhereTheSpecSays) {
    cmd::Translate(ctx(), 5, 2, 0);
    color(kGreen);
    gl().call("glRecti", {1, 1, 4, 3});
    const std::vector<Rgba> px = pixels();
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const bool in = x >= 6 && x < 9 && y >= 3 && y < 5;
            EXPECT_EQ(at(px, x, y), in ? kGreen : kBlack) << x << "," << y;
        }
    }
}

// The D3D matrix-convention fix (Task 8b) must leave GL where the spec puts
// it: glFrustum (spec 2.10.2), glTranslate and glRotate (the spec's R = u u^T
// + cos(a) (I - u u^T) + sin(a) S) compose clip = P * MV * object with column
// vectors; window x = (x_d + 1) w / 2, y = (y_d + 1) h / 2 (2.10.1), row 0 at
// the bottom. Evaluated on the CPU in double precision.
TEST_P(GlDrawTest, GlTransformsUnchangedByD3DConventionFix) {
    using M = std::array<std::array<double, 4>, 4>;
    const auto mul = [](const M& a, const M& b) {
        M r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k) r[i][j] += a[i][k] * b[k][j];
        return r;
    };
    const double l = -0.6, r = 0.4, b = -0.2, t = 0.3, n = 1.0, f = 20.0;
    const M proj{{{2 * n / (r - l), 0, (r + l) / (r - l), 0},
                  {0, 2 * n / (t - b), (t + b) / (t - b), 0},
                  {0, 0, -(f + n) / (f - n), -2 * f * n / (f - n)},
                  {0, 0, -1, 0}}};
    const M trans{{{1, 0, 0, 0.2}, {0, 1, 0, -0.1}, {0, 0, 1, -3}, {0, 0, 0, 1}}};
    const double angle = 35.0 * 3.14159265358979323846 / 180.0;
    const double len = std::sqrt(0.3 * 0.3 + 1.0 + 0.2 * 0.2);
    const double ux = 0.3 / len, uy = 1.0 / len, uz = 0.2 / len;
    const double c = std::cos(angle), s = std::sin(angle);
    const double u[3] = {ux, uy, uz};
    const double S[3][3] = {{0, -uz, uy}, {uz, 0, -ux}, {-uy, ux, 0}};
    M rot{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) rot[i][j] = u[i] * u[j] + c * ((i == j ? 1.0 : 0.0) - u[i] * u[j]) + s * S[i][j];
    rot[3][3] = 1;
    const M clip_m = mul(proj, mul(trans, rot));

    cmd::MatrixMode(ctx(), GL_PROJECTION);
    cmd::LoadIdentity(ctx());
    cmd::Frustum(ctx(), l, r, b, t, n, f);
    cmd::MatrixMode(ctx(), GL_MODELVIEW);
    cmd::LoadIdentity(ctx());
    cmd::Translate(ctx(), 0.2, -0.1, -3);
    cmd::Rotate(ctx(), 35.0, 0.3, 1.0, 0.2);

    const std::array<std::array<double, 3>, 3> obj{{{-1.4, -0.9, 0.3}, {1.3, -0.4, -0.5}, {-0.2, 1.0, 0.2}}};
    std::array<std::array<double, 2>, 3> tri{};
    for (int i = 0; i < 3; ++i) {
        std::array<double, 4> clip{};
        for (int j = 0; j < 4; ++j)
            clip[j] = clip_m[j][0] * obj[i][0] + clip_m[j][1] * obj[i][1] + clip_m[j][2] * obj[i][2] + clip_m[j][3];
        tri[i] = {(clip[0] / clip[3] + 1) * kW / 2.0, (clip[1] / clip[3] + 1) * kH / 2.0};
    }
    color(kGreen);
    cmd::Begin(ctx(), GL_TRIANGLES);
    for (const auto& p : obj) cmd::Vertex(ctx(), static_cast<float>(p[0]), static_cast<float>(p[1]),
                                          static_cast<float>(p[2]), 1);
    cmd::End(ctx());
    const std::vector<Rgba> px = pixels();
    u32 inside_count = 0, outside_count = 0;
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const double cx = x + 0.5, cy = y + 0.5;
            const auto dist = [&](const std::array<double, 2>& a, const std::array<double, 2>& bb) {
                return edge(a, bb, cx, cy) / std::hypot(bb[0] - a[0], bb[1] - a[1]);
            };
            const double e0 = dist(tri[0], tri[1]), e1 = dist(tri[1], tri[2]), e2 = dist(tri[2], tri[0]);
            if (std::min({std::abs(e0), std::abs(e1), std::abs(e2)}) < 0.01) continue;
            const bool in = (e0 > 0 && e1 > 0 && e2 > 0) || (e0 < 0 && e1 < 0 && e2 < 0);
            (in ? inside_count : outside_count)++;
            EXPECT_EQ(at(px, x, y), in ? kGreen : kBlack) << x << "," << y;
        }
    }
    EXPECT_GT(inside_count, 20u);
    EXPECT_GT(outside_count, 20u);
    EXPECT_EQ(error(), GL_NO_ERROR);
}

// Spec 2.11/2.10: primitives are clipped in clip coordinates, before the
// division by w. A vertex at w = 0 is a point at infinity: with the
// orthographic projection (clip w = object w) the triangle A, B, (0, 1, 0, 0)
// is the half-strip {P + t (0, 1) : P on AB, t >= 0}.
TEST_P(GlDrawTest, VertexAtWZeroClipsAsAPointAtInfinity) {
    color(kRed);
    cmd::Begin(ctx(), GL_TRIANGLES);
    cmd::Vertex(ctx(), 2, 1, 0, 1);
    cmd::Vertex(ctx(), 6, 1, 0, 1);
    cmd::Vertex(ctx(), 0, 1, 0, 0);
    cmd::End(ctx());
    const std::vector<Rgba> px = pixels();
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const bool in = x >= 2 && x < 6 && y >= 1;
            EXPECT_EQ(at(px, x, y), in ? kRed : kBlack) << x << "," << y;
        }
    }
    EXPECT_EQ(error(), GL_NO_ERROR);
}

// A primitive whose vertices' w have mixed signs: only its w > 0 part is
// inside the clip volume. With C = -(4, 0, 0, 1) the visible part is
// {P + t (P - (4, 0)) : P on AB, t >= 0}: rays from AB away from (4, 0),
// i.e. y >= 1 and 3 - 2y <= x_centre <= 2y + 5 (2 * (y - 1) on each side).
TEST_P(GlDrawTest, MixedSignWPrimitiveKeepsOnlyItsPositivePart) {
    color(kCyan);
    cmd::Begin(ctx(), GL_TRIANGLES);
    cmd::Vertex(ctx(), 2, 1, 0, 1);
    cmd::Vertex(ctx(), 6, 1, 0, 1);
    cmd::Vertex(ctx(), -4, 0, 0, -1);
    cmd::End(ctx());
    const std::vector<Rgba> px = pixels();
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const double cx = x + 0.5, cy = y + 0.5;
            const double t = cy - 1.0;   // the ray parameter that reaches this height, times 1
            const bool in = cy > 1.0 && cx > 2.0 - 2.0 * t && cx < 6.0 + 2.0 * t;
            EXPECT_EQ(at(px, x, y), in ? kCyan : kBlack) << x << "," << y;
        }
    }
    EXPECT_EQ(error(), GL_NO_ERROR);
}

// Spec 2.8: the arrays are read when the draw runs; changing the memory after
// glVertexPointer changes what is drawn. glDrawElements and
// glInterleavedArrays draw the same elements.
TEST_P(GlDrawTest, ClientArraysReadAtDrawTime) {
    const GuestAddr verts = kMem + 0x200, colors = kMem + 0x300;
    put_floats(verts, {0, 0, 2, 0, 2, 2, 0, 2});
    put_bytes(colors, {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255});
    gl().call("glVertexPointer", {2, GL_FLOAT, 0, verts});
    gl().call("glColorPointer", {4, GL_UNSIGNED_BYTE, 0, colors});
    gl().call("glEnableClientState", {GL_VERTEX_ARRAY});
    gl().call("glEnableClientState", {GL_COLOR_ARRAY});
    put_floats(verts, {4, 0, 6, 0, 6, 2, 4, 2});   // after the pointer
    gl().call("glDrawArrays", {GL_QUADS, 0, 4});
    // glDrawElements through a ubyte index list (two triangles of a quad).
    const GuestAddr verts2 = kMem + 0x400, idx = kMem + 0x500;
    put_floats(verts2, {8, 0, 10, 0, 10, 2, 8, 2});
    put_bytes(idx, {0, 1, 2, 0, 2, 3});
    gl().call("glVertexPointer", {2, GL_FLOAT, 0, verts2});
    put_bytes(colors, {0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255});
    gl().call("glDrawElements", {GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, idx});
    // glInterleavedArrays(GL_C4UB_V2F): 4 colour bytes then 2 floats.
    const GuestAddr inter = kMem + 0x600;
    const float xs[4] = {12, 14, 14, 12}, ys[4] = {0, 0, 2, 2};
    for (u32 k = 0; k < 4; ++k) {
        put_bytes(inter + k * 12, {0, 0, 255, 255});
        put_floats(inter + k * 12 + 4, {xs[k], ys[k]});
    }
    gl().call("glInterleavedArrays", {GL_C4UB_V2F, 0, inter});
    gl().call("glDrawArrays", {GL_QUADS, 0, 4});
    EXPECT_EQ(error(), GL_NO_ERROR);
    const std::vector<Rgba> px = pixels();
    EXPECT_EQ(at(px, 1, 1), kBlack);   // the pointer's first contents are never drawn
    EXPECT_EQ(at(px, 5, 1), kRed);
    EXPECT_EQ(at(px, 9, 1), kGreen);
    EXPECT_EQ(at(px, 13, 1), kBlue);
    EXPECT_EQ(at(px, 5, 4), kBlack);
}

// Ruling 3: a draw whose arrays reach memory the guest has not committed
// records INVALID_OPERATION and draws nothing; the host never reads it.
TEST_P(GlDrawTest, ClientArrayOutsideCommittedMemoryIsRejected) {
    (void)pixels();
    const GlBatchStats before = gl_batch_stats(ctx());
    const GuestAddr verts = kMemEnd - 24;   // three float2 vertices fit, the fourth does not
    put_floats(verts, {0, 0, 16, 0, 16, 8});
    gl().call("glVertexPointer", {2, GL_FLOAT, 0, verts});
    gl().call("glEnableClientState", {GL_VERTEX_ARRAY});
    color(kRed);
    gl().call("glDrawArrays", {GL_QUADS, 0, 4});
    EXPECT_EQ(error(), GL_INVALID_OPERATION);
    const GuestAddr idx = kMem + 0x100;
    put_bytes(idx, {0, 1, 3});
    gl().call("glDrawElements", {GL_TRIANGLES, 3, GL_UNSIGNED_BYTE, idx});
    EXPECT_EQ(error(), GL_INVALID_OPERATION);
    const std::vector<Rgba> px = pixels();
    for (const Rgba& p : px) ASSERT_EQ(p, kBlack);
    EXPECT_EQ(gl_batch_stats(ctx()).draws_recorded, before.draws_recorded);
    // The three vertices that are committed draw.
    gl().call("glDrawArrays", {GL_TRIANGLES, 0, 3});
    EXPECT_EQ(error(), GL_NO_ERROR);
    EXPECT_EQ(at(pixels(), 8, 1), kRed);
}

std::string bitness_name(const ::testing::TestParamInfo<bool>& info) { return info.param ? "x64" : "x86"; }
INSTANTIATE_TEST_SUITE_P(Bitness, GlDrawTest, ::testing::Values(false, true), bitness_name);

}  // namespace
