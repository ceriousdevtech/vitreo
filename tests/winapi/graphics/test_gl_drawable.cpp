/// @file test_gl_drawable.cpp
/// @brief The GL default framebuffer on the real Metal backend: the per-window
///        drawable's back/front/depth-stencil textures, glClear (scissor and
///        write masks, OpenGL 1.1 spec 4.2.3), glDrawBuffer, glFlush/glFinish,
///        SwapBuffers / wglSwapLayerBuffers / wglSwapMultipleBuffers, resize.
///
/// Kept out of vitreo_winapi_tests, which does not link the Metal backend.
/// Expected pixels come from the spec's rules applied to the clear values
/// (unorm conversion c * 255, exact for the values used here), never from a
/// run. Window coordinates are GL's: (0, 0) is the bottom-left pixel.
/// The window's presentation is a fake swap chain that records what it is
/// asked to present.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include <graphics/contracts/backend_device.hpp>
#include <graphics/contracts/backend_swap_chain.hpp>
#include <graphics/metal/metal_device.hpp>

#include "winapi/graphics/opengl32/gl_commands.hpp"
#include "winapi/graphics/opengl32/gl_context.hpp"
#include "winapi/graphics/opengl32/gl_drawable.hpp"
#include "winapi/graphics/opengl32/gl_enums.hpp"
#include "winapi/graphics/opengl32/gl_pixel_formats.hpp"
#include "winapi/graphics/opengl32/gl_wgl.hpp"
#include "winapi/graphics/opengl32/opengl32.hpp"
#include "gl_test_caller.hpp"

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::gl;
using test::Caller;
using test::kStale;

namespace {

// Vitreo's pixel formats (gl_pixel_formats.cpp): 1-4 alpha 0 single, 5-8
// alpha 0 double, 9-12 alpha 8 single, 13-16 alpha 8 double; within each,
// depth/stencil 24/8, 24/0, 16/0, 0/0.
constexpr i32 kDoubleAlpha = 13;        // double, alpha 8, depth 24, stencil 8
constexpr i32 kDoubleNoAlphaNoDepth = 8;
constexpr i32 kSingleAlpha = 9;         // single, alpha 8, depth 24, stencil 8

constexpr u64 kWglSwapMainPlane = 0x1, kWglSwapOverlay1 = 0x2;

struct Rgba {
    u8 r, g, b, a;
    bool operator==(const Rgba&) const = default;
};
std::ostream& operator<<(std::ostream& os, const Rgba& c) {
    return os << "(" << int(c.r) << "," << int(c.g) << "," << int(c.b) << "," << int(c.a) << ")";
}

/// A presentation target that records what it is asked to present.
class FakeSwapChain final : public graphics::BackendSwapChain {
public:
    ~FakeSwapChain() override {
        if (destroyed != nullptr) ++*destroyed;
    }
    u32* destroyed{nullptr};
    explicit FakeSwapChain(u32 width, u32 height) {
        m_desc.width = width;
        m_desc.height = height;
        m_desc.pixel_format = graphics::PixelFormat::BGRA8Unorm;
    }
    bool valid() const noexcept override { return true; }
    const graphics::BackendSwapChainDesc& desc() const noexcept override { return m_desc; }
    bool clear_back_buffer(const std::array<float, 4>&) override { return false; }
    bool present(unsigned, unsigned) override { return false; }
    bool present_texture(graphics::BackendTexture2D* source, unsigned interval, unsigned flags) override {
        presented.push_back(source);
        intervals.push_back(interval);
        // Sizes at present time: the presented texture may be freed later.
        presented_sizes.push_back({source != nullptr ? source->desc().width : 0u,
                                   source != nullptr ? source->desc().height : 0u});
        chain_sizes.push_back({m_desc.width, m_desc.height});
        (void)flags;
        return source != nullptr;
    }
    bool resize(unsigned width, unsigned height, graphics::PixelFormat format, unsigned) override {
        m_desc.width = width;
        m_desc.height = height;
        m_desc.pixel_format = format;
        return true;
    }
    std::unique_ptr<graphics::BackendRenderPass> begin_render_pass(
        bool, bool, float, const std::array<graphics::BackendTexture2D*, graphics::BACKEND_MAX_RENDER_TARGETS>&) override {
        return nullptr;
    }

    std::vector<graphics::BackendTexture2D*> presented;
    std::vector<unsigned> intervals;
    std::vector<std::pair<u32, u32>> presented_sizes;
    std::vector<std::pair<u32, u32>> chain_sizes;

private:
    graphics::BackendSwapChainDesc m_desc{};
};

struct FakeDc {
    i32 format{0};
    u64 window{0};
    i32 width{0};
    i32 height{0};
};

constexpr u64 dA = 0x5000, dB = 0x5001, dS = 0x5002, dZ = 0x5003, dSmall = 0x5004;

class GlDrawableTest : public ::testing::TestWithParam<bool> {
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
        dcs_[dA] = {kDoubleAlpha, 0x100, 16, 8};
        dcs_[dB] = {kDoubleAlpha, 0x101, 16, 8};
        dcs_[dS] = {kSingleAlpha, 0x102, 16, 8};
        dcs_[dZ] = {kDoubleNoAlphaNoDepth, 0x103, 16, 8};
        dcs_[dSmall] = {kDoubleAlpha, 0x104, 8, 6};
        mgr_->set_pixel_format_query([this](u64 dc) {
            const auto it = dcs_.find(dc);
            return it == dcs_.end() ? 0 : it->second.format;
        });
        mgr_->set_drawable_queries(
            [this](u64 dc) -> u64 {
                const auto it = dcs_.find(dc);
                return it == dcs_.end() ? 0 : it->second.window;
            },
            [this](u64 dc, i32& w, i32& h) {
                const auto it = dcs_.find(dc);
                if (it == dcs_.end()) return false;
                w = it->second.width;
                h = it->second.height;
                return true;
            });
        mgr_->set_swap_chain_factory([this](u64 hwnd, u32 w, u32 h) {
            auto chain = std::make_unique<FakeSwapChain>(w, h);
            chain->destroyed = &chains_destroyed_;
            ++factory_calls_;
            chains_[hwnd] = chain.get();
            return std::unique_ptr<graphics::BackendSwapChain>(std::move(chain));
        });
        main_ = std::make_unique<Caller>(memory_, *registry_, GetParam(), 0);
    }

    void TearDown() override {
        if (main_) main_->make(0, 0);
        set_current_gl_context_for_thread(nullptr);
        GlDrawable::detach_all();
        graphics::set_active_backend_device(nullptr);
    }

    Caller& gl() { return *main_; }

    /// Creates a context on @p dc and makes it current; returns the context.
    GlContext& current_on(u64 dc) {
        const u64 rc = gl().create(dc);
        EXPECT_NE(rc, 0u);
        EXPECT_EQ(gl().make(dc, rc), 1u);
        GlContext* ctx = mgr_->context_for_testing(rc);
        EXPECT_NE(ctx, nullptr);
        return *ctx;
    }

    std::shared_ptr<GlDrawable> drawable_of(u64 dc) { return GlDrawable::for_window(dcs_[dc].window); }

    /// The whole of @p buffer's texture as RGBA, rows in GL order (row 0 is
    /// the bottom of the window). Reading ends the open pass.
    std::vector<Rgba> pixels(u64 dc, u32 buffer) {
        auto d = drawable_of(dc);
        EXPECT_NE(d, nullptr);
        if (!d) return {};
        std::vector<u8> bgra;
        EXPECT_TRUE(d->read_color(buffer, bgra));
        const u32 w = d->width(), h = d->height();
        EXPECT_EQ(bgra.size(), static_cast<usize>(w) * h * 4u);
        std::vector<Rgba> out(static_cast<usize>(w) * h);
        for (u32 gy = 0; gy < h; ++gy) {
            const u32 row = h - 1 - gy;       // texture rows are top-down
            for (u32 x = 0; x < w; ++x) {
                const u8* p = &bgra[(static_cast<usize>(row) * w + x) * 4u];
                out[static_cast<usize>(gy) * w + x] = Rgba{p[2], p[1], p[0], p[3]};
            }
        }
        return out;
    }
    Rgba pixel(u64 dc, u32 buffer, u32 x, u32 y) {
        auto d = drawable_of(dc);
        const auto px = pixels(dc, buffer);
        return px.at(static_cast<usize>(y) * d->width() + x);
    }
    u32 clear(u32 mask) { return static_cast<u32>(gl().call("glClear", {mask})); }
    u32 error() { return static_cast<u32>(gl().call("glGetError")); }

    GuestMemory memory_;
    std::unique_ptr<ThunkRegistry> registry_;
    std::unique_ptr<Opengl32Manager> mgr_;
    std::map<u64, FakeDc> dcs_;
    std::map<u64, FakeSwapChain*> chains_;
    u32 factory_calls_{0};
    u32 chains_destroyed_{0};
    std::unique_ptr<Caller> main_;
};

constexpr Rgba kRed{255, 0, 0, 255};
constexpr Rgba kGreen{0, 255, 0, 255};
constexpr Rgba kBlue{0, 0, 255, 255};

// Spec 4.2.3: glClear sets the buffers its mask names to the clear values;
// GL_COLOR_BUFFER_BIT clears the draw buffer (GL_BACK for a double-buffered
// format, Table 6.14), so the front buffer is untouched.
TEST_P(GlDrawableTest, ClearColorReachesBackOnly) {
    GlContext& ctx = current_on(dA);
    auto d = drawable_of(dA);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->width(), 16u);
    EXPECT_EQ(d->height(), 8u);
    const std::vector<Rgba> front_before = pixels(dA, GL_FRONT);

    cmd::ClearColor(ctx, 1.0f, 0.0f, 0.0f, 1.0f);
    EXPECT_EQ(clear(GL_COLOR_BUFFER_BIT), 0u);
    EXPECT_EQ(error(), GL_NO_ERROR);
    for (const Rgba& p : pixels(dA, GL_BACK)) ASSERT_EQ(p, kRed);   // 0xFF0000FF as R,G,B,A bytes
    EXPECT_EQ(pixels(dA, GL_FRONT), front_before);
}

// Spec 4.2.3 / 4.1.2: the scissor test applies to glClear; only the pixels
// inside the scissor box (window coordinates, bottom-left origin) change. A box
// partly outside the window is clipped to it, an empty box clears nothing.
TEST_P(GlDrawableTest, ScissoredClearTouchesOnlyScissor) {
    GlContext& ctx = current_on(dSmall);   // 8x6
    cmd::ClearColor(ctx, 0.0f, 0.0f, 1.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    gl().call("glEnable", {GL_SCISSOR_TEST});
    gl().call("glScissor", {1, 0, 3, 2});
    cmd::ClearColor(ctx, 1.0f, 0.0f, 0.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    std::vector<Rgba> px = pixels(dSmall, GL_BACK);
    for (u32 y = 0; y < 6; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const bool inside = x >= 1 && x < 4 && y < 2;
            EXPECT_EQ(px[y * 8 + x], inside ? kRed : kBlue) << x << "," << y;
        }
    }
    // Clipped to the window: x 6..7, y 4..5.
    gl().call("glScissor", {6, 4, 10, 10});
    cmd::ClearColor(ctx, 0.0f, 1.0f, 0.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    // Empty box: nothing.
    gl().call("glScissor", {0, 0, 0, 6});
    cmd::ClearColor(ctx, 1.0f, 1.0f, 1.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    EXPECT_EQ(error(), GL_NO_ERROR);
    px = pixels(dSmall, GL_BACK);
    for (u32 y = 0; y < 6; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            Rgba want = (x >= 1 && x < 4 && y < 2) ? kRed : kBlue;
            if (x >= 6 && y >= 4) want = kGreen;
            EXPECT_EQ(px[y * 8 + x], want) << x << "," << y;
        }
    }
    // Disabling the test clears the whole window again.
    gl().call("glDisable", {GL_SCISSOR_TEST});
    clear(GL_COLOR_BUFFER_BIT);
    for (const Rgba& p : pixels(dSmall, GL_BACK)) ASSERT_EQ(p, (Rgba{255, 255, 255, 255}));
}

// Spec 4.2.2 / 4.2.3: glColorMask applies to glClear; a masked channel keeps
// its value. 0.2/0.4/0.6/0.8 convert to exactly 51/102/153/204.
TEST_P(GlDrawableTest, MaskedClearKeepsMaskedChannels) {
    GlContext& ctx = current_on(dA);
    cmd::ClearColor(ctx, 0.2f, 0.4f, 0.6f, 0.8f);
    clear(GL_COLOR_BUFFER_BIT);
    for (const Rgba& p : pixels(dA, GL_BACK)) ASSERT_EQ(p, (Rgba{51, 102, 153, 204}));
    gl().call("glColorMask", {1, 0, 1, 0});
    cmd::ClearColor(ctx, 1.0f, 1.0f, 1.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    for (const Rgba& p : pixels(dA, GL_BACK)) ASSERT_EQ(p, (Rgba{255, 102, 255, 204}));
    gl().call("glColorMask", {0, 0, 0, 0});
    cmd::ClearColor(ctx, 0.0f, 0.0f, 0.0f, 0.0f);
    clear(GL_COLOR_BUFFER_BIT);
    for (const Rgba& p : pixels(dA, GL_BACK)) ASSERT_EQ(p, (Rgba{255, 102, 255, 204}));
    EXPECT_EQ(error(), GL_NO_ERROR);
}

// Spec 4.3.2: a format without alpha bitplanes reads alpha as 1; the clear
// alpha has nowhere to go.
TEST_P(GlDrawableTest, AlphaLessFormatKeepsAlphaOne) {
    GlContext& ctx = current_on(dZ);
    cmd::ClearColor(ctx, 0.0f, 1.0f, 0.0f, 0.0f);
    clear(GL_COLOR_BUFFER_BIT);
    for (const Rgba& p : pixels(dZ, GL_BACK)) ASSERT_EQ(p, kGreen);
    gl().call("glColorMask", {0, 1, 1, 1});   // a masked clear (quad path) as well
    cmd::ClearColor(ctx, 1.0f, 0.0f, 1.0f, 0.0f);
    clear(GL_COLOR_BUFFER_BIT);
    for (const Rgba& p : pixels(dZ, GL_BACK)) ASSERT_EQ(p, (Rgba{0, 0, 255, 255}));
}

// Spec 4.2.2-4.2.3: glDepthMask and glStencilMask apply to glClear; a stencil
// clear writes (old & ~mask) | (value & mask).
TEST_P(GlDrawableTest, DepthAndStencilClearsHonourMasks) {
    GlContext& ctx = current_on(dA);
    auto d = drawable_of(dA);
    ASSERT_NE(d->depth_stencil(), nullptr);
    auto depth_at = [&](u32 x, u32 y) {
        std::vector<float> depth;
        EXPECT_TRUE(d->read_depth(depth));
        return depth.at(static_cast<usize>(d->height() - 1 - y) * d->width() + x);
    };
    auto stencil_at = [&](u32 x, u32 y) {
        std::vector<u8> stencil;
        EXPECT_TRUE(d->read_stencil(stencil));
        return stencil.at(static_cast<usize>(d->height() - 1 - y) * d->width() + x);
    };
    cmd::ClearDepth(ctx, 0.25);
    cmd::ClearStencil(ctx, 0x30);
    clear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    EXPECT_EQ(depth_at(0, 0), 0.25f);
    EXPECT_EQ(depth_at(15, 7), 0.25f);
    EXPECT_EQ(stencil_at(3, 2), 0x30);

    gl().call("glDepthMask", {0});
    cmd::ClearDepth(ctx, 0.75);
    clear(GL_DEPTH_BUFFER_BIT);
    EXPECT_EQ(depth_at(5, 5), 0.25f);
    gl().call("glDepthMask", {1});

    gl().call("glStencilMask", {0x0F});
    cmd::ClearStencil(ctx, 0xAB);
    clear(GL_STENCIL_BUFFER_BIT);
    EXPECT_EQ(stencil_at(3, 2), 0x3B);
    EXPECT_EQ(depth_at(3, 2), 0.25f);          // the depth plane is untouched

    // A scissored depth clear (quad path) changes only the box.
    gl().call("glStencilMask", {0xFF});
    gl().call("glEnable", {GL_SCISSOR_TEST});
    gl().call("glScissor", {0, 0, 2, 1});
    cmd::ClearDepth(ctx, 1.0);
    clear(GL_DEPTH_BUFFER_BIT);
    EXPECT_EQ(depth_at(1, 0), 1.0f);
    EXPECT_EQ(depth_at(2, 0), 0.25f);
    EXPECT_EQ(depth_at(0, 1), 0.25f);
    EXPECT_EQ(error(), GL_NO_ERROR);
}

// Clearing a buffer the pixel format lacks (depth/stencil on a depth-0 format;
// the accumulation buffer, which no format has) has no effect and no error;
// glDrawBuffer(GL_NONE) clears no colour buffer (spec 4.2.1).
TEST_P(GlDrawableTest, ClearOfAbsentBufferHasNoEffect) {
    GlContext& ctx = current_on(dZ);
    auto d = drawable_of(dZ);
    EXPECT_EQ(d->depth_stencil(), nullptr);
    cmd::ClearColor(ctx, 0.0f, 0.0f, 1.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    clear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_ACCUM_BUFFER_BIT);
    EXPECT_EQ(error(), GL_NO_ERROR);
    for (const Rgba& p : pixels(dZ, GL_BACK)) ASSERT_EQ(p, kBlue);
    gl().call("glDrawBuffer", {GL_NONE});
    cmd::ClearColor(ctx, 1.0f, 0.0f, 0.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    EXPECT_EQ(error(), GL_NO_ERROR);
    for (const Rgba& p : pixels(dZ, GL_BACK)) ASSERT_EQ(p, kBlue);
}

// Design 3: GL_FRONT_AND_BACK renders to both colour buffers, GL_FRONT to the
// front only (spec 4.2.1, Table 4.4).
TEST_P(GlDrawableTest, DrawBufferSelectsTheClearedBuffers) {
    GlContext& ctx = current_on(dA);
    gl().call("glDrawBuffer", {GL_FRONT_AND_BACK});
    cmd::ClearColor(ctx, 0.0f, 1.0f, 0.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    for (const Rgba& p : pixels(dA, GL_BACK)) ASSERT_EQ(p, kGreen);
    for (const Rgba& p : pixels(dA, GL_FRONT)) ASSERT_EQ(p, kGreen);
    gl().call("glDrawBuffer", {GL_FRONT});
    cmd::ClearColor(ctx, 1.0f, 0.0f, 0.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    for (const Rgba& p : pixels(dA, GL_FRONT)) ASSERT_EQ(p, kRed);
    for (const Rgba& p : pixels(dA, GL_BACK)) ASSERT_EQ(p, kGreen);
    // GL_LEFT names front-left and back-left (no stereo): both.
    gl().call("glDrawBuffer", {GL_LEFT});
    cmd::ClearColor(ctx, 0.0f, 0.0f, 1.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    for (const Rgba& p : pixels(dA, GL_FRONT)) ASSERT_EQ(p, kBlue);
    for (const Rgba& p : pixels(dA, GL_BACK)) ASSERT_EQ(p, kBlue);
}

// Design 3 + PFD_SWAP_EXCHANGE: SwapBuffers exchanges the buffers (the new
// back holds the old front) and presents the new front.
TEST_P(GlDrawableTest, SwapExchangesFrontAndBack) {
    GlContext& ctx = current_on(dA);
    gl().call("glDrawBuffer", {GL_FRONT});
    cmd::ClearColor(ctx, 0.0f, 0.0f, 1.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    gl().call("glDrawBuffer", {GL_BACK});
    cmd::ClearColor(ctx, 1.0f, 0.0f, 0.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    auto d = drawable_of(dA);
    graphics::BackendTexture2D* back_before = d->target(GL_BACK);

    EXPECT_EQ(gl().call("wglSwapBuffers", {dA}), 1u);
    EXPECT_EQ(gl().err(), kStale);
    EXPECT_EQ(d->target(GL_FRONT), back_before);
    for (const Rgba& p : pixels(dA, GL_FRONT)) ASSERT_EQ(p, kRed);
    for (const Rgba& p : pixels(dA, GL_BACK)) ASSERT_EQ(p, kBlue);
    ASSERT_EQ(chains_.count(0x100), 1u);
    FakeSwapChain* chain = chains_[0x100];
    ASSERT_EQ(chain->presented.size(), 1u);
    EXPECT_EQ(chain->presented.back(), d->target(GL_FRONT));
    EXPECT_EQ(chain->desc().width, 16u);
    EXPECT_EQ(chain->desc().height, 8u);

    // A second swap exchanges back.
    EXPECT_EQ(gl().call("wglSwapBuffers", {dA}), 1u);
    for (const Rgba& p : pixels(dA, GL_FRONT)) ASSERT_EQ(p, kBlue);
    EXPECT_EQ(chain->presented.size(), 2u);
}

// Design 3: a single-buffered format renders into the front buffer and
// presents it at glFlush / glFinish. SwapBuffers on it with its own context
// current is TRUE (swap_hint_probe64.expected hint-single-swapbuffers ret=1)
// and shows the window as glFlush does. With no context or another window's
// context current it is FALSE with the last error untouched
// (wgl_contract_probe64.expected swap-buffers-no-ctx-single,
// swap-buffers-single-while-other-current) and presents nothing.
TEST_P(GlDrawableTest, SingleBufferedDrawsToFront) {
    GlContext& ctx = current_on(dS);
    auto d = drawable_of(dS);
    EXPECT_EQ(d->target(GL_BACK), nullptr);
    EXPECT_EQ(ctx.state.color_buffer.draw_buffer, GL_FRONT);
    cmd::ClearColor(ctx, 1.0f, 0.0f, 0.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    for (const Rgba& p : pixels(dS, GL_FRONT)) ASSERT_EQ(p, kRed);

    gl().call("glFlush");
    ASSERT_EQ(chains_.count(0x102), 1u);
    FakeSwapChain* chain = chains_[0x102];
    ASSERT_EQ(chain->presented.size(), 1u);
    EXPECT_EQ(chain->presented[0], d->target(GL_FRONT));
    gl().call("glFinish");
    EXPECT_EQ(chain->presented.size(), 2u);
    EXPECT_EQ(error(), GL_NO_ERROR);

    EXPECT_EQ(gl().call("wglSwapBuffers", {dS}), 1u);
    EXPECT_EQ(chain->presented.size(), 3u);

    (void)current_on(dA);   // releasing dS's context may present it
    const usize before = chain->presented.size();
    EXPECT_EQ(gl().call("wglSwapBuffers", {dS}), 0u);
    EXPECT_EQ(gl().err(), kStale);
    EXPECT_EQ(chain->presented.size(), before);
}

// A double-buffered format presents only at a swap: glFlush and glFinish
// submit the work but leave the window alone.
TEST_P(GlDrawableTest, DoubleBufferedFlushPresentsNothing) {
    GlContext& ctx = current_on(dA);
    cmd::ClearColor(ctx, 1.0f, 0.0f, 0.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    gl().call("glFlush");
    gl().call("glFinish");
    const auto it = chains_.find(0x100);
    EXPECT_TRUE(it == chains_.end() || it->second->presented.empty());
}

// viewport-initial (Task 5 owns the rule): the first make-current sizes the
// drawable and the viewport to the client area.
TEST_P(GlDrawableTest, InitialViewportIsClientSize) {
    GlContext& ctx = current_on(dSmall);
    auto d = drawable_of(dSmall);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->width(), 8u);
    EXPECT_EQ(d->height(), 6u);
    EXPECT_EQ(ctx.state.viewport.width, 8);
    EXPECT_EQ(ctx.state.viewport.height, 6);
}

// Review Focus 2: after a resize the drawable reallocates at the new client
// size at the next SwapBuffers / wglMakeCurrent; the viewport is not reset
// (viewport-after-resize-swap, viewport-after-resize-remake).
TEST_P(GlDrawableTest, ResizeReallocatesButKeepsViewport) {
    GlContext& ctx = current_on(dA);   // 16x8
    auto d = drawable_of(dA);
    cmd::ClearColor(ctx, 1.0f, 0.0f, 0.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    dcs_[dA].width = 20;
    dcs_[dA].height = 10;
    EXPECT_EQ(d->width(), 16u);        // not before the next swap / make-current
    EXPECT_EQ(gl().call("wglSwapBuffers", {dA}), 1u);
    // The frame drawn at the old size was presented before the reallocation.
    FakeSwapChain* chain = chains_[0x100];
    ASSERT_NE(chain, nullptr);
    ASSERT_EQ(chain->presented.size(), 1u);
    EXPECT_EQ(chain->presented_sizes[0], (std::pair<u32, u32>{16, 8}));
    EXPECT_EQ(chain->chain_sizes[0], (std::pair<u32, u32>{16, 8}));
    EXPECT_EQ(d->width(), 20u);
    EXPECT_EQ(d->height(), 10u);
    EXPECT_EQ(d->target(GL_BACK)->desc().width, 20u);
    EXPECT_EQ(d->target(GL_FRONT)->desc().height, 10u);
    EXPECT_EQ(ctx.state.viewport.width, 16);
    EXPECT_EQ(ctx.state.viewport.height, 8);
    EXPECT_EQ(ctx.state.scissor.width, 16);
    // A clear covers the new size.
    cmd::ClearColor(ctx, 0.0f, 1.0f, 0.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    EXPECT_EQ(pixel(dA, GL_BACK, 19, 9), kGreen);
    // The swap chain follows at the next present, which resizes it first,
    // outside the drawable's lock (MetalSwapchain::resize waits for the main
    // thread; final review, Finding 4).
    EXPECT_EQ(gl().call("wglSwapBuffers", {dA}), 1u);
    ASSERT_EQ(chain->presented.size(), 2u);
    EXPECT_EQ(chain->chain_sizes[1], (std::pair<u32, u32>{20, 10}));
    EXPECT_EQ(chain->desc().width, 20u);
    EXPECT_EQ(chain->desc().height, 10u);

    // Make-current after a resize reallocates too.
    dcs_[dA].width = 12;
    dcs_[dA].height = 4;
    ASSERT_EQ(gl().make(0, 0), 1u);
    const u64 rc = wgl_handle_of(&ctx);
    ASSERT_EQ(gl().make(dA, rc), 1u);
    EXPECT_EQ(d->width(), 12u);
    EXPECT_EQ(d->height(), 4u);
    EXPECT_EQ(ctx.state.viewport.width, 16);
}

// swap-layer-*: wglSwapLayerBuffers succeeds for any fuPlanes on a
// double-buffered DC, but only the main plane exists, so only
// WGL_SWAP_MAIN_PLANE swaps.
TEST_P(GlDrawableTest, SwapLayerBuffersMainPlaneOnly) {
    current_on(dA);
    auto d = drawable_of(dA);
    graphics::BackendTexture2D* front = d->target(GL_FRONT);
    for (const u64 planes : {kWglSwapOverlay1, u64{0}, u64{0x10000}}) {
        EXPECT_EQ(gl().call("wglSwapLayerBuffers", {dA, planes}), 1u) << planes;
        EXPECT_EQ(gl().err(), kStale);
        EXPECT_EQ(d->target(GL_FRONT), front) << planes;
    }
    EXPECT_EQ(gl().call("wglSwapLayerBuffers", {dA, kWglSwapMainPlane | kWglSwapOverlay1}), 1u);
    EXPECT_NE(d->target(GL_FRONT), front);
    ASSERT_EQ(chains_[0x100]->presented.size(), 1u);
    EXPECT_EQ(gl().call("wglSwapLayerBuffers", {dA, 0xFFFFFFFFu}), 1u);
    EXPECT_EQ(d->target(GL_FRONT), front);
}

// swapmulti-*: each WGLSWAP entry is a wglSwapLayerBuffers; the return value
// is the mask of the entries that succeeded.
TEST_P(GlDrawableTest, SwapMultipleBuffersSwapsEachMainPlaneEntry) {
    const u64 ra = gl().create(dA), rb = gl().create(dB);
    ASSERT_EQ(gl().make(dB, rb), 1u);
    ASSERT_EQ(gl().make(dA, ra), 1u);
    auto a = drawable_of(dA), b = drawable_of(dB);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    graphics::BackendTexture2D* fa = a->target(GL_FRONT);
    graphics::BackendTexture2D* fb = b->target(GL_FRONT);
    const bool x64 = GetParam();
    const GuestAddr arr = 0x10800;
    const u32 stride = x64 ? 16u : 8u;
    auto entry = [&](u32 i, u64 dc, u32 flags) {
        if (x64) (void)memory_.write_u64(arr + i * stride, dc);
        else (void)memory_.write_u32(arr + i * stride, static_cast<u32>(dc));
        (void)memory_.write_u32(arr + i * stride + (x64 ? 8u : 4u), flags);
    };
    entry(0, dA, kWglSwapMainPlane);
    entry(1, dB, kWglSwapOverlay1);
    entry(2, dS, kWglSwapMainPlane);    // never made current: no drawable, single -> fails
    EXPECT_EQ(gl().call("wglSwapMultipleBuffers", {3, arr}), 0x3u);
    EXPECT_NE(a->target(GL_FRONT), fa);
    EXPECT_EQ(b->target(GL_FRONT), fb);
    entry(0, dB, kWglSwapMainPlane);
    EXPECT_EQ(gl().call("wglSwapMultipleBuffers", {1, arr}), 0x1u);
    EXPECT_NE(b->target(GL_FRONT), fb);
}

// Design 1: a clear with draws pending in the open pass draws a quad in that
// pass instead of ending it; with nothing pending a full clear is a load-action
// clear and opens no pass. A draw-buffer change, glFlush and a swap end the pass.
TEST_P(GlDrawableTest, ClearKeepsTheOpenPassAndCommandsEndIt) {
    GlContext& ctx = current_on(dA);
    auto d = drawable_of(dA);
    const u32 opened = d->passes_opened();
    cmd::ClearColor(ctx, 0.0f, 0.0f, 1.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    EXPECT_FALSE(d->pass_open());
    EXPECT_EQ(d->passes_opened(), opened);

    // A pending draw: a scissored masked clear quad is a draw in the pass.
    gl().call("glEnable", {GL_SCISSOR_TEST});
    gl().call("glScissor", {0, 0, 4, 4});
    cmd::ClearColor(ctx, 0.0f, 1.0f, 0.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    EXPECT_TRUE(d->pass_open());
    EXPECT_EQ(d->pending_draws(), 1u);
    EXPECT_EQ(d->passes_opened(), opened + 1);
    gl().call("glDisable", {GL_SCISSOR_TEST});
    cmd::ClearColor(ctx, 1.0f, 0.0f, 0.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);              // full clear, draws pending: quad in the same pass
    EXPECT_TRUE(d->pass_open());
    EXPECT_EQ(d->pending_draws(), 2u);
    EXPECT_EQ(d->passes_opened(), opened + 1);
    gl().call("glDrawBuffer", {GL_FRONT});
    EXPECT_FALSE(d->pass_open());
    for (const Rgba& p : pixels(dA, GL_BACK)) ASSERT_EQ(p, kRed);

    gl().call("glEnable", {GL_SCISSOR_TEST});
    clear(GL_COLOR_BUFFER_BIT);
    EXPECT_TRUE(d->pass_open());
    gl().call("glFlush");
    EXPECT_FALSE(d->pass_open());
    clear(GL_COLOR_BUFFER_BIT);
    EXPECT_TRUE(d->pass_open());
    EXPECT_EQ(gl().call("wglSwapBuffers", {dA}), 1u);
    EXPECT_FALSE(d->pass_open());
    EXPECT_EQ(error(), GL_NO_ERROR);
}

// Learn wglMakeCurrent: the previous context is flushed before the switch, so
// releasing a single-buffered context presents its front buffer.
TEST_P(GlDrawableTest, ReleaseFlushesThePreviousContext) {
    GlContext& ctx = current_on(dS);
    cmd::ClearColor(ctx, 1.0f, 0.0f, 0.0f, 1.0f);
    gl().call("glEnable", {GL_SCISSOR_TEST});
    gl().call("glScissor", {0, 0, 2, 2});
    clear(GL_COLOR_BUFFER_BIT);
    auto d = drawable_of(dS);
    EXPECT_TRUE(d->pass_open());
    ASSERT_EQ(gl().make(0, 0), 1u);
    EXPECT_FALSE(d->pass_open());
    ASSERT_EQ(chains_.count(0x102), 1u);
    EXPECT_EQ(chains_[0x102]->presented.size(), 1u);
    EXPECT_EQ(ctx.drawable, nullptr);
}

// The window's destroyed callback (runtime_context.cpp, WindowManager::
// set_destroyed_callback) detaches its drawable: the swap chain on the dead
// view is dropped at once, even while a context is still current on it, and
// a new window that gets the same HWND value back gets a fresh drawable and
// a new swap chain (HWND values are reused).
TEST_P(GlDrawableTest, DestroyedWindowGetsAFreshDrawable) {
    current_on(dA);
    const std::shared_ptr<GlDrawable> first = drawable_of(dA);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(gl().call("wglSwapBuffers", {dA}), 1u);
    EXPECT_EQ(factory_calls_, 1u);

    GlDrawable::detach(0x100);                       // DestroyWindow, context still current
    EXPECT_EQ(GlDrawable::for_window(0x100), nullptr);
    EXPECT_EQ(chains_destroyed_, 1u);
    chains_.erase(0x100);
    EXPECT_TRUE(first->swap());                      // the stale context swaps, presents nothing
    EXPECT_EQ(factory_calls_, 1u);
    ASSERT_EQ(gl().make(0, 0), 1u);

    // A new window with the reused HWND value and the same format.
    const u64 rc = gl().create(dA);
    ASSERT_EQ(gl().make(dA, rc), 1u);
    const std::shared_ptr<GlDrawable> second = drawable_of(dA);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(second, first);
    EXPECT_EQ(gl().call("wglSwapBuffers", {dA}), 1u);
    EXPECT_EQ(factory_calls_, 2u);
    ASSERT_EQ(chains_.count(0x100), 1u);
    EXPECT_EQ(chains_[0x100]->presented.size(), 1u);
    EXPECT_EQ(chains_[0x100]->presented[0], second->target(GL_FRONT));
}

// ---- Final review, Finding 4: no drawable lock across a wait for the main thread ----

/// The top-left pixel of @p buffer of @p d.
Rgba pixel_of(GlDrawable& d, u32 buffer) {
    std::vector<u8> bgra;
    EXPECT_TRUE(d.read_color(buffer, bgra));
    return bgra.size() >= 4 ? Rgba{bgra[2], bgra[1], bgra[0], bgra[3]} : Rgba{};
}

/// A one-shot signal between two threads.
struct Gate {
    std::promise<void> promise;
    std::shared_future<void> future{promise.get_future().share()};
    void open() { promise.set_value(); }
    [[nodiscard]] bool wait(std::chrono::milliseconds limit = std::chrono::seconds(10)) const {
        return future.wait_for(limit) == std::future_status::ready;
    }
};

/// A presentation target whose creation (the factory) or presents block until
/// released: what MetalSwapchain's dispatch_sync to the main thread does
/// while that thread is busy elsewhere.
class GatedSwapChain final : public graphics::BackendSwapChain {
public:
    GatedSwapChain(u32 w, u32 h, std::atomic<u32>& destroyed, std::atomic<u32>& presents)
        : m_destroyed(destroyed), m_presents(presents) {
        m_desc.width = w;
        m_desc.height = h;
        m_desc.pixel_format = graphics::PixelFormat::BGRA8Unorm;
    }
    ~GatedSwapChain() override { ++m_destroyed; }
    bool valid() const noexcept override { return true; }
    const graphics::BackendSwapChainDesc& desc() const noexcept override { return m_desc; }
    bool clear_back_buffer(const std::array<float, 4>&) override { return false; }
    bool present(unsigned, unsigned) override { return false; }
    bool present_texture(graphics::BackendTexture2D* source, unsigned, unsigned) override {
        Gate* entered = in_present;
        Gate* release = release_present;
        if (gate_once) in_present = release_present = nullptr;
        if (entered != nullptr) entered->open();
        if (release != nullptr) EXPECT_TRUE(release->wait());
        // What this present shows: the source's first pixel (BGRA) as it is
        // when the present runs.
        if (record_content && source != nullptr) {
            std::vector<u8> bgra(static_cast<usize>(source->desc().width) * source->desc().height * 4u);
            EXPECT_TRUE(source->readback(0, bgra.data(), source->desc().width * 4u));
            shown.push_back({bgra[2], bgra[1], bgra[0], bgra[3]});
            sources.push_back(source);
        }
        ++m_presents;
        return true;
    }
    bool gate_once{false};
    bool record_content{false};
    std::vector<Rgba> shown;
    std::vector<graphics::BackendTexture2D*> sources;
    bool resize(unsigned width, unsigned height, graphics::PixelFormat, unsigned) override {
        m_desc.width = width;
        m_desc.height = height;
        return true;
    }
    std::unique_ptr<graphics::BackendRenderPass> begin_render_pass(
        bool, bool, float, const std::array<graphics::BackendTexture2D*, graphics::BACKEND_MAX_RENDER_TARGETS>&) override {
        return nullptr;
    }
    Gate* in_present{nullptr};
    Gate* release_present{nullptr};

private:
    graphics::BackendSwapChainDesc m_desc{};
    std::atomic<u32>& m_destroyed;
    std::atomic<u32>& m_presents;
};

// The window is destroyed (on the main thread, through WindowManager's
// destroyed callback) while a render thread's first SwapBuffers is making the
// swap chain, which waits for the main thread. Before the fix the render
// thread held the drawable's lock across that wait and detach() waited for
// the lock: neither could proceed. Now detach() completes while the factory
// is still blocked, and the chain made for the dead window is released
// without being installed or presented into.
TEST_P(GlDrawableTest, WindowDestroyDuringFirstPresentDoesNotDeadlock) {
    constexpr u64 kHwnd = 0x1F0;
    std::atomic<u32> destroyed{0}, presents{0};
    Gate in_factory, release_factory;
    const GlSwapChainFactory factory = [&](u64, u32 w, u32 h) -> std::unique_ptr<graphics::BackendSwapChain> {
        in_factory.open();
        EXPECT_TRUE(release_factory.wait());
        return std::make_unique<GatedSwapChain>(w, h, destroyed, presents);
    };
    std::shared_ptr<GlDrawable> d = GlDrawable::attach(kHwnd, kDoubleAlpha, 16, 8, factory);
    ASSERT_NE(d, nullptr);
    std::thread render([&] { EXPECT_TRUE(d->swap()); });
    ASSERT_TRUE(in_factory.wait());
    // The "main thread": DestroyWindow's callback.
    std::future<void> destroy = std::async(std::launch::async, [] { GlDrawable::detach(kHwnd); });
    const bool destroyed_in_time = destroy.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    release_factory.open();   // never leave the render thread blocked
    render.join();
    destroy.wait();
    EXPECT_TRUE(destroyed_in_time) << "detach() waited for the presenting thread";
    EXPECT_EQ(GlDrawable::for_window(kHwnd), nullptr);
    EXPECT_EQ(destroyed.load(), 1u);   // the chain made for the dead window was released
    EXPECT_EQ(presents.load(), 0u);    // and never presented into
    EXPECT_TRUE(d->swap());            // a later swap of the stale drawable presents nothing
    EXPECT_EQ(presents.load(), 0u);
}

// Final rereview, N1: two presents of one window race. Thread A's present of
// frame 1 (red) is in flight, waiting (as on the main thread) inside
// present_texture; the main thread swaps frame 2 (blue). The main thread's
// SwapBuffers must not block, frame 2 must not be dropped (A presents it
// next, latest wins), and the texture A is presenting must not become the
// back buffer the main thread draws frame 3 (green) into.
TEST_P(GlDrawableTest, ConcurrentPresentsShowTheNewestFrame) {
    constexpr u64 kHwnd = 0x1F2;
    std::atomic<u32> destroyed{0}, presents{0};
    Gate in_present, release_present;
    GatedSwapChain* made = nullptr;
    const GlSwapChainFactory factory = [&](u64, u32 w, u32 h) -> std::unique_ptr<graphics::BackendSwapChain> {
        auto chain = std::make_unique<GatedSwapChain>(w, h, destroyed, presents);
        made = chain.get();
        return chain;
    };
    std::shared_ptr<GlDrawable> d = GlDrawable::attach(kHwnd, kDoubleAlpha, 16, 8, factory);
    ASSERT_NE(d, nullptr);
    ASSERT_TRUE(d->swap());   // creates the chain
    ASSERT_NE(made, nullptr);
    const auto clear_back = [&](float r, float g, float b) {
        GlClearParams p;
        p.color = true;
        p.draw_buffer = GL_BACK;
        p.color_value = {r, g, b, 1.0f};
        d->clear(p);
    };
    made->record_content = true;
    made->gate_once = true;
    made->in_present = &in_present;
    made->release_present = &release_present;

    clear_back(1, 0, 0);   // frame 1
    std::thread render([&] { EXPECT_TRUE(d->swap()); });
    ASSERT_TRUE(in_present.wait());
    clear_back(0, 0, 1);   // frame 2, on the "main thread"
    std::future<bool> second = std::async(std::launch::async, [&] { return d->swap(); });
    const bool returned = second.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    if (returned) clear_back(0, 1, 0);   // frame 3 into the back buffer
    release_present.open();
    render.join();
    EXPECT_TRUE(returned) << "the second SwapBuffers waited for the present in flight";
    EXPECT_TRUE(second.get());

    ASSERT_EQ(made->shown.size(), 2u);
    EXPECT_EQ(made->shown[0], kRed);    // frame 1, not overwritten by frame 3
    EXPECT_EQ(made->shown[1], kBlue);   // frame 2 was presented, last
    EXPECT_EQ(made->sources[1], d->target(GL_FRONT));
    EXPECT_NE(made->sources[0], d->target(GL_BACK));   // the presented texture was not recycled
    EXPECT_EQ(pixel_of(*d, GL_BACK), kGreen);
    EXPECT_EQ(pixel_of(*d, GL_FRONT), kBlue);
    GlDrawable::detach(kHwnd);
}

// A present already in flight when the window is destroyed: detach() does not
// wait for it, and the swap chain it uses lives until it ends, then is
// destroyed by the presenting thread (with no drawable lock held).
TEST_P(GlDrawableTest, WindowDestroyDuringPresentKeepsTheChainUntilItEnds) {
    constexpr u64 kHwnd = 0x1F1;
    std::atomic<u32> destroyed{0}, presents{0};
    Gate in_present, release_present;
    GatedSwapChain* made = nullptr;
    const GlSwapChainFactory factory = [&](u64, u32 w, u32 h) -> std::unique_ptr<graphics::BackendSwapChain> {
        auto chain = std::make_unique<GatedSwapChain>(w, h, destroyed, presents);
        made = chain.get();
        return chain;
    };
    std::shared_ptr<GlDrawable> d = GlDrawable::attach(kHwnd, kDoubleAlpha, 16, 8, factory);
    ASSERT_NE(d, nullptr);
    ASSERT_TRUE(d->swap());   // creates the chain and presents once
    ASSERT_NE(made, nullptr);
    ASSERT_EQ(presents.load(), 1u);
    made->in_present = &in_present;
    made->release_present = &release_present;
    std::thread render([&] { EXPECT_TRUE(d->swap()); });
    ASSERT_TRUE(in_present.wait());
    std::future<void> destroy = std::async(std::launch::async, [] { GlDrawable::detach(kHwnd); });
    const bool destroyed_in_time = destroy.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    EXPECT_EQ(destroyed.load(), 0u);   // still presenting through it
    release_present.open();
    render.join();
    destroy.wait();
    EXPECT_TRUE(destroyed_in_time) << "detach() waited for the presenting thread";
    EXPECT_EQ(presents.load(), 2u);
    EXPECT_EQ(destroyed.load(), 1u);   // released when the present ended
}

// Final review, Finding 14: a minimised window's client area is 0 x 0 at a
// SwapBuffers; the drawable keeps its buffers, so the frame drawn right after
// the restore (no wglMakeCurrent in between) lands in them and is shown by
// the next swap.
TEST_P(GlDrawableTest, MinimisedWindowKeepsItsBuffers) {
    GlContext& ctx = current_on(dA);   // 16x8
    auto d = drawable_of(dA);
    ASSERT_NE(d, nullptr);
    dcs_[dA].width = 0;
    dcs_[dA].height = 0;
    EXPECT_EQ(gl().call("wglSwapBuffers", {dA}), 1u);
    EXPECT_EQ(d->width(), 16u);
    EXPECT_EQ(d->height(), 8u);
    dcs_[dA].width = 16;
    dcs_[dA].height = 8;
    cmd::ClearColor(ctx, 0.0f, 0.0f, 1.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);
    EXPECT_EQ(pixel(dA, GL_BACK, 15, 7), kBlue);
    const graphics::BackendTexture2D* drawn = d->target(GL_BACK);
    EXPECT_EQ(gl().call("wglSwapBuffers", {dA}), 1u);
    FakeSwapChain* chain = chains_[0x100];
    ASSERT_NE(chain, nullptr);
    EXPECT_EQ(chain->presented.back(), drawn);
    EXPECT_EQ(pixel(dA, GL_FRONT, 15, 7), kBlue);
}

// Final review, Finding 5: wglDeleteContext of a context with draws still
// batched records them into the drawable (under its mutex) after the WGL lock
// is released. Here another thread holds the drawable's mutex while the
// context is deleted; a third thread's wglMakeCurrent, which needs the WGL
// lock, must not wait for it. The pending draws are still recorded.
TEST_P(GlDrawableTest, DeleteContextRecordsPendingDrawsOutsideTheWglLock) {
    Gate drawn, proceed;
    std::thread owner([&] {
        Caller t(memory_, *registry_, GetParam(), 1);
        const u64 rc = t.create(dA);
        ASSERT_EQ(t.make(dA, rc), 1u);
        t.call("glBegin", {GL_TRIANGLES});
        t.call("glVertex2i", {0, 0});
        t.call("glVertex2i", {1, 0});
        t.call("glVertex2i", {0, 1});
        t.call("glEnd");   // batched, not yet recorded
        drawn.open();
        ASSERT_TRUE(proceed.wait());
        EXPECT_EQ(t.call("wglDeleteContext", {rc}), 1u);
    });
    ASSERT_TRUE(drawn.wait());
    const std::shared_ptr<GlDrawable> d = drawable_of(dA);
    ASSERT_NE(d, nullptr);
    const u32 passes_before = d->passes_opened();
    std::future<bool> made;
    {
        std::lock_guard<std::recursive_mutex> hold(d->mutex());
        proceed.open();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));   // let the delete reach the drawable
        made = std::async(std::launch::async, [&] {
            Caller t(memory_, *registry_, GetParam(), 2);
            const u64 rc = t.create(dB);
            const bool ok = t.make(dB, rc) == 1u;
            (void)t.make(0, 0);
            return ok;
        });
        EXPECT_EQ(made.wait_for(std::chrono::seconds(10)), std::future_status::ready)
            << "wglMakeCurrent waited for wglDeleteContext's drawable lock";
    }
    owner.join();
    EXPECT_TRUE(made.get());
    EXPECT_GT(d->passes_opened(), passes_before);   // the batched triangle was recorded
}

std::string bitness_name(const ::testing::TestParamInfo<bool>& info) { return info.param ? "x64" : "x86"; }

INSTANTIATE_TEST_SUITE_P(Bitness, GlDrawableTest, ::testing::Values(false, true), bitness_name);

}  // namespace
