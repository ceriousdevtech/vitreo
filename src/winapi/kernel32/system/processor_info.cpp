// SPDX-License-Identifier: LGPL-2.1-only

/// @file processor_info.cpp
/// @brief Wave 3 Group D system/processor info stubs.
///
/// GetFirmwareType, GetMaximumProcessorCount, GetLogicalProcessorInformation, and related.
///
/// Closed cut from kernel32_ext.cpp — verified closed with
/// tools/architecture/plan_extraction.py before moving.

#include "platform/host_hardware.hpp"
#include "winapi/kernel32/system/processor_topology.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
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
// Wave 3: Process/Job/Console/File/SysInfo/Pipes/Comm/Fiber/PSAPI K32*
// =============================================================================


// ---- Group D: System Info ----
void Kernel32ExtManager::impl_GetFirmwareType(Marshaler& m) {
    GuestAddr p = m.arg_ptr(0);
    if (p) (void)m.write_u32(p, 2); // FirmwareTypeUefi
    m.set_return(1);
}
void Kernel32ExtManager::impl_GetMaximumProcessorCount(Marshaler& m) {
    // One group holds every processor; any other group number has none.
    const u32 group = static_cast<u32>(m.arg(0)) & 0xFFFFu;
    constexpr u32 ALL_PROCESSOR_GROUPS = 0xFFFFu;
    m.set_return(group == 0 || group == ALL_PROCESSOR_GROUPS ? processor_topology::processor_count() : 0);
}
void Kernel32ExtManager::impl_GetMaximumProcessorGroupCount(Marshaler& m) {
    m.set_return(1);
}
void Kernel32ExtManager::impl_GetActiveProcessorCount(Marshaler& m) {
    const u32 group = static_cast<u32>(m.arg(0));
    constexpr u32 ALL_PROCESSOR_GROUPS = 0xFFFFu;
    if (group != 0 && group != ALL_PROCESSOR_GROUPS) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    m.set_return(processor_topology::processor_count());
}
void Kernel32ExtManager::impl_GetActiveProcessorGroupCount(Marshaler& m) {
    m.set_return(1);
}
void Kernel32ExtManager::impl_GetPhysicallyInstalledSystemMemory(
    Marshaler& m) {
    const GuestAddr kilobytes_out = m.arg_ptr(0);
    if (kilobytes_out == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const u64 bytes = platform::host_hardware().memory.total_bytes;
    if (bytes == 0 || m.memory().write_u64(kilobytes_out, bytes / 1024).is_err()) {
        set_last_error(error_codes::ERROR_GEN_FAILURE);
        m.set_return(0);
        return;
    }
    set_last_error(error_codes::ERROR_SUCCESS);
    VITREO_TRACE(
        "[k32ext] GetPhysicallyInstalledSystemMemory -> %llu KB\n",
        static_cast<unsigned long long>(bytes / 1024));
    m.set_return(1);
}
void Kernel32ExtManager::impl_GetLogicalProcessorInformation(Marshaler& m) {
    // BOOL GetLogicalProcessorInformation(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION Buffer,
    //                                     PDWORD ReturnedLength)
    // Measured (processor_topology_probe "glpi"): a NULL or short buffer fails
    // with ERROR_INSUFFICIENT_BUFFER and the required length; success writes
    // the length and leaves the last error alone.
    const GuestAddr buffer = m.arg_ptr(0);
    const GuestAddr length_at = m.arg_ptr(1);
    if (length_at == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const auto capacity = m.read_u32(length_at);
    const std::vector<u8> records = processor_topology::logical_processor_information(m.cpu_is_64bit());
    const u32 required = static_cast<u32>(records.size());
    if (buffer == 0 || capacity.is_err() || capacity.value() < required) {
        (void)m.write_u32(length_at, required);
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0);
        return;
    }
    if (m_memory.write_bytes(buffer, reinterpret_cast<const Byte*>(records.data()), records.size()).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    (void)m.write_u32(length_at, required);
    m.set_return(1);
}
void Kernel32ExtManager::impl_CreateMemoryResourceNotification(Marshaler& m) {
    // HANDLE CreateMemoryResourceNotification(MEMORY_RESOURCE_NOTIFICATION_TYPE)
    //   0 = LowMemoryResourceNotification, 1 = HighMemoryResourceNotification.
    // Learn: the handle is waitable. High is signaled when memory is plentiful;
    // low when memory is scarce. Vitreo models a healthy host.
    const u32 type = static_cast<u32>(m.arg(0));
    if (type > 1u) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (!m_sync_manager) {
        set_last_error(error_codes::ERROR_INVALID_FUNCTION);
        m.set_return(0);
        return;
    }
    const u32 entry_error = get_last_error();
    const bool initial_signaled = (type == 1u); // HighMemory → signaled when healthy
    const Handle h = m_sync_manager->create_event(/*manual_reset=*/true,
                                                  initial_signaled, "");
    set_last_error(entry_error);
    if (h == NULL_HANDLE || h == INVALID_HANDLE) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        m.set_return(0);
        return;
    }
    {
        std::lock_guard lock(m_mrn_mutex);
        m_mrn_types[h] = type;
    }
    m.set_return(h);
}

void Kernel32ExtManager::impl_QueryMemoryResourceNotification(Marshaler& m) {
    // BOOL QueryMemoryResourceNotification(HANDLE, PBOOL ResourceState)
    const Handle handle = static_cast<Handle>(m.arg(0));
    const GuestAddr state_ptr = m.arg_ptr(1);
    u32 type = 0xFFFFFFFFu;
    {
        std::lock_guard lock(m_mrn_mutex);
        auto it = m_mrn_types.find(handle);
        if (it != m_mrn_types.end()) type = it->second;
    }
    if (type > 1u || state_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    // Healthy host: low condition false, high condition true.
    const bool is_high = (type == 1u);
    (void)m.write_u32(state_ptr, is_high ? 1u : 0u);
    m.set_return(1);
}
void Kernel32ExtManager::impl_DiscardVirtualMemory(Marshaler& m) {
    const u64 address = m.arg_ptr(0);
    const u64 size = m.arg(1);
    bool success = m_virtual_memory_manager != nullptr &&
                   m_virtual_memory_manager->discard_virtual_memory(
                       address, size);
    if (!success && m_file_mapping_manager != nullptr) {
        success = m_file_mapping_manager->discard_virtual_memory(
                      static_cast<GuestAddr>(address), size) ==
                  ViewDiscardResult::Success;
    }
    if (!success) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    m.set_return(error_codes::ERROR_SUCCESS);
}
void Kernel32ExtManager::impl_PrefetchVirtualMemory(Marshaler& m) {
    (void)m.arg(0); (void)m.arg(1); (void)m.arg_ptr(2); (void)m.arg(3);
    m.set_return(1);
}
void Kernel32ExtManager::impl_SetDefaultDllDirectories(Marshaler& m) {
    // BOOL SetDefaultDllDirectories(DWORD DirectoryFlags)
    // https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-setdefaultdlldirectories
    const u32 flags = static_cast<u32>(m.arg(0));
    if (flags == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (m_module_manager) {
        m_module_manager->set_default_dll_directories(flags);
    }
    m.set_return(1);
}

void Kernel32ExtManager::impl_AddDllDirectory(Marshaler& m) {
    // DLL_DIRECTORY_COOKIE AddDllDirectory(PCWSTR NewDirectory)
    // https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-adddlldirectory
    const GuestAddr path_ptr = m.arg_ptr(0);
    if (!m_module_manager || path_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const std::string path = read_wide_ascii_z(path_ptr, 520);
    if (path.empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const u64 cookie = m_module_manager->add_dll_directory(path);
    if (cookie == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    ipc::ipc_log("AddDllDirectory '%s' -> cookie=0x%llx",
                 path.c_str(), static_cast<unsigned long long>(cookie));
    m.set_return(cookie);
}

void Kernel32ExtManager::impl_RemoveDllDirectory(Marshaler& m) {
    // BOOL RemoveDllDirectory(DLL_DIRECTORY_COOKIE Cookie)
    const u64 cookie = m.arg(0);
    if (!m_module_manager || cookie == 0 ||
        !m_module_manager->remove_dll_directory(cookie)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

} // namespace vitreo::winapi
