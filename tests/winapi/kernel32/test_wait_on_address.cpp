/// @file test_wait_on_address.cpp
/// @brief WaitOnAddress / WakeByAddressSingle / WakeByAddressAll, x86 and x64.
///
/// Expected values are Learn's (WaitOnAddress: "guaranteed to return when the
/// address is signaled"; WakeByAddressSingle: "If multiple threads are waiting
/// for this address, the system wakes the first thread to wait"; AddressSize
/// "can be 1, 2, 4, or 8") and the Windows measurements in
/// tools/oracle_probe/wait_on_address_probe.expected.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/synchronization/sync.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <winapi/kernel32/thread/thread_suspend.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kData = 0x00600000;    // values waited on
constexpr GuestAddr kCompare = 0x00601000; // compare values, 8 bytes per slot
constexpr GuestAddr kStacks = 0x00610000;  // 0x1000 per caller
constexpr u32 kStackSlots = 64;
constexpr u32 kErrorTimeout = 0x5B4;       // winerror.h ERROR_TIMEOUT
constexpr u32 kInfinite = 0xFFFFFFFFu;

using Clock = std::chrono::steady_clock;

struct Result {
    u32 ret = 0xFFFFFFFFu;
    u32 last_error = 0;
    bool raised = false;
    u32 code = 0;
    u64 access = 0;
    u64 address = 0;
    std::chrono::milliseconds elapsed{0};
};

// The park hook: counts arrivals; while `hold` is set, holds the waiter in
// the window between its last compare and its park.
struct ParkGate {
    std::mutex mu;
    std::condition_variable cv;
    u32 arrived = 0;
    bool hold = false;
};
ParkGate g_gate;
void park_hook(GuestAddr) {
    std::unique_lock lk(g_gate.mu);
    ++g_gate.arrived;
    g_gate.cv.notify_all();
    g_gate.cv.wait(lk, [] { return !g_gate.hold; });
}
// The same, between joining the queue and the last compare.
ParkGate g_registered_gate;
void registered_hook(GuestAddr) {
    std::unique_lock lk(g_registered_gate.mu);
    ++g_registered_gate.arrived;
    g_registered_gate.cv.notify_all();
    g_registered_gate.cv.wait(lk, [] { return !g_registered_gate.hold; });
}
void set_hold(ParkGate& gate, bool hold) {
    {
        std::lock_guard lk(gate.mu);
        gate.hold = hold;
    }
    gate.cv.notify_all();
}

class WaitOnAddressContractTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kData, 0x2000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kStacks, 0x1000 * kStackSlots,
                                      MemoryProtection::ReadWrite).is_ok());
        for (ParkGate* gate : {&g_gate, &g_registered_gate}) {
            std::lock_guard lk(gate->mu);
            gate->arrived = 0;
            gate->hold = false;
        }
        set_wait_on_address_park_hook(park_hook);
        set_wait_on_address_registered_hook(registered_hook);
    }
    void TearDown() override {
        set_hold(g_gate, false);
        set_hold(g_registered_gate, false);
        set_wait_on_address_park_hook(nullptr);
        set_wait_on_address_registered_hook(nullptr);
        ThreadManager::set_current_thread_id(1);
    }
    void wait_registered(u32 n) {
        std::unique_lock lk(g_registered_gate.mu);
        ASSERT_TRUE(g_registered_gate.cv.wait_for(
            lk, std::chrono::seconds(5),
            [&] { return g_registered_gate.arrived >= n; }))
            << "waiter never joined the queue";
    }

    bool is64() const { return GetParam(); }

    void prepare(cpu::CpuState& cpu, u32 slot, std::initializer_list<u64> args) {
        const GuestAddr sp = kStacks + 0x1000 * slot + 0x800;
        if (is64()) {
            cpu.init_long_mode();
            cpu.gpr.rsp = sp;
            const u64* a = args.begin();
            if (args.size() > 0) cpu.gpr.rcx = a[0];
            if (args.size() > 1) cpu.gpr.rdx = a[1];
            if (args.size() > 2) cpu.gpr.r8 = a[2];
            if (args.size() > 3) cpu.gpr.r9 = a[3];
        } else {
            cpu.init_protected_mode();
            cpu.gpr.rsp = sp;
            u32 off = 4;
            for (u64 v : args) {
                EXPECT_TRUE(memory.write_u32(sp + off, static_cast<u32>(v)).is_ok());
                off += 4;
            }
        }
    }

    Result wait(GuestAddr address, GuestAddr compare, u64 size, u32 ms, u32 slot = 0) {
        cpu::CpuState cpu;
        prepare(cpu, slot, {address, compare, size, ms});
        Marshaler m(memory);
        m.attach_cpu(cpu);
        set_last_error(0xDEAD);
        const auto t0 = Clock::now();
        impl_WaitOnAddress(m);
        Result r;
        r.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
        r.ret = static_cast<u32>(cpu.gpr.rax);
        r.last_error = get_last_error();
        r.raised = cpu.pending_raise.active;
        r.code = cpu.pending_raise.code;
        r.access = cpu.pending_raise.args[0];
        r.address = cpu.pending_raise.args[1];
        return r;
    }
    void wake(GuestAddr address, bool all, u32 slot = kStackSlots - 1) {
        cpu::CpuState cpu;
        prepare(cpu, slot, {address});
        Marshaler m(memory);
        m.attach_cpu(cpu);
        if (all) impl_WakeByAddressAll(m); else impl_WakeByAddressSingle(m);
    }
    /// Compare slot `i` holds a copy of the `size` bytes at `address`.
    GuestAddr same_as(GuestAddr address, u32 i = 0) {
        u64 v = memory.read_u64(address & ~GuestAddr{7}).value();
        const GuestAddr cmp = kCompare + 8 * i;
        for (u32 b = 0; b < 8; ++b) {
            EXPECT_TRUE(memory.write_u8(cmp + b, memory.read_u8(address + b).value_or(0)).is_ok());
        }
        (void)v;
        return cmp;
    }
    void wait_arrivals(u32 n) {
        std::unique_lock lk(g_gate.mu);
        ASSERT_TRUE(g_gate.cv.wait_for(lk, std::chrono::seconds(5),
                                       [&] { return g_gate.arrived >= n; }))
            << "waiter never reached its park";
    }

    GuestMemory memory;
};

INSTANTIATE_TEST_SUITE_P(Bitness, WaitOnAddressContractTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

// The lost wake: a WakeByAddressSingle that changes nothing, landing after
// the waiter's last compare and before its park, must still end the wait.
TEST_P(WaitOnAddressContractTest, WakeBetweenCompareAndParkIsNotLost) {
    ASSERT_TRUE(memory.write_u32(kData, 7).is_ok());
    const GuestAddr cmp = same_as(kData);
    {
        std::lock_guard lk(g_gate.mu);
        g_gate.hold = true;
    }
    Result r;
    std::thread waiter([&] { r = wait(kData, cmp, 4, 3000); });
    wait_arrivals(1);                 // held exactly in the window
    wake(kData, /*all=*/false);       // no value change
    {
        std::lock_guard lk(g_gate.mu);
        g_gate.hold = false;
    }
    g_gate.cv.notify_all();
    waiter.join();
    EXPECT_EQ(r.ret, 1u) << "the wake was lost; the wait ran to its timeout";
    EXPECT_LT(r.elapsed.count(), 1000);
}

TEST_P(WaitOnAddressContractTest, WakeAllBetweenCompareAndParkIsNotLost) {
    ASSERT_TRUE(memory.write_u32(kData, 7).is_ok());
    const GuestAddr cmp = same_as(kData);
    {
        std::lock_guard lk(g_gate.mu);
        g_gate.hold = true;
    }
    Result r;
    std::thread waiter([&] { r = wait(kData, cmp, 4, 3000); });
    wait_arrivals(1);
    wake(kData, /*all=*/true);
    {
        std::lock_guard lk(g_gate.mu);
        g_gate.hold = false;
    }
    g_gate.cv.notify_all();
    waiter.join();
    EXPECT_EQ(r.ret, 1u);
    EXPECT_LT(r.elapsed.count(), 1000);
}

// wait_on_address_probe.expected "-- validation": the size is checked before
// either pointer is read (size_3_differs, bad_size_null_address_child); an
// unreadable Address or CompareAddress is an access violation, and Address
// is read first (both_bad_child faults at 0x20, not 0x10).
TEST_P(WaitOnAddressContractTest, ValidationMatchesWindows) {
    ASSERT_TRUE(memory.write_u32(kData, 5).is_ok());
    ASSERT_TRUE(memory.write_u32(kCompare, 6).is_ok());   // differs
    for (u64 size : {u64{0}, u64{3}, u64{5}, u64{16}}) {
        const Result r = wait(kData, kCompare, size, 0);
        EXPECT_FALSE(r.raised) << size;
        EXPECT_EQ(r.ret, 0u) << size;
        EXPECT_EQ(r.last_error, error_codes::ERROR_INVALID_PARAMETER) << size;
    }
    if (is64()) {
        // SIZE_T: the high half is part of the size.
        const Result r = wait(kData, kCompare, 0x100000004ull, 0);
        EXPECT_EQ(r.ret, 0u);
        EXPECT_EQ(r.last_error, error_codes::ERROR_INVALID_PARAMETER);
    }
    {
        const Result r = wait(0, kCompare, 3, 0);
        EXPECT_FALSE(r.raised);
        EXPECT_EQ(r.last_error, error_codes::ERROR_INVALID_PARAMETER);
    }
    struct Row { GuestAddr address; GuestAddr compare; u64 fault; };
    const Row rows[] = {{0, kCompare, 0}, {kData, 0, 0}, {0x10, kCompare, 0x10},
                        {kData, 0x10, 0x10}, {0x20, 0x10, 0x20}};
    for (const Row& row : rows) {
        const Result r = wait(row.address, row.compare, 4, 0);
        EXPECT_TRUE(r.raised) << row.address << "/" << row.compare;
        EXPECT_EQ(r.code, 0xC0000005u);
        EXPECT_EQ(r.access, 0u) << "a read";
        EXPECT_EQ(r.address, row.fault) << row.address << "/" << row.compare;
    }
    // WakeByAddress*(NULL) is harmless (wake_null_child).
    wake(0, false);
    wake(0, true);
}

// "-- compare": only AddressSize bytes are compared; misalignment is fine;
// a zero or elapsed timeout is FALSE / ERROR_TIMEOUT; a differing value is
// TRUE with the last error untouched.
TEST_P(WaitOnAddressContractTest, CompareUsesOnlyAddressSizeBytes) {
    ASSERT_TRUE(memory.write_u64(kData, 0x1122334455667788ull).is_ok());
    ASSERT_TRUE(memory.write_u64(kCompare, 0x1122334455667788ull ^ 0x100ull).is_ok());
    ASSERT_TRUE(memory.write_u64(kCompare + 8,
                                 0x1122334455667788ull ^ 0xFF00000000000000ull).is_ok());
    struct Row { GuestAddr cmp; u64 size; u32 ret; u32 err; };
    const Row rows[] = {
        {kCompare, 1, 0, kErrorTimeout}, {kCompare, 2, 1, 0xDEAD},
        {kCompare, 4, 1, 0xDEAD}, {kCompare, 8, 1, 0xDEAD},
        {kCompare + 8, 4, 0, kErrorTimeout}, {kCompare + 8, 8, 1, 0xDEAD},
    };
    for (const Row& row : rows) {
        const Result r = wait(kData, row.cmp, row.size, 0);
        EXPECT_EQ(r.ret, row.ret) << row.size;
        EXPECT_EQ(r.last_error, row.err) << row.size;
    }
    const GuestAddr same = same_as(kData);
    const Result r300 = wait(kData, same, 8, 300);
    EXPECT_EQ(r300.ret, 0u);
    EXPECT_EQ(r300.last_error, kErrorTimeout);
    EXPECT_GE(r300.elapsed.count(), 290);

    for (u32 i = 0; i < 16; ++i) ASSERT_TRUE(memory.write_u8(kData + 0x100 + i, u8(i)).is_ok());
    for (u32 i = 0; i < 4; ++i) ASSERT_TRUE(memory.write_u8(kCompare + 16 + i, u8(i + 1)).is_ok());
    const Result u = wait(kData + 0x101, kCompare + 16, 4, 0);
    EXPECT_EQ(u.ret, 0u);
    EXPECT_EQ(u.last_error, kErrorTimeout);
    ASSERT_TRUE(memory.write_u8(kCompare + 19, 9).is_ok());
    const Result d = wait(kData + 0x101, kCompare + 16, 4, 0);
    EXPECT_EQ(d.ret, 1u);
    EXPECT_EQ(d.last_error, 0xDEADu);
}

// wake_before_wait_not_kept: a wake with nobody waiting is not remembered.
TEST_P(WaitOnAddressContractTest, WakeWithNoWaiterIsNotRemembered) {
    ASSERT_TRUE(memory.write_u32(kData, 3).is_ok());
    const GuestAddr cmp = same_as(kData);
    wake(kData, false);
    wake(kData, true);
    const Result r = wait(kData, cmp, 4, 300);
    EXPECT_EQ(r.ret, 0u);
    EXPECT_EQ(r.last_error, kErrorTimeout);
    EXPECT_GE(r.elapsed.count(), 290);
}

// single_fifo, mixed_size_wake_single: the first thread to wait is woken,
// whatever size it compared.
TEST_P(WaitOnAddressContractTest, WakeSingleWakesTheFirstWaiter) {
    ASSERT_TRUE(memory.write_u32(kData, 9).is_ok());
    const GuestAddr cmp = same_as(kData);
    const u64 sizes[3] = {1, 4, 2};
    std::atomic<int> order{0};
    std::atomic<int> got[3] = {0, 0, 0};
    Result res[3];
    std::vector<std::thread> threads;
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back([&, i] {
            res[i] = wait(kData, cmp, sizes[i], 5000, u32(i));
            got[i] = ++order;
        });
        wait_arrivals(u32(i + 1));   // registration order is i
    }
    wake(kData, false);
    const auto until = Clock::now() + std::chrono::seconds(2);
    while (order.load() < 1 && Clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(got[0], 1);
    EXPECT_EQ(got[1], 0);
    EXPECT_EQ(got[2], 0);
    wake(kData, false);
    while (order.load() < 2 && Clock::now() < until + std::chrono::seconds(2)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(got[1], 2);
    EXPECT_EQ(got[2], 0);
    wake(kData, true);
    for (auto& t : threads) t.join();
    for (const Result& r : res) EXPECT_EQ(r.ret, 1u);
}

// mixed_size_wake_all, exact_address_neighbour_not_woken, store_without_wake.
TEST_P(WaitOnAddressContractTest, WakeAllWakesEveryWaiterOnTheExactAddressOnly) {
    ASSERT_TRUE(memory.write_u64(kData, 0).is_ok());
    const GuestAddr cmp = same_as(kData);
    const GuestAddr cmp_n = same_as(kData + 1, 1);
    Result a, b, n;
    std::thread ta([&] { a = wait(kData, cmp, 4, 3000, 0); });
    wait_arrivals(1);
    std::thread tb([&] { b = wait(kData, cmp, 1, 3000, 1); });
    wait_arrivals(2);
    std::thread tn([&] { n = wait(kData + 1, cmp_n, 1, 400, 2); });
    wait_arrivals(3);
    wake(kData, true);
    wake(kData, false);
    ta.join();
    tb.join();
    tn.join();
    EXPECT_EQ(a.ret, 1u);
    EXPECT_EQ(b.ret, 1u);
    EXPECT_LT(a.elapsed.count(), 1000);
    EXPECT_LT(b.elapsed.count(), 1000);
    EXPECT_EQ(n.ret, 0u) << "a wake names an exact address";
    EXPECT_EQ(n.last_error, kErrorTimeout);
}

// Review finding 9: a wake landing between the waiter joining the queue and
// its last compare. With the value unchanged the waiter goes on to park and
// must find itself already woken; with the value changed it takes the early
// return and must not leave a stale entry behind (a later waiter is still
// the one the next single wake reaches).
TEST_P(WaitOnAddressContractTest, WakeBetweenEnqueueAndLastCompareIsKept) {
    ASSERT_TRUE(memory.write_u32(kData, 4).is_ok());
    const GuestAddr cmp = same_as(kData);
    set_hold(g_registered_gate, true);
    Result r;
    std::thread waiter([&] { r = wait(kData, cmp, 4, 3000); });
    wait_registered(1);
    wake(kData, /*all=*/false);           // no value change
    set_hold(g_registered_gate, false);
    waiter.join();
    EXPECT_EQ(r.ret, 1u);
    EXPECT_LT(r.elapsed.count(), 1000);
}

TEST_P(WaitOnAddressContractTest, StoreAndWakeBetweenEnqueueAndLastCompareLeaveNoStaleEntry) {
    ASSERT_TRUE(memory.write_u32(kData, 4).is_ok());
    const GuestAddr cmp = same_as(kData);
    set_hold(g_registered_gate, true);
    Result first;
    std::thread waiter([&] { first = wait(kData, cmp, 4, 3000, 0); });
    wait_registered(1);
    ASSERT_TRUE(memory.write_u32(kData, 5).is_ok());
    wake(kData, /*all=*/false);
    set_hold(g_registered_gate, false);
    waiter.join();
    EXPECT_EQ(first.ret, 1u);
    // A new waiter on the new value: the next single wake must reach it.
    const GuestAddr cmp2 = same_as(kData, 1);
    Result second;
    std::thread late([&] { second = wait(kData, cmp2, 4, 3000, 1); });
    wait_arrivals(1);    // parked (the first waiter never reached its park)
    wake(kData, /*all=*/false);
    late.join();
    EXPECT_EQ(second.ret, 1u);
    EXPECT_LT(second.elapsed.count(), 1000) << "a stale entry took the wake";
}

// Review finding 5: a thread terminated while parked leaves the queue, so the
// next single wake reaches the live waiter behind it (Windows removes a
// terminated thread's wait block).
TEST_P(WaitOnAddressContractTest, TerminatedWaiterLeavesTheQueue) {
    ASSERT_TRUE(memory.write_u32(kData, 8).is_ok());
    const GuestAddr cmp = same_as(kData);
    constexpr u32 kDoomed = 0x501;
    constexpr u32 kLive = 0x502;
    Result doomed;
    Result live;
    std::thread a([&] {
        ThreadManager::set_current_thread_id(kDoomed);
        doomed = wait(kData, cmp, 4, kInfinite, 0);
    });
    wait_arrivals(1);
    std::thread b([&] {
        ThreadManager::set_current_thread_id(kLive);
        live = wait(kData, cmp, 4, 3000, 1);
    });
    wait_arrivals(2);
    EXPECT_EQ(cancel_wait_on_address_for_thread(kDoomed), 1u);
    a.join();                               // its park ended
    wake(kData, /*all=*/false);             // FIFO head is now the live waiter
    b.join();
    EXPECT_EQ(live.ret, 1u);
    EXPECT_LT(live.elapsed.count(), 1000) << "the terminated waiter absorbed the wake";
    EXPECT_EQ(cancel_wait_on_address_for_thread(kDoomed), 0u);
}

// Review nit 3: TerminateThread that lands before the waiter joins the queue
// (its cancel found nothing). The waiter sees its own termination right after
// enqueueing and does not park. Self-termination raises the flag at once.
TEST_P(WaitOnAddressContractTest, ThreadTerminatedBeforeEnqueueDoesNotPark) {
    ASSERT_TRUE(memory.write_u32(kData, 6).is_ok());
    const GuestAddr cmp = same_as(kData);
    const u32 tid = is64() ? 0x7EAD0064u : 0x7EAD0032u;   // unique per run
    Result r;
    std::thread t([&] {
        ThreadManager::set_current_thread_id(tid);
        cpu::CpuState live;
        auto guard = ThreadSuspendRegistry::instance().publish(tid, &live);
        ThreadSuspendRegistry::instance().request_terminate(tid);   // self
        r = wait(kData, cmp, 4, 2000);
    });
    t.join();
    EXPECT_EQ(r.ret, 1u);
    EXPECT_LT(r.elapsed.count(), 500) << "a terminating thread parked";
    EXPECT_EQ(g_gate.arrived, 0u) << "it reached its park";
}

TEST_P(WaitOnAddressContractTest, StoreWithoutWakeDoesNotEndAParkedWait) {
    ASSERT_TRUE(memory.write_u32(kData, 1).is_ok());
    const GuestAddr cmp = same_as(kData);
    Result r;
    std::thread t([&] { r = wait(kData, cmp, 4, 400); });
    wait_arrivals(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_TRUE(memory.write_u32(kData, 2).is_ok());
    t.join();
    EXPECT_EQ(r.ret, 0u);
    EXPECT_EQ(r.last_error, kErrorTimeout);
}

// Many waiters and wakers. Four addresses, one per compare size (1/2/4/8),
// each with six waiters and its own producer. Round k: the producer waits
// until every waiter has acknowledged round k-1, stores k, then wakes them
// -- either WakeByAddressAll, or WakeByAddressSingle once per waiter (FIFO:
// each single wake must reach a different queued waiter), sometimes with
// extra wakes that change nothing. A waiter waits until the value reaches
// its round (modulo its size). Nobody wakes anyone twice, so a lost wake leaves its
// waiter parked until its 5 s timeout, which the test counts as a failure,
// and the producer's own 10 s wait for the acknowledgements fails the round.
// Some waiters also take short timeouts that may legally expire; they must
// then report FALSE / ERROR_TIMEOUT and simply wait again.
TEST_P(WaitOnAddressContractTest, StressManyWaitersAndWakers) {
    set_wait_on_address_park_hook(nullptr);
    set_wait_on_address_registered_hook(nullptr);
    constexpr u32 kAddrs = 4;
    constexpr u32 kWaitersPerAddr = 6;
    constexpr u32 kRounds = 2000;  // values wrap on the small sizes; rounds compare equal
    const u64 sizes[kAddrs] = {1, 2, 4, 8};
    GuestAddr addrs[kAddrs];
    std::atomic<u32> acks[kAddrs];
    for (u32 a = 0; a < kAddrs; ++a) {
        addrs[a] = kData + 0x40 * a;   // separate words
        ASSERT_TRUE(memory.write_u64(addrs[a], 0).is_ok());
        acks[a] = 0;
    }
    std::atomic<u32> lost{0};
    std::atomic<u32> short_timeouts{0};
    std::atomic<u32> bad_short_timeouts{0};
    std::atomic<u32> producer_stalls{0};
    std::atomic<u32> waits{0};
    std::vector<std::thread> threads;
    for (u32 a = 0; a < kAddrs; ++a) {
        for (u32 w = 0; w < kWaitersPerAddr; ++w) {
            const u32 slot = a * kWaitersPerAddr + w;
            threads.emplace_back([&, a, w, slot] {
                const GuestAddr cmp = kCompare + 8 * slot;
                const bool short_waits = (w % 3) == 0;
                for (u32 k = 1; k <= kRounds; ++k) {
                    for (;;) {
                        const u64 seen = memory.read_u64(addrs[a]).value() &
                                         (sizes[a] == 8 ? ~u64{0}
                                                        : (u64{1} << (8 * sizes[a])) - 1);
                        const u64 mask = sizes[a] == 8 ? ~u64{0} : (u64{1} << (8 * sizes[a])) - 1;
                        if (seen == (k & mask)) break;   // round k reached
                        EXPECT_TRUE(memory.write_u64(cmp, seen).is_ok());
                        ++waits;
                        const Result r = wait(addrs[a], cmp, sizes[a],
                                              short_waits ? 1 : 5000, slot);
                        if (r.ret == 0) {
                            if (!short_waits) { ++lost; return; }
                            ++short_timeouts;
                            if (r.last_error != kErrorTimeout) ++bad_short_timeouts;
                        }
                    }
                    acks[a].fetch_add(1);
                }
            });
        }
        threads.emplace_back([&, a] {
            std::mt19937 rng(a);
            const u32 slot = kAddrs * kWaitersPerAddr + a;
            cpu::CpuState cpu;
            prepare(cpu, slot, {addrs[a]});
            Marshaler m(memory);
            m.attach_cpu(cpu);
            for (u32 k = 1; k <= kRounds; ++k) {
                const auto until = Clock::now() + std::chrono::seconds(10);
                while (acks[a].load() < kWaitersPerAddr * (k - 1)) {
                    if (Clock::now() > until) { ++producer_stalls; return; }
                    std::this_thread::yield();
                }
                EXPECT_TRUE(memory.write_u64(addrs[a], k).is_ok());
                if ((rng() % 2) == 0) {
                    impl_WakeByAddressAll(m);
                } else {
                    for (u32 i = 0; i < kWaitersPerAddr; ++i) impl_WakeByAddressSingle(m);
                }
                if ((rng() % 4) == 0) impl_WakeByAddressSingle(m);   // changes nothing
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(lost.load(), 0u) << "a waiter ran into its timeout: a wake was lost";
    EXPECT_EQ(producer_stalls.load(), 0u);
    EXPECT_EQ(bad_short_timeouts.load(), 0u);
    EXPECT_GT(waits.load(), kRounds) << "the waiters hardly ever waited";
    for (u32 a = 0; a < kAddrs; ++a) {
        EXPECT_EQ(acks[a].load(), kWaitersPerAddr * kRounds) << a;
    }
}

} // namespace
