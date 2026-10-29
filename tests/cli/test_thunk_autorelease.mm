// SPDX-License-Identifier: LGPL-2.1-only
//
// Every guest->host API crossing drains what its host call autoreleased.
//
// Contract (Apple, Advanced Memory Management Programming Guide, "Using
// Autorelease Pool Blocks"; objc runtime objc_autoreleasePoolPush/Pop): a
// thread Cocoa did not create -- every guest thread -- must create its own
// pools; an object autoreleased inside a pool is released when that pool is
// popped, not before; pools nest and are popped strictly LIFO, and popping a
// token releases everything autoreleased since its push.
//
// So a host call made through the real thunk boundary must leave nothing it
// autoreleased alive once the guest's next instruction runs, on x86 and x64,
// on the full dispatcher path and on the direct crossing, whatever way the
// host call leaves (return, control transfer, unwind, raise), and a guest
// callback nested inside a host call drains its own calls without touching
// the outer call's objects.

#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "runtime/execution/guest_run_loop.hpp"
#include "runtime/execution/thunk_direct.hpp"
#include "winapi/core/com_direct.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"

#import <Foundation/Foundation.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <vector>

#if __has_feature(objc_arc)
#error "test_thunk_autorelease.mm sends -autorelease explicitly (MRC)"
#endif

extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* context);

namespace {
std::atomic<int> g_deallocs{0};
}  // namespace

@interface VitreoThunkAutoreleaseProbe : NSObject
@end

@implementation VitreoThunkAutoreleaseProbe
- (void)dealloc {
    g_deallocs.fetch_add(1, std::memory_order_relaxed);
    [super dealloc];
}
@end

using namespace vitreo;
using namespace vitreo::cli;

namespace {

constexpr GuestAddr kMemBase = 0x20000;
constexpr usize kMemSize = 0x10000;
constexpr GuestAddr kThunkBase = 0x70000000;
constexpr GuestAddr kOuterCode = kMemBase + 0x1000;
constexpr GuestAddr kNestedCode = kMemBase + 0x2000;
constexpr GuestAddr kContinuation = kMemBase + 0x3000;
constexpr GuestAddr kWarmCode = kMemBase + 0x4000;
constexpr GuestAddr kSp = kMemBase + 0x8000;

int g_autoreleased = 0;
std::vector<int> g_seen;  // g_deallocs at each Check, in call order
GuestMemory* g_memory = nullptr;
winapi::ThunkRegistry* g_registry = nullptr;
winapi::ProcessManager* g_process = nullptr;
GuestAddr g_transfer_rip = 0;

void autorelease_one() {
    [[[VitreoThunkAutoreleaseProbe alloc] init] autorelease];
    ++g_autoreleased;
}

void check(winapi::Marshaler&) { g_seen.push_back(g_deallocs.load()); }

/// A host call that autoreleases, then runs guest code (a window procedure,
/// an enumeration callback) that itself crosses into the host, then looks
/// at what is still alive before returning.
void nested(winapi::Marshaler& m) {
    autorelease_one();
    cpu::CpuState& cpu = *m.cpu();
    bool completed = false;
    (void)run_guest_callback(cpu, *g_memory, *g_registry, *g_process,
                             kNestedCode, nullptr, 0, cpu.is_64bit(),
                             &completed);
    EXPECT_TRUE(completed);
    g_seen.push_back(g_deallocs.load());
}

/// longjmp-shaped: the host call moves rip itself and drops its return
/// address, so the dispatcher emulates no RET.
void transfer(winapi::Marshaler& m) {
    autorelease_one();
    cpu::CpuState& cpu = *m.cpu();
    cpu.set_sp(cpu.sp() + (cpu.is_64bit() ? 8u : 4u));
    cpu.rip = g_transfer_rip;
    cpu.thunk_control_transfer = true;
}

enum class DirectExit : u8 { Return, Transfer, Unwind, Raise };
DirectExit g_direct_exit = DirectExit::Return;

void direct_autorelease(void*, winapi::Marshaler& m, u32) {
    autorelease_one();
    cpu::CpuState& cpu = *m.cpu();
    switch (g_direct_exit) {
    case DirectExit::Return: break;
    case DirectExit::Transfer:
        cpu.rip = 0x140007777ull;
        cpu.thunk_control_transfer = true;
        break;
    case DirectExit::Unwind: cpu.pending_unwind.active = true; break;
    case DirectExit::Raise: cpu.pending_raise.active = true; break;
    }
}

/// Guest code: `call <gateway>` for each name in turn, then `ret`.
std::vector<u8> calls(bool is64, std::initializer_list<GuestAddr> gateways) {
    std::vector<u8> code;
    for (GuestAddr gw : gateways) {
        if (is64) {
            code.insert(code.end(), {0x48, 0xB8});  // mov rax, imm64
            for (int i = 0; i < 8; ++i) code.push_back(static_cast<u8>(gw >> (8 * i)));
        } else {
            code.push_back(0xB8);                   // mov eax, imm32
            for (int i = 0; i < 4; ++i) code.push_back(static_cast<u8>(gw >> (8 * i)));
        }
        code.insert(code.end(), {0xFF, 0xD0});      // call rax / eax
    }
    code.push_back(0xC3);                           // ret
    return code;
}

/// Guest code: `mov ecx, n ; L: push rcx ; mov rax, gw ; call rax ; pop rcx ;
/// dec ecx ; jnz L ; ret` (the same bytes in 32-bit mode with eax/ecx).
std::vector<u8> loop_calls(bool is64, GuestAddr gw, u32 n) {
    std::vector<u8> code{0xB9};
    for (int i = 0; i < 4; ++i) code.push_back(static_cast<u8>(n >> (8 * i)));
    const usize top = code.size();
    code.push_back(0x51);
    if (is64) {
        code.insert(code.end(), {0x48, 0xB8});
        for (int i = 0; i < 8; ++i) code.push_back(static_cast<u8>(gw >> (8 * i)));
    } else {
        code.push_back(0xB8);
        for (int i = 0; i < 4; ++i) code.push_back(static_cast<u8>(gw >> (8 * i)));
    }
    code.insert(code.end(), {0xFF, 0xD0, 0x59, 0xFF, 0xC9, 0x75});
    code.push_back(static_cast<u8>(static_cast<i8>(
        static_cast<long>(top) - static_cast<long>(code.size() + 1))));
    code.push_back(0xC3);
    return code;
}

/// An explicit pool the test owns around every run: anything a crossing
/// failed to drain is still alive until this pops, and a crossing that
/// popped past its own token would release @c sentinel early.
class TestPool {
public:
    TestPool() : m_token(objc_autoreleasePoolPush()) {
        [[[VitreoThunkAutoreleaseProbe alloc] init] autorelease];
    }
    ~TestPool() { objc_autoreleasePoolPop(m_token); }

private:
    void* m_token;
};

class ThunkAutoreleaseTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        g_deallocs.store(0);
        g_autoreleased = 0;
        g_seen.clear();
        g_direct_exit = DirectExit::Return;
        ASSERT_TRUE(memory.map_region(kMemBase, kMemSize,
                                      MemoryProtection::ReadWrite).is_ok());
        winapi::DllExports exports;
        exports.entries.push_back(winapi::ExportEntry{
            "Autorelease", 1, [](winapi::Marshaler&) { autorelease_one(); }});
        exports.entries.push_back(winapi::ExportEntry{"Check", 2, &check});
        exports.entries.push_back(winapi::ExportEntry{"Nested", 3, &nested});
        exports.entries.push_back(winapi::ExportEntry{"Transfer", 4, &transfer});
        exports.entries.push_back(winapi::ExportEntry{
            "Trivial", 5, [](winapi::Marshaler& m) { m.set_return(1); }});
        exports.entries.push_back(winapi::ExportEntry{
            "TrivialExtraPool", 8, [](winapi::Marshaler& m) {
                objc_autoreleasePoolPop(objc_autoreleasePoolPush());
                m.set_return(1);
            }});
        exports.entries.push_back(winapi::make_direct_export(
            "DirectAutorelease",
            winapi::DirectHandler{&direct_autorelease, nullptr, 6}, 0));
        exports.entries.push_back(winapi::make_direct_export(
            "DirectTrivial",
            winapi::DirectHandler{[](void*, winapi::Marshaler& m, u32) {
                                      m.set_return(1);
                                  },
                                  nullptr, 7},
            0));
        ASSERT_TRUE(registry.register_dll("probe.dll", std::move(exports))
                        .is_ok());
        g_memory = &memory;
        g_registry = &registry;
        g_process = &process_mgr;
        // The callback loop's first use on a thread resolves its fast-thunk
        // set: setup, not a crossing.
        const u8 ret = 0xC3;
        ASSERT_TRUE(memory.write_bytes(kWarmCode, &ret, 1).is_ok());
        cpu::CpuState warm;
        init(warm);
        warm.set_sp(kMemBase + 0x6000);
        (void)run_guest_callback(warm, memory, registry, process_mgr,
                                 kWarmCode, nullptr, 0, is64());
    }
    void TearDown() override {
        winapi::com_direct::force_for_test(winapi::com_direct::Force::None);
        g_memory = nullptr;
        g_registry = nullptr;
        g_process = nullptr;
    }

    [[nodiscard]] bool is64() const { return GetParam(); }
    void init(cpu::CpuState& cpu) const {
        if (is64()) cpu.init_long_mode(); else cpu.init_protected_mode();
    }
    GuestAddr gateway(const char* name) {
        const auto entry = registry.get_thunk_address("probe.dll", name);
        EXPECT_TRUE(entry.is_ok()) << name;
        return entry.value() + winapi::ThunkRegistry::STUB_SIZE;
    }
    void write(GuestAddr at, const std::vector<u8>& code) {
        ASSERT_TRUE(memory.write_bytes(at, code.data(), code.size()).is_ok());
    }
    bool run(GuestAddr code) {
        cpu::CpuState cpu;
        init(cpu);
        cpu.set_sp(kSp);
        bool completed = false;
        (void)run_guest_callback(cpu, memory, registry, process_mgr, code,
                                 nullptr, 0, is64(), &completed);
        return completed;
    }

    GuestMemory memory;
    winapi::ThunkRegistry registry{kThunkBase};
    winapi::ProcessManager process_mgr;
};

}  // namespace

TEST_P(ThunkAutoreleaseTest, HostCallDrainsBeforeTheGuestContinues) {
    write(kOuterCode, calls(is64(), {gateway("Autorelease"), gateway("Check"),
                                     gateway("Autorelease"), gateway("Check")}));
    {
        TestPool pool;
        ASSERT_TRUE(run(kOuterCode));
        // Each object is gone by the guest's next instruction; the test's
        // own pool (and its sentinel) is untouched.
        EXPECT_EQ(g_seen, (std::vector<int>{1, 2}));
        EXPECT_EQ(g_deallocs.load(), g_autoreleased);
    }
    EXPECT_EQ(g_deallocs.load(), g_autoreleased + 1);  // the sentinel
}

TEST_P(ThunkAutoreleaseTest, NestedGuestCallbackDrainsOnlyItsOwnCalls) {
    // Outer: call Nested ; call Check. Nested's callback: call Autorelease ;
    // call Check.
    write(kOuterCode, calls(is64(), {gateway("Nested"), gateway("Check")}));
    write(kNestedCode, calls(is64(), {gateway("Autorelease"), gateway("Check")}));
    {
        TestPool pool;
        ASSERT_TRUE(run(kOuterCode));
        ASSERT_EQ(g_seen.size(), 3u);
        // Inner crossing drained its object; the outer call's own object is
        // in the outer crossing's pool and survives the callback...
        EXPECT_EQ(g_seen[0], 1);
        EXPECT_EQ(g_seen[1], 1);
        // ...until the outer crossing returns.
        EXPECT_EQ(g_seen[2], 2);
        EXPECT_EQ(g_deallocs.load(), g_autoreleased);
    }
    EXPECT_EQ(g_deallocs.load(), g_autoreleased + 1);
}

TEST_P(ThunkAutoreleaseTest, ControlTransferDrainsAndStaysBalanced) {
    // call Transfer (never returns here) ; continuation: call Check ; ret.
    g_transfer_rip = kContinuation;
    write(kOuterCode, calls(is64(), {gateway("Transfer"), gateway("Check")}));
    write(kContinuation, calls(is64(), {gateway("Check")}));
    {
        TestPool pool;
        ASSERT_TRUE(run(kOuterCode));
        EXPECT_EQ(g_seen, (std::vector<int>{1}));
        EXPECT_EQ(g_deallocs.load(), g_autoreleased);
    }
    EXPECT_EQ(g_deallocs.load(), g_autoreleased + 1);
}

TEST_P(ThunkAutoreleaseTest, DirectCrossingDrainsOnEveryExit) {
    winapi::com_direct::force_for_test(winapi::com_direct::Force::On);
    const GuestAddr gw = gateway("DirectAutorelease");
    ASSERT_TRUE(memory.write_u64(kSp, 0x140009999ull).is_ok());
    TestPool pool;
    int expected = 0;
    for (DirectExit exit : {DirectExit::Return, DirectExit::Transfer,
                            DirectExit::Unwind, DirectExit::Raise}) {
        g_direct_exit = exit;
        cpu::CpuState cpu;
        init(cpu);
        cpu.rip = gw;
        cpu.set_sp(kSp);
        winapi::Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        winapi::ThreadSuspendRegistry::PublishGuard guard(0x7201, &cpu);
        (void)run_direct_thunk(*registry.direct_thunk(gw), cpu, memory,
                               marshaler, guard, nullptr);
        ++expected;
        EXPECT_EQ(g_deallocs.load(), expected)
            << "exit " << static_cast<int>(exit);
    }
}

TEST_P(ThunkAutoreleaseTest, CrossingCost) {
    // Not a pass/fail gate: ns per trivial crossing, full path and direct,
    // for the record in .docs/rv-there-yet-bringup.md. "extra pool" is the
    // same crossing with one more push+pop inside the handler: the same-build
    // A/B of what one boundary pool costs a realistic crossing.
    constexpr u32 kCalls = 200'000;
    constexpr GuestAddr kOtherCode = kMemBase + 0x5000;
    const auto time_run = [&](GuestAddr code) {
        const auto t0 = std::chrono::steady_clock::now();
        EXPECT_TRUE(run(code));
        return std::chrono::duration<double, std::nano>(
                   std::chrono::steady_clock::now() - t0)
                   .count() / kCalls;
    };
    const auto measure = [&](GuestAddr gw) {
        write(kOuterCode, loop_calls(is64(), gw, kCalls));
        double best = 1e30;
        for (int rep = 0; rep < 5; ++rep) best = std::min(best, time_run(kOuterCode));
        return best;
    };
    write(kOuterCode, loop_calls(is64(), gateway("Trivial"), kCalls));
    write(kOtherCode, loop_calls(is64(), gateway("TrivialExtraPool"), kCalls));
    double full = 1e30;
    double extra = 1e30;
    for (int rep = 0; rep < 9; ++rep) {  // interleaved: load drifts equally
        full = std::min(full, time_run(kOuterCode));
        extra = std::min(extra, time_run(kOtherCode));
    }
    std::fprintf(stderr,
                 "[thunk-autorelease] x%d: full %.1f ns, full + extra pool "
                 "%.1f ns (+%.1f%%)\n",
                 is64() ? 64 : 86, full, extra, 100.0 * (extra - full) / full);
    winapi::com_direct::force_for_test(winapi::com_direct::Force::On);
    const double direct = measure(gateway("DirectTrivial"));
    double pool_ns = 1e30;
    for (int rep = 0; rep < 5; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        for (u32 i = 0; i < kCalls; ++i) {
            objc_autoreleasePoolPop(objc_autoreleasePoolPush());
        }
        pool_ns = std::min(pool_ns,
                           std::chrono::duration<double, std::nano>(
                               std::chrono::steady_clock::now() - t0)
                                   .count() / kCalls);
    }
    std::fprintf(stderr,
                 "[thunk-autorelease] x%d: full %.1f ns/crossing, direct "
                 "%.1f ns/crossing, bare push+pop %.1f ns\n",
                 is64() ? 64 : 86, full, direct, pool_ns);
}

INSTANTIATE_TEST_SUITE_P(Bitness, ThunkAutoreleaseTest, ::testing::Bool(),
                         [](const auto& info) {
                             return info.param ? "x64" : "x86";
                         });
