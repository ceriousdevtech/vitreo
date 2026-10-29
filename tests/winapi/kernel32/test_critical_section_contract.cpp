/// @file test_critical_section_contract.cpp
/// @brief The whole InitializeCriticalSection{,AndSpinCount,Ex} and
///        SetCriticalSectionSpinCount contract, x86 and x64, through the
///        exported thunks.
///
/// Every expected value is Windows': the SpinCount words, DebugInfo states,
/// returns and last errors are the Windows measurements in
/// tools/oracle_probe/handles_waits_probe.expected (critical_sections block,
/// lines 150-199 x64 and 402-451 x86) and
/// tools/oracle_probe/cs_init_flags_probe.expected; the struct offsets are
/// RTL_CRITICAL_SECTION / RTL_CRITICAL_SECTION_DEBUG in the SDK winnt.h
/// (checked by tools/audit_critical_section_constants.py).

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/synchronization/critical_section.hpp>
#include <winapi/kernel32/thread/thread.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <initializer_list>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kCs = 0x00400000;
constexpr GuestAddr kCs2 = 0x00400100;
constexpr GuestAddr kStacks = 0x00500000;   // one 0x1000 stack per caller
constexpr GuestAddr kUnmapped = 0x100;      // the probe's unwritable section
constexpr u64 kNoDebugInfo64 = ~u64{0};     // (PVOID)-1

struct CallResult {
    u64 ret = 0;
    u32 last_error = 0;
    bool raised = false;
    u32 code = 0;
    u64 access = 0;
    u64 address = 0;
};

class CsContractTest : public ::testing::TestWithParam<bool> {
protected:
    static void SetUpTestSuite() { unsetenv("VITREO_NO_CS_ACCEL"); }

    void SetUp() override {
        if (std::thread::hardware_concurrency() < 2) {
            GTEST_SKIP() << "the measured spin counts are a multiprocessor's";
        }
        ASSERT_TRUE(memory.enable_linear_reservation(0x1000000));
        ASSERT_TRUE(memory.map_region(kCs, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kStacks, 0x8000,
                                      MemoryProtection::ReadWrite).is_ok());
        mgr = std::make_unique<CriticalSectionManager>(memory);
        exports = create_critical_section_thunks(*mgr);
    }
    void TearDown() override { ThreadManager::set_current_thread_id(1); }

    bool is64() const { return GetParam(); }
    const CsLayout& L() const { return is64() ? cs_layout_64 : cs_layout_32; }

    /// Call export `name` the way guest code of this bitness would.
    CallResult call(const char* name, std::initializer_list<u64> args,
                    unsigned stack_slot = 0) {
        const auto it = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [&](const ExportEntry& e) { return e.name == name; });
        EXPECT_NE(it, exports.entries.end()) << name;
        if (it == exports.entries.end()) return {};
        cpu::CpuState cpu;
        const GuestAddr sp = kStacks + 0x1000 * stack_slot + 0x800;
        if (is64()) {
            cpu.init_long_mode();
            cpu.gpr.rsp = sp;
            const u64* a = args.begin();
            if (args.size() > 0) cpu.gpr.rcx = a[0];
            if (args.size() > 1) cpu.gpr.rdx = a[1];
            if (args.size() > 2) cpu.gpr.r8 = a[2];
        } else {
            cpu.init_protected_mode();
            cpu.gpr.rsp = sp;
            u32 off = 4;
            for (u64 v : args) {
                EXPECT_TRUE(memory.write_u32(sp + off, static_cast<u32>(v)).is_ok());
                off += 4;
            }
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        set_last_error(0xDEAD);
        it->handler(m);
        CallResult r;
        r.ret = is64() ? cpu.gpr.rax : (cpu.gpr.rax & 0xFFFFFFFFu);
        r.last_error = get_last_error();
        r.raised = cpu.pending_raise.active;
        r.code = cpu.pending_raise.code;
        r.access = cpu.pending_raise.args[0];
        r.address = cpu.pending_raise.args[1];
        return r;
    }

    u64 read_ptr(GuestAddr a) {
        return is64() ? memory.read_u64(a).value_or(0xBADBADBADull)
                      : memory.read_u32(a).value_or(0xBADBADu);
    }
    u64 spin(GuestAddr cs = kCs) { return read_ptr(cs + L().off_spin_count); }
    u64 debug_info(GuestAddr cs = kCs) { return read_ptr(cs + L().off_debug_info); }
    u64 no_debug_info() const { return is64() ? kNoDebugInfo64 : 0xFFFFFFFFull; }
    void fill_cc(GuestAddr cs = kCs) {
        for (u32 i = 0; i < L().size; ++i) ASSERT_TRUE(memory.write_u8(cs + i, 0xCC).is_ok());
    }
    bool untouched(GuestAddr cs = kCs) {
        for (u32 i = 0; i < L().size; ++i) {
            if (memory.read_u8(cs + i).value_or(0) != 0xCC) return false;
        }
        return true;
    }

    // RTL_CRITICAL_SECTION_DEBUG (winnt.h): Type, CriticalSection,
    // ProcessLocksList, EntryCount, ContentionCount, Flags.
    u32 dbg_off_cs() const { return is64() ? 8 : 4; }
    u32 dbg_off_list() const { return is64() ? 16 : 8; }
    u32 dbg_off_entry() const { return is64() ? 32 : 16; }
    u32 dbg_off_contention() const { return is64() ? 36 : 20; }
    u32 dbg_off_flags() const { return is64() ? 40 : 24; }

    /// Thread 1 holds the section while thread 2 blocks in
    /// EnterCriticalSection, then releases it (the probe's contend_once).
    void contend_once(GuestAddr cs = kCs) {
        ThreadManager::set_current_thread_id(1);
        ASSERT_FALSE(call("EnterCriticalSection", {cs}).raised);
        std::atomic<bool> entered{false};
        std::thread waiter([&] {
            ThreadManager::set_current_thread_id(2);
            (void)call("EnterCriticalSection", {cs}, 1);
            entered.store(true);
            (void)call("LeaveCriticalSection", {cs}, 1);
        });
        // Long past any spin: the waiter parks.
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (mgr->contended_waits() == waits_seen_ &&
               std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT_FALSE(entered.load());
        (void)call("LeaveCriticalSection", {cs});
        waiter.join();
        EXPECT_TRUE(entered.load());
        waits_seen_ = mgr->contended_waits();
    }

    GuestMemory memory;
    std::unique_ptr<CriticalSectionManager> mgr;
    DllExports exports;
    u64 waits_seen_ = 0;
};

INSTANTIATE_TEST_SUITE_P(Bitness, CsContractTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

// handles_waits_probe.expected:151 / :403 -- "init dbg=ffffffff lock=ffffffff
// rec=00000000 owner_zero=1 sem_nonzero=0 spin=00000000020007d0".
TEST_P(CsContractTest, InitializeStoresDynamicSpin2000AndNoDebugInfo) {
    fill_cc();
    const CallResult r = call("InitializeCriticalSection", {kCs});
    ASSERT_FALSE(r.raised);
    EXPECT_EQ(spin(), 0x020007D0u);
    EXPECT_EQ(debug_info(), no_debug_info());
    EXPECT_EQ(memory.read_u32(kCs + L().off_lock_count).value(), 0xFFFFFFFFu);
    EXPECT_EQ(memory.read_u32(kCs + L().off_recursion_count).value(), 0u);
    EXPECT_EQ(read_ptr(kCs + L().off_owning_thread), 0u);
    EXPECT_EQ(read_ptr(kCs + L().off_lock_semaphore), 0u);
    EXPECT_EQ(r.last_error, 0xDEADu);
}

// InitializeCriticalSectionAndSpinCount: always TRUE, last error untouched;
// the high byte of dwSpinCount is discarded and a zero count becomes the
// dynamic default. handles_waits_probe.expected:186-199,
// cs_init_flags_probe.expected "-- and_spin_count".
TEST_P(CsContractTest, AndSpinCountStoresTheMeasuredWord) {
    struct Row { u32 spin; u32 word; };
    const Row rows[] = {
        {4000, 0x00000FA0}, {0, 0x020007D0}, {0x80000FA0, 0x00000FA0},
        {0x01000FA0, 0x00000FA0}, {0xFF000000, 0x020007D0},
        {0x02000000, 0x020007D0}, {0x02000FA0, 0x00000FA0},
        {0x00FFFFFF, 0x00FFFFFF}, {0xFFFFFFFF, 0x00FFFFFF}, {1, 1},
    };
    for (const Row& row : rows) {
        fill_cc();
        const CallResult r =
            call("InitializeCriticalSectionAndSpinCount", {kCs, row.spin});
        ASSERT_FALSE(r.raised);
        EXPECT_EQ(r.ret, 1u) << std::hex << row.spin;
        EXPECT_EQ(r.last_error, 0xDEADu) << std::hex << row.spin;
        EXPECT_EQ(spin(), row.word) << std::hex << row.spin;
        EXPECT_EQ(debug_info(), no_debug_info()) << std::hex << row.spin;
        (void)call("DeleteCriticalSection", {kCs});
    }
}

// InitializeCriticalSectionEx: every flag bit, the reserved bits and the
// spin-count high byte. handles_waits_probe.expected:159-185,
// cs_init_flags_probe.expected "-- ex_flags".
TEST_P(CsContractTest, ExValidatesFlagsAndStoresTheMeasuredWord) {
    enum class Dbg { None, Record };
    struct Row { u32 spin; u32 flags; bool ok; u32 word; Dbg dbg; };
    const Row rows[] = {
        {4000, 0x01000000, true, 0x01000FA0, Dbg::None},     // NO_DEBUG_INFO
        {4000, 0, true, 0x00000FA0, Dbg::None},
        {0, 0x20000000, false, 0, Dbg::None},                // reserved
        {0x81000FA0, 0, false, 0, Dbg::None},                // high byte in spin
        {0x01000FA0, 0, false, 0, Dbg::None},
        {4000, 0x02000000, true, 0x020007D0, Dbg::None},     // DYNAMIC_SPIN
        {4000, 0x03000000, true, 0x030007D0, Dbg::None},
        {4000, 0x08000000, true, 0x08000FA0, Dbg::None},     // RESOURCE_TYPE
        {4000, 0x10000000, true, 0x00000FA0, Dbg::Record},   // FORCE_DEBUG_INFO
        {4000, 0x00000001, true, 0x00000FA0, Dbg::None},     // low bits ignored
        {0, 0x01000000, true, 0x030007D0, Dbg::None},
        {0, 0, true, 0x020007D0, Dbg::None},
        {1, 0, true, 0x00000001, Dbg::None},
        {0x00FFFFFF, 0, true, 0x00FFFFFF, Dbg::None},
        {0, 0x00800000, true, 0x020007D0, Dbg::None},
        {4000, 0x00FFFFFF, true, 0x00000FA0, Dbg::None},
        {4000, 0x40000000, false, 0, Dbg::None},
        {4000, 0x80000000, false, 0, Dbg::None},
        {4000, 0x11000000, false, 0, Dbg::None},             // NO_DEBUG + FORCE
        {0, 0x02000000, true, 0x020007D0, Dbg::None},
        {1, 0x02000000, true, 0x020007D0, Dbg::None},
        {0x00FFFFFF, 0x02000000, true, 0x020007D0, Dbg::None},
        {0, 0x08000000, true, 0x0A0007D0, Dbg::None},
        {4000, 0x0A000000, true, 0x0A0007D0, Dbg::None},
        {4000, 0x1B000000, false, 0, Dbg::None},
        {4000, 0x24000000, false, 0, Dbg::None},             // static + reserved
        {0x01000FA0, 0x04000000, false, 0, Dbg::None},       // static + bad spin
        {0, 0x10000000, true, 0x020007D0, Dbg::Record},
        {0xFFFFFFFF, 0, false, 0, Dbg::None},
    };
    for (const Row& row : rows) {
        fill_cc();
        const CallResult r =
            call("InitializeCriticalSectionEx", {kCs, row.spin, row.flags});
        ASSERT_FALSE(r.raised);
        const std::string what = "spin=" + std::to_string(row.spin) +
                                 " flags=" + std::to_string(row.flags);
        if (!row.ok) {
            EXPECT_EQ(r.ret, 0u) << what;
            EXPECT_EQ(r.last_error, error_codes::ERROR_INVALID_PARAMETER) << what;
            EXPECT_TRUE(untouched()) << what;   // validated before any write
            continue;
        }
        EXPECT_EQ(r.ret, 1u) << what;
        EXPECT_EQ(r.last_error, 0xDEADu) << what;
        EXPECT_EQ(spin(), row.word) << what;
        EXPECT_EQ(memory.read_u32(kCs + L().off_lock_count).value(), 0xFFFFFFFFu) << what;
        if (row.dbg == Dbg::None) {
            EXPECT_EQ(debug_info(), no_debug_info()) << what;
        } else {
            const u64 d = debug_info();
            ASSERT_NE(d, 0u) << what;
            ASSERT_NE(d, no_debug_info()) << what;
            // cs_init_flags_probe forcedebug_init_record: type 0, back
            // pointer, no entries, no contention, no flags.
            EXPECT_EQ(memory.read_u16(d).value(), 0u) << what;
            EXPECT_EQ(read_ptr(d + dbg_off_cs()), kCs) << what;
            EXPECT_EQ(memory.read_u32(d + dbg_off_entry()).value(), 0u) << what;
            EXPECT_EQ(memory.read_u32(d + dbg_off_contention()).value(), 0u) << what;
            EXPECT_EQ(memory.read_u32(d + dbg_off_flags()).value(), 0u) << what;
        }
        (void)call("DeleteCriticalSection", {kCs});
    }
}

// RTL_CRITICAL_SECTION_FLAG_STATIC_INIT: TRUE without writing the structure
// (handles_waits_probe.expected:184-185 "untouched ret=00000001").
TEST_P(CsContractTest, ExStaticInitReturnsTrueAndLeavesTheSectionAlone) {
    fill_cc();
    const CallResult r =
        call("InitializeCriticalSectionEx", {kCs, 4000, 0x04000000});
    ASSERT_FALSE(r.raised);
    EXPECT_EQ(r.ret, 1u);
    EXPECT_EQ(r.last_error, 0xDEADu);
    EXPECT_TRUE(untouched());
}

// SetCriticalSectionSpinCount returns the previous count without its flag
// byte and keeps that flag byte. handles_waits_probe.expected:186-195,
// cs_init_flags_probe.expected "-- set_spin_count".
TEST_P(CsContractTest, SetSpinCountReturnsPreviousCountAndKeepsTheFlagByte) {
    struct Step { u32 set; u32 ret; u32 word; };
    struct Case { const char* init; u32 spin; u32 flags; std::vector<Step> steps; };
    const Case cases[] = {
        {"asc", 4000, 0, {{100, 0xFA0, 0x64}}},
        {"default", 0, 0, {{100, 0x7D0, 0x02000064}, {0x81000010, 0x64, 0x02000010}}},
        {"default", 0, 0, {{0, 0x7D0, 0x02000000}, {0x02000000, 0, 0x02000000},
                           {0xFFFFFFFF, 0, 0x02FFFFFF}}},
        {"ex", 4000, 0x01000000, {{100, 0xFA0, 0x01000064}}},
        {"ex", 4000, 0x08000000, {{100, 0xFA0, 0x08000064}}},
    };
    for (const Case& c : cases) {
        fill_cc();
        const std::string init = c.init;
        if (init == "asc") {
            (void)call("InitializeCriticalSectionAndSpinCount", {kCs, c.spin});
        } else if (init == "default") {
            (void)call("InitializeCriticalSection", {kCs});
        } else {
            (void)call("InitializeCriticalSectionEx", {kCs, c.spin, c.flags});
        }
        for (const Step& s : c.steps) {
            const CallResult r = call("SetCriticalSectionSpinCount", {kCs, s.set});
            ASSERT_FALSE(r.raised);
            EXPECT_EQ(r.ret, s.ret) << init << " set=" << std::hex << s.set;
            EXPECT_EQ(r.last_error, 0xDEADu);
            EXPECT_EQ(spin(), s.word) << init << " set=" << std::hex << s.set;
        }
        (void)call("DeleteCriticalSection", {kCs});
    }
}

// An unwritable section faults where Windows' first store or load lands
// (cs_init_flags_probe.expected "-- unwritable_section": 0x100 + LockCount
// for Initialize/AndSpinCount, + RecursionCount on x64 Ex and LockCount on
// x86 Ex, a READ of SpinCount for Set), while Ex validation runs first and
// STATIC_INIT touches nothing.
TEST_P(CsContractTest, UnwritableSectionRaisesTheMeasuredAccessViolation) {
    struct Row { const char* name; std::vector<u64> args; bool fault; u64 access; u64 address; u64 ret; u32 err; };
    const u64 lock = kUnmapped + L().off_lock_count;
    const u64 ex_first = kUnmapped + (is64() ? L().off_recursion_count : L().off_lock_count);
    const Row rows[] = {
        {"InitializeCriticalSection", {kUnmapped}, true, 1, lock, 0, 0},
        {"InitializeCriticalSectionAndSpinCount", {kUnmapped, 4000}, true, 1, lock, 0, 0},
        {"InitializeCriticalSectionEx", {kUnmapped, 4000, 0}, true, 1, ex_first, 0, 0},
        {"InitializeCriticalSectionEx", {kUnmapped, 4000, 0x20000000}, false, 0, 0, 0,
         error_codes::ERROR_INVALID_PARAMETER},
        {"InitializeCriticalSectionEx", {kUnmapped, 4000, 0x04000000}, false, 0, 0, 1, 0xDEAD},
        {"SetCriticalSectionSpinCount", {kUnmapped, 100}, true, 0,
         kUnmapped + L().off_spin_count, 0, 0},
    };
    for (const Row& row : rows) {
        CallResult r;
        switch (row.args.size()) {
            case 1: r = call(row.name, {row.args[0]}); break;
            case 2: r = call(row.name, {row.args[0], row.args[1]}); break;
            default: r = call(row.name, {row.args[0], row.args[1], row.args[2]}); break;
        }
        EXPECT_EQ(r.raised, row.fault) << row.name;
        if (row.fault) {
            EXPECT_EQ(r.code, 0xC0000005u) << row.name;
            EXPECT_EQ(r.access, row.access) << row.name;
            EXPECT_EQ(r.address, row.address) << row.name;
        } else {
            EXPECT_EQ(r.ret, row.ret) << row.name;
            EXPECT_EQ(r.last_error, row.err) << row.name;
        }
    }
}

// DebugInfo is (PVOID)-1 until the first contended wait gives the section a
// record; a failed TryEnter does not, NO_DEBUG_INFO never gets one, and
// ContentionCount counts the waits (cs_init_flags_probe.expected
// "-- debug_info"; handles_waits_probe.expected:157 "waiter dbg=00000001").
TEST_P(CsContractTest, FirstContendedWaitGivesTheSectionADebugRecord) {
    (void)call("InitializeCriticalSection", {kCs});
    EXPECT_EQ(debug_info(), no_debug_info());

    ThreadManager::set_current_thread_id(1);
    (void)call("EnterCriticalSection", {kCs});
    std::thread trier([&] {
        ThreadManager::set_current_thread_id(2);
        EXPECT_EQ(call("TryEnterCriticalSection", {kCs}, 1).ret, 0u);
    });
    trier.join();
    (void)call("LeaveCriticalSection", {kCs});
    EXPECT_EQ(debug_info(), no_debug_info()) << "a failed TryEnter waits for nothing";

    contend_once();
    const u64 d = debug_info();
    ASSERT_NE(d, no_debug_info());
    ASSERT_NE(d, 0u);
    EXPECT_EQ(memory.read_u16(d).value(), 0u);
    EXPECT_EQ(read_ptr(d + dbg_off_cs()), kCs);
    EXPECT_EQ(memory.read_u32(d + dbg_off_entry()).value(), 0u);
    EXPECT_EQ(memory.read_u32(d + dbg_off_contention()).value(), 1u);
    EXPECT_EQ(memory.read_u32(d + dbg_off_flags()).value(), 0u);
    // The flag byte survives the contention (spin_flags=02000000).
    EXPECT_EQ(spin() & 0xFF000000u, 0x02000000u);

    contend_once();
    EXPECT_EQ(debug_info(), d);
    EXPECT_EQ(memory.read_u32(d + dbg_off_contention()).value(), 2u);
}

TEST_P(CsContractTest, NoDebugInfoSectionNeverGetsARecord) {
    (void)call("InitializeCriticalSectionEx", {kCs, 4000, 0x01000000});
    contend_once();
    EXPECT_EQ(debug_info(), no_debug_info());
    EXPECT_EQ(spin() & 0xFF000000u, 0x01000000u);
}

TEST_P(CsContractTest, OtherSectionsGetARecordOnContention) {
    struct Case { const char* name; u32 spin; u32 flags; bool ex; u32 flag_byte; };
    const Case cases[] = {
        {"ex0", 4000, 0, true, 0},
        {"asc_flagbyte", 0x01000FA0, 0, false, 0},
        {"resource", 4000, 0x08000000, true, 0x08000000},
    };
    for (const Case& c : cases) {
        if (c.ex) (void)call("InitializeCriticalSectionEx", {kCs, c.spin, c.flags});
        else (void)call("InitializeCriticalSectionAndSpinCount", {kCs, c.spin});
        contend_once();
        const u64 d = debug_info();
        EXPECT_NE(d, no_debug_info()) << c.name;
        EXPECT_NE(d, 0u) << c.name;
        EXPECT_EQ(spin() & 0xFF000000u, c.flag_byte) << c.name;
        (void)call("DeleteCriticalSection", {kCs});
    }
}

TEST_P(CsContractTest, ForcedDebugRecordCountsContention) {
    (void)call("InitializeCriticalSectionEx", {kCs, 4000, 0x10000000});
    const u64 d = debug_info();
    ASSERT_NE(d, no_debug_info());
    EXPECT_EQ(memory.read_u32(d + dbg_off_contention()).value(), 0u);
    contend_once();
    EXPECT_EQ(debug_info(), d);
    EXPECT_EQ(memory.read_u32(d + dbg_off_contention()).value(), 1u);
}

// Windows links every record into the process's lock list
// (list_self_linked=0); it is a circular LIST_ENTRY list, and deleting a
// section takes its record out.
TEST_P(CsContractTest, DebugRecordsShareOneProcessLockList) {
    (void)call("InitializeCriticalSectionEx", {kCs, 4000, 0x10000000});
    (void)call("InitializeCriticalSectionEx", {kCs2, 4000, 0x10000000});
    const u64 e1 = debug_info(kCs) + dbg_off_list();
    const u64 e2 = debug_info(kCs2) + dbg_off_list();
    const u32 ptr = is64() ? 8 : 4;
    EXPECT_NE(read_ptr(e1), e1) << "self-linked";
    auto walk = [&](u64 start) {
        std::set<u64> seen;
        u64 at = start;
        for (int i = 0; i < 64; ++i) {
            seen.insert(at);
            const u64 next = read_ptr(at);
            EXPECT_EQ(read_ptr(next + ptr), at) << "Flink->Blink must come back";
            at = next;
            if (at == start) return seen;
        }
        ADD_FAILURE() << "list is not circular";
        return seen;
    };
    std::set<u64> seen = walk(e1);
    EXPECT_TRUE(seen.count(e2)) << "both records are on one list";
    (void)call("DeleteCriticalSection", {kCs2});
    seen = walk(e1);
    EXPECT_FALSE(seen.count(e2)) << "a deleted section's record leaves the list";
    (void)call("DeleteCriticalSection", {kCs});
}

} // namespace
