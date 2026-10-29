/// @file test_nt_alert_wait.cpp
/// @brief NtWaitForAlertByThreadId, NtAlertThreadByThreadId and
///        NtDelayExecution, x86 and x64, through the ntdll exports.
///
/// The timeout convention is Learn's (ZwWaitForSingleObject `Timeout`: NULL =
/// none, negative = relative, positive = absolute system time that tracks
/// system-time changes, 100 ns units). Every status is the Windows
/// measurement in tools/oracle_probe/nt_alert_wait_probe.expected.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <winapi/kernel32/synchronization/sync.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <winapi/system/ntdll/nt_timeout.hpp>
#include <winapi/system/ntdll/ntdll_ext.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <capsule/vfs/vfs.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kTimeout = 0x00700000;    // LARGE_INTEGERs, 8 bytes each
constexpr GuestAddr kStacks = 0x00710000;     // 0x1000 per caller
constexpr u32 kAlerted = 0x00000101;          // STATUS_ALERTED
constexpr u32 kTimedOut = 0x00000102;         // STATUS_TIMEOUT
constexpr u32 kUserApc = 0x000000C0;          // STATUS_USER_APC
constexpr u32 kNoYield = 0x40000024;          // STATUS_NO_YIELD_PERFORMED
constexpr u32 kInvalidCid = 0xC000000B;       // STATUS_INVALID_CID
constexpr u32 kAccessViolation = 0xC0000005;  // STATUS_ACCESS_VIOLATION
constexpr i64 k100ms = 1000000;               // 100 ms in 100 ns units

using Clock = std::chrono::steady_clock;

std::atomic<i64> g_fake_now{0};
i64 fake_now() { return g_fake_now.load(); }

struct Timed {
    u32 status = 0xFFFFFFFFu;
    long long ms = 0;
};

class NtAlertWaitTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kTimeout, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kStacks, 0x10000, MemoryProtection::ReadWrite).is_ok());
        mgr.set_sync_manager(&sync);
        mgr.set_thread_manager(&threads);
        exports = make_ntdll_ext_exports(mgr);
        main_tid = threads.main_thread_id();
        ThreadManager::set_current_thread_id(main_tid);
    }
    void TearDown() override {
        nt_timeout::set_system_time_source_for_test(nullptr);
        mgr.set_sync_manager(nullptr);
        mgr.set_thread_manager(nullptr);
    }

    bool is64() const { return GetParam(); }

    u32 call(const char* name, std::initializer_list<u64> args, u32 slot = 0) {
        const auto it = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [&](const ExportEntry& e) { return e.name == name; });
        EXPECT_NE(it, exports.entries.end()) << name;
        if (it == exports.entries.end()) return 0xFFFFFFFFu;
        cpu::CpuState cpu;
        const GuestAddr sp = kStacks + 0x1000 * slot + 0x800;
        if (is64()) {
            cpu.init_long_mode();
            cpu.gpr.rsp = sp;
            const u64* a = args.begin();
            if (args.size() > 0) cpu.gpr.rcx = a[0];
            if (args.size() > 1) cpu.gpr.rdx = a[1];
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
        it->handler(m);
        return static_cast<u32>(cpu.gpr.rax);
    }

    /// Store `value` in LARGE_INTEGER slot `i`; returns its address.
    GuestAddr timeout(i64 value, u32 i = 0) {
        EXPECT_TRUE(memory.write_u64(kTimeout + 8 * i, static_cast<u64>(value)).is_ok());
        return kTimeout + 8 * i;
    }
    static i64 system_now() { return nt_timeout::system_time_100ns(); }

    Timed wait_alert(GuestAddr timeout_ptr, u32 slot = 0, GuestAddr address = 0x1234) {
        const auto t0 = Clock::now();
        Timed r;
        r.status = call("NtWaitForAlertByThreadId", {address, timeout_ptr}, slot);
        r.ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        return r;
    }
    Timed delay(bool alertable, GuestAddr timeout_ptr, u32 slot = 0) {
        const auto t0 = Clock::now();
        Timed r;
        r.status = call("NtDelayExecution", {alertable ? 1u : 0u, timeout_ptr}, slot);
        r.ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        return r;
    }

    /// A second guest thread (suspended; only its identity is used).
    u32 other_thread() {
        u32 tid = 0;
        const Handle h = threads.create_thread(0, 0x1000, 0, 0x4 /*CREATE_SUSPENDED*/, &tid);
        EXPECT_NE(h, INVALID_HANDLE);
        return tid;
    }

    /// Run NtWaitForAlertByThreadId as `tid` on a host thread; alert it after
    /// `alert_after` (if set) and, so a broken wait cannot hang the suite,
    /// again after `rescue_after`.
    Timed wait_on_thread(u32 tid, GuestAddr timeout_ptr,
                         std::chrono::milliseconds alert_after,
                         std::chrono::milliseconds rescue_after = std::chrono::seconds(4),
                         void (*during)() = nullptr) {
        std::atomic<bool> done{false};
        Timed r;
        std::thread worker([&] {
            ThreadManager::set_current_thread_id(tid);
            r = wait_alert(timeout_ptr, 1);
            done.store(true);
        });
        const auto t0 = Clock::now();
        bool alerted = false;
        bool ran_during = false;
        while (!done.load()) {
            const auto now = Clock::now() - t0;
            if (during != nullptr && !ran_during && now >= std::chrono::milliseconds(50)) {
                during();
                ran_during = true;
            }
            if (!alerted && alert_after.count() > 0 && now >= alert_after) {
                EXPECT_EQ(call("NtAlertThreadByThreadId", {tid}), 0u);
                alerted = true;
            }
            if (now >= rescue_after) {
                (void)call("NtAlertThreadByThreadId", {tid});
                ADD_FAILURE() << "the wait did not end by itself; rescued";
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        worker.join();
        return r;
    }

    GuestMemory memory;
    HandleTable handles;
    SyncManager sync{handles};
    ThreadManager threads{handles};
    NtdllExtManager mgr;
    DllExports exports;
    u32 main_tid = 0;
};

INSTANTIATE_TEST_SUITE_P(Bitness, NtAlertWaitTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

// "-- alert_semantics": an alert is kept until a wait consumes it, two alerts
// are one, NULL is no time-out and the address is only a hint.
TEST_P(NtAlertWaitTest, AlertIsKeptAndCoalesced) {
    EXPECT_EQ(wait_alert(timeout(0)).status, kTimedOut);
    EXPECT_EQ(call("NtAlertThreadByThreadId", {main_tid}), 0u);
    const Timed t = wait_alert(0);
    EXPECT_EQ(t.status, kAlerted);
    EXPECT_LT(t.ms, 100);
    EXPECT_EQ(call("NtAlertThreadByThreadId", {main_tid}), 0u);
    EXPECT_EQ(call("NtAlertThreadByThreadId", {main_tid}), 0u);
    EXPECT_EQ(wait_alert(timeout(0)).status, kAlerted);
    EXPECT_EQ(wait_alert(timeout(0)).status, kTimedOut);
    EXPECT_EQ(wait_alert(timeout(0), 0, /*address=*/0).status, kTimedOut);
    EXPECT_EQ(call("NtAlertThreadByThreadId", {main_tid}), 0u);
    EXPECT_EQ(wait_alert(timeout(0), 0, /*address=*/0).status, kAlerted);
}

// alert_tid_0 / alert_tid_unknown: STATUS_INVALID_CID, and nobody is alerted.
TEST_P(NtAlertWaitTest, AlertingNoThreadIsInvalidCid) {
    EXPECT_EQ(call("NtAlertThreadByThreadId", {0}), kInvalidCid);
    EXPECT_EQ(call("NtAlertThreadByThreadId", {0x7FFFFFFC}), kInvalidCid);
    EXPECT_EQ(wait_alert(timeout(0)).status, kTimedOut);
}

// alert_before_worker_waits / alert_wakes_null_timeout_waiter.
TEST_P(NtAlertWaitTest, AlertReachesAnotherThreadParkedOrNot) {
    const u32 tid = other_thread();
    EXPECT_EQ(call("NtAlertThreadByThreadId", {tid}), 0u);
    {
        std::thread t([&] {
            ThreadManager::set_current_thread_id(tid);
            EXPECT_EQ(wait_alert(timeout(0, 1), 1).status, kAlerted);
        });
        t.join();
    }
    const Timed r = wait_on_thread(tid, 0, std::chrono::milliseconds(100));
    EXPECT_EQ(r.status, kAlerted);
    EXPECT_LT(r.ms, 1000);
}

// Vitreo's own service threads run guest code under ids outside the thread
// table; they are threads of this process, so an alert must reach them
// (review finding 1). A registered id is alerted; an unregistered one is not.
TEST_P(NtAlertWaitTest, AlertReachesARegisteredServiceThread) {
    constexpr u32 kServiceTid = 0xF0000123u;
    EXPECT_EQ(call("NtAlertThreadByThreadId", {kServiceTid}), kInvalidCid);
    threads.add_service_thread(kServiceTid);
    EXPECT_EQ(call("NtAlertThreadByThreadId", {kServiceTid}), 0u);
    {
        std::thread t([&] {
            ThreadManager::set_current_thread_id(kServiceTid);
            EXPECT_EQ(wait_alert(timeout(0, 1), 1).status, kAlerted);
        });
        t.join();
    }
    threads.remove_service_thread(kServiceTid);
    EXPECT_EQ(call("NtAlertThreadByThreadId", {kServiceTid}), kInvalidCid);
}

// Review nit: an alert sent to a service id must not carry over to the next
// waiter that reuses the id (a re-armed TP_WAIT): like a new Windows thread
// it starts unalerted.
TEST_P(NtAlertWaitTest, ReusedServiceThreadIdStartsUnalerted) {
    threads.set_service_thread_id_callback(
        [this](u32 tid) { sync.discard_alert_wait_object(tid); });
    constexpr u32 kServiceTid = 0xE0000042u;
    threads.add_service_thread(kServiceTid);
    EXPECT_EQ(call("NtAlertThreadByThreadId", {kServiceTid}), 0u);   // never consumed
    threads.remove_service_thread(kServiceTid);
    threads.add_service_thread(kServiceTid);                          // re-armed
    std::thread t([&] {
        ThreadManager::set_current_thread_id(kServiceTid);
        EXPECT_EQ(wait_alert(timeout(0, 1), 1).status, kTimedOut)
            << "the previous holder's alert carried over";
    });
    t.join();
    // An alert to the live holder still sticks.
    EXPECT_EQ(call("NtAlertThreadByThreadId", {kServiceTid}), 0u);
    std::thread u([&] {
        ThreadManager::set_current_thread_id(kServiceTid);
        EXPECT_EQ(wait_alert(timeout(0, 1), 1).status, kAlerted);
    });
    u.join();
    threads.remove_service_thread(kServiceTid);
    threads.set_service_thread_id_callback({});
}

// The RegisterWaitForSingleObject waiter registers itself for as long as it
// runs, and a peer's alert reaches it.
TEST_P(NtAlertWaitTest, AlertReachesARegisterWaitWaiterThread) {
    capsule::Vfs vfs;
    Kernel32ExtManager k32(memory, vfs);
    k32.set_sync_manager(&sync);
    k32.set_thread_manager(&threads);
    const Handle event = sync.create_event(/*manual_reset=*/false,
                                           /*initial_state=*/false, "");
    ASSERT_NE(event, NULL_HANDLE);
    constexpr GuestAddr kNewWait = kTimeout + 0x100;
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStacks + 0x3800;
    cpu.gpr.rcx = kNewWait;
    cpu.gpr.rdx = event;
    cpu.gpr.r8 = 0x10000;   // a callback address; it never runs (no signal)
    cpu.gpr.r9 = 0;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 0xFFFFFFFFu).is_ok());  // INFINITE
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, 0).is_ok());           // flags
    Marshaler m(memory);
    m.attach_cpu(cpu);
    k32.impl_RegisterWaitForSingleObject(m);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    const u32 wait_handle = memory.read_u32(kNewWait).value();
    const u32 waiter_tid = 0xF0000000u | (wait_handle & 0x0FFFFFFFu);
    const auto until = Clock::now() + std::chrono::seconds(5);
    while (!threads.is_thread_of_this_process(waiter_tid) && Clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(threads.is_thread_of_this_process(waiter_tid));
    EXPECT_EQ(call("NtAlertThreadByThreadId", {waiter_tid}), 0u);

    cpu.gpr.rcx = wait_handle;
    k32.impl_UnregisterWait(m);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    const auto gone = Clock::now() + std::chrono::seconds(5);
    while (threads.is_thread_of_this_process(waiter_tid) && Clock::now() < gone) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_FALSE(threads.is_thread_of_this_process(waiter_tid))
        << "the waiter stays registered after it exited";
}

// wait_bad_timeout_ptr: the status, not an exception.
TEST_P(NtAlertWaitTest, UnreadableTimeoutIsAccessViolation) {
    EXPECT_EQ(wait_alert(0x10).status, kAccessViolation);
}

// "-- alert_timeouts".
TEST_P(NtAlertWaitTest, AbsoluteAndRelativeTimeouts) {
    struct Row { const char* name; i64 value; bool absolute_offset; bool slow; };
    const Row rows[] = {
        {"wait_abs_1", 1, false, false},
        {"wait_abs_past_1s", -10 * 10 * k100ms, true, false},
        {"wait_abs_future_300ms", 3 * k100ms, true, true},
        {"wait_rel_300ms", -3 * k100ms, false, true},
        {"wait_rel_1", -1, false, false},
    };
    for (const Row& row : rows) {
        const i64 v = row.absolute_offset ? system_now() + row.value : row.value;
        const Timed r = wait_on_thread(main_tid, timeout(v), std::chrono::milliseconds(0));
        EXPECT_EQ(r.status, kTimedOut) << row.name;
        if (row.slow) {
            EXPECT_GE(r.ms, 290) << row.name;
            EXPECT_LT(r.ms, 2000) << row.name;
        } else {
            EXPECT_LT(r.ms, 120) << row.name;
        }
    }
}

// wait_abs_future_10s, wait_abs_max, wait_rel_max, wait_int64_min: all of
// them wait, and an alert ends them.
TEST_P(NtAlertWaitTest, LongAndExtremeTimeoutsWaitForTheAlert) {
    const i64 values[] = {system_now() + 100 * k100ms, INT64_MAX, -INT64_MAX, INT64_MIN};
    for (i64 v : values) {
        const Timed r = wait_on_thread(main_tid, timeout(v), std::chrono::milliseconds(100));
        EXPECT_EQ(r.status, kAlerted) << v;
        EXPECT_GE(r.ms, 90) << v;
        EXPECT_LT(r.ms, 1000) << v;
    }
}

// "Absolute expiration times track any changes in the system time."
TEST_P(NtAlertWaitTest, AbsoluteTimeoutTracksTheSystemClock) {
    g_fake_now.store(system_now());
    nt_timeout::set_system_time_source_for_test(fake_now);
    const i64 deadline = g_fake_now.load() + 100 * k100ms;   // +10 s
    const Timed r = wait_on_thread(
        main_tid, timeout(deadline), std::chrono::milliseconds(0),
        std::chrono::seconds(4), [] { g_fake_now.fetch_add(200 * k100ms); });
    EXPECT_EQ(r.status, kTimedOut);
    EXPECT_LT(r.ms, 1000) << "the clock moved past the deadline at 50 ms";
}

// "-- delay_execution".
TEST_P(NtAlertWaitTest, DelayTimeoutsAndBadPointers) {
    // delay_zero: nothing else is ready to run.
    EXPECT_EQ(delay(false, timeout(0)).status, kNoYield);
    EXPECT_EQ(delay(false, 0).status, kAccessViolation);
    EXPECT_EQ(delay(false, 0x10).status, kAccessViolation);
    const Timed past1 = delay(false, timeout(1));
    EXPECT_EQ(past1.status, 0u);
    EXPECT_LT(past1.ms, 120);
    const Timed past = delay(false, timeout(system_now() - 100 * k100ms));
    EXPECT_EQ(past.status, 0u);
    EXPECT_LT(past.ms, 120);
    const Timed fut = delay(false, timeout(system_now() + 3 * k100ms));
    EXPECT_EQ(fut.status, 0u);
    EXPECT_GE(fut.ms, 290);
    EXPECT_LT(fut.ms, 2000);
    const Timed rel = delay(false, timeout(-3 * k100ms));
    EXPECT_EQ(rel.status, 0u);
    EXPECT_GE(rel.ms, 290);
    EXPECT_LT(rel.ms, 2000);
}

TEST_P(NtAlertWaitTest, AlertableDelayDeliversApcs) {
    std::atomic<int> pending{0};
    std::atomic<int> ran{0};
    sync.set_alertable_apc_drain([&] {
        if (pending.exchange(0) == 0) return false;
        ++ran;
        return true;
    });
    // delay_alertable_queued_apc / delay_zero_alertable_queued_apc.
    pending = 1;
    const Timed q = delay(true, timeout(-20 * k100ms));
    EXPECT_EQ(q.status, kUserApc);
    EXPECT_LT(q.ms, 120);
    EXPECT_EQ(ran.load(), 1);
    pending = 1;
    EXPECT_EQ(delay(true, timeout(0)).status, kUserApc);
    EXPECT_EQ(ran.load(), 2);
    // delay_nonalertable_queued_apc: the APC waits; the delay runs out.
    pending = 1;
    const Timed n = delay(false, timeout(-3 * k100ms));
    EXPECT_EQ(n.status, 0u);
    EXPECT_GE(n.ms, 290);
    EXPECT_EQ(ran.load(), 2);
    pending = 0;
    // delay_alertable_no_apc_100ms.
    const Timed none = delay(true, timeout(-k100ms));
    EXPECT_EQ(none.status, 0u);
    EXPECT_GE(none.ms, 90);
    // delay_rel_max_apc / delay_abs_max_apc: an APC queued while parked.
    for (i64 v : {-INT64_MAX, INT64_MAX, -20 * k100ms}) {
        Timed r;
        std::thread t([&] {
            ThreadManager::set_current_thread_id(main_tid);
            r = delay(true, timeout(v, 1), 1);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        pending = 1;
        sync.note_apc_queued(main_tid);
        t.join();
        EXPECT_EQ(r.status, kUserApc) << v;
        EXPECT_LT(r.ms, 1000) << v;
    }
    sync.set_alertable_apc_drain({});
}

// delay_alertable_then_alert_by_tid: an alert does not end a delay, and it
// is still there for the next alert wait.
TEST_P(NtAlertWaitTest, AlertDoesNotEndADelayButIsKept) {
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_EQ(call("NtAlertThreadByThreadId", {main_tid}, 2), 0u);
    });
    const Timed d = delay(true, timeout(-3 * k100ms));
    t.join();
    EXPECT_EQ(d.status, 0u);
    EXPECT_GE(d.ms, 290);
    EXPECT_EQ(wait_alert(timeout(0)).status, kAlerted);
}

TEST_P(NtAlertWaitTest, AbsoluteDelayTracksTheSystemClock) {
    g_fake_now.store(system_now());
    nt_timeout::set_system_time_source_for_test(fake_now);
    std::thread t([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        g_fake_now.fetch_add(200 * k100ms);
    });
    const Timed d = delay(false, timeout(g_fake_now.load() + 100 * k100ms));
    t.join();
    EXPECT_EQ(d.status, 0u);
    EXPECT_LT(d.ms, 1000);
}

} // namespace
