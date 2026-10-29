/// @file test_kernel32_cpuz_process.cpp
/// @brief Unit tests for KERNEL32 process memory, app-model, exit-code, process-times and CreateProcess contracts

#include "kernel32_cpuz_fixture.hpp"
#include <chrono>
#include <winapi/kernel32/console/console.hpp>
#include <utility>
#include <winapi/kernel32/process/process.hpp>
#include <algorithm>
#include <unistd.h>

TEST_F(Kernel32CpuzTest, K32GetProcessMemoryInfoReportsRealWorkingSet) {
    // Zeroing PROCESS_MEMORY_COUNTERS and returning TRUE is indistinguishable
    // from a process using no memory. Unreal takes UsedPhysical straight from
    // WorkingSetSize, so its GC concluded there was nothing to collect and the
    // Ark dedicated server allocated without bound until the host ran out of
    // memory. A zero working set for a live process is never truthful.
    constexpr GuestAddr out = DATA_BASE + 0x600;
    cpu.init_long_mode();
    ASSERT_TRUE(cpu.is_64bit());

    // x64: DWORD cb, DWORD PageFaultCount, then 8 x SIZE_T -> 72 bytes.
    cpu.gpr.rcx = 0xFFFFFFFFFFFFFFFFull;
    cpu.gpr.rdx = out;
    cpu.gpr.r8 = 72;
    mgr.impl_K32GetProcessMemoryInfo(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    EXPECT_EQ(memory.read_u32(out + 0).value_or(0), 72u);        // cb
    const u64 peak = memory.read_u64(out + 8).value_or(0);
    const u64 working = memory.read_u64(out + 16).value_or(0);
    const u64 pagefile = memory.read_u64(out + 56).value_or(0);
    EXPECT_GT(working, 0u) << "a live process has a non-zero working set";
    EXPECT_GE(peak, working) << "peak cannot be below current";
    EXPECT_GT(pagefile, 0u) << "PagefileUsage feeds UsedVirtual";

    // A buffer too small for the struct is a parameter error, not a partial
    // write that the caller would read as real numbers.
    ASSERT_TRUE(memory.write_u32(out, 0xCCCCCCCCu).is_ok());
    cpu.gpr.rdx = out;
    cpu.gpr.r8 = 8;
    mgr.impl_K32GetProcessMemoryInfo(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(out).value_or(0), 0xCCCCCCCCu) << "untouched";

    // A NULL out pointer must fail rather than report success.
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = 72;
    mgr.impl_K32GetProcessMemoryInfo(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

// =============================================================================
// GetProcessTimes
// =============================================================================

// Falling through to the generic missing-export stub is an x86 ABI bug, not
// just a wrong return value: that stub cleans up zero bytes because it cannot
// know a missing function's stdcall argument size. For this two-argument
// stdcall it leaked 8 bytes of the caller's stack on every call. Spotify's
// browser process invokes it during startup.
TEST_F(Kernel32CpuzTest, RegisterApplicationRestart_DeclaresStdcallArity) {
    auto exports = create_kernel32_ext_thunks(mgr);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) {
            return e.name == "RegisterApplicationRestart";
        });
    ASSERT_NE(entry, exports.entries.end())
        << "must be registered so it does not hit the generic stub";
    EXPECT_EQ(static_cast<u32>(entry->stdcall_bytes), 8u);

    // Windows validates flags; bits outside the documented set are E_INVALIDARG.
    constexpr u32 E_INVALIDARG_HR = 0x80070057;
    ASSERT_TRUE(memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), 0).is_ok());
    ASSERT_TRUE(memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8),
                                 0x80000000u).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), E_INVALIDARG_HR);

    // Vitreo has no restart manager, so a valid request must not report S_OK.
    ASSERT_TRUE(memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), 0).is_ok());
    entry->handler(marshaler);
    EXPECT_NE(static_cast<u32>(cpu.gpr.rax), 0u)
        << "claiming success would promise a restart Vitreo never performs";
}

// CompareObjectHandles on two handles that do not name one object: FALSE with
// ERROR_NOT_SAME_OBJECT = 1656 (winerror.h; measured on Windows 11 x64 for
// two events, an event vs a mutex, and GetCurrentThread() vs an event). The
// constant used to be 657 (ERROR_PWD_TOO_LONG).
TEST_F(Kernel32CpuzTest, CompareObjectHandlesMismatchIsErrorNotSameObject1656) {
    auto exports = create_kernel32_ext_thunks(mgr);
    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                           [](const ExportEntry& e) {
                               return e.name == "CompareObjectHandles";
                           });
    ASSERT_NE(it, exports.entries.end());
    EXPECT_EQ(error_codes::ERROR_NOT_SAME_OBJECT, 1656u);
    const Handle e1 = sync.create_event(true, false, "");
    const Handle e2 = sync.create_event(true, false, "");
    const Handle mx = sync.create_mutex(false, "");
    ASSERT_NE(e1, NULL_HANDLE);
    ASSERT_NE(e2, NULL_HANDLE);
    ASSERT_NE(mx, NULL_HANDLE);
    const auto compare = [&](Handle a, Handle b) {
        ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, a).is_ok());
        ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, b).is_ok());
        set_last_error(0xDEADu);
        it->handler(marshaler);
    };
    compare(e1, e2);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), 1656u);
    compare(e1, mx);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), 1656u);
    compare(e1, e1);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
}

TEST_F(Kernel32CpuzTest,
       AppModelDiscoveryReportsUnpackagedProcessAndEmptyRepository) {
    constexpr GuestAddr length = DATA_BASE + 0x400;
    constexpr GuestAddr count = DATA_BASE + 0x404;
    constexpr GuestAddr output = DATA_BASE + 0x500;
    constexpr GuestAddr family = DATA_BASE + 0x600;
    constexpr u32 APPMODEL_ERROR_NO_PACKAGE = 15700;

    auto exports = create_kernel32_ext_thunks(mgr);
    const auto find_export = [&](std::string_view name) -> ExportEntry* {
        auto it = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [&](const ExportEntry& entry) { return entry.name == name; });
        return it == exports.entries.end() ? nullptr : &*it;
    };

    auto* current = find_export("GetCurrentPackageFullName");
    ASSERT_NE(current, nullptr);
    EXPECT_EQ(current->stdcall_bytes, 8u);
    ASSERT_TRUE(memory.write_u32(length, 64).is_ok());
    ASSERT_TRUE(memory.write_u16(output, u'X').is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, length).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, output).is_ok());
    current->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), APPMODEL_ERROR_NO_PACKAGE);
    EXPECT_EQ(memory.read_u32(length).value_or(0), 64u);
    EXPECT_EQ(memory.read_u16(output).value_or(0), u'X');

    auto* by_path = find_export("GetPackagePathByFullName");
    ASSERT_NE(by_path, nullptr);
    EXPECT_EQ(by_path->stdcall_bytes, 12u);
    write_guest_wide_string(family, "Contoso.App_1.0.0.0_x64__abcdefgh");
    ASSERT_TRUE(memory.write_u32(length, 260).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, family).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, length).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 12, output).is_ok());
    by_path->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), error_codes::ERROR_NOT_FOUND);

    auto* compare_handles = find_export("CompareObjectHandles");
    ASSERT_NE(compare_handles, nullptr);
    EXPECT_EQ(compare_handles->stdcall_bytes, 8u);

    auto* by_family = find_export("GetPackagesByPackageFamily");
    ASSERT_NE(by_family, nullptr);
    EXPECT_EQ(by_family->stdcall_bytes, 20u);
    write_guest_wide_string(family, "SpotifyAB.SpotifyMusic_zpdnekdrzrea0");
    ASSERT_TRUE(memory.write_u32(count, 8).is_ok());
    ASSERT_TRUE(memory.write_u32(length, 260).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, family).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, count).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 16, length).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 20, 0).is_ok());
    by_family->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), error_codes::ERROR_SUCCESS);
    EXPECT_EQ(memory.read_u32(count).value_or(1), 0u);
    EXPECT_EQ(memory.read_u32(length).value_or(1), 0u);
}

TEST_F(Kernel32CpuzTest,
       AppPolicyThreadInitializationValidatesTokenAndReturnsNone) {
    constexpr GuestAddr policy = DATA_BASE + 0x700;
    auto exports = create_kernel32_ext_thunks(mgr);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "AppPolicyGetThreadInitializationType";
        });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 8u);

    const Handle token = handles.allocate(HandleType::Token, nullptr);
    ASSERT_NE(token, NULL_HANDLE);
    ASSERT_TRUE(memory.write_u32(policy, 99).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, token).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, policy).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), error_codes::ERROR_SUCCESS);
    EXPECT_EQ(memory.read_u32(policy).value_or(99), 0u);

    ASSERT_TRUE(memory.write_u32(policy, 77).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, 0x1234u).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), error_codes::ERROR_INVALID_HANDLE);
    EXPECT_EQ(memory.read_u32(policy).value_or(0), 77u);
}

TEST_F(Kernel32CpuzTest, PackageFamilyNameConversionsHonorSizeQueryContract) {
    constexpr GuestAddr package_id = DATA_BASE + 0x800;
    constexpr GuestAddr name = DATA_BASE + 0x900;
    constexpr GuestAddr publisher = DATA_BASE + 0x980;
    constexpr GuestAddr family = DATA_BASE + 0xA00;
    constexpr GuestAddr length = DATA_BASE + 0xB00;
    constexpr GuestAddr publisher_length = DATA_BASE + 0xB04;
    constexpr GuestAddr decoded_name = DATA_BASE + 0xC00;
    constexpr GuestAddr decoded_publisher = DATA_BASE + 0xD00;
    constexpr const char* kName = "SpotifyAB.SpotifyMusic";
    constexpr const char* kPublisher = "zpdnekdrzrea0";
    constexpr const char* kFamily =
        "SpotifyAB.SpotifyMusic_zpdnekdrzrea0";

    write_guest_wide_string(name, kName);
    write_guest_wide_string(publisher, kPublisher);
    ASSERT_TRUE(memory.write_u32(package_id + 16, name).is_ok());
    ASSERT_TRUE(memory.write_u32(package_id + 28, publisher).is_ok());

    auto exports = create_kernel32_ext_thunks(mgr);
    const auto find_export = [&](std::string_view export_name) -> ExportEntry* {
        auto it = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [&](const ExportEntry& entry) {
                return entry.name == export_name;
            });
        return it == exports.entries.end() ? nullptr : &*it;
    };
    auto* from_id = find_export("PackageFamilyNameFromId");
    ASSERT_NE(from_id, nullptr);
    EXPECT_EQ(from_id->stdcall_bytes, 12u);

    ASSERT_TRUE(memory.write_u32(length, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, package_id).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, length).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 12, 0).is_ok());
    from_id->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax),
              error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(memory.read_u32(length).value_or(0),
              std::strlen(kFamily) + 1);

    ASSERT_TRUE(memory.write_u32(cpu.sp() + 12, family).is_ok());
    from_id->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), error_codes::ERROR_SUCCESS);
    EXPECT_EQ(read_guest_wide_string(family), kFamily);

    auto* split = find_export("PackageNameAndPublisherIdFromFamilyName");
    ASSERT_NE(split, nullptr);
    EXPECT_EQ(split->stdcall_bytes, 20u);
    ASSERT_TRUE(memory.write_u32(length, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(publisher_length, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, family).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, length).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 16,
                                 publisher_length).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 20, 0).is_ok());
    split->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax),
              error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(memory.read_u32(length).value_or(0), std::strlen(kName) + 1);
    EXPECT_EQ(memory.read_u32(publisher_length).value_or(0),
              std::strlen(kPublisher) + 1);

    ASSERT_TRUE(memory.write_u32(cpu.sp() + 12, decoded_name).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 20, decoded_publisher).is_ok());
    split->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), error_codes::ERROR_SUCCESS);
    EXPECT_EQ(read_guest_wide_string(decoded_name), kName);
    EXPECT_EQ(read_guest_wide_string(decoded_publisher), kPublisher);
}

TEST_F(Kernel32CpuzTest, GetProcessTimes_ReturnsTRUE_WritesNonZero) {
    constexpr GuestAddr creation = DATA_BASE + 0x800;
    constexpr GuestAddr exit_t   = DATA_BASE + 0x808;
    constexpr GuestAddr kernel_t = DATA_BASE + 0x810;
    constexpr GuestAddr user_t   = DATA_BASE + 0x818;

    // Zero-initialize
    for (GuestAddr a : {creation, exit_t, kernel_t, user_t}) {
        (void)memory.write_u32(a, 0);
        (void)memory.write_u32(a + 4, 0);
    }

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4),
                           CURRENT_PROCESS_PSEUDO);                       // hProcess
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8),  creation);  // lpCreationTime
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), exit_t);    // lpExitTime
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 16), kernel_t);  // lpKernelTime
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 20), user_t);    // lpUserTime
    mgr.impl_GetProcessTimes(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u);

    // Creation time should be non-zero (current time)
    auto lo = memory.read_u32(creation);
    auto hi = memory.read_u32(creation + 4);
    ASSERT_TRUE(lo.is_ok() && hi.is_ok());
    u64 ft = static_cast<u64>(lo.value()) | (static_cast<u64>(hi.value()) << 32);
    EXPECT_NE(ft, 0u);
}

// GetExitCodeProcess contract, measured on Windows 11
// (tools/oracle_probe/process_contract_probe.expected).
TEST_F(Kernel32CpuzTest, GetExitCodeProcessMatchesOracleForEveryHandleKind) {
    constexpr GuestAddr code_ptr = DATA_BASE + 0x900;
    const auto call = [&](Handle h) {
        (void)memory.write_u32(code_ptr, 0x77);
        set_last_error(0xDEAD);
        (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), h);
        (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8), code_ptr);
        mgr.impl_GetExitCodeProcess(marshaler);
        return std::make_pair(static_cast<u32>(cpu.gpr.rax),
                              memory.read_u32(code_ptr).value());
    };
    // GetCurrentProcess(): TRUE, STILL_ACTIVE, last error untouched.
    EXPECT_EQ(call(CURRENT_PROCESS_PSEUDO), std::make_pair(1u, 259u));
    EXPECT_EQ(get_last_error(), 0xDEADu);
    // Invalid handle and GetCurrentThread(): FALSE, ERROR_INVALID_HANDLE, untouched.
    EXPECT_EQ(call(0x1234), std::make_pair(0u, 0x77u));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    EXPECT_EQ(call(0xFFFFFFFEu), std::make_pair(0u, 0x77u));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    const Handle mutex = sync.create_mutex(false, "");
    EXPECT_EQ(call(mutex), std::make_pair(0u, 0x77u));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    // Event: TRUE, code 0, ERROR_INVALID_HANDLE (Windows quirk).
    const Handle event = sync.create_event(false, false, "");
    EXPECT_EQ(call(event), std::make_pair(1u, 0u));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    // A live tracked process: STILL_ACTIVE.
    const Handle self = sync.create_process_handle(static_cast<int>(::getpid()));
    EXPECT_EQ(call(self), std::make_pair(1u, 259u));
    EXPECT_EQ(get_last_error(), 0xDEADu);
    // SYNCHRONIZE-only process handle: ERROR_ACCESS_DENIED.
    const Handle limited = handles.allocate(
        HandleType::Process, handles.get(self, HandleType::Process), 0,
        0x00100000u);
    EXPECT_EQ(call(limited), std::make_pair(0u, 0x77u));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
}

// CreateProcessW/A carry every UTF-16 unit (and CP_ACP byte) to the child;
// narrowing each unit to its low byte corrupted non-ASCII arguments
// (process_contract_probe.expected: the child sees x\u00e9\u20ac\U0001F600).
TEST_F(Kernel32CpuzTest, CreateProcessPreservesNonAsciiCommandLine) {
    std::string seen;
    register_builtin_process("prc_unicode_probe.exe", [&](const std::string& cl) {
        seen = cl;
        return 0u;
    });
    constexpr GuestAddr cmd = DATA_BASE + 0x100;
    constexpr GuestAddr pi = DATA_BASE + 0x400;
    const std::u16string text = u"prc_unicode_probe.exe x\u00e9\u20ac\U0001F600";
    for (usize i = 0; i <= text.size(); ++i) {
        (void)memory.write_u16(cmd + i * 2, i < text.size() ? text[i] : 0);
    }
    const u32 args[10] = {0, cmd, 0, 0, 0, 0, 0, 0, 0, pi};
    for (u32 i = 0; i < 10; ++i) {
        (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4 + i * 4), args[i]);
    }
    mgr.impl_CreateProcessW(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(seen, "prc_unicode_probe.exe x\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");

    // CreateProcessA: 0xE9 in CP1252 is U+00E9, 0x80 is U+20AC.
    const char ansi[] = "prc_unicode_probe.exe \xE9\x80";
    ASSERT_TRUE(memory.write_bytes(cmd, reinterpret_cast<const Byte*>(ansi), sizeof ansi).is_ok());
    mgr.impl_CreateProcessA(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(seen, "prc_unicode_probe.exe \xC3\xA9\xE2\x82\xAC");
}

// ReadConsoleW: real input, Windows' failure codes
// (tools/oracle_probe/process_contract_probe.expected).
TEST_F(Kernel32CpuzTest, ReadConsoleWReadsTypedTextAndRejectsNonConsoleHandles) {
    ConsoleManager console(handles);
    std::string host_input = "hi\n";
    console.set_input_callback([&] { return std::exchange(host_input, std::string{}); });
    mgr.set_console_manager(&console);
    constexpr GuestAddr buf = DATA_BASE + 0xA00;
    constexpr GuestAddr count = DATA_BASE + 0xA80;
    constexpr GuestAddr control = DATA_BASE + 0xA90;
    const auto call = [&](Handle h, u32 n, GuestAddr ctl) {
        (void)memory.write_u32(count, 0x77);
        set_last_error(0xDEAD);
        const u32 args[5] = {h, buf, n, count, static_cast<u32>(ctl)};
        for (u32 i = 0; i < 5; ++i) {
            (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4 + i * 4), args[i]);
        }
        mgr.impl_ReadConsoleW(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };
    const Handle pipe = handles.allocate(HandleType::Pipe, nullptr);
    EXPECT_EQ(call(pipe, 8, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FUNCTION);
    EXPECT_EQ(memory.read_u32(count).value(), 0x77u);
    EXPECT_EQ(call(0x1234, 8, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);

    // nInitialChars = 1: buffer[0] is preserved and counted.
    (void)memory.write_u16(buf, u'>');
    (void)memory.write_u32(control + 0, 16);
    (void)memory.write_u32(control + 4, 1);
    (void)memory.write_u32(control + 8, 0);
    EXPECT_EQ(call(console.console_input_handle(), 16, control), 1u);
    EXPECT_EQ(memory.read_u32(count).value(), 5u);
    EXPECT_EQ(read_guest_wide_string(buf), std::string(">hi\r\n"));
    mgr.set_console_manager(nullptr);
}

// CEF launches a sandboxed child and then records when it started. Chromium
// CHECKs the result and executes IMMEDIATE_CRASH (`int3; ud2`) on FALSE, so
// querying a *tracked child* — not just the current process — has to succeed.
TEST_F(Kernel32CpuzTest, GetProcessTimes_TrackedChildHandleSucceeds) {
    constexpr GuestAddr creation = DATA_BASE + 0x840;
    constexpr GuestAddr exit_t   = DATA_BASE + 0x848;
    constexpr GuestAddr kernel_t = DATA_BASE + 0x850;
    constexpr GuestAddr user_t   = DATA_BASE + 0x858;

    for (GuestAddr a : {creation, exit_t, kernel_t, user_t}) {
        (void)memory.write_u32(a, 0xFFFFFFFFu);
        (void)memory.write_u32(a + 4, 0xFFFFFFFFu);
    }

    // A real tracked process handle. Using this process's own pid keeps the
    // test hermetic while still exercising the non-pseudo-handle path.
    const Handle child = sync.create_process_handle(static_cast<int>(::getpid()));
    ASSERT_NE(child, NULL_HANDLE);

    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 4), child);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 8),  creation);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 12), exit_t);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 16), kernel_t);
    (void)memory.write_u32(static_cast<u32>(cpu.gpr.rsp + 20), user_t);
    mgr.impl_GetProcessTimes(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 1u) << "a tracked process handle must be queryable";

    auto read_ft = [&](GuestAddr a) -> u64 {
        auto lo = memory.read_u32(a);
        auto hi = memory.read_u32(a + 4);
        EXPECT_TRUE(lo.is_ok() && hi.is_ok());
        return static_cast<u64>(lo.value()) |
               (static_cast<u64>(hi.value()) << 32);
    };

    // A plausible creation time: after the Windows epoch and not in the future.
    const u64 created = read_ft(creation);
    EXPECT_GT(created, 116444736000000000ULL);
    const u64 now_ft = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count())
        * 10ULL + 116444736000000000ULL;
    EXPECT_LE(created, now_ft);

    // Still running, so Windows reports a zero exit time.
    EXPECT_EQ(read_ft(exit_t), 0u);

    // CPU totals must be written (they replaced the 0xFFFF.. poison).
    EXPECT_NE(read_ft(kernel_t), 0xFFFFFFFFFFFFFFFFULL);
    EXPECT_NE(read_ft(user_t), 0xFFFFFFFFFFFFFFFFULL);
}

TEST_F(Kernel32CpuzTest, SetSystemPowerStateRequiresShutdownPrivilege) {
    auto exports = create_kernel32_ext_thunks(mgr);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "SetSystemPowerState"; });
    ASSERT_NE(entry, exports.entries.end());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, 1).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_PRIVILEGE_NOT_HELD);
}

TEST_F(Kernel32CpuzTest, GetNumaHighestNodeNumberReportsSingleNode) {
    auto exports = create_kernel32_ext_thunks(mgr);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "GetNumaHighestNodeNumber"; });
    ASSERT_NE(entry, exports.entries.end());

    constexpr GuestAddr out = DATA_BASE + 0x100;
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, 0).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, out).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u32(out).value(), 0u);
}
