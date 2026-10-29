// SPDX-License-Identifier: LGPL-2.1-only

/// @file file_time.cpp
/// @brief File-time conversion stubs.
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
// File Time
// =============================================================================

void Kernel32ExtManager::impl_CompareFileTime(Marshaler& m) {
    // LONG CompareFileTime(const FILETIME* lpFileTime1, const FILETIME* lpFileTime2)
    GuestAddr ft1 = m.arg_ptr(0);
    GuestAddr ft2 = m.arg_ptr(1);

    u64 time1 = 0;
    u64 time2 = 0;

    auto lo1 = m_memory.read_u32(ft1);
    auto hi1 = m_memory.read_u32(ft1 + 4);
    if (lo1.is_ok() && hi1.is_ok()) {
        time1 = (static_cast<u64>(hi1.value()) << 32) | lo1.value();
    }

    auto lo2 = m_memory.read_u32(ft2);
    auto hi2 = m_memory.read_u32(ft2 + 4);
    if (lo2.is_ok() && hi2.is_ok()) {
        time2 = (static_cast<u64>(hi2.value()) << 32) | lo2.value();
    }

    i32 result = 0;
    if (time1 < time2) result = -1;
    else if (time1 > time2) result = 1;

    m.set_return(static_cast<u64>(static_cast<u32>(result)));
}

} // namespace vitreo::winapi
