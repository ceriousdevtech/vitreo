/// @file test_crt_audit_contracts.cpp
/// @brief CRT contracts from .docs/audits/2026-09-23-peak-api-contracts/09_crt.md.
///
/// Expected values come from Windows runs of ucrtbase.dll, or from the
/// Learn page cited per test.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include "capsule/vfs/vfs.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/text/crt_string.hpp"
#include "winapi/msvcrt/runtime/crt_init.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "winapi/msvcrt/system/crt_environment.hpp"
#include "winapi/msvcrt/crt_ordinals.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/process/process.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

class CrtAuditTest : public ::testing::Test {
protected:
    static constexpr GuestAddr kBase = 0x100000;
    static constexpr usize kSize = 0x100000;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kBase, kSize, MemoryProtection::ReadWrite).is_ok());
        cpu.init_long_mode();
        cpu.gpr.rsp = kBase + kSize - 0x1000;  // zeroed stack slots for args 4+
        marshaler.attach_cpu(cpu);
        add(create_crt_string_thunks(string_manager, memory_manager, vfs));
        add(create_crt_memory_thunks(memory_manager));
    }

    void add(DllExports exports) {
        // Registration order: a later entry of the same name wins, exactly
        // as ThunkRegistry::register_dll indexes by name.
        for (auto& e : exports.entries) table.entries.push_back(std::move(e));
    }

    ExportEntry* find(const char* name) {
        ExportEntry* found = nullptr;
        for (auto& e : table.entries) if (e.name == name) found = &e;
        return found;
    }

    /// Call an x64 export with integer args in RCX/RDX/R8/R9.
    u64 call(const char* name, std::initializer_list<u64> args) {
        ExportEntry* e = find(name);
        EXPECT_NE(e, nullptr) << name;
        if (!e) return 0;
        const u64* a = args.begin();
        const usize n = args.size();
        cpu.gpr.rcx = n > 0 ? a[0] : 0;
        cpu.gpr.rdx = n > 1 ? a[1] : 0;
        cpu.gpr.r8 = n > 2 ? a[2] : 0;
        cpu.gpr.r9 = n > 3 ? a[3] : 0;
        cpu.gpr.rax = 0xDEADBEEFDEADBEEFull;
        e->handler(marshaler);
        return cpu.gpr.rax;
    }

    GuestAddr put_str(GuestAddr at, const std::string& s) {
        EXPECT_TRUE(memory.write_bytes(at, reinterpret_cast<const Byte*>(s.data()), s.size()).is_ok());
        EXPECT_TRUE(memory.write_u8(at + s.size(), 0).is_ok());
        return at;
    }
    GuestAddr put_wstr(GuestAddr at, const std::u16string& s) {
        for (usize i = 0; i < s.size(); ++i) EXPECT_TRUE(memory.write_u16(at + i * 2, s[i]).is_ok());
        EXPECT_TRUE(memory.write_u16(at + s.size() * 2, 0).is_ok());
        return at;
    }
    u32 errno_value() { return memory_manager.get_errno(); }

    GuestMemory memory;
    capsule::Vfs vfs;
    CrtMemoryManager memory_manager{memory, 0x7A000000};
    CrtStringManager string_manager;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    DllExports table;
};

// 09_crt.md HIGH 9/10 -- crt_ucrt_str_probe.expected: strlen_big n=100000,
// wcslen_big n=100000 (the old walk stopped at 65536).
TEST_F(CrtAuditTest, StrlenAndWcslenAreUnbounded) {
    const std::string big(100000, 'x');
    const GuestAddr s = put_str(kBase, big);
    EXPECT_EQ(call("strlen", {s}), 100000u);
    const std::u16string wbig(100000, u'x');
    const GuestAddr w = put_wstr(kBase + 0x20000, wbig);
    EXPECT_EQ(call("wcslen", {w}), 100000u);
    // A wide string starting at an odd address straddling a page boundary.
    const GuestAddr odd = kBase + 0x70000 - 3;
    put_wstr(odd, u"abcd");
    EXPECT_EQ(call("wcslen", {odd}), 4u);
}

} // namespace

namespace {

// 09_crt.md HIGH 11/12, MEDIUM _wtoi/wcstod, LOW atoi/_wtoi64 --
// crt_ucrt_str_probe.expected (strtol*, wcstoul*, wtoi*, atoi*, atoll*).
TEST_F(CrtAuditTest, StrtolFamilyMatchesUcrt) {
    const GuestAddr end = kBase + 0x80000;
    const u64 fill = 0xAAAAAAAAAAAAAAAAull;
    const GuestAddr s = put_str(kBase, "123xyz");
    ASSERT_TRUE(memory.write_u64(end, fill).is_ok());
    EXPECT_EQ(static_cast<u32>(call("strtol", {s, end, 10})), 123u);
    EXPECT_EQ(memory.read_u64(end).value(), s + 3);  // all 8 bytes written

    const GuestAddr ovf = put_str(kBase + 0x100, "99999999999");
    memory_manager.set_errno(0);
    EXPECT_EQ(static_cast<u32>(call("strtol", {ovf, end, 10})), 2147483647u);
    EXPECT_EQ(errno_value(), 34u);
    const GuestAddr neg = put_str(kBase + 0x200, "-1");
    memory_manager.set_errno(0);
    EXPECT_EQ(static_cast<u32>(call("strtoul", {neg, 0, 10})), 4294967295u);
    EXPECT_EQ(errno_value(), 0u);
    const GuestAddr none = put_str(kBase + 0x300, "abc");
    ASSERT_TRUE(memory.write_u64(end, fill).is_ok());
    EXPECT_EQ(call("strtol", {none, end, 10}) & 0xFFFFFFFF, 0u);
    EXPECT_EQ(memory.read_u64(end).value(), none);

    // wcstoul: overflow saturates with ERANGE; non-ASCII units are not
    // folded onto their low byte; fullwidth digits are digits.
    const GuestAddr w1 = put_wstr(kBase + 0x400, u"4294967296");
    memory_manager.set_errno(0);
    EXPECT_EQ(static_cast<u32>(call("wcstoul", {w1, end, 10})), 4294967295u);
    EXPECT_EQ(errno_value(), 34u);
    EXPECT_EQ(memory.read_u64(end).value(), w1 + 20);
    const GuestAddr w2 = put_wstr(kBase + 0x500, u"ı");
    EXPECT_EQ(static_cast<u32>(call("wcstoul", {w2, end, 10})), 0u);
    EXPECT_EQ(memory.read_u64(end).value(), w2);
    const GuestAddr w3 = put_wstr(kBase + 0x600, u"1‰");
    EXPECT_EQ(static_cast<u32>(call("wcstoul", {w3, end, 10})), 1u);
    const GuestAddr w4 = put_wstr(kBase + 0x700, u"１２");
    EXPECT_EQ(static_cast<u32>(call("wcstoul", {w4, end, 10})), 12u);
    std::u16string zeros(300, u'0');
    zeros += u'7';
    const GuestAddr w5 = put_wstr(kBase + 0x1000, zeros);
    EXPECT_EQ(static_cast<u32>(call("wcstoul", {w5, end, 10})), 7u);
    EXPECT_EQ(memory.read_u64(end).value(), w5 + 301 * 2);

    const GuestAddr w6 = put_wstr(kBase + 0x2000, u"3000000000");
    memory_manager.set_errno(0);
    EXPECT_EQ(static_cast<u32>(call("_wtoi", {w6})), 2147483647u);
    EXPECT_EQ(errno_value(), 34u);
    const GuestAddr a1 = put_str(kBase + 0x2100, "  -9223372036854775809");
    memory_manager.set_errno(0);
    EXPECT_EQ(call("atoll", {a1}), 0x8000000000000000ull);
    EXPECT_EQ(errno_value(), 34u);
}

} // namespace

namespace {

// 09_crt.md HIGH 2 + MEDIUM _beginthreadex error path -- crt_ucrt_rt_probe:
// bte_suspended (routine runs once, after ResumeThread), bte_null_start
// h=0x0 errno=22 ip=1, bt_null_start h=all-ones errno=22.
TEST_F(CrtAuditTest, BeginthreadexMarksLaunchedAndFailsWithZero) {
    ProcessManager process_mgr;
    HandleTable handles;
    ThreadManager thread_mgr(handles);
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory));
    int runs = 0;
    thread_mgr.set_run_callback(
        [&runs](u32, GuestAddr, GuestAddr, u64, u64, usize) { ++runs; });
    int handler_calls = 0;
    memory_manager.set_guest_caller(
        [&handler_calls](GuestAddr, const u64*, usize) -> u64 { ++handler_calls; return 0; });
    memory_manager.exchange_invalid_parameter_handler(0x1234);
    add(create_crt_process_thunks(process_mgr, crt_data, thread_mgr, memory_manager));

    const u64 h = call("_beginthreadex", {0, 0, 0x401000, 0});
    ASSERT_NE(h, 0u);
    // A running thread must be marked launched: Suspend+Resume must not
    // start the routine a second time.
    EXPECT_EQ(thread_mgr.suspend_thread(static_cast<Handle>(h)), 0u);
    EXPECT_EQ(thread_mgr.resume_thread(static_cast<Handle>(h)), 1u);
    EXPECT_LE(runs, 1);

    memory_manager.set_errno(0);
    EXPECT_EQ(call("_beginthreadex", {0, 0, 0, 0}), 0u);
    EXPECT_EQ(errno_value(), 22u);
    EXPECT_EQ(handler_calls, 1);
    EXPECT_EQ(call("_beginthread", {0, 0, 0}), ~u64{0});
    EXPECT_EQ(handler_calls, 2);
}

} // namespace

namespace {

// 09_crt.md HIGH 4 -- crt_ucrt_rt_probe: exit runs _crt_atexit/atexit
// registrations LIFO then terminates; _cexit runs them and returns.
TEST_F(CrtAuditTest, ExitAndCexitRunAtexitHandlersLifo) {
    ProcessManager process_mgr;
    HandleTable handles;
    ThreadManager thread_mgr(handles);
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory));
    std::vector<GuestAddr> ran;
    memory_manager.set_void_callback([&](GuestAddr f) {
        ran.push_back(f);
        // A registration made while the list runs is run as well.
        if (f == 0x1000) (void)memory_manager.crt_atexit(0x4000);
    });
    add(create_crt_process_thunks(process_mgr, crt_data, thread_mgr, memory_manager));

    EXPECT_EQ(call("_crt_atexit", {0x1000}) & 0xFFFFFFFF, 0u);
    EXPECT_EQ(call("atexit", {0x2000}) & 0xFFFFFFFF, 0u);
    EXPECT_EQ(call("_crt_atexit", {0x3000}) & 0xFFFFFFFF, 0u);
    call("_cexit", {});
    EXPECT_EQ(ran, (std::vector<GuestAddr>{0x3000, 0x2000, 0x1000, 0x4000}));
    EXPECT_FALSE(process_mgr.has_exited());

    ran.clear();
    EXPECT_EQ(call("_crt_atexit", {0x5000}) & 0xFFFFFFFF, 0u);
    call("exit", {5});
    EXPECT_EQ(ran, (std::vector<GuestAddr>{0x5000}));
    EXPECT_TRUE(process_mgr.has_exited());
    EXPECT_EQ(process_mgr.exit_code(), 5u);
}

// 09_crt.md HIGH 5 -- x64 __argv is pointer-width; _configure_narrow_argv
// modes 0/1/2 match crt_ucrt_rt_probe.expected (argv / argv0 / argv2_wild).
// __p___argv must not overwrite a table configure already published.
TEST_F(CrtAuditTest, ConfigureNarrowArgvX64PointerWidthAndModes) {
    EnvironmentManager env("prog.exe a \"b c\"", memory);
    CrtMemoryManager crt_mem(memory);
    CrtEnvironmentManager crt_env(env, memory, crt_mem);
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(
        memory, CrtDataBlock::DEFAULT_BASE,
        "prog.exe a \"b c\" d\\\"e"));
    CrtInitManager init_mgr;
    add(create_crt_init_thunks(init_mgr, crt_data, crt_env));

    // Mode 1: unexpanded, 8-byte slots on x64.
    EXPECT_EQ(call("_configure_narrow_argv", {1}) & 0xFFFFFFFF, 0u);
    EXPECT_EQ(memory.read_u32(crt_data.argc_addr()).value(), 4u);
    const u64 table = memory.read_u64(crt_data.argv_var_addr()).value();
    ASSERT_NE(table, 0u);
    ASSERT_NE(table, crt_data.argv_table_addr());
    // Four arguments (argc above), so the NULL terminator is argv[4]; the
    // fourth is `d"e` (Learn "Parsing C command-line arguments": \" is a
    // literal quote).
    EXPECT_EQ(memory.read_u64(table + 32).value(), 0u);  // terminator
    const u64 a0 = memory.read_u64(table).value();
    const u64 a1 = memory.read_u64(table + 8).value();
    const u64 a2 = memory.read_u64(table + 16).value();
    const u64 a3 = memory.read_u64(table + 24).value();
    EXPECT_EQ(marshaler.read_ansi_string(a0).value(), "prog.exe");
    EXPECT_EQ(marshaler.read_ansi_string(a1).value(), "a");
    EXPECT_EQ(marshaler.read_ansi_string(a2).value(), "b c");
    EXPECT_EQ(marshaler.read_ansi_string(a3).value(), "d\"e");

    // Mode 0: argc 0, argv NULL — and __p___argv must leave that alone.
    EXPECT_EQ(call("_configure_narrow_argv", {0}) & 0xFFFFFFFF, 0u);
    EXPECT_EQ(memory.read_u32(crt_data.argc_addr()).value(), 0u);
    EXPECT_EQ(memory.read_u64(crt_data.argv_var_addr()).value(), 0u);
    EXPECT_EQ(call("__p___argv", {}), crt_data.argv_var_addr());
    EXPECT_EQ(memory.read_u32(crt_data.argc_addr()).value(), 0u);
    EXPECT_EQ(memory.read_u64(crt_data.argv_var_addr()).value(), 0u);
}

TEST(CrtOrdinalsTest, DumpbinOrdinalsStampKnownExports) {
    // Expected ordinals from tools/oracle_probe/crt_ordinal_probe.expected
    // (dumpbin /exports on Windows System32).
    DllExports ucrt;
    ucrt.entries.push_back({"sin", 1, [](Marshaler&) {}, 0});
    ucrt.entries.push_back({"fopen", 1, [](Marshaler&) {}, 0});
    ucrt.entries.push_back({"_o_malloc", 0, [](Marshaler&) {}, 0});
    ucrt.entries.push_back({"not_a_windows_export", 99, [](Marshaler&) {}, 0});
    apply_ucrtbase_windows_ordinals(ucrt, true);
    ASSERT_EQ(ucrt.entries.size(), 3u);
    EXPECT_EQ(ucrt.entries[0].ordinal, 2375);
    EXPECT_EQ(ucrt.entries[1].ordinal, 2204);
    EXPECT_EQ(ucrt.entries[2].ordinal, 1600);

    DllExports msv;
    msv.entries.push_back({"sin", 1, [](Marshaler&) {}, 0});
    msv.entries.push_back({"_initterm", 1, [](Marshaler&) {}, 0});
    msv.entries.push_back({"_set_invalid_parameter_handler", 0, [](Marshaler&) {}, 0});
    msv.entries.push_back({"not_a_windows_export", 0, [](Marshaler&) {}, 0});
    apply_msvcrt_windows_ordinals(msv, true);
    ASSERT_EQ(msv.entries.size(), 2u);
    EXPECT_EQ(msv.entries[0].ordinal, 1219);
    EXPECT_EQ(msv.entries[1].ordinal, 395);
}

TEST(CrtOrdinalsTest, InternalAcceleratorTargetsSurviveWithoutBecomingGuestExports) {
    DllExports crt;
    crt.entries.push_back({"fgetc", 0, [](Marshaler&) {}});
    crt.entries.push_back({"__vitreo_fgetc_slow", 0, [](Marshaler&) {}, 0, true});
    apply_msvcrt_windows_ordinals(crt, true);
    ASSERT_EQ(crt.entries.size(), 2u);
    ThunkRegistry registry(0x71000000);
    ASSERT_TRUE(registry.register_dll("msvcrt.dll", std::move(crt)).is_ok());
    EXPECT_TRUE(registry.get_thunk_address("msvcrt.dll", "fgetc").is_ok());
    EXPECT_TRUE(registry.get_thunk_address("msvcrt.dll", "__vitreo_fgetc_slow").is_err());
    const auto internal = registry.get_thunk_address("msvcrt.dll", "__vitreo_fgetc_slow", true);
    ASSERT_TRUE(internal.is_ok());
    EXPECT_TRUE(registry.resolve_handler(internal.value()).is_ok());
    const auto exports = registry.enumerate_exports();
    ASSERT_EQ(exports.size(), 1u);
    EXPECT_EQ(exports[0].function_name, "fgetc");
}

// Read the captured Windows PE metadata, independently of the generated C++
// tables. Each case tests the entire native surface against the union of all
// four DLL/architecture surfaces, including names that must be removed.
class CrtExportSurfaceTest : public ::testing::TestWithParam<std::pair<bool, bool>> {};

std::map<std::string, u16> native_crt_exports(bool ucrt, bool x64) {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path()
        .parent_path().parent_path();
    const std::string filename = std::string(ucrt ? "ucrtbase" : "msvcrt")
        + "_exports" + (x64 ? "64" : "32") + ".tsv";
    std::ifstream input(root / "tests/oracle_data" / filename);
    EXPECT_TRUE(input.is_open()) << filename;
    std::map<std::string, u16> entries;
    std::string row;
    while (std::getline(input, row)) {
        if (row.starts_with('#') || row == "name\tordinal") continue;
        const auto tab = row.find('\t');
        if (tab == std::string::npos) {
            ADD_FAILURE() << "Invalid Windows export row: " << row;
            continue;
        }
        entries.emplace(row.substr(0, tab), static_cast<u16>(std::stoul(row.substr(tab + 1))));
    }
    return entries;
}

TEST_P(CrtExportSurfaceTest, EveryNativeNameAndOrdinalMatchesItsArchitecture) {
    const auto [ucrt, x64] = GetParam();
    const auto expected = native_crt_exports(ucrt, x64);
    ASSERT_FALSE(expected.empty());
    std::map<std::string, u16> candidates;
    for (bool candidate_ucrt : {false, true}) {
        for (bool candidate_x64 : {false, true}) {
            candidates.merge(native_crt_exports(candidate_ucrt, candidate_x64));
        }
    }
    candidates.emplace("SIN", 99); // Export names are case-sensitive.
    candidates.emplace("not_a_windows_export", 99);
    DllExports exports;
    for (const auto& [name, ordinal] : candidates) {
        exports.entries.push_back({name, 1, [](Marshaler&) {}, ordinal});
    }
    exports.entries.push_back({"__vitreo_internal", 0, [](Marshaler&) {}, 99, true});
    if (ucrt) apply_ucrtbase_windows_ordinals(exports, x64);
    else apply_msvcrt_windows_ordinals(exports, x64);
    ASSERT_EQ(exports.entries.size(), expected.size() + 1);
    for (const auto& entry : exports.entries) {
        if (entry.internal) {
            EXPECT_EQ(entry.ordinal, 0);
            continue;
        }
        const auto it = expected.find(entry.name);
        ASSERT_NE(it, expected.end()) << entry.name;
        EXPECT_EQ(entry.ordinal, it->second) << entry.name;
    }
}

INSTANTIATE_TEST_SUITE_P(WindowsPe, CrtExportSurfaceTest,
    ::testing::Values(std::pair{false, false}, std::pair{false, true},
                      std::pair{true, false}, std::pair{true, true}));

} // namespace

namespace {

// 09_crt.md LOW _initialize_onexit_table -- crt_ucrt_rt_probe "onexit":
// re-initialising a populated table keeps it; _execute_onexit_table runs a
// function registered during execution right after its registrant.
TEST_F(CrtAuditTest, OnexitTableReinitKeepsAndLateRegistrationRuns) {
    const GuestAddr table = kBase + 0x90000;
    ASSERT_EQ(memory_manager.initialize_onexit_table(table, true), 0);
    ASSERT_EQ(memory_manager.register_onexit_function(table, 0x1000, true), 0);
    ASSERT_EQ(memory_manager.register_onexit_function(table, 0x2000, true), 0);
    const u64 first = memory.read_u64(table).value();
    ASSERT_EQ(memory_manager.initialize_onexit_table(table, true), 0);
    EXPECT_EQ(memory.read_u64(table).value(), first);
    std::vector<GuestAddr> ran;
    memory_manager.set_void_callback([&](GuestAddr f) {
        ran.push_back(f);
        if (f == 0x2000) (void)memory_manager.register_onexit_function(table, 0x3000, true);
    });
    EXPECT_EQ(memory_manager.execute_onexit_table(table, true), 0);
    EXPECT_EQ(ran, (std::vector<GuestAddr>{0x2000, 0x3000, 0x1000}));
}

} // namespace
