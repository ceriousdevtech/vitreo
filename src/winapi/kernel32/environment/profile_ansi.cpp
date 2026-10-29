// SPDX-License-Identifier: LGPL-2.1-only

/// @file profile_ansi.cpp
/// @brief ANSI private-profile INI APIs and lookup_profile_int.
///
/// Get/WritePrivateProfile*A and section helpers. Owns the file-local
/// INI read/write helpers and Kernel32ExtManager::lookup_profile_int
/// used by GetPrivateProfileIntW in kernel32_ext.cpp.
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

void Kernel32ExtManager::impl_GetPrivateProfileStringA(Marshaler& m) {
    private_profile_string_a(m, m.arg_ptr(5), {});
}

// DWORD GetProfileStringA(LPCSTR lpAppName, LPCSTR lpKeyName, LPCSTR lpDefault,
//   LPSTR lpReturnedString, DWORD nSize)
// Learn: the win.ini form of GetPrivateProfileString — the file is win.ini in
// the Windows directory.
void Kernel32ExtManager::impl_GetProfileStringA(Marshaler& m) {
    private_profile_string_a(m, 0, "C:\\Windows\\win.ini");
}

void Kernel32ExtManager::private_profile_string_a(Marshaler& m, GuestAddr file_addr,
                                                  const std::string& file_override) {
    GuestAddr app_addr     = m.arg_ptr(0);
    GuestAddr key_addr     = m.arg_ptr(1);
    GuestAddr default_addr = m.arg_ptr(2);
    GuestAddr out_addr     = m.arg_ptr(3);
    u32 n_size             = m.arg32(4);

    auto read_ansi_z = [&](GuestAddr addr, u32 cap) -> std::string {
        std::string s;
        if (addr == 0) return s;
        for (u32 i = 0; i < cap; ++i) {
            auto r = m_memory.read_u8(addr + i);
            if (r.is_err() || r.value() == 0) break;
            s.push_back(static_cast<char>(r.value()));
        }
        return s;
    };

    std::string app_name  = read_ansi_z(app_addr,  260);
    std::string key_name  = read_ansi_z(key_addr,  260);
    std::string file_name = file_override.empty() ? read_ansi_z(file_addr, 512)
                                                  : file_override;

    auto write_result = [&](const std::string& value) -> u32 {
        if (out_addr == 0 || n_size == 0) return 0;
        u32 copy_len = static_cast<u32>(value.size());
        if (copy_len >= n_size) copy_len = (n_size > 0) ? n_size - 1 : 0;
        for (u32 i = 0; i < copy_len; ++i) {
            (void)m_memory.write_u8(out_addr + i, static_cast<u8>(value[i]));
        }
        (void)m_memory.write_u8(out_addr + copy_len, 0);
        return copy_len;
    };

    auto copy_default = [&]() -> u32 {
        if (default_addr == 0) {
            if (out_addr != 0 && n_size > 0)
                (void)m_memory.write_u8(out_addr, 0);
            return 0;
        }
        std::string def = read_ansi_z(default_addr, n_size);
        return write_result(def);
    };

    if (!file_name.empty()) {
        auto fh = m_vfs.open(file_name, capsule::AccessMode::Read,
                             capsule::ShareMode::Read,
                             capsule::CreationDisposition::OpenExisting);
        if (fh.is_ok()) {
            std::vector<u8> content;
            std::vector<u8> buf(4096);
            for (;;) {
                auto nr = m_vfs.read(fh.value(), std::span<u8>(buf));
                if (nr.is_err() || nr.value() == 0) break;
                content.insert(content.end(), buf.begin(),
                               buf.begin() + static_cast<std::ptrdiff_t>(nr.value()));
            }
            (void)m_vfs.close(fh.value());

            std::string text(content.begin(), content.end());
            if (text.size() >= 3 &&
                static_cast<u8>(text[0]) == 0xEF &&
                static_cast<u8>(text[1]) == 0xBB &&
                static_cast<u8>(text[2]) == 0xBF) {
                text.erase(0, 3);
            }
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
                if (!line.empty() && line.back() == '\r') line.pop_back();
                usize start = 0;
                while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
                    ++start;
                if (start >= line.size()) continue;
                std::string trimmed = line.substr(start);
                if (trimmed.front() == '[') {
                    auto end_bracket = trimmed.find(']');
                    if (end_bracket != std::string::npos) {
                        std::string section = trimmed.substr(1, end_bracket - 1);
                        in_section = iequals(section, app_name);
                    }
                    continue;
                }
                if (in_section) {
                    auto eq_pos = trimmed.find('=');
                    if (eq_pos != std::string::npos) {
                        std::string k = trimmed.substr(0, eq_pos);
                        while (!k.empty() && (k.back() == ' ' || k.back() == '\t'))
                            k.pop_back();
                        if (iequals(k, key_name)) {
                            std::string v = trimmed.substr(eq_pos + 1);
                            usize vs = 0;
                            while (vs < v.size() && (v[vs] == ' ' || v[vs] == '\t'))
                                ++vs;
                            v = v.substr(vs);
                            u32 len = write_result(v);
                            VITREO_TRACE(
                                "[k32ext] GetPrivateProfileStringA(app='%s', "
                                "key='%s', file='%s') -> found '%s' (len=%u)\n",
                                app_name.c_str(), key_name.c_str(),
                                file_name.c_str(), v.c_str(), len);
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
            VITREO_TRACE(
                "[k32ext] GetPrivateProfileStringA(app='%s', key='%s', "
                "file='%s') -> key not found, using default\n",
                app_name.c_str(), key_name.c_str(), file_name.c_str());
        } else {
            VITREO_TRACE(
                "[k32ext] GetPrivateProfileStringA(app='%s', key='%s', "
                "file='%s') -> file not found, using default\n",
                app_name.c_str(), key_name.c_str(), file_name.c_str());
        }
    }
    if (app_addr != 0 && key_addr != 0) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
    }
    m.set_return(copy_default());
}

namespace {

[[nodiscard]] std::string read_ansi_z_guest(GuestMemory& memory, GuestAddr addr,
                                            u32 cap) {
    std::string s;
    if (addr == 0) return s;
    for (u32 i = 0; i < cap; ++i) {
        auto r = memory.read_u8(addr + i);
        if (r.is_err() || r.value() == 0) break;
        s.push_back(static_cast<char>(r.value()));
    }
    return s;
}

[[nodiscard]] bool iequals_ascii(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

/// Read an entire INI file from the VFS into `text` (UTF-8 BOM stripped).
[[nodiscard]] bool read_ini_file_text(capsule::Vfs& vfs,
                                      const std::string& file_name,
                                      std::string& text) {
    text.clear();
    if (file_name.empty()) return false;
    auto fh = vfs.open(file_name, capsule::AccessMode::Read,
                       capsule::ShareMode::Read,
                       capsule::CreationDisposition::OpenExisting);
    if (fh.is_err()) return false;
    std::vector<u8> content;
    std::vector<u8> buf(4096);
    for (;;) {
        auto nr = vfs.read(fh.value(), std::span<u8>(buf));
        if (nr.is_err() || nr.value() == 0) break;
        content.insert(content.end(), buf.begin(),
                       buf.begin() + static_cast<std::ptrdiff_t>(nr.value()));
    }
    (void)vfs.close(fh.value());
    text.assign(content.begin(), content.end());
    if (text.size() >= 3 &&
        static_cast<u8>(text[0]) == 0xEF &&
        static_cast<u8>(text[1]) == 0xBB &&
        static_cast<u8>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return true;
}

[[nodiscard]] bool write_ini_file_text(capsule::Vfs& vfs,
                                       const std::string& file_name,
                                       const std::string& text) {
    if (file_name.empty()) return false;
    auto fh = vfs.open(file_name, capsule::AccessMode::Write,
                       capsule::ShareMode::None,
                       capsule::CreationDisposition::CreateAlways);
    if (fh.is_err()) return false;
    bool ok = true;
    if (!text.empty()) {
        const auto* bytes = reinterpret_cast<const u8*>(text.data());
        auto nw = vfs.write(
            fh.value(), std::span<const u8>(bytes, text.size()));
        ok = nw.is_ok() && nw.value() == text.size();
    }
    (void)vfs.close(fh.value());
    return ok;
}

[[nodiscard]] std::vector<std::string> ini_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

[[nodiscard]] std::optional<std::string> ini_section_name(
    const std::string& line) {
    usize start = 0;
    while (start < line.size() &&
           (line[start] == ' ' || line[start] == '\t')) {
        ++start;
    }
    if (start >= line.size() || line[start] != '[') return std::nullopt;
    const usize end = line.find(']', start + 1);
    if (end == std::string::npos) return std::nullopt;
    return line.substr(start + 1, end - start - 1);
}

[[nodiscard]] std::optional<std::string> ini_key_name(
    const std::string& line) {
    usize start = 0;
    while (start < line.size() &&
           (line[start] == ' ' || line[start] == '\t')) {
        ++start;
    }
    if (start >= line.size() || line[start] == ';' || line[start] == '#') {
        return std::nullopt;
    }
    const usize equals = line.find('=', start);
    if (equals == std::string::npos) return std::nullopt;
    usize end = equals;
    while (end > start &&
           (line[end - 1] == ' ' || line[end - 1] == '\t')) {
        --end;
    }
    return line.substr(start, end - start);
}

[[nodiscard]] std::string join_ini_lines(
    const std::vector<std::string>& lines) {
    std::string result;
    for (const auto& line : lines) {
        result += line;
        result += "\r\n";
    }
    return result;
}

/// Write a double-NUL-terminated multi-string into `out_addr`.
/// Returns characters written excluding the final NUL (Win32 profile contract).
[[nodiscard]] u32 write_profile_multistring(GuestMemory& memory, GuestAddr out_addr,
                                            u32 n_size,
                                            const std::vector<std::string>& entries) {
    if (out_addr == 0 || n_size == 0) return 0;

    // Need room for the final extra NUL. If the buffer is too small for even
    // an empty multi-string, MSDN: return nSize - 2 (when nSize >= 2).
    if (n_size < 2) {
        (void)memory.write_u8(out_addr, 0);
        return 0;
    }

    u32 written = 0;
    bool truncated = false;
    for (const auto& entry : entries) {
        // entry + NUL, and we must leave one byte for the final NUL.
        const u32 need = static_cast<u32>(entry.size()) + 1u;
        if (written + need + 1u > n_size) {
            truncated = true;
            break;
        }
        for (char ch : entry) {
            (void)memory.write_u8(out_addr + written, static_cast<u8>(ch));
            ++written;
        }
        (void)memory.write_u8(out_addr + written, 0);
        ++written;
    }
    (void)memory.write_u8(out_addr + written, 0);
    if (truncated) {
        return n_size >= 2 ? (n_size - 2) : 0;
    }
    return written;
}

} // namespace

u32 Kernel32ExtManager::lookup_profile_int(const std::string& app_name,
                                           const std::string& key_name,
                                           u32 n_default,
                                           const std::string& file_name) {
    // Missing file or key returns nDefault; value parsed with base-0
    // strtol (accepts decimal, 0oct, 0xhex). Leading whitespace skipped.
    // The last error follows GetPrivateProfileString, which this API is built
    // on: a key that is present sets ERROR_SUCCESS, a missing key (or file)
    // sets ERROR_FILE_NOT_FOUND. Both measured on Windows 11
    // (tools/oracle_probe/last_error_on_success_probe.expected).
    std::string text;
    if (!read_ini_file_text(m_vfs, file_name, text) || app_name.empty() ||
        key_name.empty()) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        return n_default;
    }

    bool in_section = false;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        usize start = 0;
        while (start < line.size() &&
               (line[start] == ' ' || line[start] == '\t')) {
            ++start;
        }
        if (start >= line.size()) continue;
        std::string trimmed = line.substr(start);
        if (trimmed.front() == ';') continue;
        if (trimmed.front() == '[') {
            auto end_bracket = trimmed.find(']');
            if (end_bracket != std::string::npos) {
                std::string section = trimmed.substr(1, end_bracket - 1);
                in_section = iequals_ascii(section, app_name);
            } else {
                in_section = false;
            }
            continue;
        }
        if (!in_section) continue;
        auto eq_pos = trimmed.find('=');
        if (eq_pos == std::string::npos) continue;
        std::string k = trimmed.substr(0, eq_pos);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        if (!iequals_ascii(k, key_name)) continue;
        set_last_error(error_codes::ERROR_SUCCESS);
        std::string v = trimmed.substr(eq_pos + 1);
        usize vs = 0;
        while (vs < v.size() && (v[vs] == ' ' || v[vs] == '\t')) ++vs;
        v = v.substr(vs);
        if (v.empty()) return n_default;
        char* end = nullptr;
        errno = 0;
        const long parsed = std::strtol(v.c_str(), &end, 0);
        if (end == v.c_str()) return n_default;
        return static_cast<u32>(static_cast<i32>(parsed));
    }
    set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
    return n_default;
}

void Kernel32ExtManager::impl_GetPrivateProfileIntA(Marshaler& m) {
    // UINT GetPrivateProfileIntA(LPCSTR lpAppName, LPCSTR lpKeyName,
    //                            INT nDefault, LPCSTR lpFileName)
    GuestAddr app_ptr = m.arg_ptr(0);
    GuestAddr key_ptr = m.arg_ptr(1);
    u32 n_default = static_cast<u32>(m.arg(2));
    GuestAddr file_ptr = m.arg_ptr(3);

    const std::string app_name = read_ansi_z_guest(m_memory, app_ptr, 260);
    const std::string key_name = read_ansi_z_guest(m_memory, key_ptr, 260);
    const std::string file_name = read_ansi_z_guest(m_memory, file_ptr, 512);

    const u32 value =
        lookup_profile_int(app_name, key_name, n_default, file_name);
    VITREO_TRACE(
        "[k32ext] GetPrivateProfileIntA('%s','%s', default=%u, '%s') → %u\n",
        app_name.c_str(), key_name.c_str(), n_default, file_name.c_str(),
        value);
    m.set_return(value);
}

void Kernel32ExtManager::impl_WritePrivateProfileStringA(Marshaler& m) {
    const GuestAddr app_ptr = m.arg_ptr(0);
    const GuestAddr key_ptr = m.arg_ptr(1);
    const GuestAddr value_ptr = m.arg_ptr(2);
    const GuestAddr file_ptr = m.arg_ptr(3);

    // A fully-null call flushes the Windows profile cache. Vitreo performs
    // profile I/O synchronously, so there is nothing to flush.
    if (app_ptr == 0 && key_ptr == 0 && value_ptr == 0) {
        m.set_return(1u);
        return;
    }

    const std::string app = read_ansi_z_guest(m_memory, app_ptr, 260);
    const std::string key = read_ansi_z_guest(m_memory, key_ptr, 260);
    const std::string value = read_ansi_z_guest(m_memory, value_ptr, 32768);
    const std::string file = read_ansi_z_guest(m_memory, file_ptr, 512);
    if (app.empty() || file.empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }

    std::string text;
    (void)read_ini_file_text(m_vfs, file, text);
    const auto input = ini_lines(text);
    std::vector<std::string> output;
    bool in_target = false;
    bool found_section = false;
    bool wrote_key = false;

    for (const auto& line : input) {
        const auto section = ini_section_name(line);
        if (section.has_value()) {
            if (in_target && key_ptr != 0 && value_ptr != 0 && !wrote_key) {
                output.push_back(key + "=" + value);
                wrote_key = true;
            }
            in_target = iequals_ascii(*section, app);
            if (in_target) found_section = true;
            if (in_target && key_ptr == 0) continue; // delete whole section
            output.push_back(line);
            continue;
        }
        if (in_target && key_ptr == 0) continue;
        if (in_target && key_ptr != 0) {
            const auto line_key = ini_key_name(line);
            if (line_key.has_value() && iequals_ascii(*line_key, key)) {
                if (value_ptr != 0 && !wrote_key) {
                    output.push_back(key + "=" + value);
                    wrote_key = true;
                }
                continue; // replace or delete the old key
            }
        }
        output.push_back(line);
    }

    if (in_target && key_ptr != 0 && value_ptr != 0 && !wrote_key) {
        output.push_back(key + "=" + value);
        wrote_key = true;
    }
    if (!found_section && key_ptr != 0 && value_ptr != 0) {
        if (!output.empty() && !output.back().empty()) output.emplace_back();
        output.push_back("[" + app + "]");
        output.push_back(key + "=" + value);
    }

    const bool ok = write_ini_file_text(m_vfs, file, join_ini_lines(output));
    if (!ok) set_last_error(error_codes::ERROR_ACCESS_DENIED);
    VITREO_TRACE(
        "[k32ext] WritePrivateProfileStringA(app='%s', key='%s', val='%s', "
        "file='%s') -> %s\n",
        app.c_str(), key_ptr ? key.c_str() : "(null)",
        value_ptr ? value.c_str() : "(null)", file.c_str(),
        ok ? "TRUE" : "FALSE");
    m.set_return(ok ? 1u : 0u);
}

void Kernel32ExtManager::impl_WritePrivateProfileSectionA(Marshaler& m) {
    const GuestAddr app_ptr = m.arg_ptr(0);
    const GuestAddr values_ptr = m.arg_ptr(1);
    const GuestAddr file_ptr = m.arg_ptr(2);
    const std::string app = read_ansi_z_guest(m_memory, app_ptr, 260);
    const std::string file = read_ansi_z_guest(m_memory, file_ptr, 512);
    if (app.empty() || file.empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }

    std::vector<std::string> replacement;
    if (values_ptr != 0) {
        GuestAddr cursor = values_ptr;
        for (u32 consumed = 0; consumed < 65536;) {
            std::string entry = read_ansi_z_guest(
                m_memory, cursor, 65536 - consumed);
            if (entry.empty()) break;
            replacement.push_back(entry);
            const u32 advance = static_cast<u32>(entry.size()) + 1u;
            cursor += advance;
            consumed += advance;
        }
    }

    std::string text;
    (void)read_ini_file_text(m_vfs, file, text);
    const auto input = ini_lines(text);
    std::vector<std::string> output;
    bool in_target = false;
    bool inserted = false;
    for (const auto& line : input) {
        const auto section = ini_section_name(line);
        if (section.has_value()) {
            if (in_target && !inserted && values_ptr != 0) {
                output.push_back("[" + app + "]");
                output.insert(output.end(), replacement.begin(), replacement.end());
                inserted = true;
            }
            in_target = iequals_ascii(*section, app);
            if (in_target) continue;
        }
        if (!in_target) output.push_back(line);
    }
    if (!inserted && values_ptr != 0) {
        if (!output.empty() && !output.back().empty()) output.emplace_back();
        output.push_back("[" + app + "]");
        output.insert(output.end(), replacement.begin(), replacement.end());
    }

    const bool ok = write_ini_file_text(m_vfs, file, join_ini_lines(output));
    if (!ok) set_last_error(error_codes::ERROR_ACCESS_DENIED);
    VITREO_TRACE(
        "[k32ext] WritePrivateProfileSectionA(app='%s', entries=%zu, "
        "file='%s') -> %s\n",
        app.c_str(), replacement.size(), file.c_str(), ok ? "TRUE" : "FALSE");
    m.set_return(ok ? 1u : 0u);
}

void Kernel32ExtManager::impl_GetPrivateProfileSectionA(Marshaler& m) {
    // DWORD GetPrivateProfileSectionA(LPCSTR lpAppName, LPSTR lpReturnedString,
    //                                 DWORD nSize, LPCSTR lpFileName);
    GuestAddr app_addr  = m.arg_ptr(0);
    GuestAddr out_addr  = m.arg_ptr(1);
    u32 n_size          = static_cast<u32>(m.arg(2));
    GuestAddr file_addr = m.arg_ptr(3);

    const std::string app_name  = read_ansi_z_guest(m_memory, app_addr, 260);
    const std::string file_name = read_ansi_z_guest(m_memory, file_addr, 512);

    std::vector<std::string> entries;
    std::string text;
    if (read_ini_file_text(m_vfs, file_name, text) && !app_name.empty()) {
        bool in_section = false;
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            usize start = 0;
            while (start < line.size() &&
                   (line[start] == ' ' || line[start] == '\t')) {
                ++start;
            }
            if (start >= line.size()) continue;
            std::string trimmed = line.substr(start);
            if (trimmed.front() == ';') continue;
            if (trimmed.front() == '[') {
                auto end_bracket = trimmed.find(']');
                if (end_bracket != std::string::npos) {
                    std::string section = trimmed.substr(1, end_bracket - 1);
                    in_section = iequals_ascii(section, app_name);
                } else {
                    in_section = false;
                }
                continue;
            }
            if (in_section) {
                // Keep key=value as-is (trimmed leading whitespace only).
                entries.push_back(trimmed);
            }
        }
    }

    const u32 ret = write_profile_multistring(m_memory, out_addr, n_size, entries);
    VITREO_TRACE(
        "[k32ext] GetPrivateProfileSectionA(app='%s', nSize=%u, file='%s') "
        "-> %u (%zu entries)\n",
        app_name.c_str(), n_size, file_name.c_str(), ret, entries.size());
    m.set_return(ret);
}

void Kernel32ExtManager::impl_GetPrivateProfileSectionNamesA(Marshaler& m) {
    // DWORD GetPrivateProfileSectionNamesA(LPSTR lpszReturnBuffer, DWORD nSize,
    //                                      LPCSTR lpFileName);
    GuestAddr out_addr  = m.arg_ptr(0);
    u32 n_size          = static_cast<u32>(m.arg(1));
    GuestAddr file_addr = m.arg_ptr(2);

    const std::string file_name = read_ansi_z_guest(m_memory, file_addr, 512);

    std::vector<std::string> names;
    std::string text;
    if (read_ini_file_text(m_vfs, file_name, text)) {
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            usize start = 0;
            while (start < line.size() &&
                   (line[start] == ' ' || line[start] == '\t')) {
                ++start;
            }
            if (start >= line.size()) continue;
            std::string trimmed = line.substr(start);
            if (trimmed.front() != '[') continue;
            auto end_bracket = trimmed.find(']');
            if (end_bracket == std::string::npos) continue;
            names.push_back(trimmed.substr(1, end_bracket - 1));
        }
    }

    const u32 ret = write_profile_multistring(m_memory, out_addr, n_size, names);
    VITREO_TRACE(
        "[k32ext] GetPrivateProfileSectionNamesA(nSize=%u, file='%s') "
        "-> %u (%zu names)\n",
        n_size, file_name.c_str(), ret, names.size());
    m.set_return(ret);
}

} // namespace vitreo::winapi
