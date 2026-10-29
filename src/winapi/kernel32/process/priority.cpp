// SPDX-License-Identifier: LGPL-2.1-only

/// @file priority.cpp
/// @brief Set/GetPriorityClass and GetProcessTimes.
///
/// CPU-Z Priority / Process banner.
///
/// Closed cut from kernel32_ext.cpp — verified closed with
/// tools/architecture/plan_extraction.py before moving.

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
// CPU-Z: Priority / Process
// =============================================================================

namespace {

// Learn processthreadsapi SetPriorityClass / Scheduling Priorities.
constexpr u32 kIdlePriorityClass        = 0x00000040u;
constexpr u32 kBelowNormalPriorityClass = 0x00004000u;
constexpr u32 kNormalPriorityClass      = 0x00000020u;
constexpr u32 kAboveNormalPriorityClass = 0x00008000u;
constexpr u32 kHighPriorityClass        = 0x00000080u;
constexpr u32 kRealtimePriorityClass    = 0x00000100u;
constexpr u32 kProcessModeBackgroundBegin = 0x00100000u;
constexpr u32 kProcessModeBackgroundEnd   = 0x00200000u;
constexpr u32 kProcessSetInformation      = 0x00000200u;
constexpr u32 kProcessQueryInformation    = 0x00000400u;
constexpr u32 kProcessQueryLimitedInformation = 0x00001000u;

// Lowest-first among known class bits: multi-bit OR values resolve to the
// lowest class bit set (process_contract_probe.expected combo lines).
constexpr u32 kPriorityClassOrder[] = {
    kIdlePriorityClass,
    kBelowNormalPriorityClass,
    kNormalPriorityClass,
    kAboveNormalPriorityClass,
    kHighPriorityClass,
    kRealtimePriorityClass,
};

[[nodiscard]] u32 resolve_priority_class_bits(u32 dw) noexcept {
    for (u32 cls : kPriorityClassOrder) {
        if ((dw & cls) != 0) {
            return cls;
        }
    }
    return 0;
}

} // namespace

void Kernel32ExtManager::impl_SetPriorityClass(Marshaler& m) {
    // BOOL SetPriorityClass(HANDLE hProcess, DWORD dwPriorityClass)
    // https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setpriorityclass
    const Handle process_handle = static_cast<Handle>(m.arg(0));
    const u32 priority_class = static_cast<u32>(m.arg(1));

    bool is_self = (process_handle == CURRENT_PROCESS_PSEUDO);
    ipc::ProcessObject* process = nullptr;
    if (!is_self) {
        if (m_handle_table == nullptr ||
            m_handle_table->get_type(process_handle) != HandleType::Process) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const u32 access = m_handle_table->get_access(process_handle);
        if (access != HandleAccess::All &&
            (access & kProcessSetInformation) == 0) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            m.set_return(0);
            return;
        }
        process = static_cast<ipc::ProcessObject*>(
            m_handle_table->get(process_handle, HandleType::Process));
        if (process == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        if (m_process_manager != nullptr &&
            process->pid == static_cast<pid_t>(
                m_process_manager->get_current_process_id())) {
            is_self = true;
        }
    }

    // Known class bit(s) present (alone or OR'd with junk / BACKGROUND_*):
    // store the lowest class bit. BACKGROUND_* mixed with a class is ignored
    // (combo 0x200020 → NORMAL).
    const u32 resolved = resolve_priority_class_bits(priority_class);
    if (resolved != 0) {
        if (is_self) {
            if (m_process_manager == nullptr) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            m_process_manager->set_priority_class(resolved);
            if (m_thread_manager != nullptr) {
                m_thread_manager->set_process_scheduling(
                    resolved, m_process_manager->background_mode());
            }
        } else {
            // Remote child: record on an Ext-owned map keyed by ProcessObject.
            std::lock_guard<std::mutex> classes(m_priority_class_mutex);
            m_remote_priority_class[process] = resolved;
        }
        m.set_return(1);
        return;
    }

    if (priority_class == kProcessModeBackgroundBegin ||
        priority_class == kProcessModeBackgroundEnd) {
        // Learn: BACKGROUND_* only for a handle to the current process.
        if (!is_self || m_process_manager == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        if (priority_class == kProcessModeBackgroundBegin) {
            if (m_process_manager->background_mode()) {
                set_last_error(
                    error_codes::ERROR_PROCESS_MODE_ALREADY_BACKGROUND);
                m.set_return(0);
                return;
            }
            m_process_manager->set_background_mode(true);
            // Oracle: GetPriorityClass while in background reports NORMAL
            // (process_contract_probe.expected "GetPriorityClass in background").
            m_process_manager->set_priority_class(kNormalPriorityClass);
            if (m_thread_manager != nullptr) {
                m_thread_manager->set_process_scheduling(kNormalPriorityClass,
                                                         true);
            }
            m.set_return(1);
            return;
        }
        // BACKGROUND_END
        if (!m_process_manager->background_mode()) {
            set_last_error(error_codes::ERROR_PROCESS_MODE_NOT_BACKGROUND);
            m.set_return(0);
            return;
        }
        m_process_manager->set_background_mode(false);
        if (m_thread_manager != nullptr) {
            m_thread_manager->set_process_scheduling(
                m_process_manager->priority_class(), false);
        }
        m.set_return(1);
        return;
    }

    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    m.set_return(0);
}

void Kernel32ExtManager::impl_GetPriorityClass(Marshaler& m) {
    // DWORD GetPriorityClass(HANDLE hProcess)
    // https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getpriorityclass
    const Handle process_handle = static_cast<Handle>(m.arg(0));

    if (process_handle == CURRENT_PROCESS_PSEUDO) {
        const u32 cls = m_process_manager != nullptr
                            ? m_process_manager->priority_class()
                            : kNormalPriorityClass;
        m.set_return(cls);
        return;
    }

    if (m_handle_table == nullptr ||
        m_handle_table->get_type(process_handle) != HandleType::Process) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    const u32 access = m_handle_table->get_access(process_handle);
    if (access != HandleAccess::All &&
        (access & (kProcessQueryInformation |
                   kProcessQueryLimitedInformation)) == 0) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        m.set_return(0);
        return;
    }
    auto* process = static_cast<ipc::ProcessObject*>(
        m_handle_table->get(process_handle, HandleType::Process));
    if (process == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (m_process_manager != nullptr &&
        process->pid == static_cast<pid_t>(
            m_process_manager->get_current_process_id())) {
        m.set_return(m_process_manager->priority_class());
        return;
    }
    std::lock_guard<std::mutex> classes(m_priority_class_mutex);
    const auto it = m_remote_priority_class.find(process);
    m.set_return(it != m_remote_priority_class.end() ? it->second
                                                     : kNormalPriorityClass);
}

// BOOL GetSystemTimes(LPFILETIME lpIdleTime, LPFILETIME lpKernelTime,
//                     LPFILETIME lpUserTime)
// https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getsystemtimes
//
// System-wide CPU time summed over every processor, in FILETIME 100ns units.
// Two documented quirks that a naive implementation gets wrong:
//   - lpKernelTime INCLUDES idle time, so a caller computing busy time does
//     (kernel - idle) + user. Reporting kernel without idle folded in makes
//     the machine look permanently busier than it is.
//   - Any of the three pointers may be NULL; that is not an error.
//
// coreclr's thread pool queries this through GetProcAddress on every hill
// climbing tick, so a missing export is 384 failed lookups in one pwsh start.
void Kernel32ExtManager::impl_GetSystemTimes(Marshaler& m) {
    const GuestAddr idle_ptr   = m.arg_ptr(0);
    const GuestAddr kernel_ptr = m.arg_ptr(1);
    const GuestAddr user_ptr   = m.arg_ptr(2);

    host_cpu_load_info_data_t load{};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        reinterpret_cast<host_info_t>(&load),
                        &count) != KERN_SUCCESS) {
        set_last_error(error_codes::ERROR_INVALID_FUNCTION);
        m.set_return(0);
        return;
    }

    // cpu_ticks are in units of 1/CLK_TCK second, already summed across all
    // processors, which is the same aggregation GetSystemTimes reports.
    const long hz = ::sysconf(_SC_CLK_TCK);
    const u64 per_tick_100ns =
        10000000ULL / static_cast<u64>(hz > 0 ? hz : 100);

    const u64 idle = static_cast<u64>(load.cpu_ticks[CPU_STATE_IDLE]) *
                     per_tick_100ns;
    const u64 system = static_cast<u64>(load.cpu_ticks[CPU_STATE_SYSTEM]) *
                       per_tick_100ns;
    // NICE is user time at a lowered priority; Windows has no separate
    // bucket for it, and it belongs with user time rather than being lost.
    const u64 user = (static_cast<u64>(load.cpu_ticks[CPU_STATE_USER]) +
                      static_cast<u64>(load.cpu_ticks[CPU_STATE_NICE])) *
                     per_tick_100ns;

    if ((idle_ptr != 0 && m_memory.write_u64(idle_ptr, idle).is_err()) ||
        (kernel_ptr != 0 &&
         m_memory.write_u64(kernel_ptr, system + idle).is_err()) ||
        (user_ptr != 0 && m_memory.write_u64(user_ptr, user).is_err())) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

void Kernel32ExtManager::impl_GetProcessTimes(Marshaler& m) {
    const Handle process_handle = static_cast<Handle>(m.arg(0));
    const GuestAddr creation_ptr = m.arg_ptr(1);
    const GuestAddr exit_ptr     = m.arg_ptr(2);
    const GuestAddr kernel_ptr   = m.arg_ptr(3);
    const GuestAddr user_ptr     = m.arg_ptr(4);

    constexpr u32 PROCESS_QUERY_INFORMATION = 0x00000400;
    constexpr u32 PROCESS_QUERY_LIMITED_INFORMATION = 0x00001000;

    if (creation_ptr == 0 || exit_ptr == 0 ||
        kernel_ptr == 0 || user_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    pid_t target_pid = ::getpid();
    ipc::ProcessObject* tracked = nullptr;
    if (process_handle != CURRENT_PROCESS_PSEUDO) {
        if (m_handle_table == nullptr ||
            m_handle_table->get_type(process_handle) != HandleType::Process) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        const u32 access = m_handle_table->get_access(process_handle);
        if (access != HandleAccess::All &&
            (access & (PROCESS_QUERY_INFORMATION |
                       PROCESS_QUERY_LIMITED_INFORMATION)) == 0) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            m.set_return(0);
            return;
        }
        tracked = static_cast<ipc::ProcessObject*>(
            m_handle_table->get(process_handle, HandleType::Process));
        if (tracked == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        // Builtin completed processes have pid 0; serve cached times only.
        if (tracked->pid <= 0) {
            if (!tracked->times_cached.load(std::memory_order_acquire) &&
                !tracked->exited.load(std::memory_order_acquire)) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            const u64 now = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count()) *
                                10ULL +
                            116444736000000000ULL;
            const u64 creation =
                tracked->creation_filetime.load(std::memory_order_relaxed);
            u64 exit_time =
                tracked->exit_filetime.load(std::memory_order_relaxed);
            if (exit_time == 0 &&
                tracked->exited.load(std::memory_order_acquire)) {
                exit_time = now;
            }
            if (m_memory.write_u64(creation_ptr,
                                   creation != 0 ? creation : now)
                    .is_err() ||
                m_memory.write_u64(exit_ptr, exit_time).is_err() ||
                m_memory
                    .write_u64(kernel_ptr,
                               tracked->kernel_filetime.load(
                                   std::memory_order_relaxed))
                    .is_err() ||
                m_memory
                    .write_u64(user_ptr, tracked->user_filetime.load(
                                             std::memory_order_relaxed))
                    .is_err()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            m.set_return(1);
            return;
        }
        // Querying another process is the normal case, not an unsupported one:
        // a parent records when each child started. A failure after the
        // child has launched is fatal for a caller that checks the result.
        target_pid = static_cast<pid_t>(tracked->pid);
    }

    // proc_pid_rusage reports CPU time and the process start instant for any
    // process owned by this user, so the same path serves the current process
    // and a tracked child. After the host pid is reaped it fails — fall back
    // to times cached on ProcessObject at exit (Learn: times remain valid
    // until the last handle closes).
    rusage_info_v2 ri{};
    const bool rusage_ok =
        ::proc_pid_rusage(target_pid, RUSAGE_INFO_V2,
                          reinterpret_cast<rusage_info_t*>(&ri)) == 0;
    if (!rusage_ok) {
        if (tracked != nullptr &&
            tracked->times_cached.load(std::memory_order_acquire)) {
            const u64 creation =
                tracked->creation_filetime.load(std::memory_order_relaxed);
            const u64 exit_time =
                tracked->exit_filetime.load(std::memory_order_relaxed);
            const u64 kernel =
                tracked->kernel_filetime.load(std::memory_order_relaxed);
            const u64 user =
                tracked->user_filetime.load(std::memory_order_relaxed);
            if (m_memory.write_u64(creation_ptr, creation).is_err() ||
                m_memory.write_u64(exit_ptr, exit_time).is_err() ||
                m_memory.write_u64(kernel_ptr, kernel).is_err() ||
                m_memory.write_u64(user_ptr, user).is_err()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            m.set_return(1);
            return;
        }
        set_last_error(error_codes::ERROR_INVALID_FUNCTION);
        m.set_return(0);
        return;
    }

    // Windows FILETIME counts 100ns units; proc_pid_rusage reports nanoseconds.
    const u64 kernel_time = ri.ri_system_time / 100ULL;
    const u64 user_time = ri.ri_user_time / 100ULL;

    // ri_proc_start_abstime is a mach absolute timestamp, so convert it to wall
    // clock by measuring how long ago it was and subtracting that from now.
    constexpr u64 WINDOWS_EPOCH_OFFSET_100NS = 116444736000000000ULL;
    static mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        if (tb.denom == 0) { tb.numer = 1; tb.denom = 1; }
        return tb;
    }();
    const u64 now_100ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count())
        * 10ULL + WINDOWS_EPOCH_OFFSET_100NS;
    const u64 now_abs = mach_absolute_time();
    u64 creation_time = now_100ns;
    if (ri.ri_proc_start_abstime != 0 && now_abs > ri.ri_proc_start_abstime) {
        const u64 elapsed_ns = (now_abs - ri.ri_proc_start_abstime) *
                               timebase.numer / timebase.denom;
        const u64 elapsed_100ns = elapsed_ns / 100ULL;
        creation_time = (elapsed_100ns < now_100ns)
                            ? now_100ns - elapsed_100ns
                            : now_100ns;
    }

    u64 exit_time = 0;
    if (tracked != nullptr &&
        tracked->exited.load(std::memory_order_acquire)) {
        exit_time = tracked->exit_filetime.load(std::memory_order_relaxed);
        if (exit_time == 0) exit_time = now_100ns;
        // Refresh cache for later post-reap queries.
        tracked->creation_filetime.store(creation_time,
                                         std::memory_order_relaxed);
        tracked->kernel_filetime.store(kernel_time, std::memory_order_relaxed);
        tracked->user_filetime.store(user_time, std::memory_order_relaxed);
        tracked->exit_filetime.store(exit_time, std::memory_order_relaxed);
        tracked->times_cached.store(true, std::memory_order_release);
    }

    if (m_memory.write_u64(creation_ptr, creation_time).is_err() ||
        m_memory.write_u64(exit_ptr, exit_time).is_err() ||
        m_memory.write_u64(kernel_ptr, kernel_time).is_err() ||
        m_memory.write_u64(user_ptr, user_time).is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    VITREO_TRACE("[k32ext] GetProcessTimes(pid=%d) -> TRUE "
                 "(kern=%llu, user=%llu 100ns)\n",
                 static_cast<int>(target_pid),
                 static_cast<unsigned long long>(kernel_time),
                 static_cast<unsigned long long>(user_time));
    m.set_return(1); // TRUE
}

} // namespace vitreo::winapi
