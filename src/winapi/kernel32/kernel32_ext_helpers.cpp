// SPDX-License-Identifier: LGPL-2.1-only

/// @file kernel32_ext_helpers.cpp
/// @brief Internal helpers and Error Handling from kernel32_ext.
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
// Internal Helpers
// =============================================================================

u32 Kernel32ExtManager::write_wide_literal(GuestAddr addr, const char* str) {
    u32 i = 0;
    for (; str[i] != '\0'; ++i) {
        (void)m_memory.write_u16(addr + i * 2, static_cast<u16>(str[i]));
    }
    (void)m_memory.write_u16(addr + i * 2, 0);
    return i;
}

u32 Kernel32ExtManager::guest_wstrlen(GuestAddr addr) {
    u32 len = 0;
    for (;;) {
        auto r = m_memory.read_u16(addr + len * 2);
        if (r.is_err() || r.value() == 0) break;
        ++len;
    }
    return len;
}

u32 Kernel32ExtManager::guest_strlen(GuestAddr addr) {
    // Fast path: scan directly in host memory
    constexpr usize PROBE_SIZE = 4096;
    const Byte* p = m_memory.host_ptr(addr, PROBE_SIZE);
    if (p) {
        const void* nul = ::memchr(p, 0, PROBE_SIZE);
        if (nul) return static_cast<u32>(static_cast<const Byte*>(nul) - p);
    }
    // Fallback: byte-by-byte
    u32 len = 0;
    for (;;) {
        auto r = m_memory.read_u8(addr + len);
        if (r.is_err() || r.value() == 0) break;
        ++len;
    }
    return len;
}

std::string Kernel32ExtManager::read_wide_ascii_z(GuestAddr addr, usize max_chars) {
    std::string out;
    out.reserve(64);

    if (addr == 0) {
        return out;
    }

    for (usize i = 0; i < max_chars; ++i) {
        auto r = m_memory.read_u16(addr + static_cast<u32>(i * 2));
        if (r.is_err()) {
            break;
        }
        u16 ch = r.value();
        if (ch == 0) {
            break;
        }
        out.push_back(static_cast<char>(ch & 0xFF));
    }
    return out;
}

// =============================================================================
// Error Handling
// =============================================================================

void Kernel32ExtManager::impl_SetErrorMode(Marshaler& m) {
    // UINT SetErrorMode(UINT uMode)
    u32 new_mode = static_cast<u32>(m.arg(0));
    u32 old_mode = m_error_mode;
    m_error_mode = new_mode;
    VITREO_TRACE( "[kernel32_ext] SetErrorMode(0x%x) -> 0x%x\n",
                 new_mode, old_mode);
    m.set_return(old_mode);
}

} // namespace vitreo::winapi
