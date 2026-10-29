// SPDX-License-Identifier: LGPL-2.1-only

/// @file string_ops.cpp
/// @brief String operations and CRT encoding helpers.
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
// String Operations
// =============================================================================

void Kernel32ExtManager::impl_lstrcmpW(Marshaler& m) {
    // int lstrcmpW(LPCWSTR lpString1, LPCWSTR lpString2)
    // Real Win32 sets last_error=0 on entry (verified via win_oracle_diff).
    GuestAddr s1 = m.arg_ptr(0);
    GuestAddr s2 = m.arg_ptr(1);

    for (u32 i = 0; ; ++i) {
        auto c1 = m_memory.read_u16(s1 + i * 2);
        auto c2 = m_memory.read_u16(s2 + i * 2);
        u16 ch1 = c1.is_ok() ? c1.value() : 0;
        u16 ch2 = c2.is_ok() ? c2.value() : 0;

        if (ch1 < ch2) { m.set_return(static_cast<u64>(static_cast<u32>(-1))); return; }
        if (ch1 > ch2) { m.set_return(1); return; }
        if (ch1 == 0)  { m.set_return(0); return; }
    }
}

void Kernel32ExtManager::impl_lstrcmpiA(Marshaler& m) {
    // int lstrcmpiA(LPCSTR lpString1, LPCSTR lpString2)
    GuestAddr s1 = m.arg_ptr(0);
    GuestAddr s2 = m.arg_ptr(1);

    for (u32 i = 0; ; ++i) {
        auto c1 = m_memory.read_u8(s1 + i);
        auto c2 = m_memory.read_u8(s2 + i);
        u8 ch1 = c1.is_ok() ? c1.value() : 0;
        u8 ch2 = c2.is_ok() ? c2.value() : 0;

        u8 lo1 = static_cast<u8>(std::tolower(ch1));
        u8 lo2 = static_cast<u8>(std::tolower(ch2));

        if (lo1 < lo2) { m.set_return(static_cast<u64>(static_cast<u32>(-1))); return; }
        if (lo1 > lo2) { m.set_return(1); return; }
        if (ch1 == 0)  { m.set_return(0); return; }
    }
}

void Kernel32ExtManager::impl_lstrcmpiW(Marshaler& m) {
    // int lstrcmpiW(LPCWSTR lpString1, LPCWSTR lpString2)
    GuestAddr s1 = m.arg_ptr(0);
    GuestAddr s2 = m.arg_ptr(1);

    for (u32 i = 0; ; ++i) {
        auto c1 = m_memory.read_u16(s1 + i * 2);
        auto c2 = m_memory.read_u16(s2 + i * 2);
        u16 ch1 = c1.is_ok() ? c1.value() : 0;
        u16 ch2 = c2.is_ok() ? c2.value() : 0;

        // ASCII-range case folding
        u16 lo1 = (ch1 >= 'A' && ch1 <= 'Z') ? (ch1 + 32) : ch1;
        u16 lo2 = (ch2 >= 'A' && ch2 <= 'Z') ? (ch2 + 32) : ch2;

        if (lo1 < lo2) { m.set_return(static_cast<u64>(static_cast<u32>(-1))); return; }
        if (lo1 > lo2) { m.set_return(1); return; }
        if (ch1 == 0)  { m.set_return(0); return; }
    }
}

void Kernel32ExtManager::impl_lstrcpyA(Marshaler& m) {
    // LPSTR lstrcpyA(LPSTR lpString1, LPCSTR lpString2)
    GuestAddr dst = m.arg_ptr(0);
    GuestAddr src = m.arg_ptr(1);

    for (u32 i = 0; ; ++i) {
        auto ch = m_memory.read_u8(src + i);
        u8 c = ch.is_ok() ? ch.value() : 0;
        (void)m_memory.write_u8(dst + i, c);
        if (c == 0) {
            // A caller walks strings into a ~0x504 stack buffer via lstrcpyA.
            // Log oversize copies — they smash the caller frame and show up
            // as a bad `this` in later map ops.
            if (i >= 0x400) {
                VITREO_TRACE("[string] lstrcpyA oversize len=%u dst=0x%llx src=0x%llx\n",
                             i, static_cast<unsigned long long>(dst),
                             static_cast<unsigned long long>(src));
            }
            break;
        }
    }

    m.set_return(dst);
}

void Kernel32ExtManager::impl_lstrcpynW(Marshaler& m) {
    // LPWSTR lstrcpynW(LPWSTR lpString1, LPCWSTR lpString2, int iMaxLength)
    GuestAddr dst = m.arg_ptr(0);
    GuestAddr src = m.arg_ptr(1);
    i32 max_len = static_cast<i32>(m.arg(2));

    if (max_len <= 0) {
        m.set_return(dst);
        return;
    }

    i32 i = 0;
    for (; i < max_len - 1; ++i) {
        auto ch = m_memory.read_u16(src + i * 2);
        u16 c = ch.is_ok() ? ch.value() : 0;
        (void)m_memory.write_u16(dst + i * 2, c);
        if (c == 0) { m.set_return(dst); return; }
    }
    // Null-terminate
    (void)m_memory.write_u16(dst + i * 2, 0);

    m.set_return(dst);
}

void Kernel32ExtManager::impl_lstrlenA(Marshaler& m) {
    // int lstrlenA(LPCSTR lpString)
    GuestAddr str = m.arg_ptr(0);
    u32 len = guest_strlen(str);
    m.set_return(len);
}

// =============================================================================
// Encoding / Decoding (CRT critical)
// =============================================================================

void Kernel32ExtManager::impl_EncodePointer(Marshaler& m) {
    // PVOID EncodePointer(PVOID Ptr)
    // CV-12: XOR with per-process random cookie.
    u64 ptr = m.arg(0);
    m.set_return(ptr ^ static_cast<u64>(m_pointer_cookie));
}

void Kernel32ExtManager::impl_DecodePointer(Marshaler& m) {
    // PVOID DecodePointer(PVOID Ptr)
    // CV-12: XOR with same cookie recovers original pointer.
    u64 ptr = m.arg(0);
    m.set_return(ptr ^ static_cast<u64>(m_pointer_cookie));
}

} // namespace vitreo::winapi
