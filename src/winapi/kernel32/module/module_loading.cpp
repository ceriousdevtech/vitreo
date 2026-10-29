// SPDX-License-Identifier: LGPL-2.1-only

/// @file module_loading.cpp
/// @brief Module Loading banner APIs.
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
// Module Loading
// =============================================================================

void Kernel32ExtManager::impl_LoadLibraryExA(Marshaler& m) {
    // HMODULE LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
    GuestAddr name_ptr = m.arg_ptr(0);
    (void)m.arg(1); // hFile (reserved)
    (void)m.arg(2); // dwFlags

    if (name_ptr == 0 || m_module_manager == nullptr) {
        m.set_return(0);
        return;
    }

    // Read ASCII name
    std::string name;
    for (u32 i = 0; i < 260; ++i) {
        auto r = m_memory.read_u8(name_ptr + i);
        if (r.is_err() || r.value() == 0) break;
        name.push_back(static_cast<char>(r.value()));
    }

    if (name.empty()) {
        m.set_return(0);
        return;
    }

    GuestAddr base = m_module_manager->load_library(name);
    VITREO_TRACE( "[kernel32_ext] LoadLibraryExA(\"%s\") -> 0x%08llx\n",
                 name.c_str(), static_cast<unsigned long long>(base));
    m.set_return(base);
}

// Shared body for GetModuleHandleEx{A,W}. HMODULE* is pointer-sized: writing
// only 4 bytes on x64 leaves garbage in the caller's high dword. A later
// GetProcAddress of that value misses.
void Kernel32ExtManager::get_module_handle_ex_common(Marshaler& m, bool wide) {
    constexpr u32 GET_MODULE_HANDLE_EX_FLAG_PIN = 0x01;
    constexpr u32 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT = 0x02;
    constexpr u32 GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS = 0x04;
    constexpr u32 VALID_FLAGS =
        GET_MODULE_HANDLE_EX_FLAG_PIN |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT |
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;

    u32 flags = static_cast<u32>(m.arg(0));
    GuestAddr name_ptr = m.arg_ptr(1);
    GuestAddr module_out = m.arg_ptr(2);

    // Learn: phModule is a required [out]. Windows fails NULL with
    // ERROR_INVALID_PARAMETER without taking a reference
    // (loader_contract_probe.expected GetModuleHandleExW(..., NULL) → 0 err=57).
    if (module_out == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const auto write_module = [&](GuestAddr base) {
        if (m.cpu_is_64bit()) (void)m_memory.write_u64(module_out, base);
        else                  (void)m_memory.write_u32(module_out, static_cast<u32>(base));
    };

    if ((flags & ~VALID_FLAGS) != 0 ||
        ((flags & GET_MODULE_HANDLE_EX_FLAG_PIN) != 0 &&
         (flags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT) != 0)) {
        write_module(0);
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    if (m_module_manager == nullptr) {
        write_module(0);
        set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
        m.set_return(0);
        return;
    }

    GuestAddr base = 0;
    if ((flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) != 0) {
        // With FROM_ADDRESS, lpModuleName is an address inside the module.
        auto hit = m_module_manager->module_for_address(name_ptr);
        if (hit.info != nullptr) {
            base = hit.info->base_address;
        }
    } else {
        std::string name;
        if (name_ptr != 0) {
            if (wide) {
                auto wide_name = m.read_wide_string(name_ptr, 260);
                if (wide_name.is_ok()) name = std::move(wide_name.value());
            } else {
                for (u32 i = 0; i < 260; ++i) {
                    auto r = m_memory.read_u8(name_ptr + i);
                    if (r.is_err() || r.value() == 0) break;
                    name.push_back(static_cast<char>(r.value()));
                }
            }
        }
        base = m_module_manager->get_module_handle(name);
    }

    if (base != 0 &&
        (flags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT) == 0 &&
        !m_module_manager->acquire_module_reference(
            base, (flags & GET_MODULE_HANDLE_EX_FLAG_PIN) != 0)) {
        base = 0;
    }

    write_module(base);
    if (base == 0) set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
    m.set_return(base != 0 ? 1u : 0u);
}

void Kernel32ExtManager::impl_GetModuleHandleExA(Marshaler& m) {
    // BOOL GetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule)
    get_module_handle_ex_common(m, /*wide=*/false);
}

void Kernel32ExtManager::impl_GetModuleHandleExW(Marshaler& m) {
    // BOOL GetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule)
    get_module_handle_ex_common(m, /*wide=*/true);
}

} // namespace vitreo::winapi
