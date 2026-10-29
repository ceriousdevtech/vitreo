// SPDX-License-Identifier: LGPL-2.1-only

/// @file sync_cs_mutex.cpp
/// @brief Critical-section and mutex operations on SyncManager.
///
/// Closed cut from sync.cpp — verified with plan_extraction.py.

#include "sync.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/named_object_directory.hpp"
#include "sync_internal.hpp"
#include "winapi/common/object_access_constants.hpp"
#include "graphics/common/perf_diag.hpp"
#include "winapi/common/timer_resolution.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/synchronization/critical_section.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/sync/ntsync.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"
#include "ipc/named_sync.hpp"

#include <algorithm>
#include <array>
#include <atomic>
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
// Critical Section Operations
// =============================================================================

void SyncManager::initialize_critical_section(GuestAddr cs_addr) {
    std::lock_guard<HostRwLock> lock(m_mutex);
    get_or_create_critical_section(cs_addr);
}

void SyncManager::initialize_critical_section_and_spin_count(GuestAddr cs_addr,
                                                              u32 /*spin_count*/) {
    // Spin count is ignored — we back with a plain recursive_mutex
    initialize_critical_section(cs_addr);
}

void SyncManager::enter_critical_section(GuestAddr cs_addr) {
    CriticalSection* cs = nullptr;
    // Fast path: shared lookup; the unique_ptr keeps the CS pointer stable
    // across concurrent inserts so a shared lock is sufficient for find.
    {
        std::shared_lock<HostRwLock> lock(m_mutex);
        auto it = m_critical_sections.find(cs_addr);
        if (it != m_critical_sections.end()) cs = it->second.get();
    }
    if (!cs) {
        // Lazy init for guests that skip InitializeCriticalSection.
        std::lock_guard<HostRwLock> lock(m_mutex);
        cs = get_or_create_critical_section(cs_addr);
    }
    // Acquire by polling rather than a bare lock() so a section that never
    // becomes available can name its holder. A deadlocked EnterCriticalSection
    // is otherwise indistinguishable from ordinary contention: the waiter is
    // parked inside a host thunk, so a hang dump reports only the guest return
    // address, never which section or which thread is holding it.
    // Block properly by default: this is a hot lock, and polling it turns
    // every contended acquire into a scheduler round trip. The diagnostic
    // path, which can name the holder of a section that never frees, is opt-in
    // precisely because it has to poll to do so.
    static const bool diag = std::getenv("VITREO_LOG_CS") != nullptr;
    if (!diag) {
        cs->mutex.lock();
    } else if (!cs->mutex.try_lock()) {
        const auto start = std::chrono::steady_clock::now();
        bool warned = false;
        while (!cs->mutex.try_lock()) {
            if (!warned &&
                std::chrono::steady_clock::now() - start >
                    std::chrono::seconds(5)) {
                warned = true;
                std::fprintf(stderr,
                    "[cs] STUCK EnterCriticalSection cs=0x%llx waiter=%u "
                    "owner=%u recursion=%u (>5s)\n",
                    static_cast<unsigned long long>(cs_addr),
                    ThreadManager::get_current_thread_id_static(),
                    cs->owning_thread_id, cs->recursion_count);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    cs->recursion_count++;
    cs->owning_thread_id = ThreadManager::get_current_thread_id_static();
}

bool SyncManager::try_enter_critical_section(GuestAddr cs_addr) {
    CriticalSection* cs = nullptr;
    {
        std::shared_lock<HostRwLock> lock(m_mutex);
        auto it = m_critical_sections.find(cs_addr);
        if (it != m_critical_sections.end()) cs = it->second.get();
    }
    if (!cs) {
        std::lock_guard<HostRwLock> lock(m_mutex);
        cs = get_or_create_critical_section(cs_addr);
    }
    if (cs->mutex.try_lock()) {
        cs->recursion_count++;
        cs->owning_thread_id = ThreadManager::get_current_thread_id_static();
        return true;
    }
    return false;
}

void SyncManager::leave_critical_section(GuestAddr cs_addr) {
    CriticalSection* cs = nullptr;
    {
        std::shared_lock<HostRwLock> lock(m_mutex);
        auto it = m_critical_sections.find(cs_addr);
        if (it == m_critical_sections.end()) {
            return; // Not initialized — nothing to do
        }
        cs = it->second.get();
    }
    if (cs->recursion_count > 0) {
        cs->recursion_count--;
    }
    if (cs->recursion_count == 0) {
        cs->owning_thread_id = 0;
    }
    cs->mutex.unlock();
}

void SyncManager::delete_critical_section(GuestAddr cs_addr) {
    std::lock_guard<HostRwLock> lock(m_mutex);
    m_critical_sections.erase(cs_addr);
}

bool SyncManager::wait_condition_variable(GuestAddr cv_addr,
                                          u64 observed_generation,
                                          u32 milliseconds) {
    ConditionVariable* cv = nullptr;
    {
        std::lock_guard<HostRwLock> lock(m_mutex);
        cv = get_or_create_condition_variable(cv_addr);
    }

    std::unique_lock<std::mutex> wait_lock(cv->wait_mutex);
    bool signaled = true;
    if (milliseconds == 0) {
        signaled =
            cv->generation.load(std::memory_order_acquire) != observed_generation;
    } else if (milliseconds == INFINITE_WAIT) {
        cv->cv.wait(wait_lock, [&]() {
            return m_shutdown.load(std::memory_order_acquire) ||
                   cv->generation.load(std::memory_order_acquire) !=
                       observed_generation;
        });
        signaled = !m_shutdown.load(std::memory_order_acquire);
    } else {
        signaled = cv->cv.wait_for(
            wait_lock, std::chrono::milliseconds(milliseconds), [&]() {
                return m_shutdown.load(std::memory_order_acquire) ||
                       cv->generation.load(std::memory_order_acquire) !=
                           observed_generation;
            });
        if (m_shutdown.load(std::memory_order_acquire)) signaled = false;
    }
    if (!signaled && !m_shutdown.load(std::memory_order_acquire)) {
        set_last_error(error_codes::ERROR_TIMEOUT);
    }
    return signaled;
}

u64 SyncManager::condition_variable_generation(GuestAddr cv_addr) {
    ConditionVariable* cv = nullptr;
    {
        std::lock_guard<HostRwLock> lock(m_mutex);
        cv = get_or_create_condition_variable(cv_addr);
    }
    return cv->generation.load(std::memory_order_acquire);
}

bool SyncManager::sleep_condition_variable_cs(GuestAddr cv_addr,
                                              GuestAddr cs_addr,
                                              u32 milliseconds) {
    // Host-only SyncManager CS path (unit tests). Production Enter/Leave go
    // through CriticalSectionManager; Kernel32ExtManager::impl_SleepConditionVariableCS
    // releases that manager's CS around wait_condition_variable().
    CriticalSection* cs = nullptr;
    {
        std::lock_guard<HostRwLock> lock(m_mutex);
        cs = get_or_create_critical_section(cs_addr);
        (void)get_or_create_condition_variable(cv_addr);
    }

    if (cs->owning_thread_id != ThreadManager::get_current_thread_id_static() ||
        cs->recursion_count == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }

    if (cs->recursion_count != 1) {
        VITREO_TRACE("[sync] SleepConditionVariableCS(cv=0x%llx, cs=0x%llx) unsupported recursion=%u\n",
                     static_cast<unsigned long long>(cv_addr),
                     static_cast<unsigned long long>(cs_addr),
                     cs->recursion_count);
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }

    const u64 observed_generation = condition_variable_generation(cv_addr);
    leave_critical_section(cs_addr);
    const bool signaled =
        wait_condition_variable(cv_addr, observed_generation, milliseconds);
    enter_critical_section(cs_addr);

    VITREO_TRACE("[sync] SleepConditionVariableCS(cv=0x%llx, cs=0x%llx, ms=%u) -> %s\n",
                 static_cast<unsigned long long>(cv_addr),
                 static_cast<unsigned long long>(cs_addr), milliseconds,
                 signaled ? "TRUE" : "FALSE/ERROR_TIMEOUT");
    return signaled;
}

bool SyncManager::sleep_condition_variable_cs(
    CriticalSectionManager& critical_sections,
    GuestAddr cv_addr,
    GuestAddr cs_addr,
    u32 milliseconds) {
    const u32 tid = ThreadManager::get_current_thread_id_static();
    if (cs_addr == 0 || critical_sections.owning_thread(cs_addr) != tid ||
        critical_sections.recursion_count(cs_addr) == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    const u32 recursion = critical_sections.recursion_count(cs_addr);
    if (recursion != 1) {
        VITREO_TRACE(
            "[sync] SleepConditionVariableCS(cv=0x%llx, cs=0x%llx) "
            "unsupported recursion=%u\n",
            static_cast<unsigned long long>(cv_addr),
            static_cast<unsigned long long>(cs_addr), recursion);
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }

    const u64 observed = condition_variable_generation(cv_addr);
    critical_sections.leave(cs_addr);
    const bool signaled =
        wait_condition_variable(cv_addr, observed, milliseconds);
    critical_sections.enter(cs_addr);
    return signaled;
}

void SyncManager::wake_all_condition_variable(GuestAddr cv_addr) {
    ConditionVariable* cv = nullptr;
    {
        std::lock_guard<HostRwLock> lock(m_mutex);
        cv = get_or_create_condition_variable(cv_addr);
    }

    // The generation MUST be bumped while holding the same mutex the waiters
    // check it under, even though it is atomic. A waiter evaluates the
    // predicate and parks while holding cv->wait_mutex; a waker that bumps and
    // notifies outside that mutex can land entirely inside the window between
    // "predicate said false" and "parked", and the notify is then delivered to
    // nobody. The waiter sleeps forever on an INFINITE wait. ([thread.condition]
    // is explicit that an atomic predicate still has to be modified under the
    // mutex to be published to a waiter.)
    {
        std::lock_guard<std::mutex> publish(cv->wait_mutex);
        cv->generation.fetch_add(1, std::memory_order_release);
    }
    cv->cv.notify_all();
}

void SyncManager::wake_condition_variable(GuestAddr cv_addr) {
    ConditionVariable* cv = nullptr;
    {
        std::lock_guard<HostRwLock> lock(m_mutex);
        cv = get_or_create_condition_variable(cv_addr);
    }

    // Same publication rule as wake_all_condition_variable — see the comment
    // there. This is the one that hung SyncTest.WakeConditionVariableWakesWaiter
    // roughly 1 run in 4.
    {
        std::lock_guard<std::mutex> publish(cv->wait_mutex);
        cv->generation.fetch_add(1, std::memory_order_release);
    }
    cv->cv.notify_one();
}

SrwLock* SyncManager::srw_lock_for(GuestAddr lock_addr) {
    {
        std::shared_lock<HostRwLock> guard(m_mutex);
        auto it = m_srw_locks.find(lock_addr);
        if (it != m_srw_locks.end()) return it->second.get();
    }
    std::lock_guard<HostRwLock> guard(m_mutex);
    return get_or_create_srw_lock(lock_addr);
}

SrwLock* SyncManager::srw_lock_if_present(GuestAddr lock_addr) {
    std::shared_lock<HostRwLock> guard(m_mutex);
    auto it = m_srw_locks.find(lock_addr);
    return it == m_srw_locks.end() ? nullptr : it->second.get();
}

void SyncManager::initialize_srw_lock(GuestAddr lock_addr) {
    std::lock_guard<HostRwLock> lock(m_mutex);
    get_or_create_srw_lock(lock_addr);
}

void SyncManager::acquire_srw_lock_exclusive(GuestAddr lock_addr) {
    SrwLock* lock = srw_lock_for(lock_addr);

    // Recursive exclusive acquisition is UB on real Windows.  We tolerate it
    // with a recursion counter so that emulation-induced re-entrant code
    // paths (DllMain inside _initterm, etc.) don't hard-deadlock on
    // std::mutex.  This is a valid UB outcome and doesn't break the contract.
    u32 tid = ThreadManager::get_current_thread_id_static();
    if (tid != 0 &&
        lock->owning_thread_id.load(std::memory_order_acquire) == tid) {
        ++lock->exclusive_recursion;
        return;
    }

    lock->rw.lock();
    lock->exclusively_locked = true;
    lock->exclusive_recursion = 1;
    // Publish ownership last: a concurrent acquire must never see this thread
    // named as owner before the state it owns is set up.
    lock->owning_thread_id.store(tid, std::memory_order_release);
}

bool SyncManager::try_acquire_srw_lock_exclusive(GuestAddr lock_addr) {
    SrwLock* lock = srw_lock_for(lock_addr);

    u32 tid = ThreadManager::get_current_thread_id_static();
    // SRW locks are not recursive. Real TryAcquireSRWLockExclusive is an
    // interlocked compare-exchange on the lock word, so a lock that is already
    // held fails — including when the caller itself holds it. Reporting success
    // there would hand a second, concurrent entry to code that uses the lock to
    // exclude itself (an allocator re-entering its own freelist update, say),
    // which corrupts state instead of deadlocking as Windows would.
    if (tid != 0 &&
        lock->owning_thread_id.load(std::memory_order_acquire) == tid) {
        return false;
    }

    if (!lock->rw.try_lock()) {
        return false;
    }

    lock->exclusively_locked = true;
    lock->exclusive_recursion = 1;
    lock->owning_thread_id.store(tid, std::memory_order_release);
    return true;
}

void SyncManager::release_srw_lock_exclusive(GuestAddr lock_addr) {
    SrwLock* lock = srw_lock_if_present(lock_addr);
    if (lock == nullptr) return;

    if (lock->owning_thread_id.load(std::memory_order_acquire) !=
            ThreadManager::get_current_thread_id_static() ||
        !lock->exclusively_locked) {
        return;
    }

    if (lock->exclusive_recursion > 1) {
        --lock->exclusive_recursion;
        return;
    }

    lock->exclusive_recursion = 0;
    lock->exclusively_locked = false;
    lock->owning_thread_id.store(0, std::memory_order_release);
    lock->rw.unlock();
}

void SyncManager::acquire_srw_lock_shared(GuestAddr lock_addr) {
    SrwLock* lock = srw_lock_for(lock_addr);
    // A thread that already owns this lock EXCLUSIVELY cannot also take it
    // shared: std::shared_mutex would deadlock against itself. Windows calls
    // that UB; count it as exclusive recursion, the same tolerance the
    // exclusive path already applies for emulation-induced re-entry.
    const u32 tid = ThreadManager::get_current_thread_id_static();
    if (tid != 0 &&
        lock->owning_thread_id.load(std::memory_order_acquire) == tid) {
        ++lock->exclusive_recursion;
        return;
    }
    lock->rw.lock_shared();
    lock->shared_holders.fetch_add(1, std::memory_order_release);
}

bool SyncManager::try_acquire_srw_lock_shared(GuestAddr lock_addr) {
    SrwLock* lock = srw_lock_for(lock_addr);
    const u32 tid = ThreadManager::get_current_thread_id_static();
    if (tid != 0 &&
        lock->owning_thread_id.load(std::memory_order_acquire) == tid) {
        return false;  // already held exclusively by us — Windows fails here
    }
    if (!lock->rw.try_lock_shared()) return false;
    lock->shared_holders.fetch_add(1, std::memory_order_release);
    return true;
}

bool SyncManager::release_srw_lock_shared(GuestAddr lock_addr) {
    SrwLock* lock = srw_lock_if_present(lock_addr);
    if (lock == nullptr) return false;
    const u32 tid = ThreadManager::get_current_thread_id_static();
    if (tid != 0 &&
        lock->owning_thread_id.load(std::memory_order_acquire) == tid &&
        lock->exclusive_recursion > 1) {
        --lock->exclusive_recursion;  // matched the recursion tolerance above
        return false;
    }
    i32 held = lock->shared_holders.load(std::memory_order_acquire);
    while (held > 0 &&
           !lock->shared_holders.compare_exchange_weak(
               held, held - 1, std::memory_order_acq_rel,
               std::memory_order_acquire)) {
    }
    if (held <= 0) return false;  // releasing a lock nobody holds
    lock->rw.unlock_shared();
    return held == 1;
}

bool SyncManager::sleep_condition_variable_srw(GuestAddr cv_addr,
                                               GuestAddr lock_addr,
                                               u32 milliseconds,
                                               u32 /*flags*/) {
    ConditionVariable* cv = nullptr;
    SrwLock* lock = nullptr;
    {
        std::lock_guard<HostRwLock> guard(m_mutex);
        cv = get_or_create_condition_variable(cv_addr);
        lock = get_or_create_srw_lock(lock_addr);
    }

    if (lock->owning_thread_id.load(std::memory_order_acquire) !=
            ThreadManager::get_current_thread_id_static() ||
        !lock->exclusively_locked) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }

    VITREO_TRACE(
        "[sync] SleepConditionVariableSRW ENTER cv=0x%llx lock=0x%llx ms=%u\n",
        static_cast<unsigned long long>(cv_addr),
        static_cast<unsigned long long>(lock_addr), milliseconds);

    const u64 observed_generation = cv->generation.load(std::memory_order_acquire);

    u32 saved_recursion = lock->exclusive_recursion;
    lock->exclusive_recursion = 0;
    lock->exclusively_locked = false;
    lock->owning_thread_id.store(0, std::memory_order_release);
    // Park on cv->wait_mutex, exactly as the CRITICAL_SECTION path does, rather
    // than on the SRW lock's own mutex. One condition_variable_any served two
    // DIFFERENT mutexes depending on which API the waiter came through, so
    // there was no single lock a waker could publish the generation under — a
    // waker holding wait_mutex still raced an SRW waiter, and vice versa.
    // Releasing the SRW lock first is safe because observed_generation was
    // sampled above while it was still held: any wake that lands in the gap
    // makes the predicate true immediately instead of being lost.
    lock->rw.unlock();

    bool signaled = true;
    {
        std::unique_lock<std::mutex> wait_lock(cv->wait_mutex);
        if (milliseconds == 0) {
            signaled = cv->generation.load(std::memory_order_acquire) !=
                       observed_generation;
        } else if (milliseconds == INFINITE_WAIT) {
            cv->cv.wait(wait_lock, [&]() {
                return m_shutdown.load(std::memory_order_acquire) ||
                       cv->generation.load(std::memory_order_acquire) !=
                           observed_generation;
            });
            signaled = !m_shutdown.load(std::memory_order_acquire);
        } else {
            signaled = cv->cv.wait_for(
                wait_lock, std::chrono::milliseconds(milliseconds), [&]() {
                    return m_shutdown.load(std::memory_order_acquire) ||
                           cv->generation.load(std::memory_order_acquire) !=
                               observed_generation;
                });
            if (m_shutdown.load(std::memory_order_acquire)) signaled = false;
        }
    }

    lock->rw.lock();
    lock->exclusively_locked = true;
    lock->exclusive_recursion = saved_recursion;
    lock->owning_thread_id.store(ThreadManager::get_current_thread_id_static(),
                                 std::memory_order_release);
    if (!signaled && !m_shutdown.load(std::memory_order_acquire)) {
        set_last_error(error_codes::ERROR_TIMEOUT);
    }
    VITREO_TRACE("[sync] SleepConditionVariableSRW(cv=0x%llx, lock=0x%llx, ms=%u) -> %s\n",
                 static_cast<unsigned long long>(cv_addr),
                 static_cast<unsigned long long>(lock_addr), milliseconds,
                 signaled ? "TRUE" : "FALSE/ERROR_TIMEOUT");
    return signaled;
}

// =============================================================================
// Mutex Operations
// =============================================================================

Handle SyncManager::create_mutex(bool initial_owner, const std::string& name) {
    return create_mutex(initial_owner,name,false,sync_access::MUTEX_ALL_ACCESS,{});
}
Handle SyncManager::create_mutex(bool initial_owner, const std::string& raw_name,
    bool inheritable,u32 desired_access,SecurityFactory security_factory) {
    // Learn (Kernel Object Namespaces): "Local\\" names the session
    // namespace a plain name already lives in, so Local\\X and X are one
    // object (nt_event_probe named_local_win32).
    const std::string name = sync_object_key(raw_name);
    // For named mutexes, Windows returns an existing handle if the object
    // already exists and sets GetLastError() to ERROR_ALREADY_EXISTS.

    std::lock_guard<HostRwLock> lock(m_mutex);

    if (!name.empty()) {
        auto it = m_named_mutexes.find(name);
        if (it != m_named_mutexes.end()) {
            if (auto existing = it->second.lock()) {
                u32 granted=0;
                if(!authorize_object(existing->security,desired_access,true,granted))return NULL_HANDLE;
                Handle h = m_handles.allocate(HandleType::Mutex, existing.get(),
                    inheritable?HandleFlags::Inherit:0,granted);
                m_mutexes[h] = std::move(existing);
                set_last_error(error_codes::ERROR_ALREADY_EXISTS);
                return h;
            }
            m_named_mutexes.erase(it);
        }
    }

    u32 tid = ThreadManager::get_current_thread_id_static();
    if (!name.empty() &&
        named_object_kind_conflict(name, NamedObjectKind::Mutex)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return NULL_HANDLE;
    }
    auto mtx = std::make_shared<SyncMutex>();
    if(!capture_object_security(mtx->security,security_factory))return NULL_HANDLE;
    mtx->name = name;
    mtx->transfer_name = name;
    if (ipc::ipc_enabled() && !name.empty()) {
        mtx->shared = ipc::SharedSyncObject::create_or_open(
            name, ipc::SyncKind::Mutex, initial_owner ? 1 : 0,
            static_cast<i32>(tid), /*create=*/true);
    }
    if(mtx->shared && mtx->shared->kind()!=ipc::SyncKind::Mutex) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);return NULL_HANDLE;
    }
    if (!mtx->shared) {
        mtx->nt_id = ntsync::global().create_mutex(initial_owner, tid);
    }
    using namespace sync_access;
    u32 granted=object_access::map_generic(desired_access&~MAXIMUM_ALLOWED,
        MUTEX_ALL_ACCESS,READ_CONTROL,READ_CONTROL,READ_CONTROL|SYNCHRONIZE);
    if(desired_access&MAXIMUM_ALLOWED)granted|=MUTEX_ALL_ACCESS;
    Handle h = m_handles.allocate(HandleType::Mutex, mtx.get(),
        inheritable?HandleFlags::Inherit:0,granted);

    const bool already_exists = mtx->shared && !mtx->shared->was_created();
    m_mutexes[h] = mtx;
    if (!name.empty()) {
        m_named_mutexes[name] = mtx;
        (void)named_object_register(name, NamedObjectKind::Mutex);
    }

    set_last_error(already_exists ? error_codes::ERROR_ALREADY_EXISTS
                                  : error_codes::ERROR_SUCCESS);
    return h;
}

Handle SyncManager::open_mutex(const std::string& name) {
    return open_mutex(name,sync_access::MUTEX_ALL_ACCESS,false);
}
Handle SyncManager::open_mutex(const std::string& raw_name,u32 desired_access,bool inheritable) {
    // Learn (Kernel Object Namespaces): "Local\\" names the session
    // namespace a plain name already lives in, so Local\\X and X are one
    // object (nt_event_probe named_local_win32).
    const std::string name = sync_object_key(raw_name);
    // Learn OpenMutexW: lpName NULL → ERROR_INVALID_PARAMETER; missing name →
    // ERROR_FILE_NOT_FOUND; name of a different object kind → ERROR_INVALID_HANDLE.
    if (name.empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return NULL_HANDLE;
    }
    std::lock_guard<HostRwLock> lock(m_mutex);

    auto it = m_named_mutexes.find(name);
    if (it != m_named_mutexes.end()) {
        if (auto existing = it->second.lock()) {
            u32 granted=0;
            if(!authorize_object(existing->security,desired_access,true,granted))return NULL_HANDLE;
            Handle h = m_handles.allocate(HandleType::Mutex, existing.get(),
                inheritable?HandleFlags::Inherit:0,granted);
            m_mutexes[h] = std::move(existing);
            return h;
        }
        m_named_mutexes.erase(it);
    }

    if (named_object_kind_conflict(name, NamedObjectKind::Mutex)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return NULL_HANDLE;
    }

    if (ipc::ipc_enabled()) {
        auto shared = ipc::SharedSyncObject::create_or_open(
            name, ipc::SyncKind::Mutex, 0, 0, /*create=*/false);
        if (shared && shared->kind() != ipc::SyncKind::Mutex) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return NULL_HANDLE;
        }
        if (shared) {
            // Peer DACL is not on the shm block yet; grant the mapped mask
            // under an empty descriptor (same-user capsule opens match Windows
            // default DACL). CreateSemaphore already attaches peers this way.
            using namespace sync_access;
            u32 granted=0;
            if(!authorize_object({},desired_access,true,granted))return NULL_HANDLE;
            auto mtx = std::make_shared<SyncMutex>();
            mtx->name = name;
            mtx->transfer_name = name;
            mtx->shared = std::move(shared);
            Handle h = m_handles.allocate(HandleType::Mutex, mtx.get(),
                inheritable?HandleFlags::Inherit:0,granted);
            m_mutexes[h] = mtx;
            m_named_mutexes[name] = mtx;
            (void)named_object_register(name, NamedObjectKind::Mutex);
            return h;
        }
    }

    set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
    return NULL_HANDLE;
}

bool SyncManager::release_mutex(Handle h) {
    u64 nt_id = 0;
    std::shared_ptr<ipc::SharedSyncObject> shared;
    {
        std::shared_lock<HostRwLock> lock(m_mutex);
        auto it = m_mutexes.find(h);
        if (it == m_mutexes.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return false;
        }
        shared = it->second->shared;
        nt_id = it->second->nt_id;
    }
    u32 tid = ThreadManager::get_current_thread_id_static();
    const bool ok = shared ? shared->release_mutex(tid)
                           : ntsync::global().release_mutex(nt_id, tid);
    if (!ok) {
        set_last_error(error_codes::ERROR_NOT_OWNER);
        return false;
    }
    return true;
}

void SyncManager::abandon_mutexes_for_thread(u32 tid) {
    std::vector<std::shared_ptr<ipc::SharedSyncObject>> shared_mutexes;
    {
        std::shared_lock<HostRwLock> lock(m_mutex);
        shared_mutexes.reserve(m_mutexes.size());
        for (const auto& [handle, mutex] : m_mutexes) {
            (void)handle;
            if (!mutex || !mutex->shared) continue;
            if (std::find(shared_mutexes.begin(), shared_mutexes.end(),
                          mutex->shared) == shared_mutexes.end()) {
                shared_mutexes.push_back(mutex->shared);
            }
        }
    }
    for (const auto& mutex : shared_mutexes) {
        (void)mutex->abandon_mutex(tid);
    }
}

} // namespace vitreo::winapi
