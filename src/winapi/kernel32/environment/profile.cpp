// SPDX-License-Identifier: LGPL-2.1-only

/// @file profile.cpp
/// @brief Get/WritePrivateProfileStringW — early INI profile stubs.
///
/// The first INI/Profile banner from kernel32_ext.cpp (wide-string
/// entry points). ANSI siblings live in profile_ansi.cpp.
///
/// Closed cut from kernel32_ext.cpp: nothing these functions define is used
/// elsewhere in that file, and they use nothing file-local from elsewhere —
/// verified in both directions with tools/architecture/plan_extraction.py
/// before moving.

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
// INI / Profile
// =============================================================================

void Kernel32ExtManager::impl_GetPrivateProfileStringW(Marshaler& m) {
    // DWORD GetPrivateProfileStringW(LPCWSTR lpAppName, LPCWSTR lpKeyName,
    //   LPCWSTR lpDefault, LPWSTR lpReturnedString,
    //   DWORD nSize, LPCWSTR lpFileName)
    private_profile_string_w(m, m.arg_ptr(5), {});
}

// DWORD GetProfileStringW(LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, DWORD): the
// win.ini form (Learn) — win.ini in the Windows directory.
void Kernel32ExtManager::impl_GetProfileStringW(Marshaler& m) {
    private_profile_string_w(m, 0, "C:\\Windows\\win.ini");
}

void Kernel32ExtManager::private_profile_string_w(Marshaler& m, GuestAddr file_addr,
                                                  const std::string& file_override) {
    GuestAddr app_addr     = m.arg_ptr(0);
    GuestAddr key_addr     = m.arg_ptr(1);
    GuestAddr default_addr = m.arg_ptr(2);
    GuestAddr out_addr     = m.arg_ptr(3);
    u32 n_size             = m.arg32(4);

    std::string app_name  = (app_addr  != 0) ? read_wide_ascii_z(app_addr, 260)  : "";
    std::string key_name  = (key_addr  != 0) ? read_wide_ascii_z(key_addr, 260)  : "";
    std::string file_name = !file_override.empty() ? file_override
                          : (file_addr != 0) ? read_wide_ascii_z(file_addr, 512) : "";

    VITREO_TRACE("[kernel32_ext] GetPrivateProfileStringW(app='%s', key='%s', file='%s')\n",
                 app_name.c_str(), key_name.c_str(), file_name.c_str());

    // Helper: write a narrow string as UTF-16LE to out_addr, return chars written
    auto write_result = [&](const std::string& value) -> u32 {
        if (out_addr == 0 || n_size == 0) return 0;
        u32 copy_len = static_cast<u32>(value.size());
        if (copy_len >= n_size) copy_len = n_size - 1;
        for (u32 i = 0; i < copy_len; ++i) {
            (void)m_memory.write_u16(out_addr + i * 2,
                                     static_cast<u16>(static_cast<unsigned char>(value[i])));
        }
        (void)m_memory.write_u16(out_addr + copy_len * 2, 0);
        return copy_len;
    };

    // Helper: copy the default wide string from guest memory
    auto copy_default = [&]() -> u32 {
        if (default_addr == 0) {
            if (out_addr != 0 && n_size > 0)
                (void)m_memory.write_u16(out_addr, 0);
            return 0;
        }
        std::string def = read_wide_ascii_z(default_addr, n_size);
        return write_result(def);
    };

    // Try to open and parse the INI file
    if (!file_name.empty()) {
        auto fh = m_vfs.open(file_name, capsule::AccessMode::Read,
                             capsule::ShareMode::Read,
                             capsule::CreationDisposition::OpenExisting);
        if (fh.is_ok()) {
            // Read entire file
            std::vector<u8> content;
            std::vector<u8> buf(4096);
            for (;;) {
                auto nr = m_vfs.read(fh.value(), std::span<u8>(buf));
                if (nr.is_err() || nr.value() == 0) break;
                content.insert(content.end(), buf.begin(),
                               buf.begin() + static_cast<std::ptrdiff_t>(nr.value()));
            }
            (void)m_vfs.close(fh.value());

            // Parse INI: look for [appName] section, then keyName=value
            std::string text(content.begin(), content.end());
            // Strip UTF-8 BOM if present
            if (text.size() >= 3 &&
                static_cast<u8>(text[0]) == 0xEF &&
                static_cast<u8>(text[1]) == 0xBB &&
                static_cast<u8>(text[2]) == 0xBF) {
                text.erase(0, 3);
            }

            // Case-insensitive compare helper
            auto iequals = [](const std::string& a, const std::string& b) -> bool {
                if (a.size() != b.size()) return false;
                for (usize i = 0; i < a.size(); ++i) {
                    if (std::tolower(static_cast<unsigned char>(a[i])) !=
                        std::tolower(static_cast<unsigned char>(b[i])))
                        return false;
                }
                return true;
            };

            bool in_section = false;
            std::istringstream stream(text);
            std::string line;
            while (std::getline(stream, line)) {
                // Trim \r
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                // Trim leading whitespace
                usize start = 0;
                while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
                    ++start;
                if (start >= line.size()) continue;
                std::string trimmed = line.substr(start);

                // Section header?
                if (trimmed.front() == '[') {
                    auto end_bracket = trimmed.find(']');
                    if (end_bracket != std::string::npos) {
                        std::string section = trimmed.substr(1, end_bracket - 1);
                        in_section = iequals(section, app_name);
                    }
                    continue;
                }

                // Key=value in current section
                if (in_section) {
                    auto eq_pos = trimmed.find('=');
                    if (eq_pos != std::string::npos) {
                        std::string k = trimmed.substr(0, eq_pos);
                        // Trim trailing whitespace from key
                        while (!k.empty() && (k.back() == ' ' || k.back() == '\t'))
                            k.pop_back();
                        if (iequals(k, key_name)) {
                            std::string v = trimmed.substr(eq_pos + 1);
                            // Trim leading whitespace from value
                            usize vs = 0;
                            while (vs < v.size() && (v[vs] == ' ' || v[vs] == '\t'))
                                ++vs;
                            v = v.substr(vs);
                            VITREO_TRACE("[kernel32_ext] GetPrivateProfileStringW -> found '%s'\n",
                                         v.c_str());
                            u32 len = write_result(v);
                            // Measured on Windows 11: a key that is present
                            // sets ERROR_SUCCESS; a default answer sets
                            // ERROR_FILE_NOT_FOUND (below).
                            set_last_error(error_codes::ERROR_SUCCESS);
                            m.set_return(len);
                            return;
                        }
                    }
                }
            }
            // Key not found in file — fall through to default
            VITREO_TRACE("[kernel32_ext] GetPrivateProfileStringW -> key not found, using default\n");
        } else {
            VITREO_TRACE("[kernel32_ext] GetPrivateProfileStringW -> file not found: '%s'\n",
                         file_name.c_str());
        }
    }

    // Return default
    if (app_addr != 0 && key_addr != 0) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
    }
    u32 len = copy_default();
    m.set_return(len);
}

void Kernel32ExtManager::impl_WritePrivateProfileStringW(Marshaler& m) {
    // BOOL WritePrivateProfileStringW(LPCWSTR lpAppName, LPCWSTR lpKeyName,
    //   LPCWSTR lpString, LPCWSTR lpFileName)
    GuestAddr app_addr  = m.arg_ptr(0);
    GuestAddr key_addr  = m.arg_ptr(1);
    GuestAddr val_addr  = m.arg_ptr(2);
    GuestAddr file_addr = m.arg_ptr(3);

    std::string app_name  = (app_addr  != 0) ? read_wide_ascii_z(app_addr, 260)  : "";
    std::string key_name  = (key_addr  != 0) ? read_wide_ascii_z(key_addr, 260)  : "";
    std::string value     = (val_addr  != 0) ? read_wide_ascii_z(val_addr, 1024) : "(null)";
    std::string file_name = (file_addr != 0) ? read_wide_ascii_z(file_addr, 512) : "";

    VITREO_TRACE("[kernel32_ext] WritePrivateProfileStringW(app='%s', key='%s', val='%s', file='%s') -> TRUE (stub)\n",
                 app_name.c_str(), key_name.c_str(), value.c_str(), file_name.c_str());
    m.set_return(1); // TRUE
}

// =============================================================================
// Math
// =============================================================================

void Kernel32ExtManager::impl_MulDiv(Marshaler& m) {
    // int MulDiv(int nNumber, int nNumerator, int nDenominator)
    i32 number      = static_cast<i32>(m.arg(0));
    i32 numerator   = static_cast<i32>(m.arg(1));
    i32 denominator = static_cast<i32>(m.arg(2));

    if (denominator == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(static_cast<i32>(-1))));
        return;
    }

    // CV-11: Windows MulDiv rounds to nearest: (a * b + denom/2) / denom
    i64 product = static_cast<i64>(number) * static_cast<i64>(numerator);
    // Add half the denominator for rounding, matching the sign of the product.
    if (product >= 0) {
        product += static_cast<i64>(denominator) / 2;
    } else {
        product -= static_cast<i64>(denominator) / 2;
    }
    i64 result = product / static_cast<i64>(denominator);

    m.set_return(static_cast<u64>(static_cast<u32>(static_cast<i32>(result))));
}

} // namespace vitreo::winapi
