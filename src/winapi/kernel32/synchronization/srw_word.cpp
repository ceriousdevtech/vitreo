// SPDX-License-Identifier: LGPL-2.1-only
#include "srw_word.hpp"
#include "core/memory/guest_memory.hpp"
#include <runtime/diagnostics/hang_dump.hpp>

#include <os/os_sync_wait_on_address.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi::srw_word {

namespace {

constexpr u64 kExcl    = 1u;
constexpr u64 kWaiters = 2u;
constexpr u64 kShift   = 2u;

// Exclusive re-acquisition by the owner is undefined on Windows. Vitreo
// tolerates it with a recursion count (emulation-induced re-entry such as
// DllMain inside _initterm) instead of self-deadlocking; the count lives
// with the thread, never in the word.
thread_local std::unordered_map<GuestAddr, u32> t_exclusive_recursion;

template <class T>
[[nodiscard]] std::atomic<T>* as_atomic(const Word& w) noexcept {
    return reinterpret_cast<std::atomic<T>*>(w.host);
}

[[nodiscard]] bool cas(const Word& w, u64& expected, u64 desired) noexcept {
    if (w.size == 8) {
        return as_atomic<u64>(w)->compare_exchange_weak(
            expected, desired, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }
    u32 e = static_cast<u32>(expected);
    const bool ok = as_atomic<u32>(w)->compare_exchange_weak(
        e, static_cast<u32>(desired), std::memory_order_acq_rel,
        std::memory_order_acquire);
    expected = e;
    return ok;
}

void wake_all(const Word& w) noexcept {
    (void)os_sync_wake_by_address_all(w.host, w.size,
                                      OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

void wake_any(const Word& w) noexcept {
    (void)os_sync_wake_by_address_any(w.host, w.size,
                                      OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

[[nodiscard]] u32 owner_of(u64 word) noexcept {
    return static_cast<u32>(word >> kShift);
}

/// The owner field is 30 bits wide in a 4-byte word, so a tid is compared in
/// the width the word can hold. Without the mask a tid >= 2^30 (Vitreo's
/// 0xF000000x service tids) was stored truncated and its own release read as
/// "not ours".
[[nodiscard]] u32 owner_tid(const Word& w, u32 tid) noexcept {
    return w.size == 8 ? tid : (tid & 0x3FFFFFFFu);
}

[[nodiscard]] u64 owned_word(const Word& w, u32 tid, u64 waiters) noexcept {
    return kExcl | (static_cast<u64>(owner_tid(w, tid)) << kShift) | waiters;
}

/// A fresh (non-recursive) exclusive acquisition owns no recursion: any count
/// this thread still has for the address belongs to an earlier hold whose
/// matching release never consumed it, and would otherwise turn the NEXT
/// release into a no-op that leaves the lock held forever.
void drop_stale_recursion(const Word& w) noexcept {
    if (!t_exclusive_recursion.empty()) t_exclusive_recursion.erase(w.guest);
}

// ---------------------------------------------------------------------------
// Hang-dump support: who is parked on which word, and the anomalies that
// leave a word held with nobody to release it. Touched only on contended or
// anomalous paths, never on an uncontended acquire/release.
// ---------------------------------------------------------------------------

enum class ParkKind : u32 { None = 0, Exclusive, Shared, CvSleep };

struct ParkRecord {
    std::atomic<u32> kind{0};
    std::atomic<u32> tid{0};
    std::atomic<u64> guest{0};        ///< lock (or CV) guest address
    std::atomic<void*> host{nullptr};
    std::atomic<u32> size{0};
    std::atomic<u64> lock_guest{0};   ///< CV sleeps: the lock being given up
    std::atomic<void*> lock_host{nullptr};
    std::atomic<u64> since_ns{0};
};

std::mutex g_park_mu;
std::vector<ParkRecord*>& park_records() {
    static auto* v = new std::vector<ParkRecord*>();
    return *v;
}

std::atomic<u64> g_foreign_releases{0};
std::atomic<u64> g_unheld_releases{0};
std::atomic<u64> g_stale_recursion{0};

[[nodiscard]] u64 now_ns() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

[[nodiscard]] u64 load_host(void* host, u32 size) noexcept {
    if (host == nullptr) return 0;
    return size == 8
        ? reinterpret_cast<std::atomic<u64>*>(host)->load(std::memory_order_acquire)
        : reinterpret_cast<std::atomic<u32>*>(host)->load(std::memory_order_acquire);
}

void describe_word(std::FILE* out, u64 word) {
    if (word & kExcl) {
        std::fprintf(out, "EXCLUSIVE owner_tid=%u%s", owner_of(word),
                     (word & kWaiters) ? " waiters" : "");
    } else if ((word >> kShift) != 0) {
        std::fprintf(out, "SHARED holders=%llu%s",
                     static_cast<unsigned long long>(word >> kShift),
                     (word & kWaiters) ? " waiters" : "");
    } else {
        std::fprintf(out, "FREE%s", (word & kWaiters) ? " waiters" : "");
    }
}

void dump_parked(std::FILE* out) {
    if (out == nullptr) return;
    std::fprintf(out,
                 "[hang-dump] === srw/cv words === foreign_releases=%llu "
                 "unheld_releases=%llu stale_recursion=%llu\n",
                 static_cast<unsigned long long>(g_foreign_releases.load()),
                 static_cast<unsigned long long>(g_unheld_releases.load()),
                 static_cast<unsigned long long>(g_stale_recursion.load()));
    // A debugging read of a process that is already stuck: take the record
    // list lock only (never held across a park).
    std::lock_guard lk(g_park_mu);
    const u64 now = now_ns();
    static const char* kNames[] = {"none", "AcquireExclusive", "AcquireShared",
                                   "SleepConditionVariable"};
    for (const ParkRecord* r : park_records()) {
        const u32 kind = r->kind.load(std::memory_order_acquire);
        if (kind == 0 || kind > 3) continue;
        const u32 size = r->size.load();
        const u64 since = r->since_ns.load();
        std::fprintf(out, "[hang-dump]   tid=%u %s %s=0x%llx word=0x%llx (",
                     r->tid.load(), kNames[kind],
                     kind == 3 ? "cv" : "lock",
                     static_cast<unsigned long long>(r->guest.load()),
                     static_cast<unsigned long long>(load_host(r->host.load(), size)));
        if (kind == 3) {
            std::fprintf(out, "generation");
        } else {
            describe_word(out, load_host(r->host.load(), size));
        }
        std::fprintf(out, ") parked_ms=%llu",
                     static_cast<unsigned long long>(
                         since != 0 && now > since ? (now - since) / 1000000 : 0));
        if (kind == 3 && r->lock_host.load() != nullptr) {
            const u64 lw = load_host(r->lock_host.load(), size);
            std::fprintf(out, " lock=0x%llx lock_word=0x%llx (",
                         static_cast<unsigned long long>(r->lock_guest.load()),
                         static_cast<unsigned long long>(lw));
            describe_word(out, lw);
            std::fputc(')', out);
        }
        std::fputc('\n', out);
    }
}

struct ThreadParkRecord {
    ParkRecord rec;
    ThreadParkRecord() {
        static std::once_flag registered;
        std::call_once(registered, [] {
            diagnostics::register_hang_dump_section(&dump_parked);
        });
        std::lock_guard lk(g_park_mu);
        park_records().push_back(&rec);
    }
    ~ThreadParkRecord() {
        std::lock_guard lk(g_park_mu);
        auto& v = park_records();
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (*it == &rec) { v.erase(it); break; }
        }
    }
};

/// Publishes "this thread is parked on @p w" for the hang dump while alive.
class ParkScope {
public:
    ParkScope(ParkKind kind, const Word& w, u32 tid,
              const Word* lock = nullptr) noexcept {
        thread_local ThreadParkRecord t_record;
        rec_ = &t_record.rec;
        if (rec_->kind.load(std::memory_order_relaxed) != 0) {
            rec_ = nullptr;   // nested (CV reacquire inside a CV sleep)
            return;
        }
        rec_->tid.store(tid, std::memory_order_relaxed);
        rec_->guest.store(w.guest, std::memory_order_relaxed);
        rec_->host.store(w.host, std::memory_order_relaxed);
        rec_->size.store(w.size, std::memory_order_relaxed);
        rec_->lock_guest.store(lock ? lock->guest : 0, std::memory_order_relaxed);
        rec_->lock_host.store(lock ? lock->host : nullptr, std::memory_order_relaxed);
        rec_->since_ns.store(now_ns(), std::memory_order_relaxed);
        rec_->kind.store(static_cast<u32>(kind), std::memory_order_release);
    }
    ~ParkScope() {
        if (rec_ != nullptr) rec_->kind.store(0, std::memory_order_release);
    }
    ParkScope(const ParkScope&) = delete;
    ParkScope& operator=(const ParkScope&) = delete;

private:
    ParkRecord* rec_ = nullptr;
};

void report_foreign_release(const Word& w, u32 tid, u64 word) noexcept {
    const u64 n = g_foreign_releases.fetch_add(1, std::memory_order_relaxed);
    if (n < 16) {
        std::fprintf(stderr,
                     "[srw] ReleaseSRWLockExclusive(0x%llx) by tid=%u while "
                     "tid=%u owns it: released (Windows SRW locks record no "
                     "owner; ignoring it would leave the lock held forever)\n",
                     static_cast<unsigned long long>(w.guest), tid,
                     owner_of(word));
    }
}

/// Publish WAITERS on @p word (the value the caller just observed) and park
/// until the word changes. Both the CAS and the kernel compare use the
/// observed value, so a release in between cannot be slept through.
void wait_on(const Word& w, u64 observed,
             const std::atomic<bool>* shutdown) noexcept {
    u64 expected = observed;
    const u64 flagged = observed | kWaiters;
    if ((observed & kWaiters) == 0 && !cas(w, expected, flagged)) {
        return;   // the word moved; the caller re-examines it
    }
    (void)park_until_changed(w, flagged, 100, shutdown);
}

} // namespace

bool resolve(GuestMemory& memory, GuestAddr addr, bool is_64bit,
             Word& out) noexcept {
    const u32 size = is_64bit ? 8u : 4u;
    if (addr == 0 || (addr & (size - 1)) != 0) return false;
    if (!memory.is_linear() || !memory.linear_backs(addr, size)) return false;
    out.host = memory.linear_host(addr);
    out.size = size;
    out.guest = addr;
    return out.host != nullptr;
}

u64 load(const Word& w) noexcept {
    return w.size == 8
        ? as_atomic<u64>(w)->load(std::memory_order_acquire)
        : as_atomic<u32>(w)->load(std::memory_order_acquire);
}

bool park_until_changed(const Word& w, u64 expected, u32 milliseconds,
                        const std::atomic<bool>* shutdown) noexcept {
    using namespace std::chrono;
    const bool infinite = milliseconds == 0xFFFFFFFFu;
    const auto deadline =
        steady_clock::now() + std::chrono::milliseconds(infinite ? 0 : milliseconds);
    for (;;) {
        if (shutdown != nullptr && shutdown->load(std::memory_order_acquire)) {
            return true;
        }
        if (load(w) != expected) return true;
        // Bounded parks so a shutdown flag is observed; the kernel returns
        // immediately whenever the word already differs.
        u64 park_ns = 1'000'000'000ull;
        if (!infinite) {
            const auto left = duration_cast<nanoseconds>(
                deadline - steady_clock::now()).count();
            if (left <= 0) return load(w) != expected;
            if (static_cast<u64>(left) < park_ns) park_ns = static_cast<u64>(left);
        }
        const int rc = os_sync_wait_on_address_with_timeout(
            w.host, expected, w.size, OS_SYNC_WAIT_ON_ADDRESS_NONE,
            OS_CLOCK_MACH_ABSOLUTE_TIME, park_ns);
        if (rc >= 0) return true;                     // woken
        if (errno == ETIMEDOUT) {
            if (load(w) != expected) return true;
            if (!infinite && steady_clock::now() >= deadline) return false;
            continue;                                 // bounded park expired
        }
        // EINTR / EFAULT: re-examine and either return or park again.
    }
}

void acquire_exclusive(const Word& w, u32 tid,
                       const std::atomic<bool>* shutdown) noexcept {
    std::optional<ParkScope> parked;
    for (;;) {
        u64 word = load(w);
        if ((word & ~kWaiters) == 0) {
            if (cas(w, word, owned_word(w, tid, word & kWaiters))) {
                drop_stale_recursion(w);
                return;
            }
            continue;
        }
        if ((word & kExcl) != 0 && owner_of(word) == owner_tid(w, tid)) {
            ++t_exclusive_recursion[w.guest];
            return;
        }
        if (!parked) parked.emplace(ParkKind::Exclusive, w, tid);
        wait_on(w, word, shutdown);
    }
}

bool try_acquire_exclusive(const Word& w, u32 tid) noexcept {
    // Learn: TryAcquire fails only when the lock is held. A weak CAS may fail
    // spuriously, and a WAITERS-bit flip is not "held", so retry while free.
    for (;;) {
        u64 word = load(w);
        if ((word & ~kWaiters) != 0) return false;   // held (even by us)
        if (cas(w, word, owned_word(w, tid, word & kWaiters))) {
            drop_stale_recursion(w);
            return true;
        }
    }
}

bool owned_exclusively_by(const Word& w, u32 tid) noexcept {
    const u64 word = load(w);
    return (word & kExcl) != 0 && owner_of(word) == owner_tid(w, tid);
}

void release_exclusive(const Word& w, u32 tid) noexcept {
    if (!t_exclusive_recursion.empty()) {
        if (auto it = t_exclusive_recursion.find(w.guest);
            it != t_exclusive_recursion.end()) {
            if (it->second > 0 && owned_exclusively_by(w, tid)) {
                --it->second;
                return;
            }
            if (it->second > 0) {
                g_stale_recursion.fetch_add(1, std::memory_order_relaxed);
            }
            t_exclusive_recursion.erase(it);
        }
    }
    // Windows keeps no owner in an SRWLOCK: ReleaseSRWLockExclusive clears
    // the exclusive hold whichever thread calls it (Learn asks callers to
    // release on the acquiring thread, but nothing enforces it). The owner
    // tid Vitreo keeps in the word exists for the recursion tolerance and
    // diagnostics only; treating a foreign release as "not ours" left the
    // lock held with no one left to release it.
    for (;;) {
        u64 word = load(w);
        if ((word & kExcl) == 0) {
            g_unheld_releases.fetch_add(1, std::memory_order_relaxed);
            return;   // not exclusively held: nothing to release
        }
        const bool waiters = (word & kWaiters) != 0;
        const u64 seen = word;
        if (cas(w, word, 0)) {
            if (owner_of(seen) != owner_tid(w, tid)) {
                report_foreign_release(w, tid, seen);
            }
            if (waiters) wake_all(w);
            return;
        }
    }
}

void acquire_shared(const Word& w, u32 tid,
                    const std::atomic<bool>* shutdown) noexcept {
    std::optional<ParkScope> parked;
    for (;;) {
        u64 word = load(w);
        if ((word & kExcl) == 0) {
            if (cas(w, word, word + (u64{1} << kShift))) return;
            continue;
        }
        if (owner_of(word) == owner_tid(w, tid)) {
            // Shared inside our own exclusive hold: UB on Windows; count it
            // as exclusive recursion, which is what the managed path did.
            ++t_exclusive_recursion[w.guest];
            return;
        }
        if (!parked) parked.emplace(ParkKind::Shared, w, tid);
        wait_on(w, word, shutdown);
    }
}

bool try_acquire_shared(const Word& w, u32 tid) noexcept {
    (void)tid;
    for (;;) {
        u64 word = load(w);
        if ((word & kExcl) != 0) return false;
        if (cas(w, word, word + (u64{1} << kShift))) return true;
    }
}

bool release_shared(const Word& w, u32 tid) noexcept {
    for (;;) {
        u64 word = load(w);
        if ((word & kExcl) != 0) {
            // Matched against the exclusive-recursion tolerance above.
            if (owner_of(word) == owner_tid(w, tid)) {
                auto it = t_exclusive_recursion.find(w.guest);
                if (it != t_exclusive_recursion.end() && it->second > 0) {
                    --it->second;
                }
            }
            return false;
        }
        const u64 holders = word >> kShift;
        if (holders == 0) {                           // nobody holds it
            g_unheld_releases.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        u64 desired = word - (u64{1} << kShift);
        const bool last = holders == 1;
        const bool wake = last && (word & kWaiters) != 0;
        if (wake) desired &= ~kWaiters;
        if (cas(w, word, desired)) {
            if (wake) wake_all(w);
            return last;
        }
    }
}

bool cv_sleep_srw(const Word& cv, const Word& lock, bool shared,
                  u32 milliseconds, u32 tid,
                  const std::atomic<bool>* shutdown) noexcept {
    // A recursive exclusive hold is released completely and restored
    // afterwards, as the managed path did.
    u32 saved_recursion = 0;
    if (!shared) {
        if (auto it = t_exclusive_recursion.find(lock.guest);
            it != t_exclusive_recursion.end()) {
            saved_recursion = it->second;
            t_exclusive_recursion.erase(it);
        }
    }
    ParkScope parked(ParkKind::CvSleep, cv, tid, &lock);
    const bool woken = cv_sleep_with(
        cv, milliseconds, shutdown,
        [&] {
            if (shared) (void)release_shared(lock, tid);
            else release_exclusive(lock, tid);
        },
        [&] {
            if (shared) acquire_shared(lock, tid, shutdown);
            else acquire_exclusive(lock, tid, shutdown);
        });
    if (!shared && saved_recursion != 0) {
        t_exclusive_recursion[lock.guest] = saved_recursion;
    }
    return woken;
}

void cv_wake(const Word& cv, bool all) noexcept {
    if (cv.size == 8) {
        as_atomic<u64>(cv)->fetch_add(1, std::memory_order_acq_rel);
    } else {
        as_atomic<u32>(cv)->fetch_add(1, std::memory_order_acq_rel);
    }
    if (all) wake_all(cv); else wake_any(cv);
}

} // namespace vitreo::winapi::srw_word
