// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file host_rwlock.hpp
/// @brief Reader-scalable host reader/writer lock (a drop-in SharedMutex).
///
/// libc++'s std::shared_mutex is a std::mutex plus two condition variables:
/// every lock_shared() takes that internal mutex, so readers on many threads
/// serialise against each other even when no writer exists. On a JIT lookup
/// path hit by every guest thread that turned the "shared" lock into the
/// process's hottest mutex. pthread_rwlock admits an uncontended reader with
/// a single atomic operation and only enters the kernel when a writer is
/// involved.
///
/// Satisfies the standard SharedMutex requirements, so std::shared_lock,
/// std::unique_lock and std::lock_guard work unchanged.

#include <pthread.h>

namespace vitreo {

class HostRwLock {
public:
    HostRwLock() noexcept { pthread_rwlock_init(&m_lock, nullptr); }
    ~HostRwLock() { pthread_rwlock_destroy(&m_lock); }
    HostRwLock(const HostRwLock&) = delete;
    HostRwLock& operator=(const HostRwLock&) = delete;

    void lock() noexcept { pthread_rwlock_wrlock(&m_lock); }
    bool try_lock() noexcept { return pthread_rwlock_trywrlock(&m_lock) == 0; }
    void unlock() noexcept { pthread_rwlock_unlock(&m_lock); }

    void lock_shared() noexcept { pthread_rwlock_rdlock(&m_lock); }
    bool try_lock_shared() noexcept {
        return pthread_rwlock_tryrdlock(&m_lock) == 0;
    }
    void unlock_shared() noexcept { pthread_rwlock_unlock(&m_lock); }

private:
    pthread_rwlock_t m_lock;
};

} // namespace vitreo
