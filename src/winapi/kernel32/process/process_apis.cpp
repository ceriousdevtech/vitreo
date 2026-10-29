// SPDX-License-Identifier: LGPL-2.1-only

/// @file process_apis.cpp
/// @brief Process banner APIs from kernel32_ext.
///
/// Closed cut from kernel32_ext.cpp — verified with plan_extraction.py.

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
#include "winapi/kernel32/system/sysinfo.hpp"
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

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
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
// Process
// =============================================================================

// The system affinity mask is "a bit vector in which each bit represents the
// processors that are configured into a system", and the process mask is a
// subset of it that equals the system mask until the process restricts itself.
// Reporting a hardcoded 1 told every guest this was a UNIPROCESSOR machine,
// which is a decision input, not a cosmetic one: a guest that sizes its job
// system from popcount(mask) builds a single-threaded pipeline and runs its
// whole load single-file no matter how fast the emulation underneath is.
// DWORD_PTR is pointer-sized, so a 32-bit guest sees at most 32 processors.
static u64 affinity_mask_for_guest(Marshaler& m, u64 mask) {
    return processor_topology::guest_affinity(m, mask);
}

u64 Kernel32ExtManager::process_affinity_mask(const Marshaler& m) const {
    const u64 system_mask = processor_topology::guest_affinity(m, processor_topology::processor_mask());
    return m_process_affinity_mask == 0 ? system_mask : (m_process_affinity_mask & system_mask);
}

u64 Kernel32ExtManager::effective_thread_affinity(const Marshaler& m, u64 stored) const {
    const u64 process = process_affinity_mask(m);
    const u64 effective = stored & process;
    return effective != 0 ? effective : process;
}

void Kernel32ExtManager::write_affinity_mask(Marshaler& m, GuestAddr ptr,
                                             u64 mask) {
    if (ptr == 0) return;
    if (m.cpu_is_64bit()) {
        (void)m_memory.write_u64(ptr, mask);
    } else {
        (void)m_memory.write_u32(ptr, static_cast<u32>(mask));
    }
}

void Kernel32ExtManager::impl_GetProcessAffinityMask(Marshaler& m) {
    // BOOL GetProcessAffinityMask(HANDLE hProcess,
    //   PDWORD_PTR lpProcessAffinityMask, PDWORD_PTR lpSystemAffinityMask)
    (void)m.arg(0);
    GuestAddr proc_mask_ptr = m.arg_ptr(1);
    GuestAddr sys_mask_ptr = m.arg_ptr(2);

    const u64 system_mask =
        affinity_mask_for_guest(m, processor_topology::processor_mask());
    // Unrestricted processes report the system mask; SetProcessAffinityMask
    // narrows it.
    const u64 process_mask = process_affinity_mask(m);

    write_affinity_mask(m, proc_mask_ptr, process_mask);
    write_affinity_mask(m, sys_mask_ptr, system_mask);
    m.set_return(1); // TRUE
}

void Kernel32ExtManager::impl_SetProcessAffinityMask(Marshaler& m) {
    // BOOL SetProcessAffinityMask(HANDLE hProcess, DWORD_PTR dwProcessAffinityMask)
    // "The process affinity mask cannot specify a 1 bit for a processor when
    // the system affinity mask specifies a 0 bit for that processor" — such a
    // request fails rather than being silently clamped, and an empty mask is
    // invalid.
    (void)m.arg(0);
    const u64 system_mask =
        affinity_mask_for_guest(m, processor_topology::processor_mask());
    const u64 requested = affinity_mask_for_guest(m, m.arg(1));

    if (requested == 0 || (requested & ~system_mask) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0); // FALSE
        return;
    }
    m_process_affinity_mask = requested;
    m.set_return(1); // TRUE
}

void Kernel32ExtManager::impl_ProcessIdToSessionId(Marshaler& m) {
    // BOOL ProcessIdToSessionId(DWORD dwProcessId, DWORD* pSessionId)
    (void)m.arg(0);
    GuestAddr session_ptr = m.arg_ptr(1);
    if (session_ptr != 0) {
        // Every Vitreo process runs in the interactive console session,
        // which WTSGetActiveConsoleSessionId and WTSQuerySessionInformation
        // report as 1. Session 0 is the non-interactive services session
        // (a Windows process started over ssh reports 0; one started on the
        // desktop reports the same non-zero id WTS does -- s7_misc_probe).
        (void)m_memory.write_u32(session_ptr, 1);
    }
    m.set_return(1); // TRUE
}

} // namespace vitreo::winapi
