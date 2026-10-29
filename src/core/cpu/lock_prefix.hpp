// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace vitreo::cpu {

/// Serialization for LOCK-prefixed x86 instructions.
///
/// x86 guarantees a LOCK-prefixed read-modify-write is atomic for ANY operand,
/// including one that straddles a cache line (Intel SDM Vol. 3A, 9.1.2.2 — the
/// bus lock). Vitreo satisfies that in two very different ways:
///
///   * **Naturally aligned, host-mapped operands** are done with one host
///     atomic (`std::atomic_ref`, seq_cst) — already atomic against every
///     other host atomic on the same bytes, with no lock of any kind.
///   * **Everything else** — a split lock, or an address with no direct host
///     mapping — falls back to a non-atomic read/modify/write through
///     `GuestMemory`, which is several separate host accesses and therefore
///     needs real mutual exclusion against every other LOCK operation.
///
/// The whole thing used to run under one process-wide `std::mutex`, taken
/// unconditionally at helper entry. Measured on a live multi-process browser renderer
/// (32 guest threads): `jit_lock_cmpxchg32` blocked on that mutex for ~40% of
/// all mutex-blocked samples, and mutex block/wake was 24.7% of non-parked
/// time against 5.1% spent actually running translated code. Every atomic
/// refcount and spinlock in the guest was funnelling through one lock, to
/// protect a fallback that essentially never runs.
///
/// So the fast path takes no lock, and the fallback quiesces it instead. The
/// protocol is the asymmetric Dekker used by read-mostly kernel locks:
///
///   fast path (reader)                 fallback (writer)
///   ------------------                 -----------------
///   slot.active = 1                    lock mutex
///   seq_cst fence                      writers += 1
///   if (writers != 0) -> escalate      seq_cst fence
///   <one host atomic>                  wait for every slot.active == 0
///   slot.active = 0                    <non-atomic RMW>
///                                      writers -= 1; unlock mutex
///
/// The two seq_cst fences make "both miss each other" impossible: if the
/// reader's store is not visible to the writer, the writer's store is visible
/// to the reader, so the reader escalates. A reader that escalates clears its
/// flag BEFORE blocking on the mutex, so it can never deadlock the writer that
/// is waiting for that flag.
///
/// Cost on the fast path: a store to the thread's own cache line, one DMB, and
/// a load of a global that is read-mostly and therefore Shared in every core's
/// cache. No contended line, no blocking.

/// Process-wide exclusive lock, held only by the non-atomic fallback and by
/// the dispatcher-level guards.
///
/// Guest callback workers (notably multimedia timers) can still execute JIT
/// helpers while the main run loop is tearing down. A function-local mutex is
/// destroyed by the CRT before every such worker is guaranteed to have left
/// guest code, turning a valid late LOCK instruction into a host
/// `std::system_error`. This primitive has no meaningful teardown work, so
/// keep it alive for the host process lifetime.
inline std::mutex& lock_prefix_mutex() noexcept {
    static std::mutex* s_mutex = new std::mutex();
    return *s_mutex;
}

/// One per guest thread that has ever executed a LOCK instruction. Slots are
/// published to an intrusive list and never removed, so a writer can walk the
/// list with no lock and a thread exiting mid-walk cannot free a node under
/// it. A retired thread's slot reads 0 forever, which is exactly the answer a
/// writer needs.
struct LockPrefixReaderSlot {
    std::atomic<uint32_t> active{0};
    LockPrefixReaderSlot* next{nullptr};
};

/// Number of fallback (non-atomic) LOCK operations currently in flight.
/// Read on every fast-path LOCK, written only by the rare fallback.
inline std::atomic<uint32_t>& lock_prefix_writers() noexcept {
    static std::atomic<uint32_t>* s_writers = new std::atomic<uint32_t>(0);
    return *s_writers;
}

inline std::atomic<LockPrefixReaderSlot*>& lock_prefix_reader_list() noexcept {
    static auto* s_head = new std::atomic<LockPrefixReaderSlot*>(nullptr);
    return *s_head;
}

enum class LockPrefixMode : int { None = 0, Atomic = 1, Exclusive = 2 };

/// Everything the guard keeps per thread, in ONE thread_local.
///
/// Darwin resolves every distinct `thread_local` through a TLV descriptor call
/// (`_tlv_get_addr`) — and it ignores `tls_model("initial-exec")`, so the only
/// way to pay for fewer is to HAVE fewer. Three separate thread_locals here
/// meant three descriptor calls on every LOCK-prefixed instruction, which in a
/// browser renderer is one of the hottest paths in the process. Folded into one
/// struct, a single resolution serves all three and the rest are plain offsets.
///
/// Constant-initialised and trivially destructible, so it also needs no
/// `__cxa_guard` and no thread-exit destructor registration. The slot it
/// points at is allocated on first use and intentionally leaked (see
/// LockPrefixReaderSlot).
struct LockPrefixTls {
    LockPrefixReaderSlot* slot;
    int depth;
    LockPrefixMode mode;
};

inline thread_local LockPrefixTls g_lock_prefix_tls{nullptr, 0,
                                                    LockPrefixMode::None};

[[nodiscard]] inline LockPrefixReaderSlot& lock_prefix_slot(
    LockPrefixTls& tls) {
    if (tls.slot == nullptr) [[unlikely]] {
        auto* slot = new LockPrefixReaderSlot();
        auto& head = lock_prefix_reader_list();
        slot->next = head.load(std::memory_order_relaxed);
        while (!head.compare_exchange_weak(slot->next, slot,
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
        }
        tls.slot = slot;
    }
    return *tls.slot;
}

/// `VITREO_LOCK_PREFIX_EXCLUSIVE=1` sends every LOCK-prefixed instruction down
/// the exclusive path, restoring the pre-2026-08-30 behaviour where one
/// process-wide mutex serialized all of them.
///
/// This exists to A/B the fast path against a suspected miscompare in one run.
/// A lost or torn guest atomic is invisible to any single-threaded test and
/// shows up as arbitrary corruption much later — an invalid handle, a bad
/// refcount — so when something downstream breaks after a change here, the
/// cheapest discriminator is to turn the fast path off and see whether the
/// symptom follows. Read once into a relaxed atomic; the on-path cost is one
/// load that is Shared in every core's cache.
[[nodiscard]] inline bool lock_prefix_force_exclusive() noexcept {
    static std::atomic<int>* s_state = new std::atomic<int>(-1);
    const int seen = s_state->load(std::memory_order_relaxed);
    if (seen >= 0) [[likely]] {
        return seen != 0;
    }
    const char* env = std::getenv("VITREO_LOCK_PREFIX_EXCLUSIVE");
    const bool on = env != nullptr && env[0] != '\0' && env[0] != '0';
    s_state->store(on ? 1 : 0, std::memory_order_relaxed);
    return on;
}

/// Scoped serialization for one LOCK-prefixed instruction.
///
/// Two ways to construct it:
///
///   * `LockPrefixGuard guard(host, size)` — the operand has already been
///     resolved. If `host` is non-null and naturally aligned for `size`, the
///     caller is about to use a host atomic and the guard takes the cheap
///     reader path. Otherwise it takes the exclusive lock. Passing the SAME
///     pointer and size the caller then tests is what keeps the two decisions
///     from ever disagreeing.
///   * `LockPrefixGuard guard` — no operand information (the interpreter and
///     run-loop dispatchers, which serialize a whole instruction). Always
///     exclusive, exactly as before.
///
/// Re-entrant: a LOCK op that needs serialization itself — the split-lock
/// fallback in `exec_cmpxchg` — cannot know whether its dispatcher already
/// holds the lock, and `std::mutex` is not recursive. Only the outermost guard
/// acquires anything. A nested guard that needs exclusivity while the
/// outermost took the reader path escalates the whole thread to exclusive for
/// the rest of the outermost scope, which is strictly stronger than what
/// either level asked for.
class LockPrefixGuard {
public:
    LockPrefixGuard() noexcept : LockPrefixGuard(nullptr, 0) {}

    LockPrefixGuard(const void* host, std::size_t size) noexcept
        : m_tls(&g_lock_prefix_tls), m_outermost(m_tls->depth == 0) {
        const bool can_use_host_atomic =
            host != nullptr && size != 0 &&
            (reinterpret_cast<uintptr_t>(host) % size) == 0 &&
            !lock_prefix_force_exclusive();
        if (m_outermost) {
            if (can_use_host_atomic) {
                enter_atomic();
            } else {
                enter_exclusive();
            }
        } else if (!can_use_host_atomic &&
                   m_tls->mode == LockPrefixMode::Atomic) {
            // Nested fallback under a reader. Upgrade the thread; the
            // outermost guard releases it. Clear the flag first so the writer
            // we may be about to wait for cannot be waiting on us.
            lock_prefix_slot(*m_tls).active.store(0,
                                                 std::memory_order_release);
            enter_exclusive();
        }
        ++m_tls->depth;
    }

    ~LockPrefixGuard() {
        --m_tls->depth;
        if (!m_outermost) {
            return;
        }
        switch (m_tls->mode) {
        case LockPrefixMode::Atomic:
            lock_prefix_slot(*m_tls).active.store(0,
                                                 std::memory_order_release);
            break;
        case LockPrefixMode::Exclusive:
            lock_prefix_writers().fetch_sub(1, std::memory_order_release);
            lock_prefix_mutex().unlock();
            break;
        case LockPrefixMode::None:
            break;
        }
        m_tls->mode = LockPrefixMode::None;
    }

    LockPrefixGuard(const LockPrefixGuard&) = delete;
    LockPrefixGuard& operator=(const LockPrefixGuard&) = delete;

private:
    void enter_atomic() noexcept {
        LockPrefixReaderSlot& slot = lock_prefix_slot(*m_tls);
        slot.active.store(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (lock_prefix_writers().load(std::memory_order_relaxed) != 0) {
            // A fallback is in flight. Stand down and queue behind it rather
            // than racing it; it is doing a non-atomic RMW that our host
            // atomic could otherwise land in the middle of.
            slot.active.store(0, std::memory_order_release);
            enter_exclusive();
            return;
        }
        m_tls->mode = LockPrefixMode::Atomic;
    }

    void enter_exclusive() noexcept {
        lock_prefix_mutex().lock();
        enter_exclusive_locked();
    }

    /// Publish the writer, then wait until no thread is inside a host-atomic
    /// LOCK. Callers must already hold `lock_prefix_mutex()`.
    void enter_exclusive_locked() noexcept {
        lock_prefix_writers().fetch_add(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        for (LockPrefixReaderSlot* slot =
                 lock_prefix_reader_list().load(std::memory_order_acquire);
             slot != nullptr; slot = slot->next) {
            // Readers hold for a handful of instructions — one host atomic
            // and some flag arithmetic — so spinning is right. A reader can
            // still be preempted mid-hold, and spinning out a whole quantum
            // for that would be worse than the lock this replaces, so yield
            // after a bounded spin.
            for (unsigned spins = 0;
                 slot->active.load(std::memory_order_acquire) != 0; ++spins) {
                if (spins >= 1024) {
                    std::this_thread::yield();
                    spins = 0;
                }
            }
        }
        m_tls->mode = LockPrefixMode::Exclusive;
    }

    LockPrefixTls* m_tls;
    bool m_outermost;
};

} // namespace vitreo::cpu
