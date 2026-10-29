#include <gtest/gtest.h>

#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/dwmapi/dwmapi.hpp"
#include "winapi/kernel32/error.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

class DwmApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(0x10000, 0x1000,
                                       MemoryProtection::ReadWrite).is_ok());
        cpu_.mode = cpu::CpuMode::Long;
        marshaler_.attach_cpu(cpu_);
    }

    GuestMemory memory_{};
    cpu::CpuState cpu_{};
    Marshaler marshaler_{memory_};
};

} // namespace

const ExportEntry* find_export(const DllExports& exports, const char* name) {
    for (const auto& entry : exports.entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

TEST_F(DwmApiTest, ExtendFrameSheetOfGlassMarksClientDrawn) {
    bool client_drawn = false;
    u32 seen_hwnd = 0;
    auto exports = create_dwmapi_thunks([&](u32 hwnd, bool drawn) {
        seen_hwnd = hwnd;
        client_drawn = drawn;
    });
    const auto* entry = find_export(exports, "DwmExtendFrameIntoClientArea");
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(static_cast<bool>(entry->handler));

    // MARGINS{-1,-1,-1,-1}: Learn "sheet of glass" — no window border.
    const GuestAddr margins = 0x10000;
    for (u32 i = 0; i < 4; ++i) {
        ASSERT_TRUE(memory_.write_u32(margins + i * 4, static_cast<u32>(-1))
                        .is_ok());
    }
    cpu_.gpr.rcx = 0x10450;
    cpu_.gpr.rdx = margins;
    entry->handler(marshaler_);
    EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax), 0u);
    EXPECT_EQ(seen_hwnd, 0x10450u);
    EXPECT_TRUE(client_drawn);
}

TEST_F(DwmApiTest, ExtendFrameZeroMarginsClearsClientDrawn) {
    bool client_drawn = true;
    auto exports = create_dwmapi_thunks([&](u32, bool drawn) {
        client_drawn = drawn;
    });
    const auto* entry = find_export(exports, "DwmExtendFrameIntoClientArea");
    ASSERT_NE(entry, nullptr);

    const GuestAddr margins = 0x10000;
    for (u32 i = 0; i < 4; ++i) {
        ASSERT_TRUE(memory_.write_u32(margins + i * 4, 0).is_ok());
    }
    cpu_.gpr.rcx = 0x10451;
    cpu_.gpr.rdx = margins;
    entry->handler(marshaler_);
    EXPECT_FALSE(client_drawn);
}

TEST_F(DwmApiTest, CompositionTimingUsesDisplayRefreshRate) {
    for (const double hz : {60.0, 120.0, 240.0, 59.94}) {
        auto exports = create_dwmapi_thunks({}, hz);
        const auto* entry = find_export(exports, "DwmGetCompositionTimingInfo");
        ASSERT_NE(entry, nullptr);
        ASSERT_TRUE(memory_.write_u32(0x10000, 292));
        cpu_.gpr.rcx = 0;
        cpu_.gpr.rdx = 0x10000;
        entry->handler(marshaler_);
        EXPECT_EQ(cpu_.gpr.rax, 0u);
        const u32 numerator = memory_.read_u32(0x10004).value_or(0);
        const u32 denominator = memory_.read_u32(0x10008).value_or(0);
        ASSERT_NE(denominator, 0u);
        EXPECT_NEAR(static_cast<double>(numerator) / denominator, hz, 0.001);
        EXPECT_EQ(memory_.read_u64(0x1000C).value_or(0),
                  10000000ull * denominator / numerator);
        EXPECT_EQ(memory_.read_u32(0x10014).value_or(0), numerator);
        EXPECT_EQ(memory_.read_u32(0x10018).value_or(0), denominator);
    }
}

// DwmEnableBlurBehindWindow, measured on Windows 11 (interactive session,
// tools/oracle_probe/gfx_media_exports_probe.expected).
TEST_F(DwmApiTest, EnableBlurBehindWindowValidatesLikeWindows) {
    DwmWindowServices services{
        [](u32 hwnd) {
            return hwnd == 0x100 ? DwmWindowKind::TopLevel
                 : hwnd == 0x200 ? DwmWindowKind::Child
                                 : DwmWindowKind::Invalid;
        },
        [](u64 hrgn) { return hrgn == 0x3000; }};
    auto exports = create_dwmapi_thunks({}, 60.0, services);
    const auto* entry = find_export(exports, "DwmEnableBlurBehindWindow");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 8u);

    const GuestAddr bb = 0x10000;
    auto call = [&](u64 hwnd, GuestAddr p, u32 flags, u32 enable, u64 rgn) {
        (void)memory_.write_u32(bb, flags);
        (void)memory_.write_u32(bb + 4, enable);
        (void)memory_.write_u64(bb + 8, rgn);      // x64: HRGN at +8
        (void)memory_.write_u32(bb + 16, 0);
        cpu_.gpr.rcx = hwnd;
        cpu_.gpr.rdx = p;
        entry->handler(marshaler_);
        return static_cast<u32>(cpu_.gpr.rax);
    };
    constexpr u32 kInvalid = 0x80070057u;
    EXPECT_EQ(call(0, 0, 1, 1, 0), kInvalid);            // Blur(NULL,NULL)
    EXPECT_EQ(call(0x100, 0, 1, 1, 0), kInvalid);        // Blur(wnd,NULL)
    EXPECT_EQ(call(0, bb, 1, 1, 0), kInvalid);           // Blur(NULL,&bb)
    EXPECT_EQ(call(0x12345678, bb, 1, 1, 0), kInvalid);  // bogus HWND
    EXPECT_EQ(call(0x200, bb, 1, 1, 0), kInvalid);       // child window
    EXPECT_EQ(call(0x100, bb, 1, 1, 0), 0u);             // enable
    EXPECT_EQ(call(0x100, bb, 1, 0, 0), 0u);             // disable
    EXPECT_EQ(call(0x100, bb, 0, 1, 0), kInvalid);       // dwFlags 0
    EXPECT_EQ(call(0x100, bb, 0xFFFFFFF0u, 1, 0), kInvalid);
    EXPECT_EQ(call(0x100, bb, 2, 1, 0x3000), 0u);        // real region
    EXPECT_EQ(call(0x100, bb, 2, 1, 0x4321), 0x8007000Bu);  // ERROR_BAD_FORMAT
    EXPECT_EQ(call(0x100, bb, 4, 1, 0), 0u);             // transition flag
}

// Measured on Windows 11 (tools/oracle_probe/s1win_dwm_probe.expected
// "-- dwm", x64 = x86).
TEST_F(DwmApiTest, WindowAttributesFollowMeasuredRules) {
    constexpr u32 kEHandle = 0x80070006u, kEInvalidArg = 0x80070057u;
    DwmWindowServices services;
    services.classify_window = [](u32 hwnd) {
        return hwnd == 0x100 ? DwmWindowKind::TopLevel
             : hwnd == 0x200 ? DwmWindowKind::Child : DwmWindowKind::Invalid;
    };
    services.frame_bounds = [](u32, i32* r) {
        r[0] = 57; r[1] = 60; r[2] = 343; r[3] = 253;
        return true;
    };
    services.has_frame = [](u32) { return true; };
    auto exports = create_dwmapi_thunks({}, 60.0, services);
    const auto* set = find_export(exports, "DwmSetWindowAttribute");
    const auto* get = find_export(exports, "DwmGetWindowAttribute");
    ASSERT_NE(set, nullptr);
    ASSERT_NE(get, nullptr);
    auto invoke = [&](const ExportEntry* e, u64 hwnd, u64 attr, u64 ptr, u64 size) {
        set_last_error(0xDEAD);
        cpu_.gpr.rcx = hwnd;
        cpu_.gpr.rdx = attr;
        cpu_.gpr.r8 = ptr;
        cpu_.gpr.r9 = size;
        e->handler(marshaler_);
        return static_cast<u32>(cpu_.gpr.rax);
    };
    const GuestAddr out = 0x10100;
    // DWMWA_EXTENDED_FRAME_BOUNDS is the visible frame.
    EXPECT_EQ(invoke(get, 0x100, 9, out, 16), 0u);
    EXPECT_EQ(memory_.read_u32(out).value(), 57u);
    EXPECT_EQ(memory_.read_u32(out + 12).value(), 253u);
    EXPECT_EQ(invoke(get, 0x100, 9, out, 32), 0u);           // larger buffer ok
    EXPECT_EQ(invoke(get, 0x100, 9, out, 8), 0x8007007Au);    // short RECT
    EXPECT_EQ(invoke(get, 0x100, 14, out, 2), kEInvalidArg);  // short DWORD
    for (u64 hwnd : {0x0ull, 0x200ull, 0x7FFF0ull}) {
        EXPECT_EQ(invoke(get, hwnd, 14, out, 4), kEHandle) << hwnd;
        EXPECT_EQ(get_last_error(), 6u);
        EXPECT_EQ(invoke(set, hwnd, 3, out, 4), kEHandle) << hwnd;
        EXPECT_EQ(get_last_error(), 6u);
    }
    EXPECT_EQ(invoke(get, 0x100, 0x7777, out, 4), kEInvalidArg);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(invoke(get, 0x100, 2, out, 4), kEInvalidArg);   // set-only
    EXPECT_EQ(get_last_error(), 87u);
    EXPECT_EQ(invoke(get, 0x100, 14, 0, 4), kEInvalidArg);
    EXPECT_EQ(invoke(set, 0x100, 9, out, 16), kEInvalidArg);  // get-only
    EXPECT_EQ(get_last_error(), 87u);
    EXPECT_EQ(invoke(set, 0x100, 0, out, 4), kEInvalidArg);
    EXPECT_EQ(invoke(set, 0x100, 0x7777, out, 4), kEInvalidArg);
    EXPECT_EQ(invoke(set, 0x100, 3, 0, 4), kEInvalidArg);
    ASSERT_TRUE(memory_.write_u32(out, 1).is_ok());
    EXPECT_EQ(invoke(set, 0x100, 3, out, 4), 0u);             // TRANSITIONS_FORCEDISABLED
    EXPECT_EQ(invoke(set, 0x100, 3, out, 2), kEInvalidArg);
    EXPECT_EQ(invoke(set, 0x100, 3, out, 8), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    // NCRENDERING_ENABLED follows DWMWA_NCRENDERING_POLICY.
    EXPECT_EQ(invoke(get, 0x100, 1, out, 4), 0u);
    EXPECT_EQ(memory_.read_u32(out).value(), 1u);
    ASSERT_TRUE(memory_.write_u32(out, 1).is_ok());           // DWMNCRP_DISABLED
    EXPECT_EQ(invoke(set, 0x100, 2, out, 4), 0u);
    EXPECT_EQ(invoke(get, 0x100, 1, out, 4), 0u);
    EXPECT_EQ(memory_.read_u32(out).value(), 0u);
    EXPECT_EQ(invoke(get, 0x100, 14, out, 4), 0u);            // CLOAKED
    EXPECT_EQ(memory_.read_u32(out).value(), 0u);
    EXPECT_EQ(invoke(get, 0x100, 37, out, 4), 0u);            // visible border
    EXPECT_EQ(memory_.read_u32(out).value(), 1u);
    // Frame appearance of the host-drawn frame and DWMWA_CLOAK cannot be
    // honoured: answered like a build without the attribute.
    ASSERT_TRUE(memory_.write_u32(out, 1).is_ok());
    EXPECT_EQ(invoke(set, 0x100, 20, out, 4), kEInvalidArg);
    EXPECT_EQ(invoke(set, 0x100, 13, out, 4), kEInvalidArg);
    ASSERT_TRUE(memory_.write_u32(out, 0).is_ok());
    EXPECT_EQ(invoke(set, 0x100, 13, out, 4), 0u);            // uncloak
}

TEST_F(DwmApiTest, ExtendFrameTimingAndDefWindowProcMeasured) {
    DwmWindowServices services;
    services.classify_window = [](u32 hwnd) {
        return hwnd == 0x100 ? DwmWindowKind::TopLevel
             : hwnd == 0x200 ? DwmWindowKind::Child : DwmWindowKind::Invalid;
    };
    auto exports = create_dwmapi_thunks({}, 60.0, services);
    const auto* extend = find_export(exports, "DwmExtendFrameIntoClientArea");
    const auto* timing = find_export(exports, "DwmGetCompositionTimingInfo");
    const auto* dwp = find_export(exports, "DwmDefWindowProc");
    ASSERT_NE(extend, nullptr);
    ASSERT_NE(timing, nullptr);
    ASSERT_NE(dwp, nullptr);
    const GuestAddr margins = 0x10000;
    for (u32 i = 0; i < 4; ++i) ASSERT_TRUE(memory_.write_u32(margins + i * 4, 1).is_ok());
    auto call2 = [&](const ExportEntry* e, u64 a, u64 b) {
        set_last_error(0xDEAD);
        cpu_.gpr.rcx = a;
        cpu_.gpr.rdx = b;
        e->handler(marshaler_);
        return static_cast<u32>(cpu_.gpr.rax);
    };
    EXPECT_EQ(call2(extend, 0x100, margins), 0u);
    EXPECT_EQ(get_last_error(), 0u);
    EXPECT_EQ(call2(extend, 0x7FFF0, margins), 0x80070057u);
    EXPECT_EQ(get_last_error(), 1400u);
    EXPECT_EQ(call2(extend, 0, margins), 0x80070057u);
    EXPECT_EQ(get_last_error(), 1400u);
    EXPECT_EQ(call2(extend, 0x200, margins), 0x80070057u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    const GuestAddr info = 0x10100;
    ASSERT_TRUE(memory_.write_u32(info, 292).is_ok());
    EXPECT_EQ(call2(timing, 0x100, info), 0x80070057u);   // hwnd must be NULL
    EXPECT_EQ(call2(timing, 0, info), 0u);
    ASSERT_TRUE(memory_.write_u32(info, 8).is_ok());
    EXPECT_EQ(call2(timing, 0, info), 0x88980090u);
    // DwmDefWindowProc: not handled, *plResult untouched.
    const GuestAddr lr = 0x10400;
    ASSERT_TRUE(memory_.write_u64(lr, 5).is_ok());
    cpu_.gpr.rcx = 0x100;
    cpu_.gpr.rdx = 0x84;   // WM_NCHITTEST
    cpu_.gpr.r8 = 0;
    cpu_.gpr.r9 = 0;
    cpu_.gpr.rsp = 0x10800;
    ASSERT_TRUE(memory_.write_u64(0x10800 + 0x28, lr).is_ok());
    dwp->handler(marshaler_);
    EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax), 0u);
    EXPECT_EQ(memory_.read_u64(lr).value(), 5u);
}


TEST_F(DwmApiTest, ColorizationUnsupportedPreservesOutputsAndX86Abi) {
    const auto exports = create_dwmapi_thunks();
    const auto* entry = find_export(exports, "DwmGetColorizationColor");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 8u); // dwmapi.h: two pointers, WINAPI
    for (const bool mode64 : {false, true}) {
        if (mode64) cpu_.init_long_mode(); else cpu_.init_protected_mode();
        cpu_.gpr.rsp = 0x10800;
        cpu_.gpr.rcx = 0x10000;
        cpu_.gpr.rdx = 0x10004;
        ASSERT_TRUE(memory_.write_u32(0x10804, 0x10000));
        ASSERT_TRUE(memory_.write_u32(0x10808, 0x10004));
        ASSERT_TRUE(memory_.write_u32(0x10000, 0x11223344));
        ASSERT_TRUE(memory_.write_u32(0x10004, 0x55667788));
        set_last_error(123);
        entry->handler(marshaler_);
        EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax), 0x80004001u);
        EXPECT_EQ(memory_.read_u32(0x10000).value(), 0x11223344u);
        EXPECT_EQ(memory_.read_u32(0x10004).value(), 0x55667788u);
        EXPECT_EQ(get_last_error(), 123u);
    }
}
