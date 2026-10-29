// SPDX-License-Identifier: LGPL-2.1-only
//
// SRWLOCK and CONDITION_VARIABLE implemented IN the guest word, parked with
// the kernel's address wait.
//
// Windows keeps all SRWLOCK / CONDITION_VARIABLE state inside the one
// pointer-sized word the caller owns (SRWLOCK_INIT / CONDITION_VARIABLE_INIT
// are zero) and parks contended threads with NtWaitForAlertByThreadId; there
// is no per-lock kernel object. Vitreo used to look every lock up in a
// process-wide map under a shared_mutex on EVERY acquire and release, and
// back it with a host std::shared_mutex. On a guest that takes an SRWLOCK
// on the hot path, that was 1.9M of 2.0M slow thunk dispatches while idle.
//
// The guest treats the word as opaque, so its layout is Vitreo's to choose:
//
//   bit 0      EXCL     held exclusively
//   bit 1      WAITERS  at least one thread may be parked on this word
//   bits 2..   owner guest tid while EXCL, else the shared-holder count
//
// A CONDITION_VARIABLE word is a wake generation counter. Waiters compare
// the word with os_sync_wait_on_address (size 4 or 8 to match the guest
// pointer width) and wakers bump/CAS it and wake by address, the same
// futex discipline the kernel uses; spurious wakes are permitted by both
// Windows contracts and every path re-checks.
//
// Only words that linear guest memory backs qualify (a stable host pointer,
// naturally aligned). Anything else keeps the managed SyncManager path, and
// the choice is a pure function of the address so a lock never mixes paths.
#pragma once

#include <vitreo/common.hpp>
#include <atomic>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi::srw_word {

struct Word {
    void* host = nullptr;   ///< host address of the guest word
    u32   size = 0;         ///< 4 or 8, the guest pointer width
    GuestAddr guest = 0;
};

/// True (and @p out filled) when the guest word at @p addr can be operated
/// on in place: linear memory backs it and it is aligned to its size.
[[nodiscard]] bool resolve(GuestMemory& memory, GuestAddr addr, bool is_64bit,
                           Word& out) noexcept;

void acquire_exclusive(const Word& w, u32 tid,
                       const std::atomic<bool>* shutdown = nullptr) noexcept;
[[nodiscard]] bool try_acquire_exclusive(const Word& w, u32 tid) noexcept;
void release_exclusive(const Word& w, u32 tid) noexcept;
void acquire_shared(const Word& w, u32 tid,
                    const std::atomic<bool>* shutdown = nullptr) noexcept;
[[nodiscard]] bool try_acquire_shared(const Word& w, u32 tid) noexcept;
/// Returns true when this release dropped the last shared holder.
bool release_shared(const Word& w, u32 tid) noexcept;
/// True when @p tid holds the word exclusively.
[[nodiscard]] bool owned_exclusively_by(const Word& w, u32 tid) noexcept;

/// The condition-variable half. `cv_sleep_with` releases the SRW word
/// (exclusive, or shared when @p shared), parks until the CV word changes or
/// @p milliseconds elapse, reacquires, and returns true when woken. Two
/// caller-supplied release/reacquire callbacks let the CS form reuse the
/// same park (CriticalSectionManager owns that lock).
[[nodiscard]] bool cv_sleep_srw(const Word& cv, const Word& lock, bool shared,
                                u32 milliseconds, u32 tid,
                                const std::atomic<bool>* shutdown) noexcept;
template <class Release, class Reacquire>
[[nodiscard]] bool cv_sleep_with(const Word& cv, u32 milliseconds,
                                 const std::atomic<bool>* shutdown,
                                 Release&& release, Reacquire&& reacquire) noexcept;
void cv_wake(const Word& cv, bool all) noexcept;

/// Internal: park until the word differs from @p expected or the timeout.
/// Returns false only on timeout.
[[nodiscard]] bool park_until_changed(const Word& w, u64 expected,
                                      u32 milliseconds,
                                      const std::atomic<bool>* shutdown) noexcept;
[[nodiscard]] u64 load(const Word& w) noexcept;

template <class Release, class Reacquire>
bool cv_sleep_with(const Word& cv, u32 milliseconds,
                   const std::atomic<bool>* shutdown,
                   Release&& release, Reacquire&& reacquire) noexcept {
    // Read the generation BEFORE dropping the lock: a wake that lands after
    // this read (whether before or after the park) changes the word, so the
    // kernel compare fails and the park returns at once.
    const u64 seen = load(cv);
    release();
    const bool woken = park_until_changed(cv, seen, milliseconds, shutdown);
    reacquire();
    return woken;
}

} // namespace vitreo::winapi::srw_word
