// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file spin_rwlock.hpp
/// @brief Spin-then-sleep reader/writer lock for short, hot critical sections.
///
/// Darwin's pthread_rwlock hands a contended lock off through the kernel on
/// every unlock. Under the compile memo's access pattern (every dispatch miss
/// and every compile reads it, every fresh emit writes it, each for well under
/// a microsecond) produces a pure handoff convoy in a 70-thread browser
/// process: ~11 threads parked in _pthread_rwlock_lock_slow at 11% CPU with
/// nobody holding the lock for long. This lock keeps the uncontended paths to
/// one atomic RMW, spins briefly when contended (critical sections are
/// shorter than a sleep/wake round trip), and only then sleeps.
///
/// A waiting writer blocks new readers, so a stream of readers cannot starve
/// a writer. Not recursive: a thread holding it shared must not take it
/// again while a writer may be waiting.
///
/// Satisfies the standard SharedMutex requirements, so std::shared_lock,
/// std::unique_lock and std::lock_guard work unchanged.

#include <atomic>
#include <cstdint>

namespace vitreo {

class SpinRwLock {
public:
    SpinRwLock() noexcept = default;
    SpinRwLock(const SpinRwLock&) = delete;
    SpinRwLock& operator=(const SpinRwLock&) = delete;

    void lock() noexcept {
        std::uint32_t expected = 0;
        if (m_state.compare_exchange_strong(expected, kWriter,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
            return;
        }
        lock_slow();
    }
    bool try_lock() noexcept {
        std::uint32_t s = m_state.load(std::memory_order_relaxed);
        while ((s & (kWriter | kReaderMask)) == 0) {
            // Taking it clears kWriterWaiting; any other waiting writer sets
            // it again on its next pass.
            if (m_state.compare_exchange_weak(s, kWriter,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }
    void unlock() noexcept {
        // Keep kWriterWaiting: another writer may have queued behind us.
        m_state.fetch_and(~kWriter, std::memory_order_seq_cst);
        wake_sleepers();
    }

    void lock_shared() noexcept {
        if (try_lock_shared()) return;
        lock_shared_slow();
    }
    bool try_lock_shared() noexcept {
        std::uint32_t s = m_state.load(std::memory_order_relaxed);
        while ((s & (kWriter | kWriterWaiting)) == 0) {
            if (m_state.compare_exchange_weak(s, s + 1,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }
    void unlock_shared() noexcept {
        const std::uint32_t prev =
            m_state.fetch_sub(1, std::memory_order_seq_cst);
        // Only a writer waits for readers, and only for the count to reach 0.
        if ((prev & kReaderMask) == 1) wake_sleepers();
    }

private:
    static constexpr std::uint32_t kWriter = 1u << 31;
    static constexpr std::uint32_t kWriterWaiting = 1u << 30;
    static constexpr std::uint32_t kReaderMask = kWriterWaiting - 1;
    // About 130 pause instructions before sleeping: on the order of a memo
    // critical section, far shorter than a sleep/wake round trip.
    static constexpr std::uint32_t kSpinRounds = 8;

    static void pause(std::uint32_t round) noexcept {
        const std::uint32_t n = 1u << (round < 5 ? round : 5);
        for (std::uint32_t i = 0; i < n; ++i) {
#if defined(__aarch64__)
            __asm__ __volatile__("isb" ::: "memory");
#elif defined(__x86_64__)
            __builtin_ia32_pause();
#endif
        }
    }

    /// Sleep until m_state no longer equals @p observed. The sleeper count
    /// and the state are both seq_cst, so either the unlocker sees the
    /// sleeper or the sleeper sees the unlocked state.
    void sleep_while(std::uint32_t observed) noexcept {
        m_sleepers.fetch_add(1, std::memory_order_seq_cst);
        if (m_state.load(std::memory_order_seq_cst) == observed) {
            m_state.wait(observed, std::memory_order_seq_cst);
        }
        m_sleepers.fetch_sub(1, std::memory_order_relaxed);
    }
    void wake_sleepers() noexcept {
        if (m_sleepers.load(std::memory_order_seq_cst) != 0) {
            m_state.notify_all();
        }
    }

    void lock_slow() noexcept {
        std::uint32_t round = 0;
        for (;;) {
            std::uint32_t s = m_state.load(std::memory_order_relaxed);
            if ((s & (kWriter | kReaderMask)) == 0) {
                if (m_state.compare_exchange_weak(s, kWriter,
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed)) {
                    return;
                }
                continue;
            }
            if ((s & kWriterWaiting) == 0) {
                m_state.fetch_or(kWriterWaiting, std::memory_order_relaxed);
                continue;
            }
            if (round < kSpinRounds) {
                pause(round++);
                continue;
            }
            sleep_while(s);
            round = 0;
        }
    }

    void lock_shared_slow() noexcept {
        std::uint32_t round = 0;
        for (;;) {
            std::uint32_t s = m_state.load(std::memory_order_relaxed);
            if ((s & (kWriter | kWriterWaiting)) == 0) {
                if (m_state.compare_exchange_weak(s, s + 1,
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed)) {
                    return;
                }
                continue;
            }
            if (round < kSpinRounds) {
                pause(round++);
                continue;
            }
            sleep_while(s);
            round = 0;
        }
    }

    std::atomic<std::uint32_t> m_state{0};
    std::atomic<std::uint32_t> m_sleepers{0};
};

}  // namespace vitreo
