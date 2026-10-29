/// @file test_opengl32.cpp
/// @brief WGL behaviour before contexts exist (Task 5), and the no-context GL table.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/graphics/opengl32/opengl32.hpp"
#include "winapi/kernel32/error.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

class Opengl32Test : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(MEM_BASE, 0x10000,
                                       MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(STACK_BASE, 0x1000,
                                       MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(OPENGL32_SCRATCH_BASE,
                                       OPENGL32_SCRATCH_SIZE,
                                       MemoryProtection::ReadWrite).is_ok());
        cpu_.init_protected_mode();
        registry_ = std::make_unique<ThunkRegistry>(0x70000000);
        mgr_.set_thunk_registry(registry_.get());
        auto exports = create_opengl32_thunks(mgr_);
        ASSERT_TRUE(registry_->register_dll("opengl32.dll",
                                            std::move(exports)).is_ok());
    }

    void set_args(const std::vector<u32>& args) {
        const GuestAddr frame = STACK_BASE + 0x400;
        cpu_.set_sp(frame);
        (void)memory_.write_u32(frame, 0xDEADBEEF);
        for (usize i = 0; i < args.size(); ++i) {
            (void)memory_.write_u32(
                frame + 4 + static_cast<GuestAddr>(i * 4), args[i]);
        }
    }

    [[nodiscard]] Marshaler make_marshaler() {
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        return m;
    }

    [[nodiscard]] u32 return_value() const {
        return static_cast<u32>(cpu_.gpr.rax & 0xFFFFFFFFu);
    }

    void write_ansi(GuestAddr addr, const char* text) {
        (void)memory_.write_bytes(addr, reinterpret_cast<const Byte*>(text),
                                  std::strlen(text) + 1);
    }

    static constexpr GuestAddr MEM_BASE = 0x10000;
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr NAME_ADDR = MEM_BASE + 0x200;
    static constexpr GuestAddr OUT_ADDR = MEM_BASE + 0x100;

    GuestMemory memory_;
    cpu::CpuState cpu_;
    Opengl32Manager mgr_;
    std::unique_ptr<ThunkRegistry> registry_;
};

// With no context current wglGetProcAddress answers NULL for core and
// extension names alike and sets ERROR_INVALID_HANDLE
// (wgl_contract_probe64.expected: gpa-noctx ret=NULL gle=6). The context
// cases are in test_gl_wgl.cpp.
TEST_F(Opengl32Test, GetProcAddressIsNullWithoutACurrentContext) {
    for (const char* name : {"glViewport", "glDefinitelyNotAFunction", "wglChoosePixelFormatARB"}) {
        write_ansi(NAME_ADDR, name);
        set_args({static_cast<u32>(NAME_ADDR)});
        set_last_error(0xDEAD);
        auto m = make_marshaler();
        mgr_.impl_wglGetProcAddress(m);
        EXPECT_EQ(return_value(), 0u) << name;
        EXPECT_EQ(get_last_error(), 6u) << name;
    }
}

TEST_F(Opengl32Test, WglWithoutAContextFailsWhereWindowsWouldAndGlIsTheNoContextTable) {
    mgr_.set_pixel_format_query([](u64 hdc) { return hdc == 0x5000 ? 1 : 0; });
    auto run = [&](const char* name, std::vector<u32> args) {
        auto addr = registry_->get_thunk_address("opengl32.dll", name);
        EXPECT_TRUE(addr.is_ok()) << name;
        if (!addr.is_ok()) return ~0u;
        auto handler = registry_->resolve_handler(addr.value());
        if (!handler.is_ok()) return ~0u;
        set_last_error(0xDEAD);
        set_args(args);
        auto m = make_marshaler();
        (*handler.value())(m);
        return return_value();
    };
    // "wglCreateContext(NULL)=NULL err=6", "(memdc no pf)=NULL err=2000".
    EXPECT_EQ(run("wglCreateContext", {0}), 0u);
    EXPECT_EQ(get_last_error(), 6u);
    EXPECT_EQ(run("wglCreateContext", {0x4000}), 0u);
    EXPECT_EQ(get_last_error(), 2000u);
    EXPECT_NE(run("wglCreateContext", {0x5000}), 0u);  // a DC with a format creates (test_gl_wgl.cpp)
    EXPECT_EQ(run("wglGetCurrentContext", {}), 0u);
    EXPECT_EQ(run("wglGetCurrentDC", {}), 0u);
    EXPECT_EQ(run("wglMakeCurrent", {0, 0}), 0u);   // make-null-null-nothing-current ret=0 gle=6
    EXPECT_EQ(get_last_error(), 6u);
    // The no-context table: glGetError is GL_INVALID_OPERATION, glGetString
    // NULL, glGetIntegerv leaves its output.
    EXPECT_EQ(run("glGetError", {}), 0x0502u);
    EXPECT_EQ(run("glGetString", {0x1F00}), 0u);
    (void)memory_.write_u32(OUT_ADDR, 0xFFFFFFF9u);
    EXPECT_EQ(run("glGetIntegerv", {0x0D33, static_cast<u32>(OUT_ADDR)}), 0u);
    EXPECT_EQ(memory_.read_u32(OUT_ADDR).value(), 0xFFFFFFF9u);
    for (const char* name : {"glPushAttrib", "glPopAttrib", "glVertex3f", "glScalef"}) {
        EXPECT_TRUE(registry_->get_thunk_address("opengl32.dll", name).is_ok()) << name;
    }
}

// wglSwapBuffers / wglSwapLayerBuffers, measured on Windows 11
// (tools/oracle_probe/gfx_media_exports_probe.expected).
TEST_F(Opengl32Test, SwapBuffersNeedsAPixelFormat) {
    // 0x5000 has a double-buffered format (5): a single-buffered DC does not swap
    // (wgl_contract_probe64.expected swap-buffers-no-ctx-single).
    mgr_.set_pixel_format_query([](u64 hdc) { return hdc == 0x5000 ? 5 : 0; });
    auto run = [&](const char* name, std::vector<u32> args) {
        auto addr = registry_->get_thunk_address("opengl32.dll", name);
        EXPECT_TRUE(addr.is_ok()) << name;
        if (!addr.is_ok()) return ~0u;
        auto handler = registry_->resolve_handler(addr.value());
        EXPECT_TRUE(handler.is_ok());
        if (!handler.is_ok()) return ~0u;
        EXPECT_EQ(registry_->resolve_stdcall_bytes(addr.value()), args.size() * 4) << name;
        set_last_error(0xDEAD);
        set_args(args);
        auto m = make_marshaler();
        (*handler.value())(m);
        return return_value();
    };
    EXPECT_EQ(run("wglSwapBuffers", {0}), 0u);
    EXPECT_EQ(get_last_error(), 6u);           // ERROR_INVALID_HANDLE
    EXPECT_EQ(run("wglSwapLayerBuffers", {0, 1}), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);      // untouched
    EXPECT_EQ(run("wglSwapBuffers", {0x4000}), 0u);  // no pixel format
    EXPECT_EQ(get_last_error(), 6u);
    EXPECT_EQ(run("wglSwapLayerBuffers", {0x4000, 1}), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(run("wglSwapBuffers", {0x5000}), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    for (u32 planes : {1u, 2u, 0u}) {
        EXPECT_EQ(run("wglSwapLayerBuffers", {0x5000, planes}), 1u);
    }
}

// The GL 1.1 state assertions that lived here moved to test_gl_context_state.cpp
// (MigratedRecorderAssertionsStillHold), which exercises the real context.
