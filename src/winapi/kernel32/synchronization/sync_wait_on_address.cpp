// SPDX-License-Identifier: LGPL-2.1-only

/// @file sync_wait_on_address.cpp
/// @brief WaitOnAddress / WakeByAddress* (Win8+ futex).
///
/// Closed cut from sync.cpp — verified with plan_extraction.py.

#include <os/os_sync_wait_on_address.h>
#include <mach/mach.h>
extern "C" {
// The SDK header lacks C linkage guards.
#include <mach/thread_state.h>
}
#include <vector>
#include "sync.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/common/perf_diag.hpp"
#include "winapi/common/timer_resolution.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/sync/ntsync.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"
#include "ipc/named_sync.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace vitreo::winapi {

// =============================================================================
// WaitOnAddress / WakeByAddress* (Win8+ address wait)
// =============================================================================
//
// Learn WaitOnAddress: "If the values are the same, the function does not
// return until another thread in the same process signals that the value at
// Address has changed by calling WakeByAddressSingle or WakeByAddressAll or
// the timeout elapses"; "WaitOnAddress is guaranteed to return when the
// address is signaled". WakeByAddressSingle: "If multiple threads are waiting
// for this address, the system wakes the first thread to wait."
//
// Each waiter is a queue entry on its EXACT address, and it parks on a host
// word of its own, never on the guest's. A waker unlinks the first (or every)
// entry under the shard lock, sets its `signaled` word and wakes that word. A
// wake can therefore not be lost in the window between the waiter's last
// compare and its kernel park: the waiter registered before that compare, so
// the waker finds it, and a park that starts after the wake sees `signaled`
// already set and returns at once. (Parking on the guest word, as before,
// lost exactly that wake whenever the waker had not changed the value.)
// FIFO order per address, and size independence (a 1-byte and a 4-byte
// waiter on one address are both its waiters), fall out of the same queue
// (wait_on_address_probe single_fifo / mixed_size_*). A store without a
// wake never ends a parked wait (store_without_wake), which Learn allows
// and Windows does.

namespace {

struct AddressWaiter {
    GuestAddr address = 0;
    /// The guest thread waiting (cancel_wait_on_address_for_thread).
    u32 thread_id = 0;
    /// The word this waiter parks on: 0 while queued, nonzero once it has
    /// been unlinked -- kWoken by a waker, kCancelled because its thread is
    /// being terminated. Written only under the shard lock.
    std::atomic<u32> signaled{0};
    AddressWaiter* prev = nullptr;
    AddressWaiter* next = nullptr;
};

constexpr u32 kWoken = 1;
constexpr u32 kCancelled = 2;

struct AddressQueue {
    AddressWaiter* head = nullptr;
    AddressWaiter* tail = nullptr;
};

struct WaiterShard {
    std::mutex mutex;
    std::unordered_map<GuestAddr, AddressQueue> queues;
};
constexpr usize kWaiterShards = 64;
std::array<WaiterShard, kWaiterShards>& waiter_shards() {
    static auto* shards = new std::array<WaiterShard, kWaiterShards>();
    return *shards;
}
[[nodiscard]] WaiterShard& shard_for(GuestAddr address) {
    // Fibonacci hashing: neighbouring addresses land in different shards.
    const u64 h = static_cast<u64>(address) * 0x9E3779B97F4A7C15ull;
    return waiter_shards()[(h >> 58) % kWaiterShards];
}

void enqueue_locked(WaiterShard& shard, AddressWaiter& w) {
    AddressQueue& q = shard.queues[w.address];
    w.prev = q.tail;
    w.next = nullptr;
    if (q.tail != nullptr) q.tail->next = &w; else q.head = &w;
    q.tail = &w;
}

void unlink_locked(WaiterShard& shard, AddressWaiter& w) {
    auto it = shard.queues.find(w.address);
    if (it == shard.queues.end()) return;
    AddressQueue& q = it->second;
    if (w.prev != nullptr) w.prev->next = w.next; else q.head = w.next;
    if (w.next != nullptr) w.next->prev = w.prev; else q.tail = w.prev;
    w.prev = w.next = nullptr;
    if (q.head == nullptr) shard.queues.erase(it);
}

/// Unlink and signal the first waiter (all = false) or every waiter on
/// `address`; returns how many. The kernel wakes happen after the shard lock
/// is dropped and use each word's address only as the key the parked thread
/// waits under: a woken waiter may already have returned and released its
/// word, and a wake on a key nobody waits under does nothing (another waiter
/// that later parks at the same stack address treats it as spurious).
usize wake_address(GuestAddr address, bool all) {
    WaiterShard& shard = shard_for(address);
    std::array<void*, 16> inline_keys{};
    std::vector<void*> more_keys;
    usize woken = 0;
    {
        std::lock_guard lock(shard.mutex);
        auto it = shard.queues.find(address);
        if (it == shard.queues.end()) return 0;
        // Publish the waker's stores (the value it changed) before the
        // waiter can observe `signaled`.
        std::atomic_thread_fence(std::memory_order_release);
        AddressWaiter* w = it->second.head;
        while (w != nullptr) {
            AddressWaiter* next = w->next;
            w->prev = w->next = nullptr;
            void* key = &w->signaled;
            w->signaled.store(kWoken, std::memory_order_release);
            if (woken < inline_keys.size()) inline_keys[woken] = key;
            else more_keys.push_back(key);
            ++woken;
            w = next;
            if (!all) break;
        }
        if (w == nullptr) {
            shard.queues.erase(it);
        } else {
            it->second.head = w;
            w->prev = nullptr;
        }
    }
    for (usize i = 0; i < woken; ++i) {
        void* key = i < inline_keys.size() ? inline_keys[i]
                                           : more_keys[i - inline_keys.size()];
        (void)os_sync_wake_by_address_any(key, sizeof(u32),
                                          OS_SYNC_WAKE_BY_ADDRESS_NONE);
    }
    return woken;
}

std::atomic<WaitOnAddressParkHook> g_park_hook{nullptr};
std::atomic<WaitOnAddressParkHook> g_registered_hook{nullptr};

void run_park_hook(GuestAddr address) {
    if (auto hook = g_park_hook.load(std::memory_order_relaxed)) hook(address);
}

void run_registered_hook(GuestAddr address) {
    if (auto hook = g_registered_hook.load(std::memory_order_relaxed)) hook(address);
}

/// The fault Windows takes reading an unreadable Address or CompareAddress.
/// With no CPU to raise on, FALSE / ERROR_NOACCESS.
void raise_read_fault(Marshaler& m, GuestAddr address) {
    if (!m.raise_access_violation(/*write=*/false, address)) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
    }
}

} // namespace

void set_wait_on_address_park_hook(WaitOnAddressParkHook hook) noexcept {
    g_park_hook.store(hook, std::memory_order_relaxed);
}

void set_wait_on_address_registered_hook(WaitOnAddressParkHook hook) noexcept {
    g_registered_hook.store(hook, std::memory_order_relaxed);
}

usize cancel_wait_on_address_for_thread(u32 thread_id) {
    // Windows removes a terminated thread's wait block, so its entry must
    // not stay at the head of a FIFO and absorb a WakeByAddressSingle meant
    // for a live waiter. Termination is a rare, slow operation: every shard
    // is scanned.
    usize cancelled = 0;
    for (WaiterShard& shard : waiter_shards()) {
        std::vector<void*> keys;
        {
            std::lock_guard lock(shard.mutex);
            std::vector<AddressWaiter*> mine;
            for (auto& [address, queue] : shard.queues) {
                (void)address;
                for (AddressWaiter* w = queue.head; w != nullptr; w = w->next) {
                    if (w->thread_id == thread_id) mine.push_back(w);
                }
            }
            for (AddressWaiter* w : mine) {
                unlink_locked(shard, *w);
                keys.push_back(&w->signaled);
                w->signaled.store(kCancelled, std::memory_order_release);
            }
        }
        // As in wake_address: the words are kernel keys only from here on.
        for (void* key : keys) {
            (void)os_sync_wake_by_address_any(key, sizeof(u32),
                                              OS_SYNC_WAKE_BY_ADDRESS_NONE);
        }
        cancelled += keys.size();
    }
    return cancelled;
}

void impl_WaitOnAddress(Marshaler& m) {
    // BOOL WaitOnAddress(volatile VOID* Address, PVOID CompareAddress,
    //                    SIZE_T AddressSize, DWORD dwMilliseconds)
    const GuestAddr address = m.arg_ptr(0);
    const GuestAddr compare_addr = m.arg_ptr(1);
    // SIZE_T: on x64 the whole register is the size.
    const u64 address_size = m.cpu_is_64bit() ? m.arg(2)
                                              : static_cast<u32>(m.arg(2));
    const u32 timeout_ms = static_cast<u32>(m.arg(3));

    // "This parameter can be 1, 2, 4, or 8": anything else is
    // ERROR_INVALID_PARAMETER, checked before either pointer is read
    // (wait_on_address_probe size_3_differs, bad_size_null_address_child).
    if (address_size != 1 && address_size != 2 && address_size != 4 &&
        address_size != 8) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const usize size = static_cast<usize>(address_size);
    // Address is read first, then CompareAddress; either unreadable is an
    // access violation (null_address_child, null_compare_child,
    // both_bad_child faults at Address).
    std::array<u8, 8> current{};
    if (m.memory().read_bytes(address, current.data(), size).is_err()) {
        raise_read_fault(m, address);
        return;
    }
    std::array<u8, 8> expected{};
    if (m.memory().read_bytes(compare_addr, expected.data(), size).is_err()) {
        raise_read_fault(m, compare_addr);
        return;
    }
    auto value_differs = [&]() {
        std::array<u8, 8> now{};
        if (m.memory().read_bytes(address, now.data(), size).is_err()) return true;
        return std::memcmp(now.data(), expected.data(), size) != 0;
    };
    // A differing value returns TRUE at once, last error untouched.
    if (std::memcmp(current.data(), expected.data(), size) != 0) {
        m.set_return(1);
        return;
    }
    if (timeout_ms == 0) {
        set_last_error(error_codes::ERROR_TIMEOUT);
        m.set_return(0);
        return;
    }

    const bool infinite = (timeout_ms == INFINITE_WAIT);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(infinite ? 0 : timeout_ms);

    AddressWaiter self;
    self.address = address;
    self.thread_id = ThreadManager::get_current_thread_id_static();
    WaiterShard& shard = shard_for(address);
    {
        std::lock_guard lock(shard.mutex);
        enqueue_locked(shard, self);
    }
    run_registered_hook(address);
    // TerminateThread that landed before this entry existed: its exit
    // callback's cancel_wait_on_address_for_thread found nothing to remove.
    // The terminator raises the flag before that callback runs and this
    // thread enqueued under a shard lock the cancel also takes, so either the
    // cancel saw this entry or this read sees the flag. Treated as cancelled:
    // the park ends and the run loop unwinds the thread.
    if (ThreadSuspendRegistry::current_thread_terminating()) {
        std::lock_guard lock(shard.mutex);
        if (self.signaled.load(std::memory_order_acquire) == 0) {
            unlink_locked(shard, self);
        }
        m.set_return(1);
        return;
    }
    // The last compare comes after registration: a waker that stored its
    // value before this read is seen here; one that stores after it finds
    // this waiter queued and signals it.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (value_differs()) {
        std::lock_guard lock(shard.mutex);
        if (self.signaled.load(std::memory_order_acquire) == 0) {
            unlink_locked(shard, self);
        }
        m.set_return(1);
        return;
    }
    run_park_hook(address);

    for (;;) {
        if (self.signaled.load(std::memory_order_acquire) != 0) {
            m.set_return(1);
            return;
        }
        int rc;
        if (infinite) {
            rc = os_sync_wait_on_address(&self.signaled, 0, sizeof(u32),
                                         OS_SYNC_WAIT_ON_ADDRESS_NONE);
        } else {
            const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) break;
            rc = os_sync_wait_on_address_with_timeout(
                &self.signaled, 0, sizeof(u32), OS_SYNC_WAIT_ON_ADDRESS_NONE,
                OS_CLOCK_MACH_ABSOLUTE_TIME, static_cast<u64>(left));
        }
        if (rc < 0 && errno == ETIMEDOUT) break;
        // Woken, interrupted or spurious: the signaled check decides.
    }
    // Timed out -- unless a waker unlinked this waiter meanwhile, in which
    // case its wake was for this thread and the wait succeeded.
    {
        std::lock_guard lock(shard.mutex);
        if (self.signaled.load(std::memory_order_acquire) != 0) {
            m.set_return(1);
            return;
        }
        unlink_locked(shard, self);
    }
    set_last_error(error_codes::ERROR_TIMEOUT);
    m.set_return(0);
}

void impl_WakeByAddressSingle(Marshaler& m) {
    // VOID WakeByAddressSingle(PVOID Address): the first waiter on exactly
    // this address; with none, nothing (a later waiter is not woken:
    // wake_before_wait_not_kept). NULL is harmless (wake_null_child).
    const GuestAddr address = m.arg_ptr(0);
    const usize woken = wake_address(address, /*all=*/false);
    ipc::ipc_log("WakeByAddressSingle addr=0x%llx had_waiter=%d",
                 static_cast<unsigned long long>(address), woken != 0 ? 1 : 0);
    m.set_return(0);
}

void impl_WakeByAddressAll(Marshaler& m) {
    // VOID WakeByAddressAll(PVOID Address): every waiter on exactly this
    // address, whatever compare size it used.
    (void)wake_address(m.arg_ptr(0), /*all=*/true);
    m.set_return(0);
}

// Learn: FlushProcessWriteBuffers "flushes the write queue of each processor
// that is running a thread of the current process ... It guarantees the
// visibility of write operations performed on one processor to the other
// processors." Windows does it with an IPI. macOS has no user-mode IPI and no
// membarrier(2); the mprotect/TLB-shootdown trick is not sufficient on arm64,
// where TLB maintenance is broadcast by TLBI and interrupts no remote core.
//
// Mechanism: reading another thread's register state requires the kernel to
// stop that thread (XNU thread_get_state/thread_get_register_pointer_values
// call thread_stop(), which returns only once the target is off-CPU). Taking a
// thread off a core goes through the kernel context switch, which is fully
// serialising and drains that core's store buffer; the thread resumes after
// the caller's own seq_cst fence. So every thread of the task passes a full
// barrier between the caller's pre-call and post-call accesses — the
// asymmetric-Dekker guarantee the API exists for. Guest threads are host
// threads of this task, so enumerating the task covers all of them; host-only
// threads are a harmless superset.
//
// Measured (asymmetric store-buffering litmus, fast side with no barrier,
// Apple Silicon): caller-local fence only (the previous implementation) 59
// forbidden outcomes in 2,000,000 rounds; this mechanism 0 in 200,000 (the
// old rate predicts ~6). Cost: ~15-160 us per call for 1-24 threads.
u32 flush_process_write_buffers() noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    const mach_port_t task = mach_task_self();
    if (task_threads(task, &threads, &count) != KERN_SUCCESS) {
        // Cannot enumerate: there is no weaker substitute that still honours
        // the contract, so say so loudly (the API has no failure return).
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!FlushProcessWriteBuffers: "
                                 "task_threads failed; only a local barrier was issued\n");
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return 0;
    }
    const thread_act_t self = mach_thread_self();
    u32 serialised = 0;
    std::vector<uintptr_t> values(128);
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        if (threads[i] != self) {
            uintptr_t sp = 0;
            size_t length = values.size();
            kern_return_t kr = thread_get_register_pointer_values(
                threads[i], &sp, &length, values.data());
            if (kr == KERN_INSUFFICIENT_BUFFER_SIZE) {
                values.resize(length);
                length = values.size();
                kr = thread_get_register_pointer_values(threads[i], &sp, &length,
                                                        values.data());
            }
            // A thread that exited meanwhile has nothing left to flush.
            if (kr == KERN_SUCCESS) ++serialised;
        }
        (void)mach_port_deallocate(task, threads[i]);
    }
    (void)mach_port_deallocate(task, self);
    (void)vm_deallocate(task, reinterpret_cast<vm_address_t>(threads),
                        count * sizeof(thread_act_t));
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return serialised;
}

} // namespace vitreo::winapi
