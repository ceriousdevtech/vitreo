/// @file test_critical_section_x64_guest_stress.cpp
/// @brief The x64 guest critical-section fast paths and the host halves of the
///        accelerated protocol, contending on one section from many threads.
///
/// guest_accel_x64.cpp serves the uncontended Enter/TryEnter/Leave in guest
/// code and tail-jumps into the host (__vitreo_cs_enter_slow,
/// __vitreo_cs_leave_wake) otherwise; critical_section.cpp's host halves share
/// the guest's LockCount word. Single-threaded contract tests cannot see a
/// protocol race between the two, so here the real emitted bytes run through
/// the JIT on several threads at once -- their slow paths calling the real
/// host halves -- while another thread uses the host API on the same section.
/// Learn EnterCriticalSection: one owner at a time, recursion by the owner,
/// and a waiter is woken by the release that frees the section (a lost wake
/// shows as a wait that only the host's 50 ms re-check ends).

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/synchronization/critical_section.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <winapi/msvcrt/guest_accel.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/cpu/jit/jit_compiler.hpp>
#include <core/memory/guest_memory.hpp>
#include "../cpu/jit_dispatch_execute.hpp"
#include "runtime/execution/block_decoder.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kCode = 0x00100000;
constexpr GuestAddr kSlowEnter = 0x00180000;  // never mapped: host takes over
constexpr GuestAddr kSlowLeave = 0x00180010;
constexpr GuestAddr kRet = 0x00180020;
constexpr GuestAddr kHostEnter = 0x00180030;  // the full host exports
constexpr GuestAddr kHostTryEnter = 0x00180040;
constexpr GuestAddr kHostLeave = 0x00180050;
constexpr GuestAddr kTebs = 0x00300000;      // one 0x2000 TEB per thread
constexpr GuestAddr kStacks = 0x00400000;    // one 0x1000 stack per thread
constexpr u32 kGuestThreads = 4;
constexpr u32 kIters = 20000;

class CsX64GuestStress : public ::testing::Test {
protected:
    static void SetUpTestSuite() { unsetenv("VITREO_NO_CS_ACCEL"); }

    void SetUp() override {
        ASSERT_TRUE(memory.enable_linear_reservation(0x1000000));
        ASSERT_TRUE(memory.map_region(kCode, 0x1000,
                                      MemoryProtection::ReadWriteExecute).is_ok());
        ASSERT_TRUE(memory.map_region(kCs, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kTebs, 0x2000 * 8,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kStacks, 0x1000 * 8,
                                      MemoryProtection::ReadWrite).is_ok());
        blob = build_guest_x64_code(kCode);
        for (const auto& [slot, name] : blob.extern_fixups) {
            const GuestAddr target =
                name.find("enter_slow") != std::string::npos   ? kSlowEnter
                : name.find("leave_wake") != std::string::npos ? kSlowLeave
                : name.find("TryEnter") != std::string::npos   ? kHostTryEnter
                : name.find("Enter") != std::string::npos      ? kHostEnter
                                                               : kHostLeave;
            const i32 rel = static_cast<i32>(static_cast<i64>(target) -
                                             static_cast<i64>(kCode + slot + 4));
            std::memcpy(blob.code.data() + slot, &rel, sizeof(rel));
        }
        ASSERT_TRUE(memory.write_bytes(kCode, blob.code.data(), blob.code.size()).is_ok());
        mgr = std::make_unique<CriticalSectionManager>(memory);
        mgr->note_bitness(true);
    }

    GuestAddr kCs = 0x00200000;  // per test: aligned or not

    [[nodiscard]] GuestAddr entry(const char* name) const {
        for (const auto& [n, offset] : blob.entries) {
            if (n == name) return kCode + offset;
        }
        return 0;
    }

    /// One guest thread: its own TEB (ClientId.UniqueThread = tid, the id
    /// the host side also uses), stack, CPU and JIT.
    struct GuestThread {
        CsX64GuestStress* t;
        u32 index;
        u32 tid;
        cpu::jit::JitCompiler jit;
        std::unordered_map<GuestAddr, cpu::jit::CompiledBlock*> blocks;
        cpu::CpuState cpu;
        GuestThread(CsX64GuestStress* test, u32 i)
            : t(test), index(i), tid(0x100 + 4 * i), jit(test->memory) {}

        /// Call a guest export with RCX = cs; the host serves its tail-jumps.
        u64 call(GuestAddr fn) {
            const GuestAddr cs = t->kCs;
            cpu.init_long_mode();
            const GuestAddr teb = kTebs + 0x2000ull * index;
            cpu.seg.gs_desc.base = teb;
            const GuestAddr sp = kStacks + 0x1000ull * index + 0xF00;
            (void)t->memory.write_u64(sp, kRet);
            cpu.gpr.rsp = sp;
            cpu.gpr.rcx = cs;
            cpu.rip = fn;
            for (int hops = 0; hops < 64; ++hops) {
                if (cpu.rip == kRet) return cpu.gpr.rax;
                if (cpu.rip >= kSlowEnter && cpu.rip <= kHostLeave) {
                    // The export, entered by the tail-jump: the caller's frame
                    // and argument are intact; it returns to the caller.
                    const GuestAddr a = cpu.gpr.rcx;
                    if (cpu.rip == kSlowEnter) t->mgr->enter_slow(a);
                    else if (cpu.rip == kSlowLeave) t->mgr->leave_wake(a);
                    else if (cpu.rip == kHostEnter) t->mgr->enter(a);
                    else if (cpu.rip == kHostTryEnter) cpu.gpr.rax = t->mgr->try_enter(a) ? 1 : 0;
                    else t->mgr->leave(a);
                    cpu.rip = t->memory.read_u64(cpu.gpr.rsp).value_or(0);
                    cpu.gpr.rsp += 8;
                    continue;
                }
                auto& compiled = blocks[cpu.rip];
                if (compiled == nullptr) {
                    cpu::jit::BasicBlock block =
                        cli::decode_basic_block(t->memory, cpu.rip, /*is_64bit=*/true);
                    compiled = jit.compile(block);
                    if (compiled == nullptr) {
                        ADD_FAILURE() << "JIT declined 0x" << std::hex << cpu.rip;
                        return 0;
                    }
                }
                if (cpu::jit::test::execute_like_dispatcher(jit, *compiled, cpu) !=
                    cpu::jit::JitExecResult::Continue) {
                    ADD_FAILURE() << "block at 0x" << std::hex << cpu.rip << " stopped";
                    return 0;
                }
            }
            ADD_FAILURE() << "guest call did not return";
            return 0;
        }
    };

    void run();

    GuestMemory memory;
    CrtAccelBlob blob;
    std::unique_ptr<CriticalSectionManager> mgr;
};

}  // namespace

TEST_F(CsX64GuestStress, GuestAndHostHalvesExcludeAndNeverStrandAWaiter) {
    kCs = 0x00200000;
    ASSERT_FALSE(mgr->initialize(kCs).has_value());
    run();
}

// Windows permits a misaligned CRITICAL_SECTION (one kept in a byte array).
// The host keeps such a section in its own mutex, so the guest must hand
// every call on it to the host -- or guest and host callers both "own" it.
TEST_F(CsX64GuestStress, MisalignedSectionStillExcludes) {
    kCs = 0x00200052;  // LockCount at +0x5A: 2-byte aligned only
    ASSERT_FALSE(mgr->initialize(kCs).has_value());
    run();
}

void CsX64GuestStress::run() {
    const GuestAddr enter = entry("kernel32.dll!EnterCriticalSection");
    const GuestAddr try_enter = entry("kernel32.dll!TryEnterCriticalSection");
    const GuestAddr leave = entry("kernel32.dll!LeaveCriticalSection");
    ASSERT_NE(enter, 0u);
    ASSERT_NE(try_enter, 0u);
    ASSERT_NE(leave, 0u);

    std::atomic<int> inside{0};
    std::atomic<u32> overlaps{0};
    std::atomic<u32> slow_ops{0};
    long long counter = 0;  // guarded by the section alone
    std::atomic<bool> go{false};

    // An op the host's 50 ms re-check had to end: a release that should have
    // woken this waiter did not.
    constexpr auto kStall = std::chrono::milliseconds(40);
    const auto critical = [&] {
        if (inside.fetch_add(1, std::memory_order_acq_rel) != 0) overlaps.fetch_add(1);
        ++counter;
        inside.fetch_sub(1, std::memory_order_acq_rel);
    };

    std::vector<std::thread> threads;
    for (u32 i = 0; i < kGuestThreads; ++i) {
        threads.emplace_back([&, i] {
            GuestThread g(this, i);
            ThreadManager::set_current_thread_id(g.tid);
            (void)memory.write_u64(kTebs + 0x2000ull * i + 0x48, g.tid);
            while (!go.load(std::memory_order_acquire)) {}
            for (u32 k = 0; k < kIters; ++k) {
                const auto t0 = std::chrono::steady_clock::now();
                if ((k & 7) == 3) {
                    // TryEnter until it succeeds; it never waits.
                    while (g.call(try_enter) == 0) std::this_thread::yield();
                } else {
                    (void)g.call(enter);
                }
                if (std::chrono::steady_clock::now() - t0 > kStall) slow_ops.fetch_add(1);
                if ((k & 15) == 5) (void)g.call(enter);  // recursion by the owner
                critical();
                if ((k & 15) == 5) (void)g.call(leave);
                (void)g.call(leave);
            }
        });
    }
    // The host API on the same section, as SleepConditionVariableCS and
    // Vitreo-internal callers use it.
    threads.emplace_back([&] {
        ThreadManager::set_current_thread_id(0x900);
        while (!go.load(std::memory_order_acquire)) {}
        for (u32 k = 0; k < kIters; ++k) {
            const auto t0 = std::chrono::steady_clock::now();
            mgr->enter(kCs);
            if (std::chrono::steady_clock::now() - t0 > kStall) slow_ops.fetch_add(1);
            critical();
            mgr->leave(kCs);
        }
    });
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();

    EXPECT_EQ(overlaps.load(), 0u) << "two threads owned the section at once";
    EXPECT_EQ(counter, static_cast<long long>(kGuestThreads + 1) * kIters);
    EXPECT_EQ(slow_ops.load(), 0u) << "a waiter missed its wake";
    EXPECT_EQ(memory.read_u32(kCs + 0x08).value_or(0), 0xFFFFFFFFu) << "LockCount free";
    EXPECT_EQ(memory.read_u32(kCs + 0x0C).value_or(9), 0u) << "RecursionCount";
    EXPECT_EQ(memory.read_u64(kCs + 0x10).value_or(9), 0u) << "OwningThread";
}
