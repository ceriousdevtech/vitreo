// SPDX-License-Identifier: LGPL-2.1-only
/// @file named_sync.cpp

#include <cstdlib>
#include <os/os_sync_wait_on_address.h>
#include "ipc/named_sync.hpp"
#include "ipc/shared_memory.hpp"
#include "core/sync/ntsync.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>
#include <vector>
#include <libproc.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <unistd.h>

namespace vitreo::ipc {

// Control block laid out in shared memory. All mutable state is atomic so the
// object is consumed/released without any cross-process lock (robust to a peer
// dying mid-operation). `magic` gates initialization so a non-creator waits
// until the creator has populated the immutable fields.
struct SharedSyncObject::ControlBlock {
    std::atomic<std::uint32_t> magic;        // 0 until initialized
    std::uint32_t              kind;         // SyncKind
    std::uint32_t              manual_reset; // event only
    std::int32_t               sem_max;      // semaphore only
    std::atomic<std::int32_t>  state;        // event:0/1, sem:count, mutex:0=free/1=held/2=transition
    std::atomic<std::uint32_t> owner_pid;    // mutex
    std::atomic<std::uint32_t> owner_tid;    // mutex
    std::atomic<std::uint32_t> recursion;    // mutex recursion depth
    std::atomic<std::uint32_t> abandoned;    // mutex: next acquisition reports abandonment
    /// Bumped by every state change a waiter could be waiting for, and the
    /// word waiters park on with the kernel's shared address wait. A waiter
    /// reads it, re-checks the object, and sleeps until it changes, so a
    /// peer's SetEvent wakes it directly instead of at the next poll. The
    /// block is one page; appending keeps every older field in place.
    std::atomic<std::uint32_t> wake_gen;
};

namespace {
constexpr std::uint32_t kMagic = 0x56544d53u;  // 'VTMS'
constexpr usize kBlockBytes = 4096;            // one page, room for the block

[[nodiscard]] bool process_is_live(std::uint32_t pid) {
    if (pid == 0) return false;
    // Darwin: kill(pid, 0) succeeds for zombies. Windows reports the mutex
    // abandoned once the owning process has terminated, even if the host
    // has not reaped that pid yet.
    proc_bsdinfo info {};
    const int n = ::proc_pidinfo(static_cast<pid_t>(pid), PROC_PIDTBSDINFO, 0,
                                 &info, static_cast<int>(sizeof(info)));
    if (n == static_cast<int>(sizeof(info))) {
        return info.pbi_status != SZOMB;
    }
    if (::kill(static_cast<pid_t>(pid), 0) == 0) return false;
    return errno == EPERM;
}
}

void SharedSyncObject::wake_shared(ControlBlock* c) noexcept {
    c->wake_gen.fetch_add(1, std::memory_order_acq_rel);
    (void)os_sync_wake_by_address_all(&c->wake_gen, sizeof(c->wake_gen),
                                      OS_SYNC_WAKE_BY_ADDRESS_SHARED);
}

SharedSyncObject::ControlBlock* SharedSyncObject::cb() noexcept {
    return static_cast<ControlBlock*>(m_map);
}

const SharedSyncObject::ControlBlock* SharedSyncObject::cb() const noexcept {
    return static_cast<const ControlBlock*>(m_map);
}

std::shared_ptr<SharedSyncObject> SharedSyncObject::create_or_open(
    const std::string& win_name, SyncKind kind,
    std::int32_t a, std::int32_t b, bool create) {
    // Distinct host namespace from file-mappings to avoid name collisions.
    std::string keyed = "ntsync:" + win_name;
    auto shm = SharedMemory::create_or_open(keyed, kBlockBytes, create);
    if (!shm) return nullptr;

    auto obj = std::shared_ptr<SharedSyncObject>(new SharedSyncObject());
    obj->m_shm = shm;
    obj->m_map_len = kBlockBytes;
    obj->m_map = shm->map_view(0, kBlockBytes);
    if (obj->m_map == nullptr) return nullptr;
    obj->m_kind = kind;
    obj->m_was_created = shm->is_creator();

    ControlBlock* c = obj->cb();
    if (shm->is_creator()) {
        // Fresh shm is zero-filled. Populate then publish via magic.
        c->kind = static_cast<std::uint32_t>(kind);
        c->owner_pid.store(0, std::memory_order_relaxed);
        c->owner_tid.store(0, std::memory_order_relaxed);
        c->recursion.store(0, std::memory_order_relaxed);
        c->abandoned.store(0, std::memory_order_relaxed);
        switch (kind) {
            case SyncKind::Event:
                c->manual_reset = (a != 0) ? 1u : 0u;
                c->state.store(b != 0 ? 1 : 0, std::memory_order_relaxed);
                break;
            case SyncKind::Mutex:
                c->state.store(a != 0 ? 1 : 0, std::memory_order_relaxed);
                if (a != 0) {
                    c->owner_pid.store(
                        static_cast<std::uint32_t>(::getpid()),
                        std::memory_order_relaxed);
                    c->owner_tid.store(static_cast<std::uint32_t>(b), std::memory_order_relaxed);
                    c->recursion.store(1, std::memory_order_relaxed);
                }
                break;
            case SyncKind::Semaphore:
                c->sem_max = b;
                c->state.store(a, std::memory_order_relaxed);
                break;
        }
        c->magic.store(kMagic, std::memory_order_release);
    } else {
        // Wait (bounded) for the creator to finish initialization.
        for (int i = 0; i < 100000; ++i) {
            if (c->magic.load(std::memory_order_acquire) == kMagic) break;
            std::this_thread::yield();
        }
        obj->m_kind = static_cast<SyncKind>(c->kind);
    }
    return obj;
}

SharedSyncObject::~SharedSyncObject() {
    if (m_map != nullptr) {
        SharedMemory::unmap_view(m_map, m_map_len);
    }
}

bool SharedSyncObject::set_event() {
    const bool previous = cb()->state.exchange(1, std::memory_order_acq_rel) != 0;
    wake_shared(cb());
    return previous;
}

bool SharedSyncObject::event_is_signaled() const {
    return cb()->state.load(std::memory_order_acquire) != 0;
}

bool SharedSyncObject::event_manual_reset() const {
    return cb()->manual_reset != 0;
}

bool SharedSyncObject::reset_event() {
    return cb()->state.exchange(0, std::memory_order_acq_rel) != 0;
}

bool SharedSyncObject::release_mutex(std::uint32_t tid) {
    ControlBlock* c = cb();
    if (c->owner_pid.load(std::memory_order_acquire) !=
            static_cast<std::uint32_t>(::getpid()) ||
        c->owner_tid.load(std::memory_order_acquire) != tid) {
        return false;
    }
    std::uint32_t r = c->recursion.load(std::memory_order_relaxed);
    if (r > 1) {
        c->recursion.store(r - 1, std::memory_order_relaxed);
        return true;
    }
    std::int32_t expected = 1;
    if (!c->state.compare_exchange_strong(expected, 2,
                                           std::memory_order_acq_rel)) {
        return false;
    }
    c->recursion.store(0, std::memory_order_relaxed);
    c->owner_tid.store(0, std::memory_order_relaxed);
    c->owner_pid.store(0, std::memory_order_relaxed);
    c->state.store(0, std::memory_order_release);
    wake_shared(c);
    return true;
}

bool SharedSyncObject::abandon_mutex(std::uint32_t tid) {
    if (m_kind != SyncKind::Mutex) return false;
    ControlBlock* c = cb();
    if (c->owner_pid.load(std::memory_order_acquire) !=
            static_cast<std::uint32_t>(::getpid()) ||
        c->owner_tid.load(std::memory_order_acquire) != tid) {
        return false;
    }
    std::int32_t expected = 1;
    if (!c->state.compare_exchange_strong(expected, 2,
                                           std::memory_order_acq_rel)) {
        return false;
    }
    c->recursion.store(0, std::memory_order_relaxed);
    c->owner_tid.store(0, std::memory_order_relaxed);
    c->owner_pid.store(0, std::memory_order_relaxed);
    c->abandoned.store(1, std::memory_order_release);
    c->state.store(0, std::memory_order_release);
    wake_shared(c);
    return true;
}

bool SharedSyncObject::release_semaphore(std::int32_t count, std::int32_t* previous_out) {
    ControlBlock* c = cb();
    std::int32_t prev = c->state.load(std::memory_order_acquire);
    for (;;) {
        std::int32_t next = prev + count;
        if (c->sem_max > 0 && next > c->sem_max) return false;
        if (c->state.compare_exchange_weak(prev, next, std::memory_order_acq_rel)) {
            if (previous_out) *previous_out = prev;
            wake_shared(c);
            return true;
        }
    }
}

bool SharedSyncObject::query_semaphore(std::int32_t* current_out,
                                       std::int32_t* maximum_out) const {
    if (m_kind != SyncKind::Semaphore || m_map == nullptr) {
        return false;
    }
    const ControlBlock* c = static_cast<const ControlBlock*>(m_map);
    if (current_out) {
        *current_out = c->state.load(std::memory_order_acquire);
    }
    if (maximum_out) {
        *maximum_out = c->sem_max;
    }
    return true;
}

SyncConsumeResult SharedSyncObject::try_consume(std::uint32_t tid) {
    ControlBlock* c = cb();

    switch (m_kind) {
        case SyncKind::Event:
            if (c->manual_reset) {
                return c->state.load(std::memory_order_acquire) == 1
                    ? SyncConsumeResult::Acquired
                    : SyncConsumeResult::NotReady;
            } else {
                std::int32_t expected = 1;
                return c->state.compare_exchange_strong(
                           expected, 0, std::memory_order_acq_rel)
                    ? SyncConsumeResult::Acquired
                    : SyncConsumeResult::NotReady;
            }
        case SyncKind::Mutex: {
            // Recursive acquisition by the same tid.
            if (c->owner_pid.load(std::memory_order_acquire) ==
                    static_cast<std::uint32_t>(::getpid()) &&
                c->owner_tid.load(std::memory_order_acquire) == tid &&
                c->state.load(std::memory_order_acquire) == 1) {
                c->recursion.fetch_add(1, std::memory_order_relaxed);
                return SyncConsumeResult::Acquired;
            }
            std::int32_t expected = 0;
            if (c->state.compare_exchange_strong(expected, 2,
                                                 std::memory_order_acq_rel)) {
                c->owner_pid.store(
                    static_cast<std::uint32_t>(::getpid()),
                    std::memory_order_relaxed);
                c->owner_tid.store(tid, std::memory_order_relaxed);
                c->recursion.store(1, std::memory_order_relaxed);
                const bool was_abandoned =
                    c->abandoned.exchange(0, std::memory_order_acq_rel) != 0;
                c->state.store(1, std::memory_order_release);
                return was_abandoned ? SyncConsumeResult::Abandoned
                                     : SyncConsumeResult::Acquired;
            }

            // A host process can disappear without running guest thread-exit
            // cleanup. Claim its held mutex through the transition state so
            // exactly one contender becomes the new owner.
            if (expected == 1) {
                const std::uint32_t dead_pid =
                    c->owner_pid.load(std::memory_order_acquire);
                if (!process_is_live(dead_pid)) {
                    expected = 1;
                    if (c->state.compare_exchange_strong(
                            expected, 2, std::memory_order_acq_rel)) {
                        c->owner_pid.store(
                            static_cast<std::uint32_t>(::getpid()),
                            std::memory_order_relaxed);
                        c->owner_tid.store(tid, std::memory_order_relaxed);
                        c->recursion.store(1, std::memory_order_relaxed);
                        c->abandoned.store(0, std::memory_order_relaxed);
                        c->state.store(1, std::memory_order_release);
                        return SyncConsumeResult::Abandoned;
                    }
                }
            }
            return SyncConsumeResult::NotReady;
        }
        case SyncKind::Semaphore: {
            std::int32_t cur = c->state.load(std::memory_order_acquire);
            while (cur > 0) {
                if (c->state.compare_exchange_weak(cur, cur - 1,
                                                   std::memory_order_acq_rel)) {
                    return SyncConsumeResult::Acquired;
                }
            }
            return SyncConsumeResult::NotReady;
        }
    }
    return SyncConsumeResult::NotReady;
}

void SharedSyncObject::undo_consume(std::uint32_t tid, bool was_abandoned) {
    struct WakeOnExit {
        ControlBlock* c;
        ~WakeOnExit() { wake_shared(c); }
    } wake_on_exit{cb()};
    ControlBlock* c = cb();
    switch (m_kind) {
        case SyncKind::Event:
            if (!c->manual_reset) c->state.store(1, std::memory_order_release);
            break;
        case SyncKind::Mutex:
            if (release_mutex(tid) && was_abandoned) {
                c->abandoned.store(1, std::memory_order_release);
            }
            break;
        case SyncKind::Semaphore:
            c->state.fetch_add(1, std::memory_order_acq_rel);
            break;
    }
}

std::uint32_t SharedSyncObject::wake_generation() const noexcept {
    return cb()->wake_gen.load(std::memory_order_acquire);
}

void SharedSyncObject::park_until_changed(std::uint32_t seen,
                                          std::uint64_t max_ns) noexcept {
    ControlBlock* c = cb();
    (void)os_sync_wait_on_address_with_timeout(
        &c->wake_gen, seen, sizeof(c->wake_gen),
        OS_SYNC_WAIT_ON_ADDRESS_SHARED, OS_CLOCK_MACH_ABSOLUTE_TIME, max_ns);
}

std::uint32_t wait_set(SharedSyncObject** objs, std::uint32_t count,
                       bool wait_all, std::uint32_t timeout_ms,
                       std::uint32_t tid,
                       const std::atomic<bool>* cancel_requested,
                       std::uint64_t* blocked_ns,
                       const std::atomic<bool>* shutdown_requested) {
    using namespace std::chrono;
    if (count == 0 || objs == nullptr) return ntsync::WAIT_FAILED;

    const bool infinite = (timeout_ms == ntsync::INFINITE_TIMEOUT);
    const auto deadline = steady_clock::now() + milliseconds(infinite ? 0 : timeout_ms);

    std::uint32_t backoff_us = 1;
    std::vector<std::pair<std::uint32_t, bool>> acquired;
    for (;;) {
        // Snapshot before probing: a signal after this bumps the generation
        // and the park below returns immediately.
        const std::uint32_t gen_seen =
            (count == 1 && objs[0] != nullptr) ? objs[0]->wake_generation() : 0;
        if ((cancel_requested != nullptr &&
             cancel_requested->load(std::memory_order_acquire)) ||
            (shutdown_requested != nullptr &&
             shutdown_requested->load(std::memory_order_acquire))) {
            return ntsync::WAIT_FAILED;
        }

        if (!wait_all) {
            for (std::uint32_t i = 0; i < count; ++i) {
                if (!objs[i]) continue;
                const SyncConsumeResult result = objs[i]->try_consume(tid);
                if (result != SyncConsumeResult::NotReady) {
                    return (result == SyncConsumeResult::Abandoned
                                ? ntsync::WAIT_ABANDONED_0
                                : ntsync::WAIT_OBJECT_0) + i;
                }
            }
        } else {
            acquired.clear();
            bool all = true;
            for (std::uint32_t i = 0; i < count; ++i) {
                const SyncConsumeResult result = objs[i]
                    ? objs[i]->try_consume(tid)
                    : SyncConsumeResult::NotReady;
                if (result != SyncConsumeResult::NotReady) {
                    acquired.emplace_back(
                        i, result == SyncConsumeResult::Abandoned);
                } else {
                    all = false;
                    break;
                }
            }
            if (all) {
                for (const auto& [idx, was_abandoned] : acquired) {
                    if (was_abandoned) return ntsync::WAIT_ABANDONED_0 + idx;
                }
                return ntsync::WAIT_OBJECT_0;
            }
            for (const auto& [idx, was_abandoned] : acquired) {
                objs[idx]->undo_consume(tid, was_abandoned);
            }
        }

        if (timeout_ms == 0) return ntsync::WAIT_TIMEOUT;
        if (!infinite && steady_clock::now() >= deadline) return ntsync::WAIT_TIMEOUT;

        const auto blocked_at = blocked_ns != nullptr
            ? steady_clock::now()
            : steady_clock::time_point{};
        static const bool s_shared_futex_off =
            ::access("/tmp/vitreo-sync-off-sharedfutex", F_OK) == 0 ||
            std::getenv("VITREO_SYNC_OFF_SHAREDFUTEX") != nullptr;
        if (count == 1 && objs[0] != nullptr && !s_shared_futex_off) {
            // Park on the object's wake word until a peer bumps it. The
            // generation was read before the consume attempt above, so a
            // signal landing in between makes the kernel return at once.
            // Bounded so cancellation and shutdown are still observed.
            std::uint64_t park_ns = 50'000'000;
            if (!infinite) {
                const auto left = duration_cast<nanoseconds>(
                    deadline - steady_clock::now()).count();
                if (left <= 0) return ntsync::WAIT_TIMEOUT;
                if (static_cast<std::uint64_t>(left) < park_ns) {
                    park_ns = static_cast<std::uint64_t>(left);
                }
            }
            objs[0]->park_until_changed(gen_seen, park_ns);
            if (blocked_ns != nullptr) {
                *blocked_ns += static_cast<std::uint64_t>(
                    duration_cast<nanoseconds>(steady_clock::now() - blocked_at)
                        .count());
            }
            continue;
        }
        std::this_thread::sleep_for(microseconds(backoff_us));
        if (blocked_ns != nullptr) {
            *blocked_ns += static_cast<std::uint64_t>(
                duration_cast<nanoseconds>(steady_clock::now() - blocked_at)
                    .count());
        }
        if (backoff_us < 2000) backoff_us *= 2;  // cap ~2ms
    }
}

} // namespace vitreo::ipc
