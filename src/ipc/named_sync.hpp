// SPDX-License-Identifier: LGPL-2.1-only
/// @file named_sync.hpp
/// @brief Cross-process named synchronization objects (event / mutex /
///        semaphore) for Stage 3 of cross-process IPC.
///
/// State lives in a POSIX shm control block keyed by the Windows object name,
/// so two Vitreo host processes that create/open the same name share one
/// object. Waiting is lock-free: each object's state is an atomic, consumed via
/// CAS, so a process dying mid-wait cannot wedge a cross-process lock. Waiters
/// poll with bounded backoff. Browser and launcher IPC waits are
/// event-signal driven, not hot-contended. WaitForMultipleObjects is
/// supported for all-shared sets.

#pragma once

#include <vitreo/common.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace vitreo::ipc {

class SharedMemory;

enum class SyncKind : std::uint32_t { Event = 0, Mutex = 1, Semaphore = 2 };

enum class SyncConsumeResult : std::uint8_t {
    NotReady,
    Acquired,
    Abandoned,
};

/// A handle to one cross-process named sync object (backed by shm).
class SharedSyncObject {
public:
    ~SharedSyncObject();
    SharedSyncObject(const SharedSyncObject&) = delete;
    SharedSyncObject& operator=(const SharedSyncObject&) = delete;

    /// Create or attach the named object. For Event: @p a=manual_reset,
    /// @p b=initial_state. For Mutex: @p a=initial_owner, @p b=owner_tid.
    /// For Semaphore: @p a=initial_count, @p b=max_count. Returns nullptr on
    /// failure. @p create=false attaches an existing object (OpenEvent/etc).
    [[nodiscard]] static std::shared_ptr<SharedSyncObject>
    create_or_open(const std::string& win_name, SyncKind kind,
                   std::int32_t a, std::int32_t b, bool create);

    [[nodiscard]] SyncKind kind() const noexcept { return m_kind; }
    [[nodiscard]] bool was_created() const noexcept { return m_was_created; }

    // -- Signal / release (return false on wrong-owner etc.) --
    /// Set / reset the event; returns the state before the change.
    bool set_event();
    bool reset_event();
    [[nodiscard]] bool event_is_signaled() const;
    /// Event only: true for a manual-reset (notification) event.
    [[nodiscard]] bool event_manual_reset() const;
    bool release_mutex(std::uint32_t tid);
    /// Mark a mutex owned by the current process/thread as abandoned. The next
    /// successful waiter acquires ownership and receives WAIT_ABANDONED.
    bool abandon_mutex(std::uint32_t tid);
    bool release_semaphore(std::int32_t count, std::int32_t* previous_out);
    /// Read current/max without mutating (NtQuerySemaphore).
    [[nodiscard]] bool query_semaphore(std::int32_t* current_out,
                                       std::int32_t* maximum_out) const;

    // -- Wait primitives (used by wait_set) --
    /// Try to satisfy+consume this object for @p tid without blocking.
    [[nodiscard]] SyncConsumeResult try_consume(std::uint32_t tid);
    /// Undo a consume (for WaitForMultipleObjects wait-all partial rollback).
    void undo_consume(std::uint32_t tid, bool was_abandoned = false);
    /// The object's wake generation. Read it BEFORE try_consume, then hand it
    /// to park_until_changed: a signal landing in between bumps it and the
    /// park returns at once. Per-caller, never stored in the object: several
    /// threads wait on one object and must not overwrite each other's
    /// snapshot.
    [[nodiscard]] std::uint32_t wake_generation() const noexcept;
    /// Sleep until the wake generation differs from @p seen or @p max_ns
    /// elapses. Spurious returns are fine; the caller re-probes.
    void park_until_changed(std::uint32_t seen, std::uint64_t max_ns) noexcept;

private:
    SharedSyncObject() = default;
    struct ControlBlock;
    [[nodiscard]] ControlBlock* cb() noexcept;
    [[nodiscard]] const ControlBlock* cb() const noexcept;

    std::shared_ptr<SharedMemory> m_shm;
    void*    m_map{nullptr};
    usize    m_map_len{0};
    SyncKind m_kind{SyncKind::Event};
    static void wake_shared(ControlBlock* c) noexcept;
    bool     m_was_created{false};
};

/// Block until @p count objects are satisfied (wait_all) or any one is
/// (wait-any), or @p timeout_ms elapses. Returns WAIT_OBJECT_0(+i),
/// WAIT_TIMEOUT, or WAIT_FAILED (ntsync.hpp values).
/// Either cancellation source interrupts the host wait without consuming an
/// object; shutdown is independent of per-operation cancellation.
[[nodiscard]] std::uint32_t wait_set(SharedSyncObject** objs, std::uint32_t count,
                                     bool wait_all, std::uint32_t timeout_ms,
                                     std::uint32_t tid,
                                     const std::atomic<bool>* cancel_requested = nullptr,
                                     std::uint64_t* blocked_ns = nullptr,
                                     const std::atomic<bool>* shutdown_requested = nullptr);

} // namespace vitreo::ipc
