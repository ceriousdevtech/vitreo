/// @file test_gl_wgl.cpp
/// @brief WGL contexts, sharing and the current-context rules, in both
///        bitnesses, through the real opengl32 exports.
///
/// Every expected value is a Windows 11 measurement with its probe tag
/// (tools/oracle_probe/wgl_contract_probe{32,64}.expected; the two files agree
/// on every line used here). Values the probe does not measure say so.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/gdi32/pixel_format_api.hpp"
#include "winapi/graphics/opengl32/gl_context.hpp"
#include "winapi/graphics/opengl32/gl_enums.hpp"
#include "winapi/graphics/opengl32/gl_pixel_formats.hpp"
#include "winapi/graphics/opengl32/gl_wgl.hpp"
#include "winapi/graphics/opengl32/opengl32.hpp"
#include "winapi/kernel32/error.hpp"
#include "gl_test_caller.hpp"

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::gl;

namespace {

using test::kStale;

// Vitreo's pixel formats (gl_pixel_formats.cpp): 1 = single-buffered 24/8,
// 5 = double-buffered 24/8. The probe's dA/dB are double, dS single.
constexpr i32 kDoubleFormat = 5;
constexpr i32 kSingleFormat = 1;

constexpr u64 dA = 0x5000, dB = 0x5001, dS = 0x5002, dN = 0x5003, dV = 0x5004, dMem = 0x5005;

using test::Caller;

struct FakeDc {
    i32 format{0};
    u64 window{0};
    i32 width{0};
    i32 height{0};
};

class GlWglTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(Caller::kStackBase, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(OPENGL32_SCRATCH_BASE, OPENGL32_SCRATCH_SIZE,
                                       MemoryProtection::ReadWrite).is_ok());
        registry_ = std::make_unique<ThunkRegistry>(0x70000000);
        mgr_ = std::make_unique<Opengl32Manager>();
        mgr_->set_thunk_registry(registry_.get());
        ASSERT_TRUE(registry_->register_dll("opengl32.dll", create_opengl32_thunks(*mgr_)).is_ok());
        dcs_[dA] = {kDoubleFormat, 0x100, 320, 200};
        dcs_[dB] = {kDoubleFormat, 0x101, 640, 480};
        dcs_[dS] = {kSingleFormat, 0x102, 320, 200};
        dcs_[dN] = {0, 0x103, 320, 200};
        dcs_[dV] = {kDoubleFormat, 0x104, 320, 200};
        dcs_[dMem] = {kSingleFormat, 0, 64, 32};
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
        main_ = std::make_unique<Caller>(memory_, *registry_, GetParam(), 0);
    }

    void TearDown() override {
        main_->make(0, 0);
        set_current_gl_context_for_thread(nullptr);
    }

    /// Runs @p body on a fresh thread with its own caller.
    void on_thread(const std::function<void(Caller&)>& body, int slot = 1) {
        std::thread([&] {
            Caller c(memory_, *registry_, GetParam(), slot);
            body(c);
        }).join();
    }

    Caller& gl() { return *main_; }

    GuestMemory memory_;
    std::unique_ptr<ThunkRegistry> registry_;
    std::unique_ptr<Opengl32Manager> mgr_;
    std::map<u64, FakeDc> dcs_;
    std::unique_ptr<Caller> main_;
};

// wgl_contract_probe64.expected: create-null-hdc, create-no-pixel-format,
// create-memdc-no-format, create-memdc-dib (a DC with a format creates), create-ok.
TEST_P(GlWglTest, CreateRequiresPixelFormat) {
    EXPECT_EQ(gl().create(0), 0u);   // create-null-hdc ret=NULL gle=6
    EXPECT_EQ(gl().err(), 6u);
    EXPECT_EQ(gl().create(dN), 0u);  // create-no-pixel-format ret=NULL gle=2000
    EXPECT_EQ(gl().err(), 2000u);
    dcs_[dMem].format = 0;
    EXPECT_EQ(gl().create(dMem), 0u);  // create-memdc-no-format ret=NULL gle=2000
    EXPECT_EQ(gl().err(), 2000u);
    EXPECT_EQ(gl().create(0xBAD), 0u);  // not a DC at all: no pixel format
    EXPECT_EQ(gl().err(), 2000u);

    dcs_[dMem].format = kSingleFormat;
    const u64 mem = gl().create(dMem);  // create-memdc-dib ret=other gle=untouched
    EXPECT_NE(mem, 0u);
    EXPECT_EQ(gl().err(), kStale);
    const u64 a = gl().create(dA);      // create-ok A=yes
    const u64 b = gl().create(dB);
    EXPECT_EQ(gl().err(), kStale);
    EXPECT_NE(a, 0u);
    EXPECT_NE(b, 0u);
    EXPECT_NE(a, b);
    EXPECT_NE(a, mem);
    // The HGLRC is a handle-table entry for a GlContext with the DC's format.
    ASSERT_NE(mgr_->context_for_testing(a), nullptr);
    EXPECT_EQ(wgl_handle_of(mgr_->context_for_testing(a)), a);
    EXPECT_TRUE(mgr_->context_for_testing(a)->framebuffer().double_buffer);
    EXPECT_FALSE(mgr_->context_for_testing(mem)->framebuffer().double_buffer);
    EXPECT_EQ(mgr_->context_for_testing(0x1234), nullptr);
    // Creating a context does not make it current (make-ok is a separate call).
    EXPECT_EQ(gl().cur_ctx(), 0u);
}

// create-memdc-default-bitmap-setpf / wglCreateLayerContext: layer planes do
// not exist (pf-list layer=0 for every format); layer 0 is the main plane.
// No probe line measures a non-zero layer: ERROR_INVALID_PARAMETER is Vitreo's
// choice (Learn documents NULL only).
TEST_P(GlWglTest, LayerContextCreatesOnlyTheMainPlane) {
    const u64 main_plane = gl().call("wglCreateLayerContext", {dA, 0});
    EXPECT_NE(main_plane, 0u);
    EXPECT_EQ(gl().err(), kStale);
    EXPECT_EQ(gl().call("wglCreateLayerContext", {dA, 1}), 0u);
    EXPECT_EQ(gl().err(), 87u);
    EXPECT_EQ(gl().call("wglCreateLayerContext", {dA, static_cast<u64>(-1) & 0xFFFFFFFFu}), 0u);
    EXPECT_EQ(gl().call("wglCreateLayerContext", {0, 0}), 0u);   // create-null-hdc
    EXPECT_EQ(gl().err(), 6u);
    EXPECT_EQ(gl().call("wglCreateLayerContext", {dN, 0}), 0u);  // create-no-pixel-format
    EXPECT_EQ(gl().err(), 2000u);
}

// make-mismatched-format, -rev, -while-current, make-dc-without-pixel-format,
// make-window-format-ctx-onto-bitmap-dc.
TEST_P(GlWglTest, MakeCurrentMismatchedFormatFails) {
    const u64 cA = gl().create(dA);   // double
    const u64 cS = gl().create(dS);   // single
    EXPECT_EQ(gl().make(dN, cA), 0u);  // make-dc-without-pixel-format ret=0 gle=2000
    EXPECT_EQ(gl().err(), 2000u);
    EXPECT_EQ(gl().cur_ctx(), 0u);
    EXPECT_EQ(gl().make(dS, cA), 0u);  // make-mismatched-format ret=0 gle=2000, after cur-ctx=NULL
    EXPECT_EQ(gl().err(), 2000u);
    EXPECT_EQ(gl().cur_ctx(), 0u);
    EXPECT_EQ(gl().cur_dc(), 0u);
    EXPECT_EQ(gl().make(dA, cS), 0u);  // make-mismatched-format-rev ret=0 gle=2000
    EXPECT_EQ(gl().err(), 2000u);
    EXPECT_EQ(gl().cur_ctx(), 0u);

    // make-mismatched-while-current: ret=0 gle=2000 and the current context
    // is released (make-mismatched-while-current-after cur-ctx=NULL).
    ASSERT_EQ(gl().make(dA, cA), 1u);
    EXPECT_EQ(gl().cur_ctx(), cA);
    EXPECT_EQ(gl().make(dS, cA), 0u);
    EXPECT_EQ(gl().err(), 2000u);
    EXPECT_EQ(gl().cur_ctx(), 0u);
    EXPECT_EQ(gl().cur_dc(), 0u);
    EXPECT_EQ(current_gl_context(), nullptr);

    // make-window-format-ctx-onto-bitmap-dc ret=0 gle=2000.
    EXPECT_EQ(gl().make(dMem, cA), 0u);
    EXPECT_EQ(gl().err(), 2000u);
}

// make-null-null-*, make-dc-null-ctx-*, make-null-dc-ctx-*, make-ok*,
// make-same-*, make-switch-to-ctxB, make-*-hglrc.
TEST_P(GlWglTest, MakeCurrentArgumentRules) {
    const u64 cA = gl().create(dA);
    const u64 cB = gl().create(dB);
    // make-null-null-nothing-current ret=0 gle=6, nothing current after.
    EXPECT_EQ(gl().make(0, 0), 0u);
    EXPECT_EQ(gl().err(), 6u);
    // make-dc-null-ctx-nothing-current ret=1 gle=untouched.
    EXPECT_EQ(gl().make(dA, 0), 1u);
    EXPECT_EQ(gl().err(), kStale);
    // make-null-dc-ctx-nothing-current ret=0 gle=6; make-null-dc-after NULL.
    EXPECT_EQ(gl().make(0, cA), 0u);
    EXPECT_EQ(gl().err(), 6u);
    EXPECT_EQ(gl().cur_ctx(), 0u);

    // make-ok ret=1 gle=untouched, make-ok-after cur-ctx=ctxA cur-dc=non-null.
    EXPECT_EQ(gl().make(dA, cA), 1u);
    EXPECT_EQ(gl().err(), kStale);
    EXPECT_EQ(gl().cur_ctx(), cA);
    EXPECT_EQ(gl().cur_dc(), dA);
    EXPECT_EQ(current_gl_context(), mgr_->context_for_testing(cA));
    // make-same-again ret=1; make-same-format-other-window ret=1 (new window).
    EXPECT_EQ(gl().make(dA, cA), 1u);
    EXPECT_EQ(gl().make(dV, cA), 1u);
    EXPECT_EQ(gl().cur_dc(), dV);
    // make-null-dc-ctx-while-current ret=0 gle=6 and ctxA stays current.
    EXPECT_EQ(gl().make(0, cA), 0u);
    EXPECT_EQ(gl().err(), 6u);
    EXPECT_EQ(gl().cur_ctx(), cA);
    EXPECT_EQ(current_gl_context(), mgr_->context_for_testing(cA));
    // make-switch-to-ctxB ret=1, cur-ctx=ctxB.
    EXPECT_EQ(gl().make(dB, cB), 1u);
    EXPECT_EQ(gl().cur_ctx(), cB);
    EXPECT_EQ(current_gl_context(), mgr_->context_for_testing(cB));
    // make-dc-null-ctx-releases ret=1, cur-ctx=NULL cur-dc=NULL.
    EXPECT_EQ(gl().make(dB, 0), 1u);
    EXPECT_EQ(gl().err(), kStale);
    EXPECT_EQ(gl().cur_ctx(), 0u);
    EXPECT_EQ(gl().cur_dc(), 0u);
    EXPECT_EQ(current_gl_context(), nullptr);
    // make-null-null-releases ret=1.
    ASSERT_EQ(gl().make(dA, cA), 1u);
    EXPECT_EQ(gl().make(0, 0), 1u);
    EXPECT_EQ(gl().err(), kStale);
    EXPECT_EQ(gl().cur_ctx(), 0u);

    // make-garbage-hglrc ret=0 gle=6, after cur-ctx=NULL (current is released).
    ASSERT_EQ(gl().make(dA, cA), 1u);
    EXPECT_EQ(gl().make(dA, 0x1234), 0u);
    EXPECT_EQ(gl().err(), 6u);
    EXPECT_EQ(gl().cur_ctx(), 0u);
}

// make-second-thread-same-ctx-same-dc / -other-dc / -other-ctx,
// delete-second-thread-current-ctx, make-second-thread-null-null-nothing-
// current, share-second-thread-current-ctx, make-second-thread-after-release.
TEST_P(GlWglTest, MakeCurrentOnSecondThreadFails) {
    const u64 cA = gl().create(dA);
    const u64 cB = gl().create(dB);
    ASSERT_EQ(gl().make(dA, cA), 1u);
    on_thread([&](Caller& t) {
        EXPECT_EQ(t.make(dA, cA), 0u);   // ret=0 gle=170 thread-current-after=NULL
        EXPECT_EQ(t.err(), 170u);
        EXPECT_EQ(t.cur_ctx(), 0u);
        EXPECT_EQ(t.make(dB, cA), 0u);   // same ctx, other dc: 170
        EXPECT_EQ(t.err(), 170u);
        EXPECT_EQ(t.cur_ctx(), 0u);
        EXPECT_EQ(t.make(dB, cB), 1u);   // other ctx: ret=1 thread-current-after=ctxB
        EXPECT_EQ(t.err(), kStale);
        EXPECT_EQ(t.cur_ctx(), cB);
        EXPECT_EQ(t.make(0, 0), 1u);     // release it again
        EXPECT_EQ(t.call("wglDeleteContext", {cA}), 0u);   // ret=0 gle=170
        EXPECT_EQ(t.err(), 170u);
        EXPECT_EQ(t.make(0, 0), 0u);     // nothing current on this thread: ret=0 gle=6
        EXPECT_EQ(t.err(), 6u);
        EXPECT_EQ(t.call("wglShareLists", {cA, cA}), 0u);  // ret=0 gle=170
        EXPECT_EQ(t.err(), 170u);
        // noctx-*: this thread has no context although another thread does.
        EXPECT_EQ(current_gl_context(), nullptr);
        EXPECT_EQ(t.call("glGetError"), 0x0502u);
        EXPECT_EQ(t.cur_ctx(), 0u);
        EXPECT_EQ(t.cur_dc(), 0u);
    });
    // make-second-thread-after-main: the main thread still has ctxA.
    EXPECT_EQ(gl().cur_ctx(), cA);
    EXPECT_EQ(current_gl_context(), mgr_->context_for_testing(cA));
    EXPECT_EQ(gl().call("glGetError"), 0u);   // make-second-thread-main-glGetError ret=0

    // After the owner released it another thread may take it, and the thread
    // keeps it (make-second-thread-after-release): main sees it as taken.
    ASSERT_EQ(gl().make(0, 0), 1u);
    std::atomic<bool> holding{false}, finish{false};
    std::thread holder([&] {
        Caller t(memory_, *registry_, GetParam(), 2);
        EXPECT_EQ(t.make(dA, cA), 1u);   // make-second-thread-after-release ret=1
        holding = true;
        while (!finish) std::this_thread::yield();
        EXPECT_EQ(t.make(0, 0), 1u);
    });
    while (!holding) std::this_thread::yield();
    EXPECT_EQ(gl().cur_ctx(), 0u);       // make-second-thread-after-release-main cur-ctx=NULL
    EXPECT_EQ(gl().make(dA, cA), 0u);
    EXPECT_EQ(gl().err(), 170u);
    finish = true;
    holder.join();
    EXPECT_EQ(gl().make(dA, cA), 1u);    // make-ok-5: main retakes ctxA
}

// A context stays owned by its thread until it releases it; a thread that
// ends without releasing does not strand the context.
TEST_P(GlWglTest, ThreadEndReleasesItsCurrentContext) {
    const u64 cA = gl().create(dA);
    on_thread([&](Caller& t) { ASSERT_EQ(t.make(dA, cA), 1u); });
    EXPECT_EQ(gl().make(dA, cA), 1u);
    EXPECT_EQ(gl().err(), kStale);
}

// delete-ok, make-deleted-hglrc, delete-twice, delete-null, delete-garbage,
// delete-current-*.
TEST_P(GlWglTest, DeleteCurrentContextReleasesIt) {
    const u64 cD = gl().create(dA);
    EXPECT_EQ(gl().call("wglDeleteContext", {cD}), 1u);   // delete-ok ret=1 gle=untouched
    EXPECT_EQ(gl().err(), kStale);
    EXPECT_EQ(mgr_->context_for_testing(cD), nullptr);
    EXPECT_EQ(gl().make(dA, cD), 0u);                     // make-deleted-hglrc ret=0 gle=6
    EXPECT_EQ(gl().err(), 6u);
    EXPECT_EQ(gl().cur_ctx(), 0u);
    EXPECT_EQ(gl().call("wglDeleteContext", {cD}), 0u);   // delete-twice ret=0 gle=6
    EXPECT_EQ(gl().err(), 6u);
    EXPECT_EQ(gl().call("wglDeleteContext", {0}), 0u);    // delete-null
    EXPECT_EQ(gl().err(), 6u);
    EXPECT_EQ(gl().call("wglDeleteContext", {0x1234}), 0u);  // delete-garbage
    EXPECT_EQ(gl().err(), 6u);

    const u64 cA = gl().create(dA);
    const u64 c = gl().create(dA);
    ASSERT_EQ(gl().make(dA, c), 1u);                      // delete-current-setup
    ASSERT_EQ(current_gl_context(), mgr_->context_for_testing(c));
    EXPECT_EQ(gl().call("wglDeleteContext", {c}), 1u);    // delete-current ret=1 gle=untouched
    EXPECT_EQ(gl().err(), kStale);
    EXPECT_EQ(gl().cur_ctx(), 0u);                        // cur-ctx-after=NULL cur-dc-after=NULL
    EXPECT_EQ(gl().cur_dc(), 0u);
    EXPECT_EQ(current_gl_context(), nullptr);
    EXPECT_EQ(gl().call("glGetError"), 0x0502u);          // delete-current-glGetError ret=1282
    EXPECT_EQ(gl().call("glIsEnabled", {GL_DEPTH_TEST}), 0u);  // delete-current-glIsEnabled ret=0
    EXPECT_EQ(gl().make(dA, c), 0u);                      // delete-current-then-make-deleted ret=0 gle=6
    EXPECT_EQ(gl().err(), 6u);
    EXPECT_EQ(gl().make(dA, cA), 1u);                     // delete-current-then-make-other ret=1
    EXPECT_EQ(gl().cur_ctx(), cA);
}

// share-before, share-after-gentextures(-dst/-both), share-src-current,
// share-self, share-null-src/-dst, share-already-shared, share-different-format,
// share-deleted-dst, share-source-deleted-visibility.
TEST_P(GlWglTest, ShareListsBeforeObjectsSucceedsAfterFails) {
    auto texture = [&](u64 rc, u32 name) {
        GlContext* ctx = mgr_->context_for_testing(rc);
        ASSERT_NE(ctx, nullptr);
        const GlShareGroup::Lock held = ctx->share_group->lock();
        auto& slot = ctx->share_group->textures(held)[name];
        slot = std::make_shared<GlTextureObject>();
        slot->name = name;
    };
    auto sees = [&](u64 rc, u32 name) {
        const auto& group = mgr_->context_for_testing(rc)->share_group;
        const GlShareGroup::Lock held = group->lock();
        return group->textures(held).count(name) != 0;
    };
    auto share = [&](u64 src, u64 dst) { return gl().call("wglShareLists", {src, dst}); };

    // share-before ret=1 gle=untouched: an object made in s1 after the share
    // is visible in s2 (share-before-visibility).
    const u64 c1 = gl().create(dA), c2 = gl().create(dA);
    EXPECT_EQ(share(c1, c2), 1u);
    EXPECT_EQ(gl().err(), kStale);
    texture(c1, 7);
    EXPECT_TRUE(sees(c2, 7));

    // share-after-gentextures ret=1: the source already has objects, dst empty.
    const u64 c3 = gl().create(dA), c4 = gl().create(dA);
    texture(c3, 11);
    EXPECT_EQ(share(c3, c4), 1u);
    EXPECT_EQ(gl().err(), kStale);
    EXPECT_TRUE(sees(c4, 11));

    // share-after-gentextures-dst ret=0 gle=87: dst has objects, src empty;
    // nothing leaks either way (dst-texture-visible-in-src=0).
    const u64 c5 = gl().create(dA), c6 = gl().create(dA);
    texture(c6, 12);
    EXPECT_EQ(share(c5, c6), 0u);
    EXPECT_EQ(gl().err(), 87u);
    EXPECT_FALSE(sees(c5, 12));
    EXPECT_TRUE(sees(c6, 12));

    // share-after-gentextures-both ret=0 gle=87.
    const u64 c7 = gl().create(dA), c8 = gl().create(dA);
    texture(c7, 13);
    texture(c8, 14);
    EXPECT_EQ(share(c7, c8), 0u);
    EXPECT_EQ(gl().err(), 87u);

    // share-src-current ret=1 (source current on this thread, nothing generated).
    const u64 c9 = gl().create(dA), c10 = gl().create(dA);
    ASSERT_EQ(gl().make(dA, c9), 1u);
    EXPECT_EQ(share(c9, c10), 1u);
    EXPECT_EQ(gl().err(), kStale);
    EXPECT_EQ(gl().cur_ctx(), c9);
    // share-self ret=0 gle=87; share-null-src / -null-dst ret=0 gle=6.
    EXPECT_EQ(share(c9, c9), 0u);
    EXPECT_EQ(gl().err(), 87u);
    EXPECT_EQ(share(0, c9), 0u);
    EXPECT_EQ(gl().err(), 6u);
    EXPECT_EQ(share(c9, 0), 0u);
    EXPECT_EQ(gl().err(), 6u);
    // share-already-shared ret=0 gle=87.
    EXPECT_EQ(share(c9, c10), 0u);
    EXPECT_EQ(gl().err(), 87u);
    // A context of the group may share its objects to a third empty context.
    const u64 c10b = gl().create(dA);
    EXPECT_EQ(share(c9, c10b), 1u);
    texture(c10b, 21);
    EXPECT_TRUE(sees(c9, 21));
    EXPECT_TRUE(sees(c10, 21));
    ASSERT_EQ(gl().make(0, 0), 1u);

    // share-different-format ret=1: double-buffered with single-buffered.
    const u64 c11 = gl().create(dS), c12 = gl().create(dA);
    EXPECT_EQ(share(c12, c11), 1u);
    EXPECT_EQ(gl().err(), kStale);
    // share-deleted-dst ret=0 gle=6.
    const u64 c13 = gl().create(dA);
    ASSERT_EQ(gl().call("wglDeleteContext", {c13}), 1u);
    EXPECT_EQ(share(c12, c13), 0u);
    EXPECT_EQ(gl().err(), 6u);

    // share-source-deleted-visibility list-still-visible=1: the group outlives
    // the context that created the object.
    const u64 c14 = gl().create(dA), c15 = gl().create(dA);
    ASSERT_EQ(share(c14, c15), 1u);
    texture(c14, 31);
    ASSERT_EQ(gl().call("wglDeleteContext", {c14}), 1u);
    EXPECT_TRUE(sees(c15, 31));
}

// gpa-noctx / gpa-ctx: every name is NULL without a context (gle 6); with
// one, NULL with gle 127 except the extension entry points GDI Generic serves
// (gpa-ctx glColorTableEXT / glAddSwapHintRectWIN non-null, gle untouched;
// ext-gpa lists the rest). Measured names: kGpaNames in wgl_contract_probe.c.
TEST_P(GlWglTest, GetProcAddressMatchesOracle) {
    static const char* const kNames[] = {
        "glVertex3f", "glActiveTextureARB", "", "glBogus", "glGenTextures", "glDrawArrays",
        "glDrawArraysEXT", "glBindTextureEXT", "glMultiTexCoord2fARB", "glGetStringi",
        "glGenBuffersARB", "glCreateShader", "wglSwapIntervalEXT", "wglGetExtensionsStringARB",
        "wglGetExtensionsStringEXT", "wglChoosePixelFormatARB", "wglCreateContextAttribsARB",
        "wglCreateContext", "wglGetProcAddress", "glGetString", "glArrayElementEXT",
        "glLockArraysEXT", "glBlendColorEXT", "glPolygonOffsetEXT", "glColorTableEXT",
        "glAddSwapHintRectWIN", "glTexImage3D", "glTexImage3DEXT"};
    const GuestAddr name_addr = 0x10200;
    auto gpa = [&](const char* name) {
        (void)memory_.write_bytes(name_addr, reinterpret_cast<const Byte*>(name), std::strlen(name) + 1);
        return gl().call("wglGetProcAddress", {name_addr});
    };
    for (const char* name : kNames) {
        EXPECT_EQ(gpa(name), 0u) << name;     // gpa-noctx ret=NULL gle=6
        EXPECT_EQ(gl().err(), 6u) << name;
    }
    EXPECT_EQ(gl().call("wglGetProcAddress", {0}), 0u);   // gpa-noctx name=(NULL): ret=NULL gle=6
    EXPECT_EQ(gl().err(), 6u);

    const u64 cA = gl().create(dA);
    ASSERT_EQ(gl().make(dA, cA), 1u);
    for (const char* name : kNames) {
        const std::string n = name;
        const u64 got = gpa(name);
        if (n == "glColorTableEXT" || n == "glAddSwapHintRectWIN") {
            EXPECT_NE(got, 0u) << name;           // gpa-ctx ret=non-null gle=untouched
            EXPECT_EQ(gl().err(), kStale) << name;
            continue;
        }
        EXPECT_EQ(got, 0u) << name;               // gpa-ctx ret=NULL gle=127 (core names and unknown)
        EXPECT_EQ(gl().err(), 127u) << name;      // oracle: gpa-ctx gle=127(0x7f)
    }
    EXPECT_EQ(gpa("glVertex3f"), gpa("glVertex3f"));   // gpa-ctx-stable
    // ext-gpa: the other names GDI Generic serves, and some it does not
    // (case matters, glcolortableext; GL 1.2 names without the suffix).
    for (const char* name : {"glColorSubTableEXT", "glGetColorTableEXT", "glGetColorTableParameterivEXT",
                             "glGetColorTableParameterfvEXT", "glDrawRangeElementsWIN"}) {
        EXPECT_NE(gpa(name), 0u) << name;
        EXPECT_EQ(gpa(name), gpa(name)) << name;
    }
    for (const char* name : {"glColorTable", "glColorSubTable", "glGetColorTable", "glAddSwapHintRect",
                             "glAddSwapHintRectWin", "glcolortableext", "glColorTableSGI", "glColorPointerEXT"}) {
        EXPECT_EQ(gpa(name), 0u) << name;
        EXPECT_EQ(gl().err(), 127u) << name;
    }
    // GL_EXTENSIONS lists GDI Generic's three extensions.
    const u64 ext = gl().call("glGetString", {GL_EXTENSIONS});
    ASSERT_NE(ext, 0u);
    const std::string want = "GL_WIN_swap_hint GL_EXT_bgra GL_EXT_paletted_texture";
    std::vector<u8> got(want.size() + 1);
    ASSERT_TRUE(memory_.read_bytes(static_cast<GuestAddr>(ext), got.data(), got.size()).is_ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(got.data())), want);
}

TEST_P(GlWglTest, GetCurrentDCReturnsMakeCurrentDC) {
    const u64 cA = gl().create(dA);
    EXPECT_EQ(gl().cur_dc(), 0u);                 // noctx-wglGetCurrentDC ret=NULL gle=untouched
    EXPECT_EQ(gl().err(), kStale);
    EXPECT_EQ(gl().cur_ctx(), 0u);                // noctx-wglGetCurrentContext
    EXPECT_EQ(gl().err(), kStale);
    ASSERT_EQ(gl().make(dA, cA), 1u);
    EXPECT_EQ(gl().cur_dc(), dA);                 // make-ok-after cur-dc=non-null
    EXPECT_EQ(gl().err(), kStale);
    ASSERT_EQ(gl().make(dB, cA), 1u);             // make-same-format-other-window
    EXPECT_EQ(gl().cur_dc(), dB);
    // Task 6's route from a GL command to the drawable.
    const WglBinding b = wgl_current_binding();
    EXPECT_EQ(b.hglrc, cA);
    EXPECT_EQ(b.hdc, dB);
    EXPECT_EQ(b.pixel_format, kDoubleFormat);
    EXPECT_EQ(mgr_->window_of_dc(b.hdc), 0x101u);
    // The binding is per thread.
    on_thread([&](Caller& t) {
        EXPECT_EQ(t.cur_dc(), 0u);
        EXPECT_EQ(wgl_current_binding().hglrc, 0u);
    });
}

// wglCopyContext: Learn documents "copies the state named by mask, glPushAttrib
// bit values"; no probe line measures it. Unnamed groups keep their values.
TEST_P(GlWglTest, CopyContextCopiesTheMaskedGroups) {
    const u64 src = gl().create(dA), dst = gl().create(dA);
    GlContext* s = mgr_->context_for_testing(src);
    GlContext* d = mgr_->context_for_testing(dst);
    s->state.line.width = 5.0f;
    s->state.point.size = 3.0f;
    s->state.depth_buffer.func = GL_GREATER;
    s->state.enable.set(GlCap::DepthTest, true);
    s->state.enable.set(GlCap::Blend, true);
    s->state.viewport.width = 77;
    const float line0 = d->state.line.width, point0 = d->state.point.size;
    const u32 func0 = d->state.depth_buffer.func;

    // LINE and ENABLE only.
    EXPECT_EQ(gl().call("wglCopyContext", {src, dst, GL_LINE_BIT | GL_ENABLE_BIT}), 1u);
    EXPECT_EQ(gl().err(), kStale);
    EXPECT_EQ(d->state.line.width, 5.0f);
    EXPECT_TRUE(d->state.enable.test(GlCap::DepthTest));
    EXPECT_TRUE(d->state.enable.test(GlCap::Blend));
    EXPECT_EQ(d->state.point.size, point0);
    EXPECT_EQ(d->state.depth_buffer.func, func0);
    EXPECT_NE(d->state.viewport.width, 77);
    // POINT_BIT then DEPTH_BUFFER_BIT, one more each.
    EXPECT_EQ(gl().call("wglCopyContext", {src, dst, GL_POINT_BIT}), 1u);
    EXPECT_EQ(d->state.point.size, 3.0f);
    EXPECT_EQ(d->state.depth_buffer.func, func0);
    EXPECT_EQ(gl().call("wglCopyContext", {src, dst, GL_DEPTH_BUFFER_BIT}), 1u);
    EXPECT_EQ(d->state.depth_buffer.func, static_cast<u32>(GL_GREATER));
    EXPECT_NE(line0, 5.0f);
    // Mask 0 copies nothing; ALL copies the viewport too.
    s->state.line.width = 9.0f;
    EXPECT_EQ(gl().call("wglCopyContext", {src, dst, 0}), 1u);
    EXPECT_EQ(d->state.line.width, 5.0f);
    EXPECT_EQ(gl().call("wglCopyContext", {src, dst, 0xFFFFFFFFu}), 1u);
    EXPECT_EQ(d->state.line.width, 9.0f);
    EXPECT_EQ(d->state.viewport.width, 77);

    // The source may be current on the calling thread; an unknown handle
    // fails with 6; a source or destination current on another thread is busy
    // (170, Vitreo's choice: its state is changing there, unlocked; final
    // review, Finding 6).
    ASSERT_EQ(gl().make(dA, src), 1u);
    EXPECT_EQ(gl().call("wglCopyContext", {src, dst, GL_LINE_BIT}), 1u);
    EXPECT_EQ(gl().call("wglCopyContext", {src, 0x1234, GL_LINE_BIT}), 0u);
    EXPECT_EQ(gl().err(), 6u);
    EXPECT_EQ(gl().call("wglCopyContext", {0, dst, GL_LINE_BIT}), 0u);
    EXPECT_EQ(gl().err(), 6u);
    on_thread([&](Caller& t) {
        EXPECT_EQ(t.call("wglCopyContext", {dst, src, GL_LINE_BIT}), 0u);
        EXPECT_EQ(t.err(), 170u);
        EXPECT_EQ(t.call("wglCopyContext", {src, dst, GL_LINE_BIT}), 0u);   // the source
        EXPECT_EQ(t.err(), 170u);
    });
}

// viewport-initial, viewport-after-resize-no-remake, viewport-remake-same,
// viewport-after-resize-remake, -release-and-make, -explicit-set,
// viewport-second-context-first-make.
TEST_P(GlWglTest, InitialViewportIsSetOnFirstMakeCurrentOnly) {
    auto vp = [&](u64 rc) {
        const GlContext* c = mgr_->context_for_testing(rc);
        return std::vector<i32>{c->state.viewport.x, c->state.viewport.y, c->state.viewport.width,
                                c->state.viewport.height, c->state.scissor.x, c->state.scissor.y,
                                c->state.scissor.width, c->state.scissor.height};
    };
    const u64 cV = gl().create(dV);
    // viewport-initial client=320x200 viewport=0,0,320,200 scissor=0,0,320,200
    ASSERT_EQ(gl().make(dV, cV), 1u);
    EXPECT_EQ(vp(cV), (std::vector<i32>{0, 0, 320, 200, 0, 0, 320, 200}));
    // viewport-after-resize-no-remake: client=400x300, still 320x200.
    dcs_[dV].width = 400;
    dcs_[dV].height = 300;
    EXPECT_EQ(vp(cV), (std::vector<i32>{0, 0, 320, 200, 0, 0, 320, 200}));
    // viewport-remake-same / -after-resize-remake: unchanged.
    EXPECT_EQ(gl().make(dV, cV), 1u);
    EXPECT_EQ(vp(cV), (std::vector<i32>{0, 0, 320, 200, 0, 0, 320, 200}));
    // viewport-after-resize-release-and-make: unchanged.
    ASSERT_EQ(gl().make(0, 0), 1u);
    ASSERT_EQ(gl().make(dV, cV), 1u);
    EXPECT_EQ(vp(cV), (std::vector<i32>{0, 0, 320, 200, 0, 0, 320, 200}));
    // viewport-explicit-set: glViewport(10,20,30,40) moves the viewport only,
    // and a later resize + remake leaves both alone.
    gl().call("glViewport", {10, 20, 30, 40});
    EXPECT_EQ(vp(cV), (std::vector<i32>{10, 20, 30, 40, 0, 0, 320, 200}));
    dcs_[dV].width = 500;
    dcs_[dV].height = 400;
    ASSERT_EQ(gl().make(0, 0), 1u);
    ASSERT_EQ(gl().make(dV, cV), 1u);
    EXPECT_EQ(vp(cV), (std::vector<i32>{10, 20, 30, 40, 0, 0, 320, 200}));
    // viewport-second-context-first-make client=500x400: a new context's
    // first make-current takes the window size as it is now.
    const u64 c2 = gl().create(dV);
    ASSERT_EQ(gl().make(dV, c2), 1u);
    EXPECT_EQ(vp(c2), (std::vector<i32>{0, 0, 500, 400, 0, 0, 500, 400}));
    // A failed make-current does not consume the first-time setup.
    const u64 c3 = gl().create(dA);
    EXPECT_EQ(gl().make(dS, c3), 0u);
    EXPECT_FALSE(mgr_->context_for_testing(c3)->has_been_current());
}

// A memory DC (no window) takes its size from the bitmap query.
TEST_P(GlWglTest, InitialViewportOfAMemoryDcIsItsBitmap) {
    const u64 c = gl().create(dMem);
    ASSERT_EQ(gl().make(dMem, c), 1u);
    const GlContext* ctx = mgr_->context_for_testing(c);
    EXPECT_EQ(ctx->state.viewport.width, 64);
    EXPECT_EQ(ctx->state.viewport.height, 32);
    EXPECT_EQ(ctx->state.scissor.width, 64);
    EXPECT_EQ(ctx->state.scissor.height, 32);
}

// wglUseFontBitmaps/Outlines are Phase 2: they fail with FALSE.
TEST_P(GlWglTest, UseFontFailsLoudly) {
    for (const char* name : {"wglUseFontBitmapsA", "wglUseFontBitmapsW"}) {
        EXPECT_EQ(gl().call(name, {dA, 32, 96, 1000}), 0u) << name;
        EXPECT_EQ(gl().err(), 50u) << name;
    }
    for (const char* name : {"wglUseFontOutlinesA", "wglUseFontOutlinesW"}) {
        EXPECT_EQ(gl().call(name, {dA, 32, 96, 1000, 0, 0, 0, 0}), 0u) << name;
        EXPECT_EQ(gl().err(), 50u) << name;
    }
}

static std::string bitness_name(const ::testing::TestParamInfo<bool>& info) {
    return info.param ? "x64" : "x86";
}

// wglGetDefaultProcAddress: wgldefproc-noctx / wgldefproc-ctx measured NULL with
// ERROR_PROC_NOT_FOUND (127) for glVertex3f, wglSwapIntervalEXT, "", glTexImage3D
// and glBogus, with and without a context; non-NULL (gle untouched) for
// glColorTableEXT and glAddSwapHintRectWIN either way, the pointers
// wglGetProcAddress gives (ext-defproc same-as-gpa=1). Windows faults on a NULL
// name; Vitreo answers NULL/127.
TEST_P(GlWglTest, GetDefaultProcAddressIsNullWithNotFound) {
    const GuestAddr name_addr = 0x10200;
    auto defproc = [&](const char* name) {
        (void)memory_.write_bytes(name_addr, reinterpret_cast<const Byte*>(name), std::strlen(name) + 1);
        return gl().call("wglGetDefaultProcAddress", {name_addr});
    };
    for (const char* name : {"glVertex3f", "wglSwapIntervalEXT", "", "glTexImage3D", "glBogus"}) {
        EXPECT_EQ(defproc(name), 0u) << name;     // wgldefproc-noctx ret=NULL gle=127
        EXPECT_EQ(gl().err(), 127u) << name;
    }
    std::vector<u64> noctx;
    for (const char* name : {"glColorTableEXT", "glAddSwapHintRectWIN"}) {
        noctx.push_back(defproc(name));
        EXPECT_NE(noctx.back(), 0u) << name;      // wgldefproc-noctx ret=non-null gle=untouched
        EXPECT_EQ(gl().err(), kStale) << name;
    }
    EXPECT_EQ(gl().call("wglGetDefaultProcAddress", {0}), 0u);   // name=(NULL): Windows faults
    EXPECT_EQ(gl().err(), 127u);
    const u64 cA = gl().create(dA);
    ASSERT_EQ(gl().make(dA, cA), 1u);
    for (const char* name : {"glVertex3f", "wglSwapIntervalEXT", "", "glTexImage3D", "glBogus"}) {
        EXPECT_EQ(defproc(name), 0u) << name;     // wgldefproc-ctx ret=NULL gle=127
        EXPECT_EQ(gl().err(), 127u) << name;
    }
    EXPECT_EQ(defproc("glColorTableEXT"), noctx[0]);   // wgldefproc-ctx non-null, ext-defproc same-as-gpa
    EXPECT_EQ(defproc("glAddSwapHintRectWIN"), noctx[1]);
    const GuestAddr name_addr2 = 0x10200;
    (void)memory_.write_bytes(name_addr2, reinterpret_cast<const Byte*>("glColorTableEXT"), 16);
    EXPECT_EQ(gl().call("wglGetProcAddress", {name_addr2}), noctx[0]);
}

// wgllayer-describe-*: no pixel format has a layer plane.
TEST_P(GlWglTest, DescribeLayerPlaneRejectsEveryPlane) {
    const GuestAddr buf = 0x10400;
    (void)memory_.write_u32(buf, 0xABABABABu);
    auto describe = [&](u64 dc, u64 pf, u64 plane, u64 nbytes, u64 out) {
        return gl().call("wglDescribeLayerPlane", {dc, pf, plane, nbytes, out});
    };
    EXPECT_EQ(describe(dA, 5, 1, 32, buf), 0u);   // plane1-size ret=0 gle=87
    EXPECT_EQ(gl().err(), 87u);
    EXPECT_EQ(describe(dA, 5, 1, 0, buf), 0u);    // plane1-nbytes0-buf 87
    EXPECT_EQ(gl().err(), 87u);
    EXPECT_EQ(describe(dA, 5, 1, 0, 0), 0u);      // plane1-nbytes0-null 87
    EXPECT_EQ(gl().err(), 87u);
    EXPECT_EQ(describe(dA, 5, 0, 32, buf), 0u);   // plane0-size ret=0 gle=untouched
    EXPECT_EQ(gl().err(), kStale);
    EXPECT_EQ(describe(dA, 5, 0xFFFFFFFFu, 32, buf), 0u);   // plane-1-size 87
    EXPECT_EQ(gl().err(), 87u);
    EXPECT_EQ(describe(dA, 5, 0x7FFFFFFFu, 32, buf), 0u);   // plane-max-size 87
    EXPECT_EQ(gl().err(), 87u);
    EXPECT_EQ(describe(dA, 0, 1, 32, buf), 0u);   // plane1-pf0 87
    EXPECT_EQ(gl().err(), 87u);
    EXPECT_EQ(describe(dA, 17, 1, 32, buf), 0u);  // plane1-pf-count+1 87
    EXPECT_EQ(gl().err(), 87u);
    EXPECT_EQ(describe(dA, 5, 1, 32, 0), 0u);     // plane1-null-buf 87
    EXPECT_EQ(gl().err(), 87u);
    EXPECT_EQ(describe(0, 5, 1, 32, buf), 0u);    // plane1-null-hdc ret=0 gle=6
    EXPECT_EQ(gl().err(), 6u);
    EXPECT_EQ(describe(dN, 5, 1, 32, buf), 0u);   // plane1-dc-no-format 87
    EXPECT_EQ(gl().err(), 87u);
    EXPECT_EQ(memory_.read_u32(buf).value(), 0xABABABABu);   // written=0 in every row
}

// wgllayer-setpal-*, -getpal-*, -realize-*: FALSE everywhere; 87 for plane != 0,
// the last error untouched for plane 0, 2000 for a NULL hdc or a DC without a format.
TEST_P(GlWglTest, LayerPaletteCallsFailAsMeasured) {
    const GuestAddr colors = 0x10400, out = 0x10440;
    for (int i = 0; i < 4; ++i) {
        (void)memory_.write_u32(colors + 4 * i, 0x010203u * (i + 1));
        (void)memory_.write_u32(out + 4 * i, 0x77777777u);
    }
    struct Row { const char* tag; const char* name; std::vector<u64> args; u32 gle; };
    const u64 m1 = 0xFFFFFFFFu;
    const std::vector<Row> rows = {
        {"wgllayer-setpal-plane1", "wglSetLayerPaletteEntries", {dA, 1, 0, 4, colors}, 87},
        {"wgllayer-setpal-plane0", "wglSetLayerPaletteEntries", {dA, 0, 0, 4, colors}, kStale},
        {"wgllayer-setpal-plane-1", "wglSetLayerPaletteEntries", {dA, m1, 0, 4, colors}, 87},
        {"wgllayer-setpal-count0", "wglSetLayerPaletteEntries", {dA, 1, 0, 0, colors}, 87},
        {"wgllayer-setpal-null", "wglSetLayerPaletteEntries", {dA, 1, 0, 4, 0}, 87},
        {"wgllayer-setpal-null-hdc", "wglSetLayerPaletteEntries", {0, 1, 0, 4, colors}, 2000},
        {"wgllayer-setpal-no-format", "wglSetLayerPaletteEntries", {dN, 1, 0, 4, colors}, 2000},
        {"wgllayer-getpal-plane1", "wglGetLayerPaletteEntries", {dA, 1, 0, 4, out}, 87},
        {"wgllayer-getpal-plane0", "wglGetLayerPaletteEntries", {dA, 0, 0, 4, out}, kStale},
        {"wgllayer-getpal-count0", "wglGetLayerPaletteEntries", {dA, 1, 0, 0, out}, 87},
        {"wgllayer-getpal-null", "wglGetLayerPaletteEntries", {dA, 1, 0, 4, 0}, 87},
        {"wgllayer-getpal-null-hdc", "wglGetLayerPaletteEntries", {0, 1, 0, 4, out}, 2000},
        {"wgllayer-getpal-no-format", "wglGetLayerPaletteEntries", {dN, 1, 0, 4, out}, 2000},
        {"wgllayer-realize-plane1-true", "wglRealizeLayerPalette", {dA, 1, 1}, 87},
        {"wgllayer-realize-plane1-false", "wglRealizeLayerPalette", {dA, 1, 0}, 87},
        {"wgllayer-realize-plane0", "wglRealizeLayerPalette", {dA, 0, 1}, kStale},
        {"wgllayer-realize-plane-1", "wglRealizeLayerPalette", {dA, m1, 1}, 87},
        {"wgllayer-realize-null-hdc", "wglRealizeLayerPalette", {0, 1, 1}, 2000},
        {"wgllayer-realize-no-format", "wglRealizeLayerPalette", {dN, 1, 1}, 2000},
    };
    for (const Row& r : rows) {
        EXPECT_EQ(gl().call(r.name, r.args), 0u) << r.tag;
        EXPECT_EQ(gl().err(), r.gle) << r.tag;
    }
    for (int i = 0; i < 4; ++i) {   // getpal-plane1 out=77777777 untouched=1
        EXPECT_EQ(memory_.read_u32(out + 4 * i).value(), 0x77777777u);
    }
}

// opengl32's pixel-format exports and gdi32's are one code path: wglpf-X lines
// equal their wglpf-X-gdi lines on the same input in the same order, and a
// format set through one is seen by the other (wglpf-cross-*). Counts and
// indices are Vitreo's list (16 formats), the return/last-error shapes are the
// measured ones.
class GlWglPixelFormatTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(Caller::kStackBase, 0x10000, MemoryProtection::ReadWrite).is_ok());
        registry_ = std::make_unique<ThunkRegistry>(0x70000000);
        gdi_ = std::make_unique<GdiManager>(handles_);
        mgr_ = std::make_unique<Opengl32Manager>();
        mgr_->set_pixel_format_api(Opengl32Manager::PixelFormatApi{
            [this](Marshaler& m) { pixel_format_choose(*gdi_, m); },
            [this](Marshaler& m) { pixel_format_describe(*gdi_, m); },
            [this](Marshaler& m) { pixel_format_get(*gdi_, m); },
            [this](Marshaler& m) { pixel_format_set(*gdi_, m); }});
        ASSERT_TRUE(registry_->register_dll("opengl32.dll", create_opengl32_thunks(*mgr_)).is_ok());
        ASSERT_TRUE(registry_->register_dll("gdi32.dll", make_gdi32_exports(*gdi_)).is_ok());
        c_ = std::make_unique<Caller>(memory_, *registry_, GetParam(), 0);
    }

    /// The wgl call and the gdi call on the same arguments.
    struct Pair { u64 w_ret, g_ret; u32 w_gle, g_gle; };
    Pair both(const char* wgl, const char* gdi, std::vector<u64> w_args, std::vector<u64> g_args) {
        Pair p{};
        p.w_ret = c_->call_in("opengl32.dll", wgl, std::move(w_args));
        p.w_gle = get_last_error();
        p.g_ret = c_->call_in("gdi32.dll", gdi, std::move(g_args));
        p.g_gle = get_last_error();
        return p;
    }

    static constexpr GuestAddr kReq = 0x10100, kOut = 0x10200;

    void write_request(u32 flags, u8 color, u8 depth) {
        std::array<u8, 40> d{};
        d[0] = 40; d[2] = 1;
        for (int i = 0; i < 4; ++i) d[4 + i] = static_cast<u8>(flags >> (8 * i));
        d[8] = PFD_TYPE_RGBA; d[9] = color; d[23] = depth;
        (void)memory_.write_bytes(kReq, d.data(), d.size());
    }

    GuestMemory memory_;
    HandleTable handles_;
    std::unique_ptr<ThunkRegistry> registry_;
    std::unique_ptr<GdiManager> gdi_;
    std::unique_ptr<Opengl32Manager> mgr_;
    std::unique_ptr<Caller> c_;
};

TEST_P(GlWglPixelFormatTest, OpenglAndGdiPixelFormatCallsAreOneCodePath) {
    const u64 count = gl_pixel_formats().size();
    // Memory DCs: each is its own format owner (screen DCs would share the desktop's).
    const Handle screen = gdi_->get_dc(0);
    const Handle w = gdi_->create_compatible_dc(screen), g = gdi_->create_compatible_dc(screen),
                 x = gdi_->create_compatible_dc(screen), y = gdi_->create_compatible_dc(screen);
    ASSERT_NE(w, g);
    write_request(PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, 32, 24);
    const i32 want = choose_gl_pixel_format({PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32, 0, 0, 24, 0});

    // wglpf-getpf-unset / wglpf-getpf-null-hdc, in a process where no
    // SetPixelFormat has succeeded yet: getpf-unset ret=0 gle=untouched.
    auto p = both("wglGetPixelFormat", "GetPixelFormat", {w}, {g});
    EXPECT_EQ(p.w_ret, 0u); EXPECT_EQ(p.g_ret, 0u);
    EXPECT_EQ(p.w_gle, kStale); EXPECT_EQ(p.g_gle, kStale);

    // wglpf-choose-base(-gdi), -null-hdc ret=0 gle=6.
    p = both("wglChoosePixelFormat", "ChoosePixelFormat", {w, kReq}, {g, kReq});
    EXPECT_EQ(p.w_ret, static_cast<u64>(want)); EXPECT_EQ(p.g_ret, p.w_ret);
    EXPECT_EQ(p.w_gle, kStale); EXPECT_EQ(p.g_gle, kStale);
    p = both("wglChoosePixelFormat", "ChoosePixelFormat", {0, kReq}, {0, kReq});
    EXPECT_EQ(p.w_ret, 0u); EXPECT_EQ(p.g_ret, 0u);
    EXPECT_EQ(p.w_gle, 6u); EXPECT_EQ(p.g_gle, 6u);

    // wglpf-describe-*: nBytes 0 / NULL ppfd count, bad index / short nBytes 87,
    // NULL hdc 6 (even for the count), >= 40 writes exactly the descriptor.
    struct D { const char* tag; u64 idx, nbytes, out; u64 ret; u32 gle; };
    const std::vector<D> describes = {
        {"idx3-nbytes0-buf", 3, 0, kOut, count, kStale},
        {"idx3-nbytes0-null", 3, 0, 0, count, kStale},
        {"idx0-count", 0, 0, 0, count, kStale},
        {"idx0-40", 0, 40, kOut, 0, 87},
        {"idx-count+1-40", count + 1, 40, kOut, 0, 87},
        {"idx-neg-40", 0xFFFFFFFFu, 40, kOut, 0, 87},
        {"idx3-nbytes39", 3, 39, kOut, 0, 87},
        {"idx3-nbytes1", 3, 1, kOut, 0, 87},
        {"idx3-null-buf-40", 3, 40, 0, count, kStale},
    };
    for (const D& d : describes) {
        SCOPED_TRACE(std::string("wglpf-describe-") + d.tag);
        p = both("wglDescribePixelFormat", "DescribePixelFormat", {w, d.idx, d.nbytes, d.out}, {g, d.idx, d.nbytes, d.out});
        EXPECT_EQ(p.w_ret, d.ret); EXPECT_EQ(p.g_ret, d.ret);
        EXPECT_EQ(p.w_gle, d.gle); EXPECT_EQ(p.g_gle, d.gle);
    }
    p = both("wglDescribePixelFormat", "DescribePixelFormat", {0, 3, 40, kOut}, {0, 3, 40, kOut});
    EXPECT_EQ(p.w_ret, 0u); EXPECT_EQ(p.g_ret, 0u);                  // wglpf-describe-null-hdc
    EXPECT_EQ(p.w_gle, 6u); EXPECT_EQ(p.g_gle, 6u);
    p = both("wglDescribePixelFormat", "DescribePixelFormat", {0, 0, 0, 0}, {0, 0, 0, 0});
    EXPECT_EQ(p.w_gle, 6u); EXPECT_EQ(p.g_gle, 6u);                  // wglpf-describe-null-hdc-count
    // idx3-40 and idx3-nbytes100 write the same 40 bytes through both.
    std::array<u8, 40> expected = gl_pixel_format_descriptor(*gl_pixel_format(3));
    for (const u64 nbytes : {40u, 100u}) {
        for (const char* dll : {"opengl32.dll", "gdi32.dll"}) {
            std::array<u8, 44> fill{};
            fill.fill(0xAB);
            (void)memory_.write_bytes(kOut, fill.data(), fill.size());
            const char* name = std::string(dll) == "gdi32.dll" ? "DescribePixelFormat" : "wglDescribePixelFormat";
            EXPECT_EQ(c_->call_in(dll, name, {w, 3, nbytes, kOut}), count);
            EXPECT_EQ(get_last_error(), kStale);
            std::array<u8, 44> got{};
            (void)memory_.read_bytes(kOut, got.data(), got.size());
            EXPECT_TRUE(std::equal(expected.begin(), expected.end(), got.begin())) << dll << " " << nbytes;
            EXPECT_EQ(got[40], 0xAB);   // nothing past the descriptor
        }
    }

    // wglpf-setpf-*: index 0 / count+1 / -1 -> 87, NULL hdc 6, NULL ppfd TRUE.
    for (const u64 idx : {u64{0}, count + 1, u64{0xFFFFFFFFu}}) {
        p = both("wglSetPixelFormat", "SetPixelFormat", {w, idx, kReq}, {g, idx, kReq});
        EXPECT_EQ(p.w_ret, 0u); EXPECT_EQ(p.g_ret, 0u);
        EXPECT_EQ(p.w_gle, 87u); EXPECT_EQ(p.g_gle, 87u);
    }
    p = both("wglSetPixelFormat", "SetPixelFormat", {0, 3, kReq}, {0, 3, kReq});
    EXPECT_EQ(p.w_ret, 0u); EXPECT_EQ(p.g_ret, 0u);
    EXPECT_EQ(p.w_gle, 6u); EXPECT_EQ(p.g_gle, 6u);
    p = both("wglSetPixelFormat", "SetPixelFormat", {w, 3, 0}, {g, 3, 0});   // setpf-null-ppfd
    EXPECT_EQ(p.w_ret, 1u); EXPECT_EQ(p.g_ret, 1u);
    EXPECT_EQ(p.w_gle, kStale); EXPECT_EQ(p.g_gle, kStale);
    p = both("wglGetPixelFormat", "GetPixelFormat", {w}, {g});         // getpf-after-null-ppfd ret=3
    EXPECT_EQ(p.w_ret, 3u); EXPECT_EQ(p.g_ret, 3u);
    EXPECT_EQ(p.w_gle, kStale); EXPECT_EQ(p.g_gle, kStale);
    p = both("wglSetPixelFormat", "SetPixelFormat", {w, 1, kReq}, {g, 1, kReq});   // second-different
    EXPECT_EQ(p.w_ret, 0u); EXPECT_EQ(p.g_ret, 0u);
    EXPECT_EQ(p.w_gle, 2000u); EXPECT_EQ(p.g_gle, 2000u);
    p = both("wglGetPixelFormat", "GetPixelFormat", {w}, {g});         // after-second-different ret=3
    EXPECT_EQ(p.w_ret, 3u); EXPECT_EQ(p.g_ret, 3u);
    p = both("wglSetPixelFormat", "SetPixelFormat", {w, 3, kReq}, {g, 3, kReq});   // second-same TRUE
    EXPECT_EQ(p.w_ret, 1u); EXPECT_EQ(p.g_ret, 1u);
    EXPECT_EQ(p.w_gle, kStale); EXPECT_EQ(p.g_gle, kStale);

    // After a successful set anywhere: a DC without a format (or NULL) is
    // 0 with 2000 through both (wglpf-getpf-unset, wglpf-getpf-null-hdc).
    p = both("wglGetPixelFormat", "GetPixelFormat", {x}, {y});
    EXPECT_EQ(p.w_ret, 0u); EXPECT_EQ(p.g_ret, 0u);
    EXPECT_EQ(p.w_gle, 2000u); EXPECT_EQ(p.g_gle, 2000u);
    p = both("wglGetPixelFormat", "GetPixelFormat", {0}, {0});
    EXPECT_EQ(p.w_gle, 2000u); EXPECT_EQ(p.g_gle, 2000u);

    // wglpf-cross-*: a format set through wgl is seen by gdi and the reverse;
    // a different set afterwards fails (2000), the same one succeeds.
    EXPECT_EQ(c_->call_in("opengl32.dll", "wglSetPixelFormat", {x, 5, kReq}), 1u);
    EXPECT_EQ(c_->call_in("gdi32.dll", "GetPixelFormat", {x}), 5u);
    EXPECT_EQ(c_->call_in("gdi32.dll", "SetPixelFormat", {y, 6, kReq}), 1u);
    EXPECT_EQ(c_->call_in("opengl32.dll", "wglGetPixelFormat", {y}), 6u);
    EXPECT_EQ(c_->call_in("opengl32.dll", "wglSetPixelFormat", {y, 7, kReq}), 0u);
    EXPECT_EQ(get_last_error(), 2000u);
    EXPECT_EQ(c_->call_in("opengl32.dll", "wglSetPixelFormat", {y, 6, kReq}), 1u);
}

// gdi32!SwapBuffers is opengl32's swap (Learn SwapBuffers; the probe's
// swap-buffers-* lines call gdi32): with opengl32 wired, gdi32 hands the DC to
// it; the result and last error are the measured ones either way.
// A success's stray gle 6 (swap-buffers ret=1 gle=6) is not reproduced: the
// gfx_media_exports_probe run of the same call leaves it untouched.
TEST_P(GlWglPixelFormatTest, GdiSwapBuffersDelegatesToOpengl32) {
    // Window DCs (each its own window) and a memory DC (no window).
    const Handle dbl = gdi_->get_dc(0x3100), sgl = gdi_->get_dc(0x3102), none = gdi_->get_dc(0x3104);
    const Handle mem = gdi_->create_compatible_dc(gdi_->get_dc(0));
    ASSERT_EQ(c_->call_in("gdi32.dll", "SetPixelFormat", {dbl, 5, kReq}), 1u);   // double-buffered
    ASSERT_EQ(c_->call_in("gdi32.dll", "SetPixelFormat", {sgl, 1, kReq}), 1u);   // single-buffered
    ASSERT_EQ(c_->call_in("gdi32.dll", "SetPixelFormat", {mem, 1, kReq}), 1u);
    mgr_->set_pixel_format_query([this](u64 dc) { return gdi_->pixel_format(static_cast<Handle>(dc)); });
    mgr_->set_drawable_queries(
        [this](u64 dc) -> u64 { return gdi_->window_from_dc(static_cast<Handle>(dc)); },
        [](u64, i32& w, i32& h) { w = 16; h = 8; return true; });
    std::vector<u64> handed;
    for (const bool wired : {false, true}) {
        SCOPED_TRACE(wired);
        if (wired) {
            gdi_->set_swap_buffers_handler([&](Handle dc) {
                handed.push_back(dc);
                return mgr_->swap_buffers(dc);
            });
        }
        EXPECT_EQ(c_->call_in("gdi32.dll", "SwapBuffers", {0}), 0u);      // swap-buffers-null-hdc
        EXPECT_EQ(get_last_error(), 6u);
        EXPECT_EQ(c_->call_in("gdi32.dll", "SwapBuffers", {none}), 0u);   // swap-buffers-no-format
        EXPECT_EQ(get_last_error(), 6u);
        EXPECT_EQ(c_->call_in("gdi32.dll", "SwapBuffers", {sgl}), 0u);    // swap-buffers-no-ctx-single
        EXPECT_EQ(get_last_error(), kStale);
        EXPECT_EQ(c_->call_in("gdi32.dll", "SwapBuffers", {dbl}), 1u);    // swap-buffers-no-ctx-double
        EXPECT_EQ(c_->call_in("gdi32.dll", "SwapBuffers", {mem}), 1u);    // swap-buffers-memdc
        EXPECT_EQ(get_last_error(), kStale);
    }
    EXPECT_EQ(handed, (std::vector<u64>{0, none, sgl, dbl, mem}));
}

// swap-buffers-*: wglSwapBuffers is SwapBuffers. NULL or a DC without a
// format: FALSE, 6; a single-buffered DC: FALSE, untouched; a double-buffered
// one: TRUE with or without a current context, also for another window.
TEST_P(GlWglTest, SwapBuffersMatchesOracle) {
    EXPECT_EQ(gl().call("wglSwapBuffers", {0}), 0u);     // swap-buffers-null-hdc
    EXPECT_EQ(gl().err(), 6u);
    EXPECT_EQ(gl().call("wglSwapBuffers", {dN}), 0u);    // swap-buffers-no-format
    EXPECT_EQ(gl().err(), 6u);
    EXPECT_EQ(gl().call("wglSwapBuffers", {dS}), 0u);    // swap-buffers-no-ctx-single
    EXPECT_EQ(gl().err(), kStale);
    EXPECT_EQ(gl().call("wglSwapBuffers", {dA}), 1u);    // swap-buffers-no-ctx-double
    // swap-buffers-memdc ret=1 gle=untouched: a DC without a window (dMem,
    // single-buffered format) succeeds.
    EXPECT_EQ(gl().call("wglSwapBuffers", {dMem}), 1u);
    EXPECT_EQ(gl().err(), kStale);
    const u64 a = gl().create(dA);
    ASSERT_EQ(gl().make(dA, a), 1u);
    EXPECT_EQ(gl().call("wglSwapBuffers", {dA}), 1u);    // swap-buffers
    EXPECT_EQ(gl().call("wglSwapBuffers", {dB}), 1u);    // swap-buffers-other-window
    EXPECT_EQ(gl().call("wglSwapBuffers", {dS}), 0u);    // swap-buffers-single-while-other-current
    EXPECT_EQ(gl().err(), kStale);
}

// swap-layer-*: TRUE for every fuPlanes on a double-buffered DC (with or
// without a context); FALSE with the last error untouched on a single-buffered
// DC; a NULL hdc or a DC without a format is FALSE with the error GetPixelFormat
// gives such a DC: 2000 once any SetPixelFormat has succeeded in the process
// (swap-layer-main-plane-no-format / -null-hdc gle=2000), untouched before
// (gfx_media_exports_probe wglSwapLayerBuffers(NULL,1) / (dc,no pf) err=0xdead).
TEST_P(GlWglTest, SwapLayerBuffersMatchesOracle) {
    bool ever_set = false;
    mgr_->set_pixel_format_ever_set_query([&] { return ever_set; });
    EXPECT_EQ(gl().call("wglSwapLayerBuffers", {0, 1}), 0u);
    EXPECT_EQ(gl().err(), kStale);
    EXPECT_EQ(gl().call("wglSwapLayerBuffers", {dN, 1}), 0u);
    EXPECT_EQ(gl().err(), kStale);
    ever_set = true;
    EXPECT_EQ(gl().call("wglSwapLayerBuffers", {0, 1}), 0u);    // swap-layer-main-plane-null-hdc
    EXPECT_EQ(gl().err(), 2000u);
    EXPECT_EQ(gl().call("wglSwapLayerBuffers", {dN, 1}), 0u);   // swap-layer-main-plane-no-format
    EXPECT_EQ(gl().err(), 2000u);
    EXPECT_EQ(gl().call("wglSwapLayerBuffers", {dS, 1}), 0u);   // swap-layer-main-plane-single
    EXPECT_EQ(gl().err(), kStale);
    EXPECT_EQ(gl().call("wglSwapLayerBuffers", {dA, 1}), 1u);   // swap-layer-main-plane-no-ctx
    const u64 a = gl().create(dA);
    ASSERT_EQ(gl().make(dA, a), 1u);
    // swap-layer-main-plane, -overlay1, -underlay1, -main+overlay1, -zero, -all-bits.
    for (const u64 planes : {u64{1}, u64{2}, u64{0x10000}, u64{3}, u64{0}, u64{0xFFFFFFFFu}}) {
        EXPECT_EQ(gl().call("wglSwapLayerBuffers", {dA, planes}), 1u) << planes;
    }
}

// swapmulti-*: the return is the mask of the entries that succeeded; each
// entry follows wglSwapLayerBuffers except that a DC without a format leaves
// the last error untouched and a NULL hdc sets 6; n = 0 is 0, n > 16
// (WGL_SWAPMULTIPLE_MAX) is 0 with ERROR_INVALID_FUNCTION and swaps nothing.
TEST_P(GlWglTest, SwapMultipleBuffersMatchesOracle) {
    const bool x64 = GetParam();
    const GuestAddr arr = 0x10800;
    const u32 stride = x64 ? 16u : 8u;            // WGLSWAP {HDC hdc; UINT uiFlags;}
    auto entry = [&](u32 i, u64 dc, u32 flags) {
        if (x64) (void)memory_.write_u64(arr + i * stride, dc);
        else (void)memory_.write_u32(arr + i * stride, static_cast<u32>(dc));
        (void)memory_.write_u32(arr + i * stride + (x64 ? 8u : 4u), flags);
    };
    auto run = [&](u64 n, GuestAddr p) { return gl().call("wglSwapMultipleBuffers", {n, p}); };
    EXPECT_EQ(run(0, 0), 0u);                                   // swapmulti-zero-null
    EXPECT_EQ(gl().err(), kStale);
    entry(0, dA, 1);
    EXPECT_EQ(run(0, arr), 0u);                                 // swapmulti-zero-nonnull
    EXPECT_EQ(gl().err(), kStale);
    EXPECT_EQ(run(1, arr), 1u);                                 // swapmulti-one-main-noctx
    const u64 a = gl().create(dA);
    ASSERT_EQ(gl().make(dA, a), 1u);
    EXPECT_EQ(run(1, arr), 1u);                                 // swapmulti-one-main
    entry(1, dB, 1);
    EXPECT_EQ(run(2, arr), 3u);                                 // swapmulti-two-main
    for (const u32 flags : {2u, 0u, 0xFFFFFFFFu}) {             // -one-overlay1, -one-flags0, -one-all-bits
        entry(0, dA, flags);
        EXPECT_EQ(run(1, arr), 1u) << flags;
    }
    entry(0, dS, 1);
    EXPECT_EQ(run(1, arr), 0u);                                 // swapmulti-single
    EXPECT_EQ(gl().err(), kStale);
    entry(0, dN, 1);
    EXPECT_EQ(run(1, arr), 0u);                                 // swapmulti-no-format
    EXPECT_EQ(gl().err(), kStale);
    entry(0, 0, 1);
    EXPECT_EQ(run(1, arr), 0u);                                 // swapmulti-null-hdc
    EXPECT_EQ(gl().err(), 6u);
    entry(0, dN, 1); entry(1, dA, 1);
    EXPECT_EQ(run(2, arr), 2u);                                 // swapmulti-bad-then-good
    entry(0, dA, 1); entry(1, dN, 1);
    EXPECT_EQ(run(2, arr), 1u);                                 // swapmulti-good-then-bad
    entry(0, dA, 1); entry(1, dS, 1);
    EXPECT_EQ(run(2, arr), 1u);                                 // swapmulti-double-then-single
    entry(0, 0, 1); entry(1, dA, 1);
    EXPECT_EQ(run(2, arr), 2u);                                 // swapmulti-null-then-good
    entry(0, dA, 1); entry(1, dA, 1);
    EXPECT_EQ(run(2, arr), 3u);                                 // swapmulti-same-dc-twice
    for (u32 i = 0; i < 20; ++i) entry(i, dA, 1);
    EXPECT_EQ(run(16, arr), 0xFFFFu);                           // swapmulti-max16
    EXPECT_EQ(run(17, arr), 0u);                                // swapmulti-over-max17
    EXPECT_EQ(gl().err(), 1u);
    EXPECT_EQ(run(20, arr), 0u);                                // swapmulti-over-max20
    EXPECT_EQ(gl().err(), 1u);
    // swapmulti-null-array faults on Windows (EXC=0xc0000005); Vitreo reads
    // nothing and returns 0 with the last error untouched.
    EXPECT_EQ(run(1, 0), 0u);
    EXPECT_EQ(gl().err(), kStale);
}

// Spec 4.2.3: glClear's mask may hold only the four buffer bits
// (INVALID_VALUE otherwise); glClear, glFlush and glFinish are not allowed
// between glBegin and glEnd (spec 2.6.3: INVALID_OPERATION). With no backend
// device there is no drawable and a valid clear does nothing, without error.
TEST_P(GlWglTest, ClearFlushFinishErrors) {
    const u64 a = gl().create(dA);
    ASSERT_EQ(gl().make(dA, a), 1u);
    GlContext* ctx = mgr_->context_for_testing(a);
    ASSERT_NE(ctx, nullptr);
    for (const u64 bad : {u64{0x1}, u64{0x00004001}, u64{0x80000000u}}) {
        gl().call("glClear", {bad});
        EXPECT_EQ(gl().call("glGetError"), GL_INVALID_VALUE) << bad;
    }
    gl().call("glClear", {GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT |
                          GL_ACCUM_BUFFER_BIT});
    EXPECT_EQ(gl().call("glGetError"), GL_NO_ERROR);
    gl().call("glFlush");
    gl().call("glFinish");
    EXPECT_EQ(gl().call("glGetError"), GL_NO_ERROR);
    for (const char* name : {"glClear", "glFlush", "glFinish"}) {
        ctx->begin_mode = GL_TRIANGLES;
        gl().call(name, std::string(name) == "glClear" ? std::vector<u64>{GL_COLOR_BUFFER_BIT}
                                                      : std::vector<u64>{});
        ctx->begin_mode = kGlOutsideBeginEnd;
        EXPECT_EQ(gl().call("glGetError"), GL_INVALID_OPERATION) << name;
    }
}

INSTANTIATE_TEST_SUITE_P(Bitness, GlWglPixelFormatTest, ::testing::Values(false, true), bitness_name);

INSTANTIATE_TEST_SUITE_P(Bitness, GlWglTest, ::testing::Values(false, true), bitness_name);

}  // namespace
