// SPDX-License-Identifier: LGPL-2.1-only

/// @file semaphore_ext.cpp
/// @brief ReleaseSemaphore, CreateSemaphoreW, and QueueUserAPC.
///
/// CPU-Z Sync / Mutex / Semaphore banner.
///
/// Closed cut from kernel32_ext.cpp — verified closed with
/// tools/architecture/plan_extraction.py before moving.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/synthetic_handle_registry.hpp"
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/synchronization/critical_section.hpp"
#include "winapi/common/object_access_constants.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "ipc/process_session.hpp"
#include "core/pe/loader/teb_setup.hpp"
#include "core/pe/parser/manifest_info.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "platform/macos/elevation_authorization.hpp"
#include "windowing/resources/resource_walker.hpp"

#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/win_path.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <limits>
#include <random>
#include <sys/sysctl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <libproc.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace vitreo::winapi {

// =============================================================================
// CPU-Z: Sync / Mutex / Semaphore
// =============================================================================

void Kernel32ExtManager::impl_ReleaseSemaphore(Marshaler& m) {
    Handle h = static_cast<Handle>(m.arg(0));
    i32 release_count = static_cast<i32>(m.arg(1));
    GuestAddr prev_ptr = m.arg_ptr(2);
    if (m_sync_manager) {
        i32 previous = 0;
        bool ok = m_sync_manager->release_semaphore(h, release_count, &previous);
        if (ok && prev_ptr) (void)m.write_u32(prev_ptr, static_cast<u32>(previous));
        VITREO_TRACE("[k32ext] ReleaseSemaphore(0x%x, %d) -> %s prev=%d\n",
                     h, release_count, ok ? "TRUE" : "FALSE", previous);
        m.set_return(ok ? 1u : 0u);
        return;
    }
    if (prev_ptr) (void)m.write_u32(prev_ptr, 0);
    m.set_return(1);
}

void Kernel32ExtManager::impl_CreateSemaphoreW(Marshaler& m) {
    const GuestAddr attributes = m.arg_ptr(0);
    i32 initial = static_cast<i32>(m.arg(1));
    i32 maximum = static_cast<i32>(m.arg(2));
    GuestAddr name_ptr = m.arg_ptr(3);
    std::string name;
    if (name_ptr) {
        auto r = m.read_wide_string(name_ptr);
        if (r.is_ok()) name = r.value();
    }
    if (maximum <= 0 || initial < 0 || initial > maximum) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    bool inherit = false;
    if (attributes) {
        const auto value =
            m.read_u32(attributes + (m.cpu_is_64bit() ? 16 : 8));
        if (value.is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
        inherit = value.value() != 0;
    }
    if (m_sync_manager) {
        // The descriptor is validated like CreateEvent's (SACL labels and
        // audit ACEs; tools/oracle_probe/sync_sacl_label_probe.c). The
        // semaphore object itself does not keep it yet.
        if (attributes) {
            object_access::Descriptor security;
            if (!m_sync_manager->capture_object_security(
                    security, sync_security_attributes_factory(m, attributes))) {
                m.set_return(0);
                return;
            }
        }
        Handle h = m_sync_manager->create_semaphore(
            initial, maximum, name, inherit, sync_access::SEMAPHORE_ALL_ACCESS);
        VITREO_TRACE("[k32ext] CreateSemaphoreW(init=%d,max=%d,name='%s') -> 0x%x\n",
                     initial, maximum, name.c_str(), h);
        m.set_return(static_cast<u64>(h));
        return;
    }
    u32 handle = 0xCE000000 + (++m_semaphore_counter);
    synthetic_handles::note_issued(static_cast<Handle>(handle));
    m.set_return(handle);
}

void Kernel32ExtManager::impl_QueueUserAPC(Marshaler& m) {
    // DWORD QueueUserAPC(PAPCFUNC pfnAPC, HANDLE hThread, ULONG_PTR dwData)
    // Learn: nonzero on success; APC runs only in an alertable wait on the
    // target thread (FIFO). Do not return success without enqueueing.
    const GuestAddr pfn = m.arg_ptr(0);
    const Handle thread = static_cast<Handle>(m.arg(1));
    const u64 data = m.arg(2);

    // A NULL pfnAPC is NOT rejected: Windows queues it and the target faults
    // when it is delivered (thread_lifecycle_probe QUEUEAPC_NULL_FN ret=1).
    if (!m_thread_manager) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    // Learn QueueUserAPC: the handle needs THREAD_SET_CONTEXT.
    if (const u32 error = m_thread_manager->thread_handle_error(
            thread, ThreadManager::THREAD_SET_CONTEXT);
        error != 0) {
        set_last_error(error);
        m.set_return(0);
        return;
    }

    // Detect terminate-in-progress for ERROR_GEN_FAILURE (Learn).
    if (thread != ThreadManager::get_current_thread()) {
        GuestThread* gt = m_thread_manager->get_thread_by_handle(thread);
        if (gt == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        if (gt->terminated) {
            set_last_error(error_codes::ERROR_GEN_FAILURE);
            m.set_return(0);
            return;
        }
    } else {
        GuestThread* gt =
            m_thread_manager->get_thread(m_thread_manager->get_current_thread_id());
        if (gt != nullptr && gt->terminated) {
            set_last_error(error_codes::ERROR_GEN_FAILURE);
            m.set_return(0);
            return;
        }
    }

    if (!m_thread_manager->queue_user_apc(thread, pfn, data)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    ipc::ipc_log("[k32ext] QueueUserAPC pfn=0x%llx thread=0x%x data=0x%llx\n",
                 static_cast<unsigned long long>(pfn), thread,
                 static_cast<unsigned long long>(data));
    m.set_return(1);
}

} // namespace vitreo::winapi
