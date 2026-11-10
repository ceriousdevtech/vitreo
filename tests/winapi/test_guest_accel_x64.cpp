/// @file test_guest_accel_x64.cpp
/// @brief Executes the x64 guest accelerators and checks them against the
///        contracts they implement, in the interpreter and in the JIT.
///
/// Hand-written machine code in the guest's address space is not trusted by
/// inspection. Each case runs the emitted bytes and compares the result with
/// the contract's source: Microsoft Learn TlsGetValue and the TEB layout
/// Vitreo's own TlsSetValue writes (tls.cpp), and, for _finite/_isnan, the
/// values Windows returned (tools/oracle_probe/crt_classify_probe.expected).
/// Both engines run every case: production reaches this code through the JIT,
/// which addresses the TEB through GS.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/msvcrt/guest_accel.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <winapi/kernel32/thread/tls.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/cpu/interpreter/interpreter.hpp>
#include <core/cpu/jit/jit_compiler.hpp>
#include "../cpu/jit_dispatch_execute.hpp"
#include <core/memory/guest_memory.hpp>
#include "runtime/execution/block_decoder.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kCodeBase = 0x0000000140100000ull;
constexpr GuestAddr kStackBase = 0x0000000140200000ull;
constexpr GuestAddr kTebBase = 0x0000000140300000ull;
constexpr GuestAddr kExpansion = 0x0000000140310000ull;
constexpr GuestAddr kRetMagic = 0x0000000140DEAD00ull;  // never mapped
// Where every host tail-jump lands in this harness (install resolves them to
// the slow exports): reaching it means the fast path handed the call over.
constexpr GuestAddr kSlowMagic = 0x0000000140DEAD10ull;  // never mapped
// Where a tail-jump to a full host export (not a __vitreo_* slow half) lands.
constexpr GuestAddr kHostMagic = 0x0000000140DEAD20ull;  // never mapped
constexpr u64 kTestTid = 0x1234;
constexpr u32 kSentinelError = 0x1234u;

enum class Engine { Interpreter, Jit };

class GuestAccelX64Test : public ::testing::TestWithParam<Engine> {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kCodeBase, 0x1000,
                                      MemoryProtection::ReadWriteExecute).is_ok());
        ASSERT_TRUE(memory.map_region(kStackBase, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kTebBase, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kExpansion, 0x4000,
                                      MemoryProtection::ReadWrite).is_ok());
        blob = build_guest_x64_code(kCodeBase);
        ASSERT_FALSE(blob.code.empty());
        for (const auto& [slot, name] : blob.extern_fixups) {
            const GuestAddr target =
                name.find("__vitreo_") != std::string::npos ? kSlowMagic : kHostMagic;
            const i32 rel = static_cast<i32>(
                static_cast<i64>(target) -
                static_cast<i64>(kCodeBase + slot + 4));
            std::memcpy(blob.code.data() + slot, &rel, sizeof(rel));
        }
        // TEB ClientId.UniqueThread, which the critical sections own by.
        ASSERT_TRUE(memory.write_u64(kTebBase + 0x48, kTestTid).is_ok());
        ASSERT_TRUE(memory.write_bytes(kCodeBase, blob.code.data(),
                                       blob.code.size()).is_ok());
    }

    [[nodiscard]] GuestAddr entry(const std::string& name) const {
        for (const auto& [n, offset] : blob.entries) {
            if (n == name) return kCodeBase + offset;
        }
        return 0;
    }

    /// Call one accelerator with RCX/XMM0 set and run it until it returns to
    /// the sentinel. Returns the final CPU state.
    cpu::CpuState call(const std::string& name, u64 rcx, u64 xmm0_bits) {
        const GuestAddr fn = entry(name);
        EXPECT_NE(fn, 0u) << name << " was not emitted";
        cpu::CpuState cpu;
        cpu.init_long_mode();
        cpu.seg.gs_desc.base = kTebBase;
        cpu.gpr.rcx = rcx;
        cpu.gpr.rdx = 0x5555555555555555ull;
        cpu.sse.xmm[0].u64_[0] = xmm0_bits;
        const GuestAddr sp = kStackBase + 0x1000;
        EXPECT_TRUE(memory.write_u64(sp, kRetMagic).is_ok());
        cpu.gpr.rsp = sp;
        cpu.rip = fn;
        took_slow_path = false;
        took_host_export = false;
        for (int blocks = 0; blocks < 16 && cpu.rip != kRetMagic; ++blocks) {
            if (cpu.rip == kSlowMagic || cpu.rip == kHostMagic) {
                took_host_export = cpu.rip == kHostMagic;
                // A tail-jump: the host export sees the caller's frame and
                // its argument, exactly as a direct call would have left them.
                took_slow_path = !took_host_export;
                EXPECT_EQ(cpu.gpr.rsp, sp) << name << " moved the stack";
                EXPECT_EQ(cpu.gpr.rcx, rcx) << name << " lost its argument";
                EXPECT_EQ(memory.read_u64(sp).value_or(0), kRetMagic);
                return cpu;
            }
            if (GetParam() == Engine::Interpreter) {
                cpu::Interpreter interp(cpu, memory);
                const cpu::ExecResult r = interp.run(1);
                EXPECT_TRUE(r == cpu::ExecResult::Continue ||
                            r == cpu::ExecResult::Branch)
                    << "step result " << static_cast<int>(r);
                continue;
            }
            cpu::jit::BasicBlock block =
                cli::decode_basic_block(memory, cpu.rip, /*is_64bit=*/true);
            EXPECT_GT(block.instruction_count, 0u);
            if (block.instruction_count == 0) break;
            cpu::jit::JitCompiler jit(memory);
            cpu::jit::CompiledBlock* compiled = jit.compile(block);
            EXPECT_NE(compiled, nullptr) << "JIT declined at 0x" << std::hex
                                         << cpu.rip;
            if (compiled == nullptr) break;
            EXPECT_EQ(cpu::jit::test::execute_like_dispatcher(jit, *compiled, cpu),
                      cpu::jit::JitExecResult::Continue);
        }
        EXPECT_EQ(cpu.rip, kRetMagic) << name << " did not return";
        EXPECT_EQ(cpu.gpr.rsp, sp + 8) << name << " left the stack unbalanced";
        EXPECT_FALSE(cpu.flags.df()) << name << " left DF set";
        return cpu;
    }

    void set_last_error(u32 value) {
        ASSERT_TRUE(memory.write_u32(kTebBase + 0x68, value).is_ok());
    }
    [[nodiscard]] u32 last_error() {
        return memory.read_u32(kTebBase + 0x68).value_or(0xDEADDEADu);
    }

    GuestMemory memory;
    CrtAccelBlob blob;
    bool took_slow_path = false;
    bool took_host_export = false;
};

// Learn TlsGetValue: the slot's value, LastError cleared on success; an index
// past the process's slots returns 0 with ERROR_INVALID_PARAMETER. Indexes
// 0-63 live in TEB.TlsSlots; 64-1087 in TEB.TlsExpansionSlots, which reads 0
// until the thread's first expansion TlsSetValue allocates it.
TEST_P(GuestAccelX64Test, TlsGetValueMatchesTheTebContract) {
    for (u32 i = 0; i < TLS_MINIMUM_AVAILABLE; ++i) {
        ASSERT_TRUE(memory.write_u64(kTebBase + 0x1480 + 8ull * i,
                                     0xA000000000000000ull | i).is_ok());
    }
    const auto expect_value = [&](u64 rcx, u64 want) {
        SCOPED_TRACE(::testing::Message() << "rcx=0x" << std::hex << rcx);
        set_last_error(kSentinelError);
        const auto cpu = call("kernel32.dll!TlsGetValue", rcx, 0);
        EXPECT_EQ(cpu.gpr.rax, want);
        EXPECT_EQ(last_error(), 0u) << "success clears LastError";
    };
    expect_value(0, 0xA000000000000000ull);
    expect_value(5, 0xA000000000000005ull);
    expect_value(63, 0xA00000000000003Full);
    // DWORD argument: the upper half of RCX is not part of the index.
    expect_value(0xFFFFFFFF00000007ull, 0xA000000000000007ull);

    // Expansion slots, not yet allocated on this thread: 0, success.
    ASSERT_TRUE(memory.write_u64(kTebBase + 0x1780, 0).is_ok());
    expect_value(64, 0);
    expect_value(TLS_TOTAL_SLOTS - 1, 0);
    // Allocated: slot (index - 64) of the array.
    ASSERT_TRUE(memory.write_u64(kTebBase + 0x1780, kExpansion).is_ok());
    ASSERT_TRUE(memory.write_u64(kExpansion + 0, 0xB0).is_ok());
    ASSERT_TRUE(memory.write_u64(kExpansion + 8ull * 36, 0xB36).is_ok());
    ASSERT_TRUE(memory.write_u64(kExpansion + 8ull * (TLS_TOTAL_SLOTS - 65),
                                 0xBFF).is_ok());
    expect_value(64, 0xB0);
    expect_value(100, 0xB36);
    expect_value(TLS_TOTAL_SLOTS - 1, 0xBFF);

    for (const u64 bad : {u64{TLS_TOTAL_SLOTS}, u64{0xFFFFFFFFu}}) {
        SCOPED_TRACE(::testing::Message() << "invalid index " << bad);
        set_last_error(kSentinelError);
        const auto cpu = call("kernel32.dll!TlsGetValue", bad, 0);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        EXPECT_EQ(last_error(), 87u) << "ERROR_INVALID_PARAMETER";
    }
}

// The values Windows returned for every class of double, both CRTs
// (crt_classify_probe.expected): exactly 1 or 0.
TEST_P(GuestAccelX64Test, FiniteAndIsnanReturnWhatWindowsReturns) {
    const auto path = std::filesystem::path(__FILE__).parent_path()
                          .parent_path().parent_path() /
                      "tests/oracle_data/crt_classify_probe.expected";
    std::ifstream in(path);
    ASSERT_TRUE(in.good()) << path;
    std::string line;
    usize checked = 0;
    while (std::getline(in, line)) {
        std::istringstream row(line);
        std::string dll, fn, bits, arrow;
        int want = -1;
        ASSERT_TRUE(bool(row >> dll >> fn >> bits >> arrow >> want)) << line;
        SCOPED_TRACE(line);
        const u64 x = std::stoull(bits, nullptr, 16);
        const auto cpu = call(dll + "!" + fn, 0x7777777777777777ull, x);
        EXPECT_EQ(cpu.gpr.rax, static_cast<u64>(want))
            << "the int result, zero-extended into RAX";
        ++checked;
    }
    EXPECT_EQ(checked, 60u);
}

INSTANTIATE_TEST_SUITE_P(Engines, GuestAccelX64Test,
                         ::testing::Values(Engine::Interpreter, Engine::Jit),
                         [](const auto& info) {
                             return info.param == Engine::Interpreter
                                        ? std::string("Interpreter")
                                        : std::string("Jit");
                         });


// ---- Critical sections ------------------------------------------------------
// Learn EnterCriticalSection / TryEnterCriticalSection / LeaveCriticalSection
// on RTL_CRITICAL_SECTION x64 (critical_section.hpp cs_layout_64), with the
// Server 2003 SP1+ LockCount encoding the host halves share
// (critical_section.cpp): -1 free, -2 held, -2-4*n held with n waiters.

namespace {
constexpr GuestAddr kCs = kExpansion + 0x2000;
constexpr u32 kFree = 0xFFFFFFFFu;
constexpr u32 kHeld = 0xFFFFFFFEu;
constexpr u32 kHeldTwoWaiters = 0xFFFFFFFEu - 8u;
}  // namespace

class GuestAccelX64CsTest : public GuestAccelX64Test {
protected:
    void init_cs(u32 lock, u32 recursion, u64 owner) {
        ASSERT_TRUE(memory.write_u64(kCs + 0x00, 0).is_ok());     // DebugInfo
        ASSERT_TRUE(memory.write_u32(kCs + 0x08, lock).is_ok());  // LockCount
        ASSERT_TRUE(memory.write_u32(kCs + 0x0C, recursion).is_ok());
        ASSERT_TRUE(memory.write_u64(kCs + 0x10, owner).is_ok());  // OwningThread
        ASSERT_TRUE(memory.write_u64(kCs + 0x18, 0).is_ok());     // LockSemaphore
        ASSERT_TRUE(memory.write_u64(kCs + 0x20, 0x020007D0ull).is_ok());  // SpinCount
    }
    [[nodiscard]] u32 lock() { return memory.read_u32(kCs + 0x08).value_or(7); }
    [[nodiscard]] u32 recursion() { return memory.read_u32(kCs + 0x0C).value_or(7); }
    [[nodiscard]] u64 owner() { return memory.read_u64(kCs + 0x10).value_or(7); }
    void expect_untouched_tail() {
        EXPECT_EQ(memory.read_u64(kCs + 0x00).value_or(7), 0u) << "DebugInfo";
        EXPECT_EQ(memory.read_u64(kCs + 0x18).value_or(7), 0u) << "LockSemaphore";
        EXPECT_EQ(memory.read_u64(kCs + 0x20).value_or(7), 0x020007D0ull)
            << "SpinCount";
    }
};

TEST_P(GuestAccelX64CsTest, EnterTakesAFreeSectionWithoutTheHost) {
    init_cs(kFree, 0, 0);
    (void)call("kernel32.dll!EnterCriticalSection", kCs, 0);
    EXPECT_FALSE(took_slow_path);
    EXPECT_EQ(lock(), kHeld) << "-1 -> -2: held, no waiters";
    EXPECT_EQ(recursion(), 1u);
    EXPECT_EQ(owner(), kTestTid) << "OwningThread is TEB ClientId.UniqueThread";
    expect_untouched_tail();
}

TEST_P(GuestAccelX64CsTest, EnterByTheOwnerNests) {
    init_cs(kHeld, 1, kTestTid);
    (void)call("kernel32.dll!EnterCriticalSection", kCs, 0);
    EXPECT_FALSE(took_slow_path);
    EXPECT_EQ(recursion(), 2u);
    EXPECT_EQ(lock(), kHeld) << "a nested enter does not touch LockCount";
    EXPECT_EQ(owner(), kTestTid);
}

TEST_P(GuestAccelX64CsTest, EnterOfASectionAnotherThreadHoldsWaitsInTheHost) {
    for (const u32 held : {kHeld, kHeldTwoWaiters}) {
        SCOPED_TRACE(held);
        init_cs(held, 3, 0x9999);
        (void)call("kernel32.dll!EnterCriticalSection", kCs, 0);
        EXPECT_TRUE(took_slow_path) << "a held section is __vitreo_cs_enter_slow's";
        EXPECT_EQ(lock(), held);
        EXPECT_EQ(recursion(), 3u);
        EXPECT_EQ(owner(), 0x9999u);
    }
}

TEST_P(GuestAccelX64CsTest, TryEnterTakesNestsOrRefusesAtOnce) {
    init_cs(kFree, 0, 0);
    auto cpu = call("kernel32.dll!TryEnterCriticalSection", kCs, 0);
    EXPECT_FALSE(took_slow_path);
    EXPECT_EQ(cpu.gpr.rax, 1u) << "entered: nonzero";
    EXPECT_EQ(lock(), kHeld);
    EXPECT_EQ(recursion(), 1u);
    EXPECT_EQ(owner(), kTestTid);

    cpu = call("kernel32.dll!TryEnterCriticalSection", kCs, 0);
    EXPECT_EQ(cpu.gpr.rax, 1u) << "already owned by the caller: nonzero";
    EXPECT_EQ(recursion(), 2u);
    EXPECT_EQ(lock(), kHeld);

    for (const u32 held : {kHeld, kHeldTwoWaiters}) {
        SCOPED_TRACE(held);
        init_cs(held, 1, 0x9999);
        cpu = call("kernel32.dll!TryEnterCriticalSection", kCs, 0);
        EXPECT_FALSE(took_slow_path) << "TryEnter never waits";
        EXPECT_EQ(cpu.gpr.rax, 0u) << "owned by another thread: zero";
        EXPECT_EQ(lock(), held);
        EXPECT_EQ(recursion(), 1u);
        EXPECT_EQ(owner(), 0x9999u);
    }
    expect_untouched_tail();
}

TEST_P(GuestAccelX64CsTest, LeaveUnnestsThenReleasesWithoutTheHost) {
    init_cs(kHeld, 2, kTestTid);
    (void)call("kernel32.dll!LeaveCriticalSection", kCs, 0);
    EXPECT_FALSE(took_slow_path);
    EXPECT_EQ(recursion(), 1u);
    EXPECT_EQ(lock(), kHeld) << "still held after the inner leave";
    EXPECT_EQ(owner(), kTestTid);

    (void)call("kernel32.dll!LeaveCriticalSection", kCs, 0);
    EXPECT_FALSE(took_slow_path) << "no waiters encoded: nobody to wake";
    EXPECT_EQ(lock(), kFree);
    EXPECT_EQ(recursion(), 0u);
    EXPECT_EQ(owner(), 0u);
    expect_untouched_tail();
}

TEST_P(GuestAccelX64CsTest, LeaveWithWaitersReleasesThenWakesInTheHost) {
    init_cs(kHeldTwoWaiters, 1, kTestTid);
    (void)call("kernel32.dll!LeaveCriticalSection", kCs, 0);
    EXPECT_TRUE(took_slow_path) << "__vitreo_cs_leave_wake wakes a waiter";
    EXPECT_EQ(lock(), kFree) << "released before the host is asked to wake";
    EXPECT_EQ(recursion(), 0u);
    EXPECT_EQ(owner(), 0u) << "OwningThread cleared before the release";
}

// A section that has been contended keeps LockSemaphore = (HANDLE)-1, and its
// releases always reach the host: LockCount can read a plain -2 while a
// waiter that a barging acquire overtook is back asleep, and only the host
// knows it is registered.
TEST_P(GuestAccelX64CsTest, LeaveOfAContendedSectionAlwaysAsksTheHostToWake) {
    init_cs(kHeld, 1, kTestTid);
    ASSERT_TRUE(memory.write_u64(kCs + 0x18, ~0ull).is_ok());  // LockSemaphore
    (void)call("kernel32.dll!LeaveCriticalSection", kCs, 0);
    EXPECT_TRUE(took_slow_path) << "__vitreo_cs_leave_wake checks its waiters";
    EXPECT_EQ(lock(), kFree) << "released before the host is asked to wake";
    EXPECT_EQ(recursion(), 0u);
    EXPECT_EQ(owner(), 0u);
    EXPECT_EQ(memory.read_u64(kCs + 0x18).value_or(0), ~0ull)
        << "LockSemaphore is the host's; the guest only reads it";
}

// The host halves share LockCount only when it is 4-byte aligned; a
// misaligned section is the host's own mutex (lock_word). Windows permits
// one (a CRITICAL_SECTION in a byte array lands anywhere), so all three
// functions hand it, untouched, to the full host export.
TEST_P(GuestAccelX64CsTest, MisalignedSectionsAreTheHostsAlone) {
    for (const u64 misalign : {1ull, 2ull, 3ull}) {
        const GuestAddr cs = kCs + 0x100 + misalign;
        SCOPED_TRACE(misalign);
        for (const char* fn : {"kernel32.dll!EnterCriticalSection",
                               "kernel32.dll!TryEnterCriticalSection",
                               "kernel32.dll!LeaveCriticalSection"}) {
            SCOPED_TRACE(fn);
            ASSERT_TRUE(memory.write_u32(cs + 0x08, kFree).is_ok());
            ASSERT_TRUE(memory.write_u32(cs + 0x0C, 0).is_ok());
            ASSERT_TRUE(memory.write_u64(cs + 0x10, 0).is_ok());
            (void)call(fn, cs, 0);
            EXPECT_TRUE(took_host_export);
            EXPECT_EQ(memory.read_u32(cs + 0x08).value_or(7), kFree);
            EXPECT_EQ(memory.read_u32(cs + 0x0C).value_or(7), 0u);
            EXPECT_EQ(memory.read_u64(cs + 0x10).value_or(7), 0u);
        }
    }
}

TEST_P(GuestAccelX64CsTest, EnterLeavePairsRoundTrip) {
    init_cs(kFree, 0, 0);
    for (int i = 0; i < 3; ++i) {
        (void)call("kernel32.dll!EnterCriticalSection", kCs, 0);
    }
    EXPECT_EQ(recursion(), 3u);
    for (int i = 0; i < 3; ++i) {
        (void)call("ntdll.dll!RtlLeaveCriticalSection", kCs, 0);
        EXPECT_FALSE(took_slow_path);
    }
    EXPECT_EQ(lock(), kFree);
    EXPECT_EQ(recursion(), 0u);
    EXPECT_EQ(owner(), 0u);
}

INSTANTIATE_TEST_SUITE_P(Engines, GuestAccelX64CsTest,
                         ::testing::Values(Engine::Interpreter, Engine::Jit),
                         [](const auto& info) {
                             return info.param == Engine::Interpreter
                                        ? std::string("Interpreter")
                                        : std::string("Jit");
                         });

// Guest-facing resolution hands out the guest implementation; the thunk itself
// is unchanged for internal users.
TEST(GuestAccelX64Install, RegistersTheGuestImplementationForEachExport) {
    GuestMemory memory;
    constexpr GuestAddr kThunks = 0x00007FF000000000ull;
    ThunkRegistry registry(kThunks);
    const auto noop = [](Marshaler&) {};
    for (const char* dll : {"kernel32.dll", "ucrtbase.dll", "msvcrt.dll"}) {
        DllExports exports;
        if (std::string(dll) == "kernel32.dll") {
            exports.entries.push_back({"TlsGetValue", 1, noop});
        } else {
            exports.entries.push_back({"_finite", 1, noop});
            exports.entries.push_back({"_isnan", 2, noop});
        }
        ASSERT_TRUE(registry.register_dll(dll, std::move(exports)).is_ok());
    }
    const GuestAddr tls_thunk =
        registry.get_thunk_address("kernel32.dll", "TlsGetValue").value();
    EXPECT_EQ(registry.guest_entry(tls_thunk), tls_thunk);

    EXPECT_EQ(install_guest_x64_accelerators(memory, registry), 5u);

    const GuestAddr base = kThunks + X64_ACCEL_OFFSET;
    const CrtAccelBlob blob = build_guest_x64_code(base);
    for (const auto& [name, offset] : blob.entries) {
        const auto bang = name.find('!');
        auto thunk = registry.get_thunk_address(name.substr(0, bang),
                                                name.substr(bang + 1));
        if (thunk.is_err()) continue;  // kernelbase.dll is not registered here
        SCOPED_TRACE(name);
        EXPECT_EQ(registry.guest_entry(thunk.value()), base + offset);
        EXPECT_TRUE(registry.is_in_thunk_region(thunk.value()));
        EXPECT_FALSE(registry.is_in_thunk_region(base + offset));
        // An inline-hook check on the resolved function sees ordinary code.
        const u8 first = memory.read_u8(base + offset).value_or(0);
        EXPECT_NE(first, 0xE9u);
        EXPECT_NE(first, 0xEBu);
    }
    // Without the linear model the host halves keep a private mutex, so the
    // critical sections stay on their host thunks (and their slow exports are
    // not registered here at all).
    for (const char* cs : {"EnterCriticalSection", "TryEnterCriticalSection",
                           "LeaveCriticalSection"}) {
        bool listed = false;
        for (const auto& [name, offset] : blob.entries) {
            listed |= name == std::string("kernel32.dll!") + cs;
        }
        EXPECT_TRUE(listed) << cs << " is emitted";
    }
    // The thunk address itself is what get_thunk_address still returns.
    EXPECT_EQ(registry.get_thunk_address("kernel32.dll", "TlsGetValue").value(),
              tls_thunk);
}

}  // namespace
