/// @file test_gl_display_list_metal.cpp
/// @brief OpenGL 1.1 display lists replayed on the real Metal backend: a list
///        of 10,000 immediate-mode vertices batches exactly as the same
///        commands issued directly (one pass, the same draws, no new
///        pipelines) and draws the same pixels; vertex arrays compiled into a
///        list draw the values they held at compile time (spec 5.4).
///
/// Kept out of vitreo_winapi_tests, which does not link the Metal backend.
/// Commands go through the registered exports (recording happens there), in
/// both bitnesses, with integer forms only (the caller passes integer
/// registers). Expected pixels come from the spec's rules evaluated here:
/// each 1x1 quad covers the pixel whose centre it contains, the last quad
/// drawn over a pixel wins (no depth test, spec 2.5 command order).

#include <gtest/gtest.h>

#include <array>
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
constexpr i32 kW = 32, kH = 32;
constexpr GuestAddr kArrays = 0x14000;

struct Rgba {
    u8 r, g, b, a;
    bool operator==(const Rgba&) const = default;
};

class GlDisplayListMetalTest : public ::testing::TestWithParam<bool> {
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
        gl_ = std::make_unique<Caller>(memory_, *registry_, GetParam(), 0);
        rc_ = gl_->create(kDc);
        ASSERT_NE(rc_, 0u);
        ASSERT_EQ(gl_->make(kDc, rc_), 1u);
        ctx_ = mgr_->context_for_testing(rc_);
        ASSERT_NE(ctx_, nullptr);
        drawable_ = GlDrawable::for_window(kWindow);
        ASSERT_NE(drawable_, nullptr);
        cmd::MatrixMode(*ctx_, GL_PROJECTION);
        cmd::Ortho(*ctx_, 0, kW, 0, kH, -1, 1);
        cmd::MatrixMode(*ctx_, GL_MODELVIEW);
    }

    void TearDown() override {
        drawable_.reset();
        if (gl_) gl_->make(0, 0);
        set_current_gl_context_for_thread(nullptr);
        GlDrawable::detach_all();
        graphics::set_active_backend_device(nullptr);
    }

    u64 call(const char* name, std::vector<u64> args = {}) { return gl_->call(name, std::move(args)); }
    u32 error() { return static_cast<u32>(call("glGetError")); }

    void clear() {
        cmd::ClearColor(*ctx_, 0, 0, 0, 0);
        cmd::Clear(*ctx_, GL_COLOR_BUFFER_BIT);
    }

    /// The back buffer, row 0 = the bottom row. Reading ends the pass.
    std::vector<Rgba> pixels() {
        std::vector<u8> bgra;
        EXPECT_TRUE(drawable_->read_color(GL_BACK, bgra));
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

    static Rgba quad_color(u32 q) {
        return Rgba{static_cast<u8>(q * 7), static_cast<u8>(q * 13 + 40), static_cast<u8>(q * 29 + 90), 255};
    }

    /// 2,500 1x1 quads (10,000 vertices), each its own glColor3ub and
    /// glBegin(GL_QUADS) .. glEnd, which immediate mode merges into one
    /// pending draw; quad q covers pixel (q mod 32, (q / 32) mod 32).
    void issue_quads() {
        for (u32 q = 0; q < 2500; ++q) {
            call("glBegin", {GL_QUADS});
            const Rgba c = quad_color(q);
            call("glColor3ub", {c.r, c.g, c.b});
            const u64 x = q % kW, y = (q / kW) % kH;
            call("glVertex2i", {x, y});
            call("glVertex2i", {x + 1, y});
            call("glVertex2i", {x + 1, y + 1});
            call("glVertex2i", {x, y + 1});
            call("glEnd");
        }
    }

    static std::vector<Rgba> expected_quads() {
        std::vector<Rgba> px(kW * kH, Rgba{0, 0, 0, 0});
        for (u32 q = 0; q < 2500; ++q) px[((q / kW) % kH) * kW + q % kW] = quad_color(q);
        return px;
    }

    struct Counts {
        u32 passes;
        u64 pipelines;
        u64 draws;
    };
    Counts counts() {
        const GlBatchStats s = gl_batch_stats(*ctx_);
        return Counts{drawable_->passes_opened(), s.pipelines_created, s.draws_recorded};
    }

    GuestMemory memory_;
    std::unique_ptr<ThunkRegistry> registry_;
    std::unique_ptr<Opengl32Manager> mgr_;
    std::unique_ptr<Caller> gl_;
    u64 rc_{0};
    GlContext* ctx_{nullptr};
    std::shared_ptr<GlDrawable> drawable_;
};

std::string bitness_name(const ::testing::TestParamInfo<bool>& info) { return info.param ? "x64" : "x86"; }

// Ruling 8: replaying a large list batches exactly as immediate mode does,
// with no per-command work beyond the commands themselves.
TEST_P(GlDisplayListMetalTest, ReplayOfTenThousandVerticesBatchesLikeImmediateMode) {
    const std::vector<Rgba> want = expected_quads();

    // Immediate mode.
    clear();
    (void)pixels();
    const Counts i0 = counts();
    issue_quads();
    EXPECT_EQ(error(), GL_NO_ERROR);
    const std::vector<Rgba> immediate = pixels();
    const Counts i1 = counts();
    EXPECT_EQ(immediate, want);
    EXPECT_LE(i1.passes - i0.passes, 1u);
    EXPECT_GT(i1.draws - i0.draws, 0u);
    EXPECT_LE(i1.draws - i0.draws, 4u);   // 2,500 glBegin/glEnd pairs merge

    // GL_COMPILE draws nothing.
    clear();
    (void)pixels();
    const Counts c0 = counts();
    call("glNewList", {1, GL_COMPILE});
    issue_quads();
    call("glEndList");
    EXPECT_EQ(error(), GL_NO_ERROR);
    const std::vector<Rgba> compiled = pixels();
    const Counts c1 = counts();
    EXPECT_EQ(c1.draws, c0.draws);
    EXPECT_EQ(compiled, std::vector<Rgba>(kW * kH, Rgba{0, 0, 0, 0}));

    // Replay: one pass, the same draws as immediate mode, no new pipeline
    // (the immediate run created every pipeline it needs).
    const Counts r0 = counts();
    call("glCallList", {1});
    EXPECT_EQ(error(), GL_NO_ERROR);
    const std::vector<Rgba> replayed = pixels();
    const Counts r1 = counts();
    EXPECT_EQ(replayed, want);
    EXPECT_LE(r1.passes - r0.passes, 1u);
    EXPECT_EQ(r1.draws - r0.draws, i1.draws - i0.draws);
    EXPECT_EQ(r1.pipelines, r0.pipelines);
    EXPECT_LE(i1.pipelines - i0.pipelines, 2u);

    // Replaying it four times in one frame stays in one pass.
    clear();
    (void)pixels();
    const Counts f0 = counts();
    for (int k = 0; k < 4; ++k) call("glCallList", {1});
    const std::vector<Rgba> frame = pixels();
    const Counts f1 = counts();
    EXPECT_EQ(frame, want);
    EXPECT_LE(f1.passes - f0.passes, 1u);
    EXPECT_LE(f1.draws - f0.draws, 4 * (i1.draws - i0.draws));
    EXPECT_EQ(f1.pipelines, f0.pipelines);
}

// Spec 5.4: vertex array pointers are dereferenced when glDrawArrays /
// glDrawElements are compiled; later changes to the arrays, the pointers or
// the enables do not reach the replay.
TEST_P(GlDisplayListMetalTest, CompiledArrayDrawsKeepTheirCompileTimeData) {
    // Four quads at (4k, 4) .. (4k + 4, 8), colours from a GL_UNSIGNED_BYTE array.
    const GuestAddr verts = kArrays, colors = kArrays + 0x400, idx = kArrays + 0x800;
    std::vector<i32> v;
    std::vector<u8> c;
    const Rgba quad[4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}};
    for (i32 k = 0; k < 4; ++k) {
        const i32 x = 4 * k;
        for (const auto& [dx, dy] : {std::pair{0, 0}, std::pair{4, 0}, std::pair{4, 4}, std::pair{0, 4}}) {
            v.push_back(x + dx);
            v.push_back(4 + dy);
            c.insert(c.end(), {quad[k].r, quad[k].g, quad[k].b, quad[k].a});
        }
    }
    ASSERT_TRUE(memory_.write_bytes(verts, reinterpret_cast<const Byte*>(v.data()), v.size() * 4).is_ok());
    ASSERT_TRUE(memory_.write_bytes(colors, c.data(), c.size()).is_ok());
    const std::vector<u8> order = {12, 13, 14, 15, 0, 1, 2, 3};   // quads 3 and 0, drawn 12 rows up by a translate
    ASSERT_TRUE(memory_.write_bytes(idx, order.data(), order.size()).is_ok());
    call("glVertexPointer", {2, GL_INT, 0, verts});
    call("glColorPointer", {4, GL_UNSIGNED_BYTE, 0, colors});
    call("glEnableClientState", {GL_VERTEX_ARRAY});
    call("glEnableClientState", {GL_COLOR_ARRAY});
    call("glNewList", {7, GL_COMPILE});
    call("glDrawArrays", {GL_QUADS, 0, 16});
    call("glEndList");
    call("glNewList", {8, GL_COMPILE});
    call("glDrawElements", {GL_QUADS, 8, GL_UNSIGNED_BYTE, idx});
    call("glEndList");
    EXPECT_EQ(error(), GL_NO_ERROR);
    // Clobber everything the draws read.
    std::vector<u8> junk(0x900, 0x11);
    ASSERT_TRUE(memory_.write_bytes(kArrays, junk.data(), junk.size()).is_ok());
    call("glDisableClientState", {GL_COLOR_ARRAY});
    call("glVertexPointer", {2, GL_INT, 0, 0});
    clear();
    call("glCallList", {7});
    cmd::Translate(*ctx_, 0, 12, 0);
    call("glCallList", {8});
    cmd::LoadIdentity(*ctx_);
    EXPECT_EQ(error(), GL_NO_ERROR);
    const std::vector<Rgba> px = pixels();
    auto at = [&](i32 x, i32 y) { return px[static_cast<usize>(y * kW + x)]; };
    for (i32 k = 0; k < 4; ++k) {
        for (i32 y = 4; y < 8; ++y) {
            for (i32 x = 4 * k; x < 4 * k + 4; ++x) EXPECT_EQ(at(x, y), quad[k]) << x << "," << y;
            // The indexed list drew quads 3 and 0 at y + 12, at their own x.
            for (i32 x = 4 * k; x < 4 * k + 4; ++x) {
                const Rgba want = (k == 0 || k == 3) ? quad[k] : Rgba{0, 0, 0, 0};
                EXPECT_EQ(at(x, y + 12), want) << x << "," << y + 12;
            }
        }
    }
    EXPECT_EQ(at(20, 2), (Rgba{0, 0, 0, 0}));
}

INSTANTIATE_TEST_SUITE_P(Bitness, GlDisplayListMetalTest, ::testing::Values(false, true), bitness_name);

}  // namespace
