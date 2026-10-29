/// @file test_nt_wait_single.cpp
/// @brief NtWaitForSingleObject, x86 and x64, through the ntdll export.
///
/// Every status is the Windows measurement in
/// tools/oracle_probe/nt_wait_single_probe.expected (x64 and WoW64 agree).

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/synchronization/sync.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <winapi/system/ntdll/ntdll_ext.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kTimeout = 0x00700000;  // one LARGE_INTEGER
constexpr GuestAddr kStack = 0x00710800;
constexpr u32 kSuccess = 0x00000000;
constexpr u32 kTimedOut = 0x00000102;          // STATUS_TIMEOUT
constexpr u32 kUserApc = 0x000000C0;           // STATUS_USER_APC
constexpr u32 kInvalidHandle = 0xC0000008;     // STATUS_INVALID_HANDLE
constexpr u32 kAccessDenied = 0xC0000022;      // STATUS_ACCESS_DENIED
constexpr u32 kAccessViolation = 0xC0000005;   // STATUS_ACCESS_VIOLATION
constexpr u32 kNotImplemented = 0xC0000002;    // STATUS_NOT_IMPLEMENTED
constexpr u64 kNoTimeout = 0;                  // a NULL Timeout pointer
constexpr u32 kEventModifyState = 0x0002;
constexpr Handle kCurrentProcess = 0xFFFFFFFF;
constexpr Handle kCurrentThread = 0xFFFFFFFE;

class NtWaitSingleTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kTimeout, 0x20000,
                                      MemoryProtection::ReadWrite).is_ok());
        mgr.set_sync_manager(&sync);
        mgr.set_thread_manager(&threads);
        mgr.set_handle_table(&handles);
        exports = make_ntdll_ext_exports(mgr);
        ThreadManager::set_current_thread_id(threads.main_thread_id());
    }
    void TearDown() override {
        sync.set_alertable_apc_drain({});
        mgr.set_sync_manager(nullptr);
        mgr.set_thread_manager(nullptr);
        mgr.set_handle_table(nullptr);
    }

    bool is64() const { return GetParam(); }

    /// NtWaitForSingleObject(handle, alertable, timeout_ptr).
    u32 wait(u64 handle, bool alertable, u64 timeout_ptr) {
        const auto it = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [](const ExportEntry& e) { return e.name == "NtWaitForSingleObject"; });
        EXPECT_NE(it, exports.entries.end());
        if (it == exports.entries.end()) return 0xFFFFFFFFu;
        EXPECT_EQ(it->stdcall_bytes, 12u);
        cpu::CpuState cpu;
        if (is64()) {
            cpu.init_long_mode();
            cpu.gpr.rsp = kStack;
            cpu.gpr.rcx = handle;
            cpu.gpr.rdx = alertable ? 1u : 0u;
            cpu.gpr.r8 = timeout_ptr;
        } else {
            cpu.init_protected_mode();
            cpu.gpr.rsp = kStack;
            EXPECT_TRUE(memory.write_u32(kStack + 4, static_cast<u32>(handle)).is_ok());
            EXPECT_TRUE(memory.write_u32(kStack + 8, alertable ? 1u : 0u).is_ok());
            EXPECT_TRUE(memory.write_u32(kStack + 12, static_cast<u32>(timeout_ptr)).is_ok());
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        it->handler(m);
        return static_cast<u32>(cpu.gpr.rax);
    }
    u32 wait_for(u64 handle, bool alertable, i64 timeout) {
        EXPECT_TRUE(memory.write_u64(kTimeout, static_cast<u64>(timeout)).is_ok());
        return wait(handle, alertable, kTimeout);
    }

    GuestMemory memory;
    HandleTable handles;
    SyncManager sync{handles};
    ThreadManager threads{handles};
    NtdllExtManager mgr;
    DllExports exports;
};

INSTANTIATE_TEST_SUITE_P(Bitness, NtWaitSingleTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

TEST_P(NtWaitSingleTest, TimeoutConventions) {
    const Handle manual = sync.create_event(true, true, "");
    EXPECT_EQ(wait(manual, false, kNoTimeout), kSuccess);
    EXPECT_EQ(wait_for(manual, false, 0), kSuccess);
    ASSERT_TRUE(sync.reset_event(manual));
    EXPECT_EQ(wait_for(manual, false, 0), kTimedOut);

    // "unsignaled, 30 ms relative" waits the interval out.
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(wait_for(manual, false, -300000), kTimedOut);
    EXPECT_GE(std::chrono::steady_clock::now() - start,
              std::chrono::milliseconds(25));
    // "unsignaled, absolute 1 (past)" expires at once.
    EXPECT_EQ(wait_for(manual, false, 1), kTimedOut);
}

TEST_P(NtWaitSingleTest, WaitsAcquireWhatTheyWaitFor) {
    const Handle automatic = sync.create_event(false, true, "");
    EXPECT_EQ(wait_for(automatic, false, 0), kSuccess);
    EXPECT_EQ(wait_for(automatic, false, 0), kTimedOut);

    const Handle semaphore = sync.create_semaphore(1, 1, "");
    EXPECT_EQ(wait_for(semaphore, false, 0), kSuccess);
    EXPECT_EQ(wait_for(semaphore, false, 0), kTimedOut);

    const Handle mutex = sync.create_mutex(true, "");
    EXPECT_EQ(wait_for(mutex, false, 0), kSuccess);  // owned by the caller
    EXPECT_TRUE(sync.release_mutex(mutex));
    EXPECT_TRUE(sync.release_mutex(mutex));
}

TEST_P(NtWaitSingleTest, HandleFailures) {
    EXPECT_EQ(wait_for(0x1234, false, 0), kInvalidHandle);
    EXPECT_EQ(wait_for(0, false, 0), kInvalidHandle);
    // The pseudo handles name objects that are not signaled while alive.
    const u64 process = is64() ? ~u64{0} : kCurrentProcess;
    const u64 thread = is64() ? ~u64{1} : kCurrentThread;
    EXPECT_EQ(wait_for(process, false, 0), kTimedOut);
    EXPECT_EQ(wait_for(thread, false, 0), kTimedOut);

    // "signaled, no SYNCHRONIZE access".
    const Handle event = sync.create_event(true, true, "");
    const Handle modify_only = sync.duplicate_handle(event, 0, kEventModifyState);
    ASSERT_NE(modify_only, NULL_HANDLE);
    EXPECT_EQ(wait_for(modify_only, false, 0), kAccessDenied);

    // A native service leaves the thread's last error alone.
    set_last_error(1234);
    EXPECT_EQ(wait_for(0x1234, false, 0), kInvalidHandle);
    EXPECT_EQ(get_last_error(), 1234u);
}

TEST_P(NtWaitSingleTest, UnreadableTimeoutIsAccessViolation) {
    const Handle event = sync.create_event(true, true, "");
    EXPECT_EQ(wait(event, false, 1), kAccessViolation);
}

TEST_P(NtWaitSingleTest, AlertableWaitsRunQueuedApcsFirst) {
    std::atomic<int> pending{0};
    std::atomic<int> ran{0};
    sync.set_alertable_apc_drain([&] {
        if (pending.exchange(0) == 0) return false;
        ++ran;
        return true;
    });
    const Handle event = sync.create_event(true, false, "");
    pending = 1;
    EXPECT_EQ(wait_for(event, false, 0), kTimedOut);  // "nonalertable, apc queued"
    EXPECT_EQ(ran.load(), 0);
    EXPECT_EQ(wait_for(event, true, 0), kUserApc);    // "alertable, apc queued"
    EXPECT_EQ(ran.load(), 1);
    ASSERT_TRUE(sync.set_event(event));
    pending = 1;
    EXPECT_EQ(wait_for(event, true, 0), kUserApc);    // "... signaled"
    EXPECT_EQ(ran.load(), 2);
}

// A live handle the wait core cannot wait on fails loudly rather than as
// an invalid handle (Windows: a file is signaled, a key or section without
// SYNCHRONIZE is STATUS_ACCESS_DENIED).
TEST_P(NtWaitSingleTest, UnmodelledHandleKindsFailAsNotImplemented) {
    const Handle key = handles.allocate(HandleType::Registry, nullptr);
    ASSERT_NE(key, INVALID_HANDLE);
    EXPECT_EQ(wait_for(key, false, 0), kNotImplemented);
}

} // namespace
