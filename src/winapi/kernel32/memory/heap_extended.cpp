// SPDX-License-Identifier: LGPL-2.1-only

/// @file heap_extended.cpp
/// @brief Heap Extended banner APIs.
///
/// Closed cut from kernel32_ext.cpp — verified with plan_extraction.py.

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
// Heap Extended
// =============================================================================

void Kernel32ExtManager::impl_HeapLock(Marshaler& m) {
    // BOOL HeapLock(HANDLE hHeap)
    (void)m.arg(0);
    m.set_return(1); // TRUE
}

void Kernel32ExtManager::impl_HeapUnlock(Marshaler& m) {
    // BOOL HeapUnlock(HANDLE hHeap)
    (void)m.arg(0);
    m.set_return(1); // TRUE
}

void Kernel32ExtManager::impl_HeapQueryInformation(Marshaler& m) {
    // BOOL HeapQueryInformation(HANDLE HeapHandle,
    //   HEAP_INFORMATION_CLASS HeapInformationClass,
    //   PVOID HeapInformation, SIZE_T HeapInformationLength,
    //   PSIZE_T ReturnLength)
    (void)m.arg(0);
    (void)m.arg(1);
    (void)m.arg_ptr(2);
    (void)m.arg(3);
    (void)m.arg_ptr(4);
    m.set_return(0); // FALSE
}

void Kernel32ExtManager::impl_HeapWalk(Marshaler& m) {
    // BOOL HeapWalk(HANDLE hHeap, LPPROCESS_HEAP_ENTRY lpEntry)
    (void)m.arg(0);
    (void)m.arg_ptr(1);
    m.set_return(0); // FALSE — no more entries
}

} // namespace vitreo::winapi
