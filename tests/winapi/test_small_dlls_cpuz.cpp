/// @file test_small_dlls_cpuz.cpp
/// @brief Unit tests for small DLL stubs (IMM32, MSIMG32, WINSPOOL, IPHLPAPI,
///        COMCTL32, SHLWAPI) exercised by CPU-Z (32-bit stdcall)

#include <gtest/gtest.h>
#include <algorithm>
#include <vitreo/common.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <vitreo/virtual_tsc.hpp>

#include <vector>
#include <array>
#include <net/if.h>

#include <winapi/ui/imm32/imm32.hpp>
#include <winapi/graphics/dwmapi/dwmapi.hpp>
#include <winapi/graphics/msimg32/msimg32.hpp>
#include <winapi/system/winspool/winspool.hpp>
#include <winapi/networking/iphlpapi/iphlpapi.hpp>
#include <winapi/networking/tapi32/tapi32.hpp>
#include <winapi/ui/comctl32/comctl32.hpp>
#include <winapi/system/shlwapi/shlwapi.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <winapi/kernel32/synchronization/sync.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <capsule/vfs/vfs.hpp>

using namespace vitreo;
using namespace vitreo::winapi;
namespace if_row2_contract {
#include "winapi/networking/iphlpapi/if_row2_contract.inc"
}

// =============================================================================
// Helper: find an ExportEntry by name
// =============================================================================

static ExportEntry* find_export(DllExports& exports, const char* name) {
    for (auto& e : exports.entries) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

// =============================================================================
// Test Fixture — 32-bit stdcall calling convention
// =============================================================================

class SmallDllsCpuzTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto res = memory.map_region(DATA_BASE, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(res.is_ok());
        auto stack_res = memory.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(stack_res.is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        marshaler.attach_cpu(cpu);
    }

    /// Push a 32-bit stdcall argument at the given index
    void push_arg(usize index, u32 value) {
        (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4 + index * 4), value);
    }

    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr DATA_BASE  = 0x1000;

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
};

// =============================================================================
// IMM32 — ImmGetOpenStatus (1 arg, returns 0)
// =============================================================================

TEST_F(SmallDllsCpuzTest, Imm32_ImmGetOpenStatus) {
    Imm32Manager imm_mgr;
    push_arg(0, 0x1234); // HIMC
    imm_mgr.impl_ImmGetOpenStatus(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

// =============================================================================
// IMM32 — ImmGetContext returns the thread default HIMC (not NULL)
// =============================================================================

TEST_F(SmallDllsCpuzTest, Imm32_ImmGetContext) {
    Imm32Manager imm_mgr;
    push_arg(0, 0x5678); // HWND
    imm_mgr.impl_ImmGetContext(marshaler);
    const u32 himc = static_cast<u32>(cpu.gpr.rax);
    EXPECT_NE(himc, 0u);

    // Release of the retrieved default HIMC succeeds.
    push_arg(0, 0x5678);
    push_arg(1, himc);
    imm_mgr.impl_ImmReleaseContext(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
}

TEST_F(SmallDllsCpuzTest, Imm32_ImmCreateAssociateAndDestroyContext) {
    Imm32Manager imm_mgr;
    auto exports = create_imm32_thunks(imm_mgr);
    auto* create = find_export(exports, "ImmCreateContext");
    auto* associate = find_export(exports, "ImmAssociateContext");
    auto* destroy = find_export(exports, "ImmDestroyContext");
    auto* get = find_export(exports, "ImmGetContext");
    ASSERT_NE(create, nullptr);
    ASSERT_NE(associate, nullptr);
    ASSERT_NE(destroy, nullptr);
    ASSERT_NE(get, nullptr);

    create->handler(marshaler);
    const u32 created = static_cast<u32>(cpu.gpr.rax);
    ASSERT_NE(created, 0u);

    push_arg(0, 0x10000); // HWND
    push_arg(1, created);
    associate->handler(marshaler);
    const u32 previous = static_cast<u32>(cpu.gpr.rax);
    EXPECT_NE(previous, 0u); // prior default HIMC

    push_arg(0, 0x10000);
    get->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), created);

    push_arg(0, created);
    destroy->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);

    push_arg(0, 0x10000);
    get->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), previous);
}

// =============================================================================
// IMM32 — ImmReleaseContext rejects unknown HIMC
// =============================================================================

TEST_F(SmallDllsCpuzTest, Imm32_ImmReleaseContext) {
    Imm32Manager imm_mgr;
    push_arg(0, 0x5678); // HWND
    push_arg(1, 0x1234); // unknown HIMC
    imm_mgr.impl_ImmReleaseContext(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

TEST_F(SmallDllsCpuzTest, Imm32_ImmGetIMEFileNameWReportsNoIme) {
    Imm32Manager imm_mgr;
    auto exports = create_imm32_thunks(imm_mgr);
    auto* get_filename = find_export(exports, "ImmGetIMEFileNameW");
    ASSERT_NE(get_filename, nullptr);

    constexpr GuestAddr buffer = DATA_BASE + 0x100;
    ASSERT_TRUE(memory.write_u16(buffer, 0xCCCC).is_ok());
    push_arg(0, 0x04090409); // en-US HKL
    push_arg(1, static_cast<u32>(buffer));
    push_arg(2, 32);
    get_filename->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(memory.read_u16(buffer).value(), 0u);
}

TEST_F(SmallDllsCpuzTest, Imm32_ImmAssociateContextExIsStdcall12AndSucceeds) {
    // Learn: ImmAssociateContextEx is stdcall with 3 args. A missing-export
    // stub with 0 cleanup bytes left IACE_DEFAULT (0x10) on the stack; qwindows
    // popped it into EBX and later crashed at fault_addr=0x14.
    Imm32Manager imm_mgr;
    auto exports = create_imm32_thunks(imm_mgr);
    auto* associate_ex = find_export(exports, "ImmAssociateContextEx");
    ASSERT_NE(associate_ex, nullptr);
    EXPECT_EQ(associate_ex->stdcall_bytes, 12u);

    constexpr u32 kIaceDefault = 0x10;
    push_arg(0, 0x1000F); // HWND
    push_arg(1, 0);       // HIMC
    push_arg(2, kIaceDefault);
    associate_ex->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);

    auto* default_ime = find_export(exports, "ImmGetDefaultIMEWnd");
    ASSERT_NE(default_ime, nullptr);
    EXPECT_EQ(default_ime->stdcall_bytes, 4u);
    push_arg(0, 0x1000E);
    default_ime->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

TEST_F(SmallDllsCpuzTest, Dwmapi_GetCompositionTimingInfoIsStdcall8) {
    // Learn: missing stub (stdcall_bytes=0) made CEF gpu-process abort with
    // TerminateProcess(0xc0000409) immediately after the call.
    auto exports = create_dwmapi_thunks();
    auto* timing = find_export(exports, "DwmGetCompositionTimingInfo");
    ASSERT_NE(timing, nullptr);
    EXPECT_EQ(timing->stdcall_bytes, 8u);

    // Learn DWM_TIMING_INFO: dwmapi.h packs its structures to 1 byte, so the
    // ULONGLONG members are unpadded and sizeof is 292. The QPC_TIME members
    // must also share the guest QueryPerformanceCounter time base, or a
    // compositor phase-locking to composition gets a nonsense frame clock.
    constexpr GuestAddr info = DATA_BASE + 0x200;
    constexpr u32 kSize = 292;
    std::vector<Byte> zeros(kSize, 0);
    ASSERT_TRUE(memory.write_bytes(info, zeros.data(), zeros.size()).is_ok());
    ASSERT_TRUE(memory.write_u32(info, kSize).is_ok());

    // Learn: since Windows 8.1 hwnd must be NULL; a window handle is
    // E_INVALIDARG (s1win_dwm_probe).
    push_arg(0, 0x10011);
    push_arg(1, static_cast<u32>(info));
    timing->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80070057u); // E_INVALIDARG

    push_arg(0, 0); // HWND: NULL, the system-wide timing
    push_arg(1, static_cast<u32>(info));
    timing->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u); // S_OK
    EXPECT_EQ(memory.read_u32(info).value(), kSize);
    // UNSIGNED_RATIO rates: the default 60 Hz monitor, in whatever terms the
    // ratio is stored (millihertz keeps fractional modes such as 59.94).
    auto rate_hz = [&](GuestAddr at) {
        const u32 den = memory.read_u32(at + 4).value();
        return den == 0 ? 0.0
                        : static_cast<double>(memory.read_u32(at).value()) / den;
    };
    EXPECT_DOUBLE_EQ(rate_hz(info + 4), 60.0);   // rateRefresh

    const u64 frequency = kQpcHz;
    const u64 refresh_period = memory.read_u64(info + 12).value();
    EXPECT_EQ(refresh_period, frequency / 60);
    EXPECT_DOUBLE_EQ(rate_hz(info + 20), 60.0);  // rateCompose

    // qpcVBlank shares the guest QueryPerformanceCounter time base and lands
    // on a refresh boundary at or before now.
    const u64 qpc_vblank = memory.read_u64(info + 28).value();
    const u64 now = virtual_qpc_ticks();
    EXPECT_GT(qpc_vblank, 0u);
    EXPECT_LE(qpc_vblank, now);
    EXPECT_LT(now - qpc_vblank, refresh_period);
    EXPECT_EQ(qpc_vblank % refresh_period, 0u);
    EXPECT_EQ(memory.read_u64(info + 36).value(), // cRefresh
              qpc_vblank / refresh_period);
}

TEST_F(SmallDllsCpuzTest, Dwmapi_GetCompositionTimingInfoRejectsWrongCbSize) {
    // Learn: cbSize must be sizeof(DWM_TIMING_INFO) or the call fails.
    auto exports = create_dwmapi_thunks();
    auto* timing = find_export(exports, "DwmGetCompositionTimingInfo");
    ASSERT_NE(timing, nullptr);

    constexpr GuestAddr info = DATA_BASE + 0x400;
    ASSERT_TRUE(memory.write_u32(info, 320).is_ok());
    push_arg(0, 0x10011);
    push_arg(1, static_cast<u32>(info));
    timing->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80070057u); // E_INVALIDARG
}

// =============================================================================
// MSIMG32 — TransparentBlt (11 args, returns 1)
// =============================================================================

TEST_F(SmallDllsCpuzTest, Msimg32_TransparentBlt) {
    Msimg32Manager msimg_mgr;
    // TransparentBlt(hdcDest, xD, yD, wD, hD, hdcSrc, xS, yS, wS, hS, crTransparent)
    push_arg(0, 0xDC01);     // hdcDest
    push_arg(1, 0);          // xoriginDest
    push_arg(2, 0);          // yoriginDest
    push_arg(3, 100);        // wDest
    push_arg(4, 100);        // hDest
    push_arg(5, 0xDC02);     // hdcSrc
    push_arg(6, 0);          // xoriginSrc
    push_arg(7, 0);          // yoriginSrc
    push_arg(8, 100);        // wSrc
    push_arg(9, 100);        // hSrc
    push_arg(10, 0x00FF00FF); // crTransparent (magenta)
    msimg_mgr.impl_TransparentBlt(marshaler);
    // DCs that do not exist: FALSE (s3gdi_misc_probe "TB null dst r=0").
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

// =============================================================================
// MSIMG32 — AlphaBlend (11 args, returns 1)
// =============================================================================

TEST_F(SmallDllsCpuzTest, Msimg32_AlphaBlend) {
    Msimg32Manager msimg_mgr;
    push_arg(0, 0xDC01);     // hdcDest
    push_arg(1, 0);          // xoriginDest
    push_arg(2, 0);          // yoriginDest
    push_arg(3, 200);        // wDest
    push_arg(4, 200);        // hDest
    push_arg(5, 0xDC02);     // hdcSrc
    push_arg(6, 0);          // xoriginSrc
    push_arg(7, 0);          // yoriginSrc
    push_arg(8, 200);        // wSrc
    push_arg(9, 200);        // hSrc
    push_arg(10, 0x00FF0000); // BLENDFUNCTION: AC_SRC_OVER, ca=255
    msimg_mgr.impl_AlphaBlend(marshaler);
    // No GdiManager is wired here and the DC handles are fabricated, so there
    // is nothing to blend. AlphaBlend must say so rather than report a blend
    // that never happened — the old stub returned TRUE unconditionally, which
    // left Battle.net-Setup.exe's layered window blank while every call
    // claimed success. Real blending is covered by GdiAlphaBlendTest.
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

// =============================================================================
// WINSPOOL — ClosePrinter (1 arg). OpenPrinterW never succeeds (no printers
// in the capsule), so no printer handle exists and closing one is FALSE with
// ERROR_INVALID_HANDLE (Learn ClosePrinter), not the old unconditional TRUE.
// =============================================================================

TEST_F(SmallDllsCpuzTest, Winspool_ClosePrinter) {
    WinspoolManager ws_mgr;
    push_arg(0, 0xAA01); // hPrinter
    ws_mgr.impl_ClosePrinter(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

// =============================================================================
// WINSPOOL — OpenPrinterW (3 args, returns 0 — no printers)
// =============================================================================

TEST_F(SmallDllsCpuzTest, Winspool_OpenPrinterW) {
    WinspoolManager ws_mgr;
    // Write printer name "FakePrinter\0" as UTF-16LE
    constexpr GuestAddr name_addr = DATA_BASE + 0x100;
    const char16_t name[] = u"FakePrinter";
    for (usize i = 0; i <= 11; ++i) {
        (void)memory.write_u16(name_addr + i * 2, static_cast<u16>(name[i]));
    }
    constexpr GuestAddr handle_out = DATA_BASE + 0x200;
    (void)memory.write_u64(handle_out, 0xDEADBEEFu);

    push_arg(0, name_addr);   // pPrinterName
    push_arg(1, handle_out);  // phPrinter
    push_arg(2, 0);           // pDefault = NULL
    ws_mgr.impl_OpenPrinterW(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(memory.read_u64(handle_out).value_or(1), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PRINTER_NAME);

    push_arg(0, name_addr);
    push_arg(1, 0); // phPrinter NULL
    push_arg(2, 0);
    ws_mgr.impl_OpenPrinterW(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(SmallDllsCpuzTest, Winspool_AnsiPrinterExportsAreRegistered) {
    WinspoolManager ws_mgr;
    auto exports = create_winspool_thunks(ws_mgr);
    ExportEntry* open_a = find_export(exports, "OpenPrinterA");
    ExportEntry* doc_a = find_export(exports, "DocumentPropertiesA");
    ASSERT_NE(open_a, nullptr);
    ASSERT_NE(doc_a, nullptr);
    EXPECT_EQ(open_a->stdcall_bytes, 12u);
    EXPECT_EQ(doc_a->stdcall_bytes, 24u);

    constexpr GuestAddr handle_out = DATA_BASE + 0x200;
    (void)memory.write_u64(handle_out, 0xDEADBEEFu);
    push_arg(0, 0);
    push_arg(1, handle_out);
    push_arg(2, 0);
    open_a->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(memory.read_u64(handle_out).value_or(1), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PRINTER_NAME);
}

TEST_F(SmallDllsCpuzTest, Winspool_EnumPortsAEmptySuccess) {
    // Learn: EnumPorts may succeed with zero ports; pcbNeeded/pcReturned = 0.
    WinspoolManager ws_mgr;
    auto exports = create_winspool_thunks(ws_mgr);
    ExportEntry* entry = find_export(exports, "EnumPortsA");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr needed = DATA_BASE + 0x300;
    constexpr GuestAddr returned = DATA_BASE + 0x304;
    (void)memory.write_u32(needed, 0xFFFFFFFFu);
    (void)memory.write_u32(returned, 0xFFFFFFFFu);

    push_arg(0, 0);          // pName = NULL (local)
    push_arg(1, 1);          // Level = 1
    push_arg(2, 0);          // pPorts
    push_arg(3, 0);          // cbBuf
    push_arg(4, needed);     // pcbNeeded
    push_arg(5, returned);   // pcReturned
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(memory.read_u32(needed).value_or(1), 0u);
    EXPECT_EQ(memory.read_u32(returned).value_or(1), 0u);
}

// =============================================================================
// IPHLPAPI — GetIfEntry (1 arg, returns 0 = NO_ERROR)
// =============================================================================

TEST_F(SmallDllsCpuzTest, Iphlpapi_IcmpOwnedHandleRejectsInvalidOperations) {
    IphlpapiManager ip_mgr;
    auto exports = create_iphlpapi_thunks(ip_mgr);
    const auto* create = find_export(exports, "IcmpCreateFile");
    const auto* close = find_export(exports, "IcmpCloseHandle");
    const auto* echo = find_export(exports, "IcmpSendEcho");
    ASSERT_NE(create, nullptr);
    ASSERT_NE(close, nullptr);
    ASSERT_NE(echo, nullptr);
    // Create and close own native ICMP resources; invalid operations preserve
    // the caller's reply storage (Windows).
    for (bool wide : {false, true}) {
        if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        set_last_error(0xdead);
        create->handler(marshaler);
        ASSERT_NE(cpu.gpr.rax, wide ? ~u64{0} : u64{0xffffffffu});
        const u64 handle = cpu.gpr.rax;
        EXPECT_EQ(get_last_error(), 0u);

        // Oracle: close-null and invalid-handle return 0 / error 87 in
        // icmp_contract_probe32.expected and icmp_contract_probe64.expected.
        if (wide) cpu.gpr.rcx = 0; else push_arg(0, 0);
        set_last_error(0xdead);
        close->handler(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

        constexpr GuestAddr reply = DATA_BASE + 0x800;
        for (u32 i = 0; i < 128; ++i) ASSERT_TRUE(memory.write_u8(reply + i, 0xcc).is_ok());
        if (wide) {
            cpu.gpr.rdx = 0x0100007f;
            cpu.gpr.r8 = 0;
            cpu.gpr.r9 = 0;
            ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 40, 0).is_ok());
            ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 48, reply).is_ok());
            ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 56, 128).is_ok());
            ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 64, 0).is_ok());
        } else {
            push_arg(1, 0x0100007f); push_arg(2, 0); push_arg(3, 0);
            push_arg(4, 0); push_arg(5, static_cast<u32>(reply));
            push_arg(6, 128); push_arg(7, 0);
        }
        set_last_error(0xdead);
        echo->handler(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
        for (u32 i = 0; i < 128; ++i) EXPECT_EQ(memory.read_u8(reply + i).value_or(0), 0xcc);
        if (wide) cpu.gpr.rcx = handle; else push_arg(0, static_cast<u32>(handle));
        set_last_error(0xdead); close->handler(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 1u);
        EXPECT_EQ(get_last_error(), 0xdeadu);
    }
}

TEST_F(SmallDllsCpuzTest, Iphlpapi_IcmpLoopbackMarshalsRepliesAndOracleErrors) {
    IphlpapiManager mgr;
    constexpr GuestAddr data = DATA_BASE + 0x100, reply = DATA_BASE + 0x400;
    const std::array<u8, 5> payload{1, 2, 3, 4, 5};
    ASSERT_TRUE(memory.write_bytes(data, payload.data(), payload.size()).is_ok());
    for (bool wide : {false, true}) {
        if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        mgr.impl_IcmpCreateFile(marshaler);
        const u64 handle = cpu.gpr.rax;
        ASSERT_NE(handle, wide ? ~u64{0} : u64{0xffffffffu});
        const u32 header = wide ? 40 : 28;
        auto echo = [&](GuestAddr input, u32 input_size, u32 size, GuestAddr options = 0) {
            if (wide) {
                cpu.gpr.rcx = handle; cpu.gpr.rdx = 0x0100007f;
                cpu.gpr.r8 = input; cpu.gpr.r9 = input_size;
                (void)memory.write_u64(cpu.gpr.rsp + 40, options);
                (void)memory.write_u64(cpu.gpr.rsp + 48, reply);
                (void)memory.write_u64(cpu.gpr.rsp + 56, size);
                (void)memory.write_u64(cpu.gpr.rsp + 64, 1000);
            } else {
                push_arg(0, static_cast<u32>(handle)); push_arg(1, 0x0100007f);
                push_arg(2, static_cast<u32>(input)); push_arg(3, input_size);
                push_arg(4, static_cast<u32>(options)); push_arg(5, reply); push_arg(6, size); push_arg(7, 1000);
            }
            mgr.impl_IcmpSendEcho(marshaler);
        };
        echo(data, 5, 512);
        ASSERT_EQ(cpu.gpr.rax, 1u);
        EXPECT_EQ(get_last_error(), 0u);
        EXPECT_EQ(memory.read_u32(reply).value_or(0), 0x0100007fu);
        EXPECT_EQ(memory.read_u32(reply + 4).value_or(1), 0u);
        EXPECT_EQ(memory.read_u16(reply + 12).value_or(0), 5u);
        const u64 output = wide ? memory.read_u64(reply + 16).value_or(0) : memory.read_u32(reply + 16).value_or(0);
        EXPECT_EQ(output, reply + header);
        for (usize i = 0; i < payload.size(); ++i) EXPECT_EQ(memory.read_u8(output + i).value_or(0), payload[i]);
        (void)memory.write_u8(reply, 0xcc);
        echo(data, 5, header - 1);
        EXPECT_EQ(cpu.gpr.rax, 0u); EXPECT_EQ(get_last_error(), 87u);
        EXPECT_EQ(memory.read_u8(reply).value_or(0), 0xccu);
        echo(0, 5, 512);
        EXPECT_EQ(cpu.gpr.rax, 0u); EXPECT_EQ(get_last_error(), 998u);
        EXPECT_EQ(memory.read_u8(reply).value_or(0), 0xccu);
        echo(data, 5, header);
        EXPECT_EQ(cpu.gpr.rax, 0u); EXPECT_EQ(get_last_error(), 11050u);
        EXPECT_EQ(memory.read_u32(reply + 4).value_or(0), 11050u);
        constexpr GuestAddr options = DATA_BASE + 0x180, option_bytes = DATA_BASE + 0x1c0;
        (void)memory.write_u64(options, 128);
        (void)memory.write_u64(options + 8, 0);
        (void)memory.write_u8(options + 2, 2); // IP_FLAG_DF
        echo(data, 5, 512, options); ASSERT_EQ(cpu.gpr.rax, 1u);
        (void)memory.write_u8(options + 2, 0);
        (void)memory.write_u8(options + 3, 1);
        (void)memory.write_u8(option_bytes, 0); // RFC 791 end-of-options
        if (wide) (void)memory.write_u64(options + 8, option_bytes);
        else (void)memory.write_u32(options + 4, option_bytes);
        echo(data, 5, 512, options); ASSERT_EQ(cpu.gpr.rax, 1u);
        echo(0, 0, 512); ASSERT_EQ(cpu.gpr.rax, 1u);
        EXPECT_EQ(memory.read_u16(reply + 12).value_or(1), 0u);
        if (wide) cpu.gpr.rcx = handle; else push_arg(0, static_cast<u32>(handle));
        mgr.impl_IcmpCloseHandle(marshaler); EXPECT_EQ(cpu.gpr.rax, 1u);
    }
}

TEST_F(SmallDllsCpuzTest, Iphlpapi_GetIfEntry) {
    IphlpapiManager ip_mgr;
    push_arg(0, DATA_BASE + 0x300); // pIfRow
    ip_mgr.impl_GetIfEntry(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1168u); // index zero is not an interface
}

TEST_F(SmallDllsCpuzTest, Iphlpapi_GetIfTableReportsSizeAndSortedRealInterfaces) {
    IphlpapiManager ip_mgr;
    constexpr GuestAddr base = 0x140000000;
    ASSERT_TRUE(memory.map_region(base, 0x100000, MemoryProtection::ReadWrite).is_ok());
    cpu.init_long_mode();
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = base;
    cpu.gpr.r8 = 1;
    ip_mgr.impl_GetIfTable(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 122u);
    const u32 needed = memory.read_u32(base).value();
    ASSERT_GE(needed, 864u);
    ASSERT_LT(needed, 0xFF000u);
    cpu.gpr.rcx = base + 0x1000;
    ip_mgr.impl_GetIfTable(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const u32 count = memory.read_u32(base + 0x1000).value();
    EXPECT_EQ(needed, 4 + count * 860);
    u32 previous = 0;
    for (u32 i = 0; i < count; ++i) {
        const GuestAddr row = base + 0x1004 + i * 860;
        const u32 index = memory.read_u32(row + 512).value();
        EXPECT_GT(index, previous);
        previous = index;
        EXPECT_NE(memory.read_u16(row).value(), 0u);
    }
    const u32 first = memory.read_u32(base + 0x1004 + 512).value();
    ASSERT_TRUE(memory.write_u32(base + 0x200 + 512, first).is_ok());
    cpu.gpr.rcx = base + 0x200;
    ip_mgr.impl_GetIfEntry(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(base + 0x200 + 512).value(), first);
    cpu.gpr.rdx = 0;
    ip_mgr.impl_GetIfTable(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 87u);
}

// =============================================================================
// IPHLPAPI — GetAdaptersInfo
// =============================================================================

TEST_F(SmallDllsCpuzTest, Iphlpapi_GetAdaptersInfo) {
    IphlpapiManager ip_mgr;
    constexpr GuestAddr buf_addr = DATA_BASE + 0x400;
    constexpr GuestAddr size_addr = DATA_BASE + 0x500;
    // Set a small buffer size to potentially trigger ERROR_BUFFER_OVERFLOW
    (void)memory.write_u32(size_addr, 4);

    push_arg(0, buf_addr);
    push_arg(1, size_addr);
    ip_mgr.impl_GetAdaptersInfo(marshaler);
    // Either ERROR_SUCCESS (0) or ERROR_BUFFER_OVERFLOW (111)
    u32 ret = static_cast<u32>(cpu.gpr.rax);
    EXPECT_TRUE(ret == 0u || ret == 111u);
}

TEST_F(SmallDllsCpuzTest, Iphlpapi_GetAdaptersInfoUsesX64LayoutFor64BitGuests) {
    // IP_ADAPTER_INFO is laid out by the C compiler, so a 64-bit guest reads
    // 8-byte pointers and their padding. Writing the 32-bit offsets to an x64
    // guest put IpAddressList 12 bytes early and IpMask 4 bytes early inside
    // it, so Steam read its subnet mask as "255.255.0"; inet_addr() answered
    // INADDR_NONE and the resulting NULL was dereferenced at +0x30.
    constexpr GuestAddr buf_addr = DATA_BASE + 0x2000;
    constexpr GuestAddr size_addr = DATA_BASE + 0x500;

    cpu.init_long_mode();
    ASSERT_TRUE(cpu.is_64bit());

    // Size query first: the x64 struct is larger than the x86 one.
    (void)memory.write_u32(size_addr, 0);
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = size_addr;
    IphlpapiManager ip_mgr;
    ip_mgr.impl_GetAdaptersInfo(marshaler);
    const u32 ret = static_cast<u32>(cpu.gpr.rax);
    if (ret == 232u /* ERROR_NO_DATA: no IPv4 interface on this host */) {
        GTEST_SKIP() << "host has no primary IPv4 adapter";
    }
    ASSERT_EQ(ret, 111u); // ERROR_BUFFER_OVERFLOW
    const u32 need = memory.read_u32(size_addr).value_or(0);
    // One 704-byte x64 IP_ADAPTER_INFO per IPv4 adapter plus chained extra
    // IP_ADDR_STRINGs (iphlp_adapters_probe64 sizeof.IP_ADAPTER_INFO=704).
    EXPECT_GE(need, 704u) << "x64 sizeof(IP_ADAPTER_INFO)";

    cpu.gpr.rcx = buf_addr;
    cpu.gpr.rdx = size_addr;
    ip_mgr.impl_GetAdaptersInfo(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);

    const auto read_string = [&](GuestAddr at) {
        std::string out;
        for (u32 i = 0; i < 16; ++i) {
            const u8 c = memory.read_u8(at + i).value_or(0);
            if (c == 0) break;
            out.push_back(static_cast<char>(c));
        }
        return out;
    };

    // x64: IpAddressList at +440, IpAddress.String at +8, IpMask.String at +24.
    const std::string ip = read_string(buf_addr + 440 + 8);
    const std::string mask = read_string(buf_addr + 440 + 24);
    EXPECT_FALSE(ip.empty());
    EXPECT_FALSE(mask.empty());
    // A dotted quad, not a truncated one — this is what inet_addr must parse.
    EXPECT_EQ(std::count(mask.begin(), mask.end(), '.'), 3)
        << "mask read back as \"" << mask << "\"";
    EXPECT_EQ(std::count(ip.begin(), ip.end(), '.'), 3)
        << "ip read back as \"" << ip << "\"";

    // GatewayList follows a 48-byte IP_ADDR_STRING.
    const std::string gw = read_string(buf_addr + 440 + 48 + 8);
    EXPECT_EQ(std::count(gw.begin(), gw.end(), '.'), 3)
        << "gateway read back as \"" << gw << "\"";
}

TEST_F(SmallDllsCpuzTest, Iphlpapi_GetNetworkParams) {
    IphlpapiManager ip_mgr;
    constexpr GuestAddr size_addr = DATA_BASE + 0x100;
    (void)memory.write_u32(size_addr, 0);
    push_arg(0, 0);
    push_arg(1, size_addr);
    ip_mgr.impl_GetNetworkParams(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 111u); // ERROR_BUFFER_OVERFLOW
    const u32 need = memory.read_u32(size_addr).value_or(0);
    // sizeof(FIXED_INFO) on x86, plus one IP_ADDR_STRING per further resolver.
    EXPECT_GE(need, 584u);
    EXPECT_EQ((need - 584u) % 40u, 0u);

    auto map = memory.map_region(0x21000, 0x400, MemoryProtection::ReadWrite);
    ASSERT_TRUE(map.is_ok());
    constexpr GuestAddr info = 0x21000;
    (void)memory.write_u32(size_addr, need);
    push_arg(0, info);
    push_arg(1, size_addr);
    ip_mgr.impl_GetNetworkParams(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(memory.read_u32(info + 580).value_or(1), 0u); // EnableDns (iphlp_adapters_probe gnp)
}

TEST_F(SmallDllsCpuzTest, Iphlpapi_GetAdaptersAddresses) {
    IphlpapiManager ip_mgr;
    constexpr GuestAddr size_addr = DATA_BASE + 0x180;
    (void)memory.write_u32(size_addr, 0);
    push_arg(0, 2); // AF_INET
    push_arg(1, 0);
    push_arg(2, 0);
    push_arg(3, 0);
    push_arg(4, size_addr);
    ip_mgr.impl_GetAdaptersAddresses(marshaler);
    const u32 ret1 = static_cast<u32>(cpu.gpr.rax);
    ASSERT_TRUE(ret1 == 111u || ret1 == 232u); // OVERFLOW or NO_DATA
    if (ret1 == 232u) return;
    const u32 need = memory.read_u32(size_addr).value_or(0);
    ASSERT_GE(need, 376u);
    auto map = memory.map_region(0x22000, 0x40000, MemoryProtection::ReadWrite);
    ASSERT_TRUE(map.is_ok());
    ASSERT_LE(need, 0x40000u);
    constexpr GuestAddr buf = 0x22000;
    (void)memory.write_u32(size_addr, need);
    push_arg(0, 2);
    push_arg(1, 0);
    push_arg(2, 0);
    push_arg(3, buf);
    push_arg(4, size_addr);
    ip_mgr.impl_GetAdaptersAddresses(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    // x86 IP_ADAPTER_ADDRESSES_LH is 376 bytes (iphlp_adapters_probe32).
    EXPECT_EQ(memory.read_u32(buf).value_or(0), 376u); // Length
}

TEST_F(SmallDllsCpuzTest, Iphlpapi_GetAdaptersAddressesUsesX64Layout) {
    IphlpapiManager ip_mgr;
    constexpr GuestAddr size_addr = DATA_BASE + 0x180;
    constexpr GuestAddr buf = 0x22000;
    ASSERT_TRUE(memory.map_region(
        buf, 0x40000, MemoryProtection::ReadWrite).is_ok());

    cpu.init_long_mode();
    cpu.gpr.rsp = STACK_BASE + 0x800;
    cpu.gpr.rcx = 2; // AF_INET
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = 0;
    cpu.gpr.r9 = 0;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, size_addr).is_ok());
    ASSERT_TRUE(memory.write_u32(size_addr, 0).is_ok());
    ip_mgr.impl_GetAdaptersAddresses(marshaler);

    const u32 first_result = static_cast<u32>(cpu.gpr.rax);
    ASSERT_TRUE(first_result == 111u || first_result == 232u);
    if (first_result == 232u) return;

    ASSERT_LE(memory.read_u32(size_addr).value_or(~0u), 0x40000u);
    cpu.gpr.r9 = buf;
    ip_mgr.impl_GetAdaptersAddresses(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    // x64 IP_ADAPTER_ADDRESSES_LH: Length 448, 8-byte pointers at +24
    // (FirstUnicastAddress) and +48 (FirstDnsServerAddress); unicast node
    // Length 64 with its SOCKET_ADDRESS pointer at +16 (iphlp_adapters_probe64).
    EXPECT_EQ(memory.read_u32(buf).value_or(0), 448u);
    const u64 unicast = memory.read_u64(buf + 24).value_or(0);
    ASSERT_GE(unicast, buf);
    ASSERT_LT(unicast, buf + 0x40000);
    EXPECT_EQ(memory.read_u32(unicast).value_or(0), 64u);
    const u64 sockaddr = memory.read_u64(unicast + 16).value_or(0);
    EXPECT_GE(sockaddr, buf);
    EXPECT_LT(sockaddr, buf + 0x40000);
}

TEST_F(SmallDllsCpuzTest, Iphlpapi_GuidLookupValidatesIdentityAndFailureOutput) {
    IphlpapiManager mgr;
    const u32 loopback = ::if_nametoindex("lo0");
    ASSERT_NE(loopback, 0u);
    constexpr GuestAddr guid = DATA_BASE + 0x200, luid = DATA_BASE + 0x220;
    for (bool wide : {false, true}) {
        if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        auto args = [&](GuestAddr a, GuestAddr b) {
            if (wide) { cpu.gpr.rcx = a; cpu.gpr.rdx = b; }
            else { push_arg(0, static_cast<u32>(a)); push_arg(1, static_cast<u32>(b)); }
        };
        args(loopback, luid);
        mgr.impl_ConvertInterfaceIndexToLuid(marshaler);
        const u64 original = memory.read_u64(luid).value_or(0);
        args(luid, guid); mgr.impl_ConvertInterfaceLuidToGuid(marshaler);
        args(guid, luid); mgr.impl_ConvertInterfaceGuidToLuid(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        EXPECT_EQ(memory.read_u64(luid).value_or(0), original);
        // Every GUID byte contributes to identity, including Data4.
        for (u32 i = 0; i < 16; ++i) {
            const u8 old = memory.read_u8(guid + i).value_or(0);
            ASSERT_TRUE(memory.write_u8(guid + i, old ^ 0x80).is_ok());
            ASSERT_TRUE(memory.write_u64(luid, ~u64{0}).is_ok());
            set_last_error(0xdead);
            mgr.impl_ConvertInterfaceGuidToLuid(marshaler);
            EXPECT_EQ(cpu.gpr.rax, 87u);
            EXPECT_EQ(memory.read_u64(luid).value_or(1), 0u);
            EXPECT_EQ(get_last_error(), 0u);
            ASSERT_TRUE(memory.write_u8(guid + i, old).is_ok());
        }
        ASSERT_TRUE(memory.write_u64(luid, ~u64{0}).is_ok());
        args(0, luid); set_last_error(0xdead);
        mgr.impl_ConvertInterfaceGuidToLuid(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 87u);
        EXPECT_EQ(memory.read_u64(luid).value_or(0), ~u64{0});
        EXPECT_EQ(get_last_error(), 0xdeadu);
    }
}

TEST_F(SmallDllsCpuzTest, Iphlpapi_GetBestInterfaceUsesDestinationAndChecksOutput) {
    IphlpapiManager mgr;
    for (bool wide : {false, true}) {
        if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        auto args = [&](GuestAddr out) {
            if (wide) { cpu.gpr.rcx = 0x0100007f; cpu.gpr.rdx = out; }
            else { push_arg(0, 0x0100007f); push_arg(1, static_cast<u32>(out)); }
        };
        args(DATA_BASE); set_last_error(0xdead);
        mgr.impl_GetBestInterface(marshaler);
        ASSERT_EQ(cpu.gpr.rax, 0u);
        EXPECT_EQ(memory.read_u32(DATA_BASE).value_or(0), ::if_nametoindex("lo0"));
        EXPECT_EQ(get_last_error(), 0u);
        for (GuestAddr out : {GuestAddr{0}, GuestAddr{0xdead0000}}) {
            args(out); set_last_error(0xdead);
            mgr.impl_GetBestInterface(marshaler);
            EXPECT_EQ(cpu.gpr.rax, 87u);
            EXPECT_EQ(get_last_error(), 0xdeadu);
        }
    }
}

TEST_F(SmallDllsCpuzTest, Iphlpapi_AddressNotificationsOwnPendingCompletionAndCancellation) {
    HandleTable handles;
    SyncManager sync(handles);
    IphlpapiManager mgr;
    mgr.set_notification_managers(handles, sync);
    capsule::Vfs vfs;
    Kernel32ExtManager kernel(memory, vfs);
    kernel.set_handle_table(&handles);
    kernel.set_sync_manager(&sync);
    constexpr GuestAddr out = DATA_BASE + 0x100, ov = DATA_BASE + 0x200;
    const Handle event = sync.create_event(true, true, "");
    for (bool wide : {false, true}) {
        if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        auto args = [&](GuestAddr a, GuestAddr b) {
            if (wide) { cpu.gpr.rcx = a; cpu.gpr.rdx = b; }
            else { push_arg(0, static_cast<u32>(a)); push_arg(1, static_cast<u32>(b)); }
        };
        ASSERT_TRUE(memory.write_u64(out, ~u64{0}).is_ok());
        ASSERT_TRUE(memory.write_u64(ov, 0).is_ok());
        if (wide) {
            ASSERT_TRUE(memory.write_u64(ov + 8, 0xcccc).is_ok());
            ASSERT_TRUE(memory.write_u64(ov + 24, event).is_ok());
        } else {
            ASSERT_TRUE(memory.write_u32(ov + 4, 0xcccc).is_ok());
            ASSERT_TRUE(memory.write_u32(ov + 16, event).is_ok());
        }
        args(out, ov); mgr.impl_NotifyAddrChange(marshaler);
        ASSERT_EQ(cpu.gpr.rax, 997u);
        EXPECT_EQ(get_last_error(), 997u);
        const Handle notification = memory.read_u32(out).value_or(0);
        EXPECT_EQ(handles.get_type(notification), HandleType::NetworkNotification);
        if (wide) EXPECT_EQ(memory.read_u64(out).value_or(0), notification);
        EXPECT_EQ(memory.read_u32(ov).value_or(0), 0x103u);
        EXPECT_EQ(memory.read_u32(ov + (wide ? 8 : 4)).value_or(0), 0xccccu);
        EXPECT_EQ(sync.wait_for_single_object(event, 0), WAIT_TIMEOUT);
        mgr.impl_NotifyAddrChange(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 997u);
        EXPECT_EQ(memory.read_u32(out).value_or(0), notification);
        args(ov, 0); mgr.impl_CancelIPChangeNotify(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 1u);
        EXPECT_EQ(get_last_error(), 0u);
        EXPECT_EQ(sync.wait_for_single_object(event, 0), WAIT_OBJECT_0);
        EXPECT_EQ(memory.read_u32(ov).value_or(0), 0xc0000120u);
        EXPECT_EQ(memory.read_u32(ov + (wide ? 8 : 4)).value_or(1), 0u);
        args(notification, ov);
        if (wide) { cpu.gpr.r8 = out + 16; cpu.gpr.r9 = 1; }
        else { push_arg(2, static_cast<u32>(out + 16)); push_arg(3, 1); }
        ASSERT_TRUE(memory.write_u32(out + 16, 0xcccc).is_ok());
        kernel.impl_GetOverlappedResult(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        EXPECT_EQ(get_last_error(), 995u);
        EXPECT_EQ(memory.read_u32(out + 16).value_or(1), 0u);
        args(ov, 0); mgr.impl_CancelIPChangeNotify(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        EXPECT_EQ(get_last_error(), 0u);
        if (wide) ASSERT_TRUE(memory.write_u64(ov + 24, 0xdead).is_ok());
        else ASSERT_TRUE(memory.write_u32(ov + 16, 0xdead).is_ok());
        args(out, ov); mgr.impl_NotifyAddrChange(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 6u);
        EXPECT_EQ(get_last_error(), 6u);
        EXPECT_EQ(memory.read_u32(ov).value_or(0), 0x103u);
        args(ov, 0); mgr.impl_CancelIPChangeNotify(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        if (wide) ASSERT_TRUE(memory.write_u64(ov + 24, event).is_ok());
        else ASSERT_TRUE(memory.write_u32(ov + 16, event).is_ok());
        // A null HANDLE output is a documented-ambiguous, oracle-valid async call.
        args(0, ov); mgr.impl_NotifyAddrChange(marshaler);
        ASSERT_EQ(cpu.gpr.rax, 997u);
        args(ov, 0); mgr.impl_CancelIPChangeNotify(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 1u);
    }
    (void)sync.close_handle(event);
}

TEST_F(SmallDllsCpuzTest, Iphlpapi_GetIfEntry2SelectorsAndCompleteSoftwareLoopbackSnapshot) {
    IphlpapiManager mgr;
    constexpr GuestAddr row = DATA_BASE + 0x200;
    for (bool wide : {false, true}) {
        if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        if (wide) cpu.gpr.rcx = row; else push_arg(0, row);
        ASSERT_TRUE(memory.write_u64(row, 0).is_ok());
        ASSERT_TRUE(memory.write_u32(row + 8, 0).is_ok());
        mgr.impl_GetIfEntry2(marshaler); EXPECT_EQ(cpu.gpr.rax, 87u);
        ASSERT_TRUE(memory.write_u32(row + 8, 0x7fffffff).is_ok());
        mgr.impl_GetIfEntry2(marshaler); EXPECT_EQ(cpu.gpr.rax, 2u);
        ASSERT_TRUE(memory.write_u64(row, 1).is_ok());
        ASSERT_TRUE(memory.write_u32(row + 8, ::if_nametoindex("lo0")).is_ok());
        mgr.impl_GetIfEntry2(marshaler); EXPECT_EQ(cpu.gpr.rax, 2u);
        EXPECT_EQ(memory.read_u64(row).value_or(0), 1u);
        ASSERT_TRUE(memory.write_u64(row, 0).is_ok());
        ASSERT_TRUE(memory.write_u32(row + 12, 0xcccccccc).is_ok());
        mgr.impl_GetIfEntry2(marshaler);
        using namespace if_row2_contract;
        ASSERT_EQ(cpu.gpr.rax, 0u);
        EXPECT_EQ(memory.read_u32(row + kIfRow2Type).value_or(0), kLoopbackType);
        EXPECT_EQ(memory.read_u32(row + kIfRow2MediaType).value_or(0), kLoopbackMediaType);
        EXPECT_EQ(memory.read_u32(row + kIfRow2PhysicalAddressLength).value_or(1), 0u);
        EXPECT_EQ(memory.read_u32(row + kIfRow2Mtu).value_or(0) > 0, true);
        EXPECT_EQ(memory.read_u64(row + kIfRow2InterfaceLuid).value_or(0) >> 48, kLoopbackType);
        EXPECT_EQ(memory.read_u64(row + kIfRow2TransmitLinkSpeed).value_or(0), kLoopbackTransmitLinkSpeed);
        EXPECT_EQ(memory.read_u64(row + kIfRow2ReceiveLinkSpeed).value_or(0), kLoopbackReceiveLinkSpeed);
        // Windows reports no loopback counters, including after a real echo.
        for (u32 i = kIfRow2InOctets; i < kIfRow2Size; ++i)
            EXPECT_EQ(memory.read_u8(row + i).value_or(0xff), 0u);
        // Alias is Windows' "Loopback Pseudo-Interface 1" (iphlp_adapters_probe ift2).
        EXPECT_EQ(memory.read_u16(row + kIfRow2Alias).value_or(0), static_cast<u16>('L'));
        const u64 loopback_luid = memory.read_u64(row).value_or(0);
        ASSERT_TRUE(memory.write_u32(row + kIfRow2InterfaceIndex, 0x7fffffff).is_ok());
        mgr.impl_GetIfEntry2(marshaler); // valid LUID wins over invalid index
        ASSERT_EQ(cpu.gpr.rax, 0u);
        EXPECT_EQ(memory.read_u64(row).value_or(0), loopback_luid);
        EXPECT_EQ(memory.read_u32(row + kIfRow2InterfaceIndex).value_or(0), ::if_nametoindex("lo0"));
    }
}

TEST_F(SmallDllsCpuzTest, Iphlpapi_ConvertInterfaceLuidRoundTrip) {
    IphlpapiManager ip_mgr;
    constexpr GuestAddr luid = DATA_BASE + 0x200;
    constexpr GuestAddr index = DATA_BASE + 0x210;
    const u32 loopback = ::if_nametoindex("lo0");
    push_arg(0, loopback);
    push_arg(1, luid);
    ip_mgr.impl_ConvertInterfaceIndexToLuid(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    push_arg(0, luid);
    push_arg(1, index);
    ip_mgr.impl_ConvertInterfaceLuidToIndex(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(memory.read_u32(index).value_or(0), loopback);
    for (u32 missing : {0u, 0x7fffffffu}) {
        ASSERT_TRUE(memory.write_u64(luid, ~u64{0}).is_ok());
        push_arg(0, missing); push_arg(1, luid);
        ip_mgr.impl_ConvertInterfaceIndexToLuid(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 2u);
        EXPECT_EQ(memory.read_u64(luid).value_or(1), 0u);
    }
}

// =============================================================================
// IPHLPAPI — GetExtendedTcpTable (OWNER_PID size-query + fill)
// =============================================================================

TEST_F(SmallDllsCpuzTest, Iphlpapi_GetExtendedTcpTableOwnerPid) {
    IphlpapiManager ip_mgr;
    constexpr GuestAddr size_addr = DATA_BASE + 0x200;
    constexpr u32 AF_INET_GUEST = 2;
    constexpr u32 TCP_TABLE_OWNER_PID_ALL = 5;

    // Size query with null table / zero capacity.
    (void)memory.write_u32(size_addr, 0);
    push_arg(0, 0);          // pTcpTable
    push_arg(1, size_addr);  // pdwSize
    push_arg(2, 0);          // bOrder
    push_arg(3, AF_INET_GUEST);
    push_arg(4, TCP_TABLE_OWNER_PID_ALL);
    push_arg(5, 0);          // Reserved
    ip_mgr.impl_GetExtendedTcpTable(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 122u); // ERROR_INSUFFICIENT_BUFFER
    const u32 needed = memory.read_u32(size_addr).value_or(0);
    ASSERT_GE(needed, 4u);
    ASSERT_EQ((needed - 4u) % 24u, 0u);

    // Fill with a large enough guest buffer (may be empty on restricted hosts).
    auto map = memory.map_region(0x20000, 0x20000, MemoryProtection::ReadWrite);
    ASSERT_TRUE(map.is_ok());
    constexpr GuestAddr large_table = 0x20000;
    (void)memory.write_u32(size_addr, needed);
    push_arg(0, large_table);
    push_arg(1, size_addr);
    push_arg(2, 0);
    push_arg(3, AF_INET_GUEST);
    push_arg(4, TCP_TABLE_OWNER_PID_ALL);
    push_arg(5, 0);
    ip_mgr.impl_GetExtendedTcpTable(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    const u32 count = memory.read_u32(large_table).value_or(0xFFFFFFFFu);
    EXPECT_EQ(count, (needed - 4u) / 24u);
}

// =============================================================================
// COMCTL32 — ImageList_GetIconSize (3 args, returns TRUE for valid list)
// =============================================================================

TEST_F(SmallDllsCpuzTest, Comctl32_ImageListGetIconSize) {
    ComCtl32Manager ctl_mgr;

    // First create an image list to get a valid handle
    push_arg(0, 16);   // cx
    push_arg(1, 16);   // cy
    push_arg(2, 0x21); // flags
    push_arg(3, 10);   // cInitial
    push_arg(4, 5);    // cGrow
    ctl_mgr.impl_ImageList_Create(marshaler);
    u32 himl = static_cast<u32>(cpu.gpr.rax);
    ASSERT_NE(himl, 0u);

    // Now test GetIconSize
    constexpr GuestAddr cx_addr = DATA_BASE + 0x600;
    constexpr GuestAddr cy_addr = DATA_BASE + 0x604;
    (void)memory.write_u32(cx_addr, 0);
    (void)memory.write_u32(cy_addr, 0);

    push_arg(0, himl);
    push_arg(1, cx_addr);
    push_arg(2, cy_addr);
    ctl_mgr.impl_ImageList_GetIconSize(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
}

// =============================================================================
// SHLWAPI — PathFindExtensionW (finds extension in path)
// =============================================================================

TEST_F(SmallDllsCpuzTest, Shlwapi_IsOSOrdinal437MatchesWorkstationModel) {
    DllExports exports = create_shlwapi_thunks();
    auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) { return candidate.ordinal == 437; });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->name, "IsOS");

    push_arg(0, 1); // OS_NT
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);

    push_arg(0, 0); // OS_WINDOWS
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);

    push_arg(0, 20); // OS_PROFESSIONAL
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);

    push_arg(0, 29); // OS_ANYSERVER
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
}

TEST_F(SmallDllsCpuzTest, Shlwapi_PathFindExtensionW) {
    DllExports exports = create_shlwapi_thunks();
    auto* entry = find_export(exports, "PathFindExtensionW");
    ASSERT_NE(entry, nullptr);

    // Write "C:\\test\\file.exe\0" as UTF-16LE
    constexpr GuestAddr path_addr = DATA_BASE + 0x700;
    const char16_t path[] = u"C:\\test\\file.exe";
    for (usize i = 0; i <= 16; ++i) {
        (void)memory.write_u16(path_addr + i * 2, static_cast<u16>(path[i]));
    }

    push_arg(0, path_addr);
    entry->handler(marshaler);

    GuestAddr result = static_cast<GuestAddr>(cpu.gpr.rax);
    // Should point to the '.' before "exe"
    EXPECT_NE(result, 0u);
    auto dot = memory.read_u16(result);
    ASSERT_TRUE(dot.is_ok());
    EXPECT_EQ(dot.value(), static_cast<u16>('.'));
}

// =============================================================================
// SHLWAPI — PathIsUNCW (FALSE for normal path)
// =============================================================================

TEST_F(SmallDllsCpuzTest, Shlwapi_PathIsUNCW_NormalPath) {
    DllExports exports = create_shlwapi_thunks();
    auto* entry = find_export(exports, "PathIsUNCW");
    ASSERT_NE(entry, nullptr);

    // Write "C:\\Windows\0" as UTF-16LE
    constexpr GuestAddr path_addr = DATA_BASE + 0x800;
    const char16_t path[] = u"C:\\Windows";
    for (usize i = 0; i <= 10; ++i) {
        (void)memory.write_u16(path_addr + i * 2, static_cast<u16>(path[i]));
    }

    push_arg(0, path_addr);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u); // FALSE
}

// =============================================================================
// SHLWAPI — PathIsUNCW (TRUE for \\server\share)
// =============================================================================

TEST_F(SmallDllsCpuzTest, Shlwapi_PathIsUNCW_UNCPath) {
    DllExports exports = create_shlwapi_thunks();
    auto* entry = find_export(exports, "PathIsUNCW");
    ASSERT_NE(entry, nullptr);

    // Write "\\\\server\\share\0" as UTF-16LE
    constexpr GuestAddr path_addr = DATA_BASE + 0x900;
    const char16_t path[] = u"\\\\server\\share";
    for (usize i = 0; i <= 14; ++i) {
        (void)memory.write_u16(path_addr + i * 2, static_cast<u16>(path[i]));
    }

    push_arg(0, path_addr);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u); // TRUE
}

// =============================================================================
// TAPI32 — no telephony hardware; lineInitialize fails honestly
// =============================================================================

TEST_F(SmallDllsCpuzTest, Tapi32_LineInitializeNullPointers) {
    DllExports exports = create_tapi32_thunks();
    auto* entry = find_export(exports, "lineInitialize");
    ASSERT_NE(entry, nullptr);

    push_arg(0, 0);
    push_arg(1, 0);
    push_arg(2, 0);
    push_arg(3, 0);
    push_arg(4, 0);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80000035u); // LINEERR_INVALPOINTER
}

TEST_F(SmallDllsCpuzTest, Tapi32_LineInitializeReportsNoDriver) {
    DllExports exports = create_tapi32_thunks();
    auto* init = find_export(exports, "lineInitialize");
    auto* shutdown = find_export(exports, "lineShutdown");
    ASSERT_NE(init, nullptr);
    ASSERT_NE(shutdown, nullptr);

    constexpr GuestAddr app_addr = DATA_BASE + 0xA00;
    constexpr GuestAddr devs_addr = DATA_BASE + 0xA10;
    ASSERT_TRUE(memory.write_u32(app_addr, 0xDEADBEEFu).is_ok());
    ASSERT_TRUE(memory.write_u32(devs_addr, 7u).is_ok());

    push_arg(0, static_cast<u32>(app_addr));
    push_arg(1, 0x10000);
    push_arg(2, 0);
    push_arg(3, 0);
    push_arg(4, static_cast<u32>(devs_addr));
    init->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80000043u); // LINEERR_NODRIVER
    EXPECT_EQ(memory.read_u32(app_addr).value(), 0xDEADBEEFu);
    EXPECT_EQ(memory.read_u32(devs_addr).value(), 7u);

    push_arg(0, 0x1000);
    shutdown->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80000050u); // LINEERR_UNINITIALIZED
}

TEST_F(SmallDllsCpuzTest, ImmUiMessageClassifiesWithoutInventingAnImeWindow) {
    Imm32Manager manager;
    auto exports = create_imm32_thunks(manager);
    for (const char* name : {"ImmIsUIMessageA", "ImmIsUIMessageW"}) {
        auto* entry = find_export(exports, name);
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->stdcall_bytes, 16u);
        push_arg(0, 0);
        for (u32 message : {0x10du, 0x10fu, 0x281u, 0x283u, 0x285u}) {
            push_arg(1, message);
            entry->handler(marshaler);
            EXPECT_EQ(cpu.gpr.rax, 1u);
        }
        push_arg(1, 0x100); // WM_KEYDOWN is application input, not IME UI.
        entry->handler(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        push_arg(0, 0x1234);
        push_arg(1, 0x283);
        entry->handler(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 0u);
    }
}

// ImmGetCompositionFontW reports the context's font -- Windows 11's default
// for a fresh en-US context is 16/7, weight 700, ANSI_CHARSET,
// VARIABLE_PITCH|FF_SWISS, "System" -- and ImmSetCompositionFontW replaces it
// (tools/oracle_probe/s2msg_input_probe.expected immgetcompositionfontw).
// The ANSI forms fail loudly: Steam imports only the W pair.
TEST_F(SmallDllsCpuzTest, ImmCompositionFontRoundTripsPerContext) {
    Imm32Manager manager;
    auto exports = create_imm32_thunks(manager);
    for (const char* name : {"ImmSetCompositionFontA", "ImmSetCompositionFontW",
                             "ImmGetCompositionFontA", "ImmGetCompositionFontW"}) {
        auto* entry = find_export(exports, name);
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->stdcall_bytes, 8u);
    }
    auto* get_w = find_export(exports, "ImmGetCompositionFontW");
    auto* set_w = find_export(exports, "ImmSetCompositionFontW");
    push_arg(0, 0xe11c0001);
    push_arg(1, DATA_BASE);
    get_w->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(DATA_BASE).value(), 16u);        // lfHeight
    EXPECT_EQ(memory.read_u32(DATA_BASE + 4).value(), 7u);     // lfWidth
    EXPECT_EQ(memory.read_u32(DATA_BASE + 16).value(), 700u);  // lfWeight
    EXPECT_EQ(memory.read_u8(DATA_BASE + 27).value(), 34u);    // pitch/family
    EXPECT_EQ(memory.read_u16(DATA_BASE + 28).value(), u'S');
    ASSERT_TRUE(memory.write_u32(DATA_BASE, 22).is_ok());
    push_arg(0, 0xe11c0001);
    push_arg(1, DATA_BASE);
    set_w->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    ASSERT_TRUE(memory.write_u32(DATA_BASE, 0).is_ok());
    push_arg(0, 0xe11c0001);
    push_arg(1, DATA_BASE);
    get_w->handler(marshaler);
    EXPECT_EQ(memory.read_u32(DATA_BASE).value(), 22u);
    auto* get_a = find_export(exports, "ImmGetCompositionFontA");
    push_arg(0, 0xe11c0001);
    push_arg(1, DATA_BASE);
    get_a->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

// Values measured by ip_compartment_contract_probe on Windows in both ABIs.
TEST_F(SmallDllsCpuzTest, Iphlpapi_ThreadCompartmentsDefaultResetAndIsolation) {
    IphlpapiManager mgr;
    auto exports = create_iphlpapi_thunks(mgr);
    ASSERT_NE(find_export(exports, "GetCurrentThreadCompartmentId"), nullptr);
    ASSERT_NE(find_export(exports, "SetCurrentThreadCompartmentId"), nullptr);
    const auto saved_tid = ThreadManager::get_current_thread_id_static();
    for (const bool wide : {false, true}) {
        if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        for (const u32 tid : {101u, 102u, 101u}) {
            ThreadManager::set_current_thread_id(tid);
            set_last_error(0xdead);
            mgr.impl_GetCurrentThreadCompartmentId(marshaler);
            EXPECT_EQ(cpu.gpr.rax, 1u);
            EXPECT_EQ(get_last_error(), 0u);
            for (const u32 id : {0u, 1u, 2u, 0xffffffffu, 0xfffffffeu}) {
                if (wide) cpu.gpr.rcx = 0xdeadbeef00000000ull | id;
                else push_arg(0, id);
                set_last_error(0xdead);
                mgr.impl_SetCurrentThreadCompartmentId(marshaler);
                EXPECT_EQ(cpu.gpr.rax, id <= 1 ? 0u : 1168u);
                EXPECT_EQ(get_last_error(), 0u);
                mgr.impl_GetCurrentThreadCompartmentId(marshaler);
                EXPECT_EQ(cpu.gpr.rax, 1u);
            }
        }
    }
    ThreadManager::set_current_thread_id(saved_tid);
}
