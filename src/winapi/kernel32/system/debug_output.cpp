// SPDX-License-Identifier: LGPL-2.1-only

/// @file debug_output.cpp
/// @brief Debug output APIs.
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
// Debug Output
// =============================================================================

void Kernel32ExtManager::impl_OutputDebugStringA(Marshaler& m) {
    // void OutputDebugStringA(LPCSTR lpOutputString)
    GuestAddr str_ptr = m.arg_ptr(0);
    if (str_ptr != 0) {
        std::string s;
        for (u32 i = 0; i < 1024; ++i) {
            auto r = m_memory.read_u8(str_ptr + i);
            if (r.is_err() || r.value() == 0) break;
            s.push_back(static_cast<char>(r.value()));
        }
        VITREO_TRACE("[OutputDebugStringA] %s\n", s.c_str());
        // OutputDebugString diagnostics are ODS-only; surface them under
        // VITREO_WIN_DIAG so startup blockers are visible without TRACE.
        if (std::getenv("VITREO_WIN_DIAG")) {
            ipc::ipc_log("OutputDebugStringA: %s", s.c_str());
        }
    }
}

void Kernel32ExtManager::impl_OutputDebugStringW(Marshaler& m) {
    // void OutputDebugStringW(LPCWSTR lpOutputString)
    GuestAddr str_ptr = m.arg_ptr(0);
    if (str_ptr != 0) {
        std::string s = read_wide_ascii_z(str_ptr, 1024);
        VITREO_TRACE("[OutputDebugStringW] %s\n", s.c_str());
        if (std::getenv("VITREO_WIN_DIAG")) {
            ipc::ipc_log("OutputDebugStringW: %s", s.c_str());
        }
    }
}

void Kernel32ExtManager::impl_DebugBreak(Marshaler& m) {
    // void DebugBreak(void)
    VITREO_TRACE( "[kernel32_ext] DebugBreak() — continuing\n");
}

} // namespace vitreo::winapi
