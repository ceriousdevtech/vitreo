// SPDX-License-Identifier: LGPL-2.1-only

/// @file toolhelp.cpp
/// @brief Toolhelp32 snapshot stubs.
///
/// CPU-Z Toolhelp32 Snapshot banner.
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
// CPU-Z: Toolhelp32 Snapshot
// =============================================================================

void Kernel32ExtManager::impl_CreateToolhelp32Snapshot(Marshaler& m) {
    const u32 flags = static_cast<u32>(m.arg(0));
    const u32 filter_pid = static_cast<u32>(m.arg(1)); // th32ProcessID

    u32 handle = ++m_toolhelp_handle;
    ToolhelpSnapshot snapshot;
    if ((flags & 0x2u) != 0 && m_process_snapshot_provider) {
        snapshot.processes = m_process_snapshot_provider();
    }
    if (snapshot.processes.empty()) {
        u32 pid = m_process_manager
            ? m_process_manager->get_current_process_id()
            : 1000u;
        std::string exe = "vitreo.exe";
        std::string image_path = exe;
        if (m_module_manager) {
            if (const auto* info = m_module_manager->get_module_info_by_handle(
                    m_module_manager->main_module_base())) {
                if (!info->name.empty()) {
                    exe = info->name;
                    image_path = info->name;
                }
            }
        }
        auto separator = exe.find_last_of("\\/");
        if (separator != std::string::npos) exe = exe.substr(separator + 1);
        snapshot.processes.push_back(
            {pid, 0, std::move(exe), std::move(image_path)});
    }
    // TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32. Tool Help snapshots are
    // immutable views: copy the module metadata now instead of consulting the
    // live loader table during Module32First/Next.
    const u32 self_pid = m_process_manager
        ? m_process_manager->get_current_process_id()
        : static_cast<u32>(::getpid());
    if ((flags & 0x18u) != 0 &&
        (filter_pid == 0 || filter_pid == self_pid) &&
        m_module_manager != nullptr) {
        for (GuestAddr module_handle :
             m_module_manager->get_all_module_handles()) {
            const ModuleInfo* info =
                m_module_manager->get_module_info_by_handle(module_handle);
            if (info == nullptr || info->state != ModuleInfo::State::Loaded) {
                continue;
            }
            u64 image_size = 0;
            if (info->image_end > info->base_address) {
                image_size = info->image_end - info->base_address;
            } else if (info->synthetic_image) {
                image_size = SYNTHETIC_MODULE_SLOT_SIZE;
            }
            snapshot.modules.push_back({
                info->base_address,
                static_cast<u32>(std::min<u64>(
                    image_size, std::numeric_limits<u32>::max())),
                info->name,
                info->path,
            });
        }
    }
    // TH32CS_SNAPTHREAD — enumerate guest threads in this process.
    // Cross-process thread lists are not modeled; filter_pid must be 0 or
    // the current PID for a non-empty result.
    if ((flags & 0x4u) != 0 && m_thread_manager != nullptr) {
        if (filter_pid == 0 || filter_pid == self_pid) {
            snapshot.threads = m_thread_manager->list_thread_ids();
        }
    }
    const usize count = snapshot.processes.size();
    const usize module_count = snapshot.modules.size();
    const usize thread_count = snapshot.threads.size();
    {
        std::lock_guard<std::mutex> snapshots(m_toolhelp_mutex);
        m_toolhelp_snapshots.insert_or_assign(handle, std::move(snapshot));
    }
    synthetic_handles::note_issued(static_cast<Handle>(handle));
    VITREO_TRACE(
        "[k32ext] CreateToolhelp32Snapshot(flags=0x%X pid=%u) -> 0x%X "
        "(%zu processes, %zu modules, %zu threads)\n",
        flags, filter_pid, handle, count, module_count, thread_count);
    m.set_return(handle);
}

void Kernel32ExtManager::impl_Process32First(Marshaler& m) {
    (void)write_next_process_entry(m, static_cast<u32>(m.arg(0)),
                                   m.arg_ptr(1), false, true);
}

void Kernel32ExtManager::impl_Process32Next(Marshaler& m) {
    (void)write_next_process_entry(m, static_cast<u32>(m.arg(0)),
                                   m.arg_ptr(1), false, false);
}

void Kernel32ExtManager::impl_Thread32First(Marshaler& m) {
    (void)write_next_thread_entry(m, static_cast<u32>(m.arg(0)),
                                  m.arg_ptr(1), true);
}

void Kernel32ExtManager::impl_Thread32Next(Marshaler& m) {
    (void)write_next_thread_entry(m, static_cast<u32>(m.arg(0)),
                                  m.arg_ptr(1), false);
}

} // namespace vitreo::winapi
