// SPDX-License-Identifier: LGPL-2.1-only

/// @file sync_semaphore.cpp
/// @brief Semaphore operations on SyncManager.
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
// Semaphore Operations
// =============================================================================

Handle SyncManager::create_semaphore(i32 initial_count, i32 max_count,
                                     const std::string& raw_name,
                                     bool inheritable, u32 desired_access) {
    // Learn (Kernel Object Namespaces): "Local\\" names the session
    // namespace a plain name already lives in, so Local\\X and X are one
    // object (nt_event_probe named_local_win32).
    const std::string name = sync_object_key(raw_name);
    if (max_count <= 0 || initial_count < 0 || initial_count > max_count) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    using namespace sync_access;
    const u32 access =
        desired_access == 0 ? SEMAPHORE_ALL_ACCESS : desired_access;
    std::lock_guard<HostRwLock> lock(m_mutex);
    if (!name.empty()) {
        auto it = m_named_semaphores.find(name);
        if (it != m_named_semaphores.end()) {
            if (auto existing = it->second.lock()) {
                u32 granted = object_access::map_generic(
                    access & ~MAXIMUM_ALLOWED, SEMAPHORE_ALL_ACCESS,
                    READ_CONTROL, READ_CONTROL | SEMAPHORE_MODIFY_STATE,
                    READ_CONTROL | SYNCHRONIZE);
                if (access & MAXIMUM_ALLOWED) granted |= SEMAPHORE_ALL_ACCESS;
                Handle h = m_handles.allocate(
                    HandleType::Semaphore, existing.get(),
                    inheritable ? HandleFlags::Inherit : 0, granted);
                m_semaphores[h] = std::move(existing);
                set_last_error(error_codes::ERROR_ALREADY_EXISTS);
                return h;
            }
            m_named_semaphores.erase(it);
        }
    }
    if (!name.empty() &&
        named_object_kind_conflict(name, NamedObjectKind::Semaphore)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return 0;
    }
    auto sem = std::make_shared<SyncSemaphore>();
    sem->name = name;
    sem->transfer_name = name;
    if (ipc::ipc_enabled() && !name.empty()) {
        sem->shared = ipc::SharedSyncObject::create_or_open(
            name, ipc::SyncKind::Semaphore, initial_count, max_count, /*create=*/true);
    }
    if (!sem->shared) {
        sem->nt_id = ntsync::global().create_semaphore(initial_count, max_count);
    }
    u32 granted = object_access::map_generic(
        access & ~MAXIMUM_ALLOWED, SEMAPHORE_ALL_ACCESS, READ_CONTROL,
        READ_CONTROL | SEMAPHORE_MODIFY_STATE, READ_CONTROL | SYNCHRONIZE);
    if (access & MAXIMUM_ALLOWED) granted |= SEMAPHORE_ALL_ACCESS;
    Handle h = m_handles.allocate(HandleType::Semaphore, sem.get(),
                                  inheritable ? HandleFlags::Inherit : 0, granted);
    const bool already_exists = sem->shared && !sem->shared->was_created();
    m_semaphores[h] = sem;
    if (!name.empty()) {
        m_named_semaphores[name] = sem;
        (void)named_object_register(name, NamedObjectKind::Semaphore);
    }
    set_last_error(already_exists ? error_codes::ERROR_ALREADY_EXISTS
                                  : error_codes::ERROR_SUCCESS);
    return h;
}

Handle SyncManager::open_semaphore(const std::string& name) {
    return open_semaphore(name, sync_access::SEMAPHORE_ALL_ACCESS, false);
}

Handle SyncManager::open_semaphore(const std::string& raw_name, u32 desired_access,
                                   bool inheritable) {
    // Learn (Kernel Object Namespaces): "Local\\" names the session
    // namespace a plain name already lives in, so Local\\X and X are one
    // object (nt_event_probe named_local_win32).
    const std::string name = sync_object_key(raw_name);
    if (name.empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return NULL_HANDLE;
    }
    using namespace sync_access;
    std::lock_guard<HostRwLock> lock(m_mutex);

    auto it = m_named_semaphores.find(name);
    if (it != m_named_semaphores.end()) {
        if (auto existing = it->second.lock()) {
            u32 granted = object_access::map_generic(
                desired_access & ~MAXIMUM_ALLOWED, SEMAPHORE_ALL_ACCESS,
                READ_CONTROL, READ_CONTROL | SEMAPHORE_MODIFY_STATE,
                READ_CONTROL | SYNCHRONIZE);
            if (desired_access & MAXIMUM_ALLOWED) granted |= SEMAPHORE_ALL_ACCESS;
            if (desired_access == 0) {
                set_last_error(error_codes::ERROR_ACCESS_DENIED);
                return NULL_HANDLE;
            }
            Handle h = m_handles.allocate(
                HandleType::Semaphore, existing.get(),
                inheritable ? HandleFlags::Inherit : 0, granted);
            m_semaphores[h] = std::move(existing);
            return h;
        }
        m_named_semaphores.erase(it);
    }

    if (named_object_kind_conflict(name, NamedObjectKind::Semaphore)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return NULL_HANDLE;
    }

    if (ipc::ipc_enabled()) {
        auto shared = ipc::SharedSyncObject::create_or_open(
            name, ipc::SyncKind::Semaphore, 0, 0, /*create=*/false);
        if (shared && shared->kind() != ipc::SyncKind::Semaphore) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return NULL_HANDLE;
        }
        if (shared) {
            if (desired_access == 0) {
                set_last_error(error_codes::ERROR_ACCESS_DENIED);
                return NULL_HANDLE;
            }
            u32 granted = object_access::map_generic(
                desired_access & ~MAXIMUM_ALLOWED, SEMAPHORE_ALL_ACCESS,
                READ_CONTROL, READ_CONTROL | SEMAPHORE_MODIFY_STATE,
                READ_CONTROL | SYNCHRONIZE);
            if (desired_access & MAXIMUM_ALLOWED) granted |= SEMAPHORE_ALL_ACCESS;
            auto sem = std::make_shared<SyncSemaphore>();
            sem->name = name;
            sem->transfer_name = name;
            sem->shared = std::move(shared);
            Handle h = m_handles.allocate(
                HandleType::Semaphore, sem.get(),
                inheritable ? HandleFlags::Inherit : 0, granted);
            m_semaphores[h] = sem;
            m_named_semaphores[name] = sem;
            (void)named_object_register(name, NamedObjectKind::Semaphore);
            return h;
        }
    }

    set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
    return NULL_HANDLE;
}

bool SyncManager::release_semaphore(Handle h, i32 release_count, i32* previous_out) {
    // Learn ReleaseSemaphore + handles_waits_probe.expected:
    //   lReleaseCount <= 0 → ERROR_INVALID_PARAMETER
    //   missing SEMAPHORE_MODIFY_STATE → ERROR_ACCESS_DENIED
    //   unknown/wrong-type handle → ERROR_INVALID_HANDLE
    //   count would exceed maximum → ERROR_TOO_MANY_POSTS
    if (release_count <= 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    u64 nt_id = 0;
    std::shared_ptr<ipc::SharedSyncObject> shared;
    {
        std::shared_lock<HostRwLock> lock(m_mutex);
        auto it = m_semaphores.find(h);
        if (it == m_semaphores.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return false;
        }
        if (!(m_handles.get_access(h) & sync_access::SEMAPHORE_MODIFY_STATE)) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            return false;
        }
        shared = it->second->shared;
        nt_id = it->second->nt_id;
    }
    const bool ok = shared
        ? shared->release_semaphore(release_count, previous_out)
        : ntsync::global().release_semaphore(nt_id, release_count, previous_out);
    if (!ok) {
        set_last_error(error_codes::ERROR_TOO_MANY_POSTS);
        return false;
    }
    return true;
}

bool SyncManager::query_semaphore(Handle h, i32* current_out,
                                  i32* maximum_out) const {
    u64 nt_id = 0;
    std::shared_ptr<ipc::SharedSyncObject> shared;
    {
        std::shared_lock<HostRwLock> lock(m_mutex);
        auto it = m_semaphores.find(h);
        if (it == m_semaphores.end()) return false;
        shared = it->second->shared;
        nt_id = it->second->nt_id;
    }
    if (shared) {
        return shared->query_semaphore(current_out, maximum_out);
    }
    auto snap = ntsync::global().snapshot(nt_id);
    if (!snap || snap->type != ntsync::ObjType::Semaphore) {
        return false;
    }
    if (current_out) *current_out = snap->sem_count;
    if (maximum_out) *maximum_out = snap->sem_max;
    return true;
}

// Caller holds m_mutex (shared is enough): every wait path takes it before
// resolving, and this must not take it again (HostRwLock is not re-entrant
// once a writer is queued).
u64 SyncManager::resolve_nt_id(Handle h) const {
    if (auto it = m_events.find(h); it != m_events.end()) return it->second->nt_id;
    if (auto it = m_mutexes.find(h); it != m_mutexes.end()) return it->second->nt_id;
    if (auto it = m_semaphores.find(h); it != m_semaphores.end()) return it->second->nt_id;
    if (auto it = m_extra_waitables.find(h); it != m_extra_waitables.end()) {
        return it->second;
    }
    return 0;
}

void SyncManager::register_waitable(Handle h, u64 nt_id) {
    std::unique_lock<HostRwLock> lock(m_mutex);
    m_extra_waitables[h] = nt_id;
}

void SyncManager::unregister_waitable(Handle h) {
    std::unique_lock<HostRwLock> lock(m_mutex);
    m_extra_waitables.erase(h);
}

std::shared_ptr<ipc::SharedSyncObject> SyncManager::resolve_shared(Handle h) const {
    if (auto it = m_events.find(h); it != m_events.end()) return it->second->shared;
    if (auto it = m_mutexes.find(h); it != m_mutexes.end()) return it->second->shared;
    if (auto it = m_semaphores.find(h); it != m_semaphores.end()) return it->second->shared;
    return nullptr;
}

} // namespace vitreo::winapi
