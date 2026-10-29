// SPDX-License-Identifier: LGPL-2.1-only

/// @file file_ops.cpp
/// @brief Extended file and system operation stubs for kernel32 (7-Zip installer)

#include "winapi/kernel32/filesystem/file_ops.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "ipc/ipc_gate.hpp"

#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

/// @file system_queries.cpp
/// @brief System Queries
///
/// Closed cut from file_ops.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// System Queries
// =============================================================================

void FileOpsManager::impl_GetSystemDirectoryW(Marshaler& m) {
    // UINT GetSystemDirectoryW(LPWSTR lpBuffer, UINT uSize)
    // Learn: if uSize is too small, return required size (incl. NUL) and do not
    // treat a truncated write as success (Windows leaves the buffer untouched).
    GuestAddr buffer = m.arg_ptr(0);
    u32 size         = static_cast<u32>(m.arg(1));

    // "C:\Windows\System32" is 19 chars + null
    static constexpr const char* SYS_DIR = "C:\\Windows\\System32";
    static constexpr u32 SYS_DIR_LEN = 19;

    if (size == 0 || buffer == 0 || size <= SYS_DIR_LEN) {
        m.set_return(SYS_DIR_LEN + 1);
        return;
    }

    for (u32 i = 0; i < SYS_DIR_LEN; ++i) {
        (void)m_memory.write_u16(buffer + i * 2,
                                 static_cast<u16>(SYS_DIR[i]));
    }
    (void)m_memory.write_u16(buffer + SYS_DIR_LEN * 2, 0);
    m.set_return(SYS_DIR_LEN);
}

void FileOpsManager::impl_GetVersion(Marshaler& m) {
    // DWORD GetVersion(void)
    // Returns: LOBYTE(LOWORD) = major, HIBYTE(LOWORD) = minor, HIWORD = build
    // Keep consistent with GetVersionExW (sysinfo.cpp) and modern Windows expectations.
    // Windows 10: major=10, minor=0, build=19041
    // Packed: (19041 << 16) | (0 << 8) | 10 = 0x4A61000A
    constexpr u32 version = (19041u << 16) | (0u << 8) | 10u;
    m.set_return(version);
}

// VITREO_LOG_LOADLIB=1: report the guest return address of a dynamic load.
// Full-path loads arrive here rather than through LoadLibraryA/W, so the
// caller of an explicit-path load is only visible from this entry point.
static void log_loadlibex_caller(Marshaler& m, const std::string& what) {
    static const bool on = std::getenv("VITREO_LOG_LOADLIB") != nullptr;
    if (!on || !m.has_cpu()) return;
    const GuestAddr sp = static_cast<GuestAddr>(m.cpu()->sp());
    u64 caller = 0;
    if (m.cpu_is_64bit()) {
        auto r = m.read_u64(sp);
        if (r.is_ok()) caller = r.value();
    } else {
        auto r = m.read_u32(sp);
        if (r.is_ok()) caller = r.value();
    }
    fprintf(stderr, "[file] LoadLibraryEx caller ret=0x%llx for \"%s\"\n",
            static_cast<unsigned long long>(caller), what.c_str());
}

void FileOpsManager::impl_LoadLibraryExW(Marshaler& m) {
    // HMODULE LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
    const GuestAddr name_ptr = m.arg_ptr(0);
    const u64 h_file = m.arg(1); // reserved, must be NULL
    u32 flags = static_cast<u32>(m.arg(2));

    constexpr u32 LOAD_LIBRARY_AS_DATAFILE           = 0x00000002;
    constexpr u32 LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE = 0x00000040;
    constexpr u32 LOAD_LIBRARY_AS_IMAGE_RESOURCE     = 0x00000020;

    if (name_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    auto name_res = m.read_wide_string(name_ptr);
    if (name_res.is_err() || name_res.value().empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    // Learn: hFile is reserved and must be NULL.
    if (h_file != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }

    std::string name_str = name_res.value();
    log_loadlibex_caller(m, name_str);

    if (flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                 LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {
        if (m_modules == nullptr) {
            set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
            m.set_return(0u);
            return;
        }
        GuestAddr handle = m_modules->load_as_data_file(name_str, flags);
        VITREO_TRACE("[file] LoadLibraryExW(\"%s\", flags=0x%x) → data-file 0x%llx\n",
                     name_str.c_str(), flags,
                     static_cast<unsigned long long>(handle));
        m.set_return(handle);
        return;
    }

    if (m_modules) {
        GuestAddr base = m_modules->load_library(name_str, flags);
        if (std::getenv("VITREO_LOG_LOADLIB")) {
            fprintf(stderr,
                    "[file] LoadLibraryEx result base=0x%llx gle=%u for \"%s\"\n",
                    static_cast<unsigned long long>(base),
                    static_cast<unsigned>(get_last_error()),
                    name_str.c_str());
        }
        if (base != 0) {
            VITREO_TRACE( "[file] LoadLibraryExW(\"%s\") → 0x%llx\n",
                         name_str.c_str(),
                         static_cast<unsigned long long>(base));
            m.set_return(base);
            return;
        }
    }

    // DLL not found — return NULL (correct Win32 behavior)
    VITREO_TRACE( "[file] LoadLibraryExW(\"%s\"): not found → NULL\n",
                 name_str.c_str());
    m.set_return(0u);
}

void FileOpsManager::impl_GetStartupInfoA(Marshaler& m) {
    // void GetStartupInfoA(LPSTARTUPINFOA lpStartupInfo)
    // STARTUPINFOA is 68 bytes on x86 and 104 bytes on x64; its trailing
    // pointer-width fields (lpReserved2, hStdInput/Output/Error) must be
    // covered or the guest CRT reads stack garbage from them.
    // Same structure and contents as GetStartupInfoW (the A/W layouts are
    // identical): the creator's dwFlags, wShowWindow, geometry and, with
    // STARTF_USESTDHANDLES, the std handles.
    GuestAddr si_ptr = m.arg_ptr(0);
    if (si_ptr == 0) return;
    write_guest_startup_info(m_memory, si_ptr, m.cpu_is_64bit());
}

} // namespace vitreo::winapi
