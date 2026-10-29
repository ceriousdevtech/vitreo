// SPDX-License-Identifier: LGPL-2.1-only

/// @file atom.cpp
/// @brief Global atom table APIs.
///
/// CPU-Z Atom Table banner.
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
// CPU-Z: Atom Table
// =============================================================================

void Kernel32ExtManager::impl_GlobalAddAtomW(Marshaler& m) {
    GuestAddr str_ptr = m.arg_ptr(0);
    std::lock_guard<std::mutex> atoms(m_atom_mutex);
    u16 atom = m_next_atom++;
    if (str_ptr != 0) {
        auto r = m.read_wide_string(str_ptr);
        if (r.is_ok()) {
            std::wstring ws(r.value().begin(), r.value().end());
            m_atoms[atom] = ws;
            VITREO_TRACE("[k32ext] GlobalAddAtomW('%s') -> 0x%X\n", r.value().c_str(), atom);
        }
    }
    m.set_return(atom);
}

void Kernel32ExtManager::impl_GlobalFindAtomW(Marshaler& m) {
    (void)m.arg_ptr(0); // lpString
    VITREO_TRACE("[k32ext] GlobalFindAtomW -> 0 (not found)\n");
    m.set_return(0);
}

void Kernel32ExtManager::impl_GlobalDeleteAtom(Marshaler& m) {
    u16 atom = static_cast<u16>(m.arg(0));
    {
        std::lock_guard<std::mutex> atoms(m_atom_mutex);
        m_atoms.erase(atom);
    }
    VITREO_TRACE("[k32ext] GlobalDeleteAtom(0x%X) -> 0\n", atom);
    m.set_return(0); // success
}

void Kernel32ExtManager::impl_GlobalGetAtomNameW(Marshaler& m) {
    u16 atom = static_cast<u16>(m.arg(0));
    GuestAddr buf_ptr = m.arg_ptr(1);
    (void)m.arg(2); // nSize

    std::lock_guard<std::mutex> atoms(m_atom_mutex);
    auto it = m_atoms.find(atom);
    if (it != m_atoms.end() && buf_ptr != 0) {
        // Write as UTF-16LE
        const auto& ws = it->second;
        for (usize i = 0; i < ws.size(); ++i) {
            (void)m_memory.write_u16(buf_ptr + static_cast<u32>(i * 2),
                                      static_cast<u16>(ws[i]));
        }
        (void)m_memory.write_u16(buf_ptr + static_cast<u32>(ws.size() * 2), 0);
        u32 ret_len = static_cast<u32>(ws.size());
        VITREO_TRACE("[k32ext] GlobalGetAtomNameW(0x%X) -> %u\n", atom, ret_len);
        m.set_return(ret_len);
    } else {
        if (buf_ptr != 0) {
            (void)m_memory.write_u16(buf_ptr, 0);
        }
        VITREO_TRACE("[k32ext] GlobalGetAtomNameW(0x%X) -> 0\n", atom);
        m.set_return(0);
    }
}

} // namespace vitreo::winapi
