#include "winapi/common/object_access_constants.hpp"
// SPDX-License-Identifier: LGPL-2.1-only

/// @file sync.cpp
/// @brief Implementation of synchronization primitives (THREAD-002)

#include "sync.hpp"
#include "sync_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/named_object_directory.hpp"
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

/// @file event_operations.cpp
/// @brief Event Operations
///
/// Closed cut from sync.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Event Operations
// =============================================================================

bool SyncManager::authorize_object(const object_access::Descriptor& security,
    u32 desired_access,bool mutex,u32& granted) const {
    using namespace sync_access;
    if(desired_access==0) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);return false;
    }
    const auto result=object_access::evaluate(security,object_token(),desired_access,
        mutex?MUTEX_ALL_ACCESS:EVENT_ALL_ACCESS,READ_CONTROL|(mutex?0u:1u),
        READ_CONTROL|(mutex?0u:EVENT_MODIFY_STATE),READ_CONTROL|SYNCHRONIZE,granted);
    if(result==object_access::Result::Granted)return true;
    if(result==object_access::Result::Unsupported) {
        static std::atomic_flag reported=ATOMIC_FLAG_INIT;
        if(!reported.test_and_set())std::fprintf(stderr,"[vitreo] UNIMPLEMENTED kernel32!SynchronizationAccess: SACL or non-basic DACL ACE\n");
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
    } else set_last_error(result==object_access::Result::Invalid?
        error_codes::ERROR_INVALID_ACL:error_codes::ERROR_ACCESS_DENIED);
    return false;
}
bool SyncManager::capture_object_security(object_access::Descriptor& security,
    const SecurityFactory& factory) const {
    const auto token=object_token();security.owner=token.user;
    if(!token.default_dacl.empty())security.dacl=token.default_dacl;
    if(factory && !factory(security))return false;
    // Measured (sync_sacl_label_probe): a label above the caller's own
    // integrity level (System from a High caller) is ERROR_PRIVILEGE_NOT_HELD.
    if(security.has_label && security.label_rid>token.integrity_rid){
        set_last_error(error_codes::ERROR_PRIVILEGE_NOT_HELD);return false;
    }
    // Measured: audit ACEs are accepted from an elevated caller (whose token
    // holds SeSecurityPrivilege, even disabled) and refused with
    // ERROR_PRIVILEGE_NOT_HELD from a filtered Medium token that lacks it.
    if(security.has_audit && !token.security_privilege_held){
        set_last_error(error_codes::ERROR_PRIVILEGE_NOT_HELD);return false;
    }
    // Validate all ACEs before publishing an object even though initial creation
    // grants its requested handle independently of the new object's DACL.
    u32 ignored=0;
    const auto result=object_access::evaluate(security,token,0,
        sync_access::EVENT_ALL_ACCESS,sync_access::READ_CONTROL|1u,
        sync_access::READ_CONTROL|sync_access::EVENT_MODIFY_STATE,
        sync_access::READ_CONTROL|sync_access::SYNCHRONIZE,ignored);
    if(result==object_access::Result::Unsupported) {
        static std::atomic_flag reported=ATOMIC_FLAG_INIT;
        if(!reported.test_and_set())std::fprintf(stderr,"[vitreo] UNIMPLEMENTED kernel32!SynchronizationSecurity: SACL or non-basic DACL ACE\n");
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);return false;
    }
    // Measured (sync_dacl_ace_layout_probe): a DACL RtlValidAcl rejects (an
    // AceSize that is not a multiple of 4, a SID cut off by its AceSize) is
    // ERROR_INVALID_ACL.
    if(result==object_access::Result::Invalid) {
        set_last_error(error_codes::ERROR_INVALID_ACL);return false;
    }
    return true;
}

namespace {
u32 event_access_mask(u32 access) {
    using namespace sync_access;
    if (access == HandleAccess::All) return EVENT_ALL_ACCESS;
    u32 result = access & ~(GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL | MAXIMUM_ALLOWED);
    if (access & GENERIC_READ) result |= READ_CONTROL | 1u; // EVENT_QUERY_STATE
    if (access & GENERIC_WRITE) result |= READ_CONTROL | EVENT_MODIFY_STATE;
    if (access & GENERIC_EXECUTE) result |= READ_CONTROL | SYNCHRONIZE;
    if (access & (GENERIC_ALL | MAXIMUM_ALLOWED)) result |= EVENT_ALL_ACCESS;
    return result;
}
}

Handle SyncManager::create_event(bool manual_reset, bool initial_state,
                                  const std::string& name, bool inheritable,
                                  SyncTraceContext trace, bool waitable_timer) {
    return create_event(manual_reset, initial_state, name, inheritable, trace,
                        sync_access::EVENT_ALL_ACCESS, {}, waitable_timer);
}

Handle SyncManager::create_event(bool manual_reset, bool initial_state,
                                  const std::string& raw_name, bool inheritable,
                                  SyncTraceContext trace, u32 desired_access,
                                  SecurityFactory security_factory,
                                  bool waitable_timer) {
    // Learn (Kernel Object Namespaces): "Local\\" names the session
    // namespace a plain name already lives in, so Local\\X and X are one
    // object (nt_event_probe named_local_win32).
    const std::string name = sync_object_key(raw_name);
    std::lock_guard<HostRwLock> lock(m_mutex);

    if (!name.empty()) {
        auto named = m_named_events.find(name);
        if (named != m_named_events.end()) {
            if (auto existing = named->second.lock()) {
                u32 granted=0;
                if(!authorize_object(existing->security,desired_access,false,granted))return NULL_HANDLE;
                Handle h = m_handles.allocate(
                    HandleType::Event, existing.get(),
                    inheritable ? HandleFlags::Inherit : 0, granted);
                m_events[h] = std::move(existing);
                set_last_error(error_codes::ERROR_ALREADY_EXISTS);
                return h;
            }
            m_named_events.erase(named);
        }
    }

    if (!name.empty() &&
        named_object_kind_conflict(name, NamedObjectKind::Event)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return NULL_HANDLE;
    }
    auto evt = std::make_shared<SyncEvent>();
    if(!capture_object_security(evt->security,security_factory))return NULL_HANDLE;
    evt->name = name;
    evt->transfer_name = name;
    evt->waitable_timer = waitable_timer;
    initialize_event_causality(*evt, manual_reset, initial_state, trace);
    if (ipc::ipc_enabled() && inheritable && evt->name.empty()) {
        evt->transfer_name = make_sync_transfer_name('E');
        // Preserve the existing inherited-handle descriptor format, which
        // carries one name for both rendezvous and child import.
        evt->name = evt->transfer_name;
    }
    // Cross-process: a named event is backed by shared memory so a peer process
    // creating/opening the same name shares one object.
    if (ipc::ipc_enabled() && !evt->transfer_name.empty()) {
        evt->shared = ipc::SharedSyncObject::create_or_open(
            evt->transfer_name, ipc::SyncKind::Event, manual_reset ? 1 : 0,
            initial_state ? 1 : 0, /*create=*/true);
        ipc::ipc_log("CreateEvent '%s' -> shared=%s",
                     evt->name.empty() ? "<unnamed-transfer>"
                                       : evt->name.c_str(),
                     evt->shared ? "yes" : "no");
        if (evt->shared && evt->shared->kind() != ipc::SyncKind::Event) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return NULL_HANDLE;
        }
    }
    if (!evt->shared) {
        evt->nt_id = ntsync::global().create_event(manual_reset, initial_state);
    }

    Handle h = m_handles.allocate(HandleType::Event, evt.get(),
                                  inheritable ? HandleFlags::Inherit : 0, event_access_mask(desired_access));

    const bool already_exists = evt->shared && !evt->shared->was_created();
    m_events[h] = evt;
    if (!evt->name.empty()) {
        m_named_events[evt->name] = evt;
        (void)named_object_register(evt->name, NamedObjectKind::Event);
    }
    set_last_error(already_exists ? error_codes::ERROR_ALREADY_EXISTS
                                  : error_codes::ERROR_SUCCESS);
    return h;
}

Handle SyncManager::open_event(u32 desired_access, const std::string& raw_name,
                               bool inheritable) {
    // Learn (Kernel Object Namespaces): "Local\\" names the session
    // namespace a plain name already lives in, so Local\\X and X are one
    // object (nt_event_probe named_local_win32).
    const std::string name = sync_object_key(raw_name);
    // Native event_name_pipe_duplicate_probe: an empty name is an invalid
    // object handle, distinct from a NULL name rejected by the API thunk.
    if (name.empty()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return NULL_HANDLE;
    }

    std::lock_guard<HostRwLock> lock(m_mutex);
    auto named = m_named_events.find(name);
    if (named != m_named_events.end()) {
        if (auto existing = named->second.lock()) {
            u32 granted=0;
            if(!authorize_object(existing->security,desired_access,false,granted))return NULL_HANDLE;
            Handle h = m_handles.allocate(
                HandleType::Event, existing.get(),
                inheritable ? HandleFlags::Inherit : 0, granted);
            m_events[h] = std::move(existing);
            return h;
        }
        m_named_events.erase(named);
    }

    if (named_object_kind_conflict(name, NamedObjectKind::Event)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return NULL_HANDLE;
    }

    if (ipc::ipc_enabled()) {
        auto shared = ipc::SharedSyncObject::create_or_open(
            name, ipc::SyncKind::Event, 0, 0, /*create=*/false);
        if (shared && shared->kind() != ipc::SyncKind::Event) {
            ipc::ipc_log("OpenEvent '%s' -> object type mismatch", name.c_str());
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return NULL_HANDLE;
        }
        if (shared) {
            u32 granted=0;
            if(!authorize_object({},desired_access,false,granted))return NULL_HANDLE;
            auto event = std::make_shared<SyncEvent>();
            event->name = name;
            event->transfer_name = name;
            event->shared = std::move(shared);
            Handle h = m_handles.allocate(
                HandleType::Event, event.get(),
                inheritable ? HandleFlags::Inherit : 0, granted);
            m_events[h] = event;
            m_named_events[name] = event;
            (void)named_object_register(name, NamedObjectKind::Event);
            return h;
        }
    }

    ipc::ipc_log("OpenEvent '%s' -> miss", name.c_str());
    set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
    return NULL_HANDLE;
}

std::string SyncManager::prepare_inherited_handles() {
    const auto handles = m_handles.handles_with_flags(HandleFlags::Inherit);
    return prepare_inherited_handles(handles);
}

std::string SyncManager::prepare_inherited_handles(
    std::span<const Handle> selected_handles) {
    std::string descriptors;
    for (Handle handle : selected_handles) {
        if ((m_handles.get_flags(handle) & HandleFlags::Inherit) == 0) {
            continue;
        }
        const HandleType type = m_handles.get_type(handle);
        if (type == HandleType::Event || type == HandleType::Mutex ||
            type == HandleType::Semaphore) {
            const auto transferred = prepare_transferred_handle(
                handle, m_handles.get_access(handle), /*inheritable=*/true);
            if (!transferred) continue;
            if (!descriptors.empty()) descriptors.push_back(';');
            descriptors += "I," + std::to_string(handle) + "," +
                           hex_encode_sync_name(*transferred);
        } else if (type == HandleType::Process) {
            auto* process = static_cast<ipc::ProcessObject*>(
                m_handles.get(handle, HandleType::Process));
            if (process) {
                if (!descriptors.empty()) descriptors.push_back(';');
                descriptors += "P," + std::to_string(handle) + "," +
                               std::to_string(process->pid);
            }
        }
    }
    return descriptors;
}

bool SyncManager::import_inherited_handles(std::string_view descriptors) {
    usize offset = 0;
    while (offset < descriptors.size()) {
        const usize end = descriptors.find(';', offset);
        const std::string_view item = descriptors.substr(
            offset, end == std::string_view::npos ? descriptors.size() - offset
                                                   : end - offset);
        const usize first = item.find(',');
        const usize second = first == std::string_view::npos
                                 ? std::string_view::npos
                                 : item.find(',', first + 1);
        if (first != 1 || second == std::string_view::npos) return false;
        const Handle handle = static_cast<Handle>(
            std::stoul(std::string(item.substr(first + 1, second - first - 1))));
        if (item[0] == 'I') {
            const auto transferred =
                hex_decode_sync_name(item.substr(second + 1));
            if (!transferred ||
                import_transferred_handle_impl(*transferred, handle) != handle) {
                return false;
            }
        } else if (item[0] == 'E') {
            const std::string name(item.substr(second + 1));
            auto event = std::make_shared<SyncEvent>();
            event->name = name;
            event->shared = ipc::SharedSyncObject::create_or_open(
                name, ipc::SyncKind::Event, 0, 0, /*create=*/false);
            if (!event->shared ||
                !m_handles.allocate_at(handle, HandleType::Event, event.get(),
                                       HandleFlags::Inherit)) {
                return false;
            }
            std::lock_guard<HostRwLock> lock(m_mutex);
            m_events[handle] = event;
            m_named_events[name] = event;
        } else if (item[0] == 'P') {
            const pid_t pid = static_cast<pid_t>(
                std::stol(std::string(item.substr(second + 1))));
            ipc::ProcessObject* process = ipc::track_process(pid);
            if (!process ||
                !m_handles.allocate_at(handle, HandleType::Process, process,
                                       HandleFlags::Inherit)) {
                return false;
            }
        } else {
            return false;
        }
        if (item[0] == 'I') {
            if (auto shared = resolve_shared(handle)) {
                std::fprintf(stderr,
                             "[sync] inherited event pid=%d h=0x%x "
                             "signaled=%d\n",
                             static_cast<int>(::getpid()),
                             static_cast<unsigned>(handle),
                             shared->event_is_signaled() ? 1 : 0);
                std::fflush(stderr);
            }
        }
        ipc::ipc_log("inherited %c handle 0x%x", item[0],
                     static_cast<unsigned>(handle));
        if (end == std::string_view::npos) break;
        offset = end + 1;
    }
    return true;
}

std::optional<std::string> SyncManager::prepare_transferred_handle(
    Handle source, u32 access, bool inheritable) {
    if (!ipc::ipc_enabled()) {
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        return std::nullopt;
    }
    const u32 source_access = m_handles.get_access(source);
    const HandleType type = m_handles.get_type(source);
    // NtDuplicateObject drops access bits the object type does not define.
    // DuplicateHandle of a mutex with SYNCHRONIZE|EVENT_MODIFY_STATE succeeds
    // on Windows even though EVENT_MODIFY_STATE is not a mutex right.
    if (access != HandleAccess::All) {
        u32 valid = 0;
        if (type == HandleType::Event) valid = sync_access::EVENT_ALL_ACCESS;
        else if (type == HandleType::Mutex) valid = sync_access::MUTEX_ALL_ACCESS;
        else if (type == HandleType::Semaphore) {
            valid = sync_access::SEMAPHORE_ALL_ACCESS;
        }
        if (valid != 0) access &= valid;
        if (access == 0) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            return std::nullopt;
        }
    }
    if (source_access != HandleAccess::All &&
        (access & ~source_access) != 0) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return std::nullopt;
    }
    std::unique_lock<HostRwLock> lock(m_mutex);
    ipc::SyncKind kind{};
    std::string visible_name;
    std::string transfer_name;
    std::shared_ptr<ipc::SharedSyncObject> shared;

    if (type == HandleType::Event) {
        const auto found = m_events.find(source);
        if (found == m_events.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return std::nullopt;
        }
        auto& event = *found->second;
        kind = ipc::SyncKind::Event;
        visible_name = event.name;
        transfer_name = event.transfer_name;
        shared = event.shared;
        if (!shared) {
            const auto state = ntsync::global().snapshot(event.nt_id);
            if (!state || state->type != ntsync::ObjType::Event) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                return std::nullopt;
            }
            transfer_name = make_sync_transfer_name('E');
            shared = ipc::SharedSyncObject::create_or_open(
                transfer_name, kind, state->manual_reset ? 1 : 0,
                state->signaled ? 1 : 0, /*create=*/true);
            if (!shared) {
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return std::nullopt;
            }
            // Publish shared backing before destroying the local ntsync id so a
            // parked RegisterWait/WFMO waiter that wakes on destroy can retry
            // against SharedSyncObject instead of seeing a dead HANDLE.
            event.shared = shared;
            event.transfer_name = transfer_name;
            const u64 old_nt = event.nt_id;
            event.nt_id = 0;
            ntsync::global().destroy(old_nt);
        }
    } else if (type == HandleType::Mutex) {
        const auto found = m_mutexes.find(source);
        if (found == m_mutexes.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return std::nullopt;
        }
        auto& mutex = *found->second;
        kind = ipc::SyncKind::Mutex;
        visible_name = mutex.name;
        transfer_name = mutex.transfer_name;
        shared = mutex.shared;
        if (!shared) {
            const auto state = ntsync::global().snapshot(mutex.nt_id);
            if (!state || state->type != ntsync::ObjType::Mutex) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                return std::nullopt;
            }
            // SharedSyncObject models recursive ownership after import, but
            // its constructor accepts only the first acquisition. Refuse an
            // already-recursed or abandoned mutex rather than losing state.
            if (state->recursion > 1 || state->abandoned) {
                set_last_error(error_codes::ERROR_NOT_SUPPORTED);
                return std::nullopt;
            }
            transfer_name = make_sync_transfer_name('M');
            shared = ipc::SharedSyncObject::create_or_open(
                transfer_name, kind, state->recursion != 0 ? 1 : 0,
                static_cast<i32>(state->owner_tid), /*create=*/true);
            if (!shared) {
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return std::nullopt;
            }
            mutex.shared = shared;
            mutex.transfer_name = transfer_name;
            const u64 old_nt = mutex.nt_id;
            mutex.nt_id = 0;
            ntsync::global().destroy(old_nt);
        }
    } else if (type == HandleType::Semaphore) {
        const auto found = m_semaphores.find(source);
        if (found == m_semaphores.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return std::nullopt;
        }
        auto& semaphore = *found->second;
        kind = ipc::SyncKind::Semaphore;
        visible_name = semaphore.name;
        transfer_name = semaphore.transfer_name;
        shared = semaphore.shared;
        if (!shared) {
            const auto state = ntsync::global().snapshot(semaphore.nt_id);
            if (!state || state->type != ntsync::ObjType::Semaphore) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                return std::nullopt;
            }
            transfer_name = make_sync_transfer_name('S');
            shared = ipc::SharedSyncObject::create_or_open(
                transfer_name, kind, state->sem_count, state->sem_max,
                /*create=*/true);
            if (!shared) {
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return std::nullopt;
            }
            semaphore.shared = shared;
            semaphore.transfer_name = transfer_name;
            const u64 old_nt = semaphore.nt_id;
            semaphore.nt_id = 0;
            ntsync::global().destroy(old_nt);
        }
    } else {
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        return std::nullopt;
    }

    const u32 flags = inheritable ? HandleFlags::Inherit : 0;
    return "S," + std::to_string(static_cast<u32>(kind)) + "," +
           std::to_string(access) + "," + std::to_string(flags) + "," +
           hex_encode_sync_name(transfer_name) + "," +
           hex_encode_sync_name(visible_name);
}

Handle SyncManager::import_transferred_handle(
    std::string_view descriptor) {
    return import_transferred_handle_impl(descriptor, std::nullopt);
}

Handle SyncManager::import_transferred_handle_impl(
    std::string_view descriptor,
    std::optional<Handle> inherited_handle) {
    std::array<std::string_view, 6> fields{};
    usize field_start = 0;
    for (usize index = 0; index < fields.size(); ++index) {
        const usize comma = descriptor.find(',', field_start);
        if (index + 1 == fields.size()) {
            if (comma != std::string_view::npos) return NULL_HANDLE;
            fields[index] = descriptor.substr(field_start);
        } else {
            if (comma == std::string_view::npos) return NULL_HANDLE;
            fields[index] =
                descriptor.substr(field_start, comma - field_start);
            field_start = comma + 1;
        }
    }
    if (fields[0] != "S") return NULL_HANDLE;
    const auto kind_value = parse_sync_u32(fields[1]);
    const auto access = parse_sync_u32(fields[2]);
    const auto flags = parse_sync_u32(fields[3]);
    const auto transfer_name = hex_decode_sync_name(fields[4]);
    const auto visible_name = hex_decode_sync_name(fields[5]);
    if (!kind_value || *kind_value > static_cast<u32>(ipc::SyncKind::Semaphore) ||
        !access || !flags || !transfer_name || transfer_name->empty() ||
        !visible_name) {
        return NULL_HANDLE;
    }
    const auto kind = static_cast<ipc::SyncKind>(*kind_value);
    auto shared = ipc::SharedSyncObject::create_or_open(
        *transfer_name, kind, 0, 0, /*create=*/false);
    if (!shared || shared->kind() != kind) return NULL_HANDLE;

    std::unique_lock<HostRwLock> lock(m_mutex);
    Handle handle = NULL_HANDLE;
    const auto allocate = [&](HandleType type, void* object) {
        if (!inherited_handle) {
            return m_handles.allocate(type, object, *flags, *access);
        }
        return m_handles.allocate_at(
                   *inherited_handle, type, object, *flags, *access)
            ? *inherited_handle
            : NULL_HANDLE;
    };
    if (kind == ipc::SyncKind::Event) {
        auto event = std::make_shared<SyncEvent>();
        event->shared = std::move(shared);
        event->name = *visible_name;
        event->transfer_name = *transfer_name;
        handle = allocate(HandleType::Event, event.get());
        if (handle == NULL_HANDLE) return NULL_HANDLE;
        m_events[handle] = event;
        if (!event->name.empty()) m_named_events[event->name] = event;
    } else if (kind == ipc::SyncKind::Mutex) {
        auto mutex = std::make_shared<SyncMutex>();
        mutex->shared = std::move(shared);
        mutex->name = *visible_name;
        mutex->transfer_name = *transfer_name;
        handle = allocate(HandleType::Mutex, mutex.get());
        if (handle == NULL_HANDLE) return NULL_HANDLE;
        m_mutexes[handle] = mutex;
        if (!visible_name->empty()) m_named_mutexes[*visible_name] = mutex;
    } else {
        auto semaphore = std::make_shared<SyncSemaphore>();
        semaphore->shared = std::move(shared);
        semaphore->name = *visible_name;
        semaphore->transfer_name = *transfer_name;
        handle = allocate(HandleType::Semaphore, semaphore.get());
        if (handle == NULL_HANDLE) return NULL_HANDLE;
        m_semaphores[handle] = semaphore;
        if (!visible_name->empty()) {
            m_named_semaphores[*visible_name] = semaphore;
        }
    }
    ipc::ipc_log(
        "imported runtime sync handle=0x%x kind=%u transfer='%s'",
        static_cast<unsigned>(handle), static_cast<unsigned>(kind),
        transfer_name->c_str());
    return handle;
}

bool SyncManager::close_handle(Handle h) {
    const HandleType type = m_handles.get_type(h);
    if (type != HandleType::Event && type != HandleType::Mutex &&
        type != HandleType::Semaphore) {
        return false;
    }
    (void)on_handle_close(h, type);
    return m_handles.close(h);
}

bool SyncManager::on_handle_close(Handle h, HandleType type) noexcept {
    if (type != HandleType::Event && type != HandleType::Mutex &&
        type != HandleType::Semaphore) {
        return false;
    }

    // Tear down ntsync object too, if any.
    u64 nt_id = 0;
    {
        std::lock_guard<HostRwLock> lock(m_mutex);
        if (type == HandleType::Event) {
            auto it = m_events.find(h);
            if (it == m_events.end()) return true;
            auto event = it->second;
            m_events.erase(it);
            // The local ntsync object belongs to the shared event wrapper,
            // not to any individual HANDLE.
            if (event.use_count() == 1) {
                nt_id = event->nt_id;
                if (!event->name.empty()) {
                    named_object_unregister(event->name,
                                            NamedObjectKind::Event);
                    m_named_events.erase(event->name);
                }
            }
        } else if (type == HandleType::Mutex) {
            auto it = m_mutexes.find(h);
            if (it == m_mutexes.end()) return true;
            auto mutex = it->second;
            m_mutexes.erase(it);
            if (mutex.use_count() == 1) {
                nt_id = mutex->nt_id;
                if (!mutex->name.empty()) {
                    named_object_unregister(mutex->name,
                                            NamedObjectKind::Mutex);
                    m_named_mutexes.erase(mutex->name);
                }
            }
        } else {
            auto it = m_semaphores.find(h);
            if (it == m_semaphores.end()) return true;
            auto semaphore = it->second;
            m_semaphores.erase(it);
            if (semaphore.use_count() == 1) {
                nt_id = semaphore->nt_id;
                if (!semaphore->name.empty()) {
                    named_object_unregister(semaphore->name,
                                            NamedObjectKind::Semaphore);
                    m_named_semaphores.erase(semaphore->name);
                }
            }
        }
    }
    if (nt_id) ntsync::global().destroy(nt_id);
    return true;
}

Handle SyncManager::duplicate_handle(Handle source, u32 flags, u32 access) {
    const HandleType type = m_handles.get_type(source);
    if (type != HandleType::Event && type != HandleType::Mutex &&
        type != HandleType::Semaphore) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return NULL_HANDLE;
    }

    const u32 source_access = m_handles.get_access(source);
    if (source_access != HandleAccess::All &&
        access != HandleAccess::All &&
        (access & ~source_access) != 0) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return NULL_HANDLE;
    }

    std::lock_guard<HostRwLock> lock(m_mutex);
    Handle duplicate = NULL_HANDLE;
    if (type == HandleType::Event) {
        const auto found = m_events.find(source);
        if (found == m_events.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return NULL_HANDLE;
        }
        duplicate = m_handles.allocate(
            HandleType::Event, found->second.get(), flags, access);
        if (duplicate == NULL_HANDLE) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return NULL_HANDLE;
        }
        m_events[duplicate] = found->second;
    } else if (type == HandleType::Mutex) {
        const auto found = m_mutexes.find(source);
        if (found == m_mutexes.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return NULL_HANDLE;
        }
        duplicate = m_handles.allocate(
            HandleType::Mutex, found->second.get(), flags, access);
        if (duplicate == NULL_HANDLE) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return NULL_HANDLE;
        }
        m_mutexes[duplicate] = found->second;
    } else {
        const auto found = m_semaphores.find(source);
        if (found == m_semaphores.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return NULL_HANDLE;
        }
        duplicate = m_handles.allocate(
            HandleType::Semaphore, found->second.get(), flags, access);
        if (duplicate == NULL_HANDLE) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return NULL_HANDLE;
        }
        m_semaphores[duplicate] = found->second;
    }

    ipc::ipc_log(
        "DuplicateHandle same-process source=0x%x type=%s -> 0x%x access=0x%x",
        static_cast<unsigned>(source), handle_type_to_string(type),
        static_cast<unsigned>(duplicate), access);
    return duplicate;
}

bool SyncManager::set_event(Handle h, SyncTraceContext trace) {
    return set_event_state(h, trace, /*timer=*/false, /*check_access=*/true);
}

bool SyncManager::set_event(Handle h, SyncTraceContext trace, bool* previous_state) {
    return set_event_state(h, trace, /*timer=*/false, /*check_access=*/true, previous_state);
}

bool SyncManager::reset_event(Handle h, SyncTraceContext trace, bool* previous_state) {
    return reset_event_state(h, trace, /*timer=*/false, previous_state);
}

bool SyncManager::query_event(Handle h, bool& manual_reset, bool& signaled) const {
    std::shared_ptr<SyncEvent> event;
    {
        std::shared_lock<HostRwLock> lock(m_mutex);
        auto it = m_events.find(h);
        if (it == m_events.end() || it->second->waitable_timer) return false;
        event = it->second;
    }
    if (event->shared) {
        manual_reset = event->shared->event_manual_reset();
        signaled = event->shared->event_is_signaled();
        return true;
    }
    const auto object = ntsync::global().snapshot(event->nt_id);
    if (!object) return false;
    manual_reset = object->manual_reset;
    signaled = object->signaled;
    return true;
}

std::string SyncManager::object_name(Handle h) const {
    std::shared_lock<HostRwLock> lock(m_mutex);
    std::string name;
    if (auto it = m_events.find(h); it != m_events.end()) {
        name = it->second->name;
    } else if (auto mi = m_mutexes.find(h); mi != m_mutexes.end()) {
        name = mi->second->name;
    } else if (auto si = m_semaphores.find(h); si != m_semaphores.end()) {
        name = si->second->name;
    }
    // An unnamed inheritable / duplicated object carries a private
    // rendezvous name (make_sync_transfer_name); Windows reports none.
    if (name.rfind("VitreoTransferredSync_", 0) == 0) return {};
    return name;
}

bool SyncManager::signal_waitable_timer(Handle h, SyncTraceContext trace) {
    // Expiry is performed by the kernel, not through the caller's handle
    // rights, so a handle duplicated without TIMER_MODIFY_STATE still fires.
    return set_event_state(h, trace, /*timer=*/true, /*check_access=*/false);
}

bool SyncManager::set_event_state(Handle h, SyncTraceContext trace, bool timer,
                                  bool check_access, bool* previous_state) {
    u64 nt_id = 0;
    std::shared_ptr<ipc::SharedSyncObject> shared;
    std::shared_ptr<SyncEvent> event;
    std::string name;
    {
        std::shared_lock<HostRwLock> lock(m_mutex);
        auto it = m_events.find(h);
        if (it == m_events.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return false;
        }
        if (it->second->waitable_timer != timer) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return false;
        }
        if (check_access &&
            !(m_handles.get_access(h) & sync_access::EVENT_MODIFY_STATE)) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            return false;
        }
        event = it->second;
        shared = event->shared;
        nt_id = event->nt_id;
        name = event->name;
    }
    if (shared) {
        if (causal_trace_enabled() || !name.empty()) {
            if (causal_trace_enabled()) {
                ipc::ipc_log(
                    "SetEvent(shared) h=0x%x name='%s' caller=0x%llx parents=%s",
                    static_cast<unsigned>(h), name.c_str(),
                    static_cast<unsigned long long>(trace.caller),
                    format_parent_chain(trace.parents, trace.parent_count)
                        .c_str());
            } else {
                ipc::ipc_log("SetEvent(shared) h=0x%x name='%s'",
                             static_cast<unsigned>(h), name.c_str());
            }
        }
        const bool previous = shared->set_event();
        if (previous_state) *previous_state = previous;
        // Shared waiters poll SharedSyncObject state; also nudge any local
        // ntsync waiters that may still be parked across a promote window.
        ntsync::global().notify_waiters();
        record_event_transition(event, "set", trace);
        return true;
    }
    const bool result = ntsync::global().set_event(nt_id, previous_state);
    if (result) {
        if (causal_trace_enabled()) {
            ipc::ipc_log(
                "SetEvent h=0x%x name='%s' tid=%u caller=0x%llx parents=%s",
                static_cast<unsigned>(h), name.c_str(),
                ThreadManager::get_current_thread_id_static(),
                static_cast<unsigned long long>(trace.caller),
                format_parent_chain(trace.parents, trace.parent_count).c_str());
        }
        record_event_transition(event, "set", trace);
    }
    return result;
}

bool SyncManager::pulse_event(Handle h, SyncTraceContext trace) {
    return pulse_event(h, trace, nullptr);
}

bool SyncManager::pulse_event(Handle h, SyncTraceContext trace, bool* previous_state) {
    // learn.microsoft.com PulseEvent: releases the threads already waiting and
    // leaves the event non-signalled. Composing SetEvent+ResetEvent cannot do
    // that -- the reset lands before any waiter re-tests its condition, so the
    // wake is lost and the waiter blocks forever.
    u64 nt_id = 0;
    std::shared_ptr<SyncEvent> event;
    {
        std::shared_lock<HostRwLock> lock(m_mutex);
        auto it = m_events.find(h);
        if (it == m_events.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return false;
        }
        if (it->second->waitable_timer) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return false;
        }
        if (!(m_handles.get_access(h) & sync_access::EVENT_MODIFY_STATE)) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            return false;
        }
        event = it->second;
        nt_id = event->nt_id;
    }
    const bool result = ntsync::global().pulse_event(nt_id, previous_state);
    if (result) record_event_transition(event, "pulse", trace);
    return result;
}

bool SyncManager::reset_event(Handle h, SyncTraceContext trace) {
    return reset_event_state(h, trace, /*timer=*/false);
}

bool SyncManager::is_waitable_timer(Handle h) const {
    std::shared_lock<HostRwLock> lock(m_mutex);
    const auto it = m_events.find(h);
    return it != m_events.end() && it->second->waitable_timer;
}

bool SyncManager::reset_waitable_timer(Handle h, SyncTraceContext trace) {
    // TIMER_MODIFY_STATE and EVENT_MODIFY_STATE are both 0x0002 (winnt.h).
    return reset_event_state(h, trace, /*timer=*/true);
}

bool SyncManager::reset_event_state(Handle h, SyncTraceContext trace,
                                    bool timer, bool* previous_state) {
    u64 nt_id = 0;
    std::shared_ptr<ipc::SharedSyncObject> shared;
    std::shared_ptr<SyncEvent> event;
    {
        std::shared_lock<HostRwLock> lock(m_mutex);
        auto it = m_events.find(h);
        if (it == m_events.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return false;
        }
        if (it->second->waitable_timer != timer) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return false;
        }
        if (!(m_handles.get_access(h) & sync_access::EVENT_MODIFY_STATE)) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            return false;
        }
        event = it->second;
        shared = event->shared;
        nt_id = event->nt_id;
    }
    if (shared) {
        const bool previous = shared->reset_event();
        if (previous_state) *previous_state = previous;
        record_event_transition(event, "reset", trace);
        return true;
    }
    const bool result = ntsync::global().reset_event(nt_id, previous_state);
    if (result) record_event_transition(event, "reset", trace);
    return result;
}

} // namespace vitreo::winapi
