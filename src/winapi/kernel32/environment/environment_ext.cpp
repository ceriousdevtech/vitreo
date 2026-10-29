// SPDX-License-Identifier: LGPL-2.1-only

/// @file kernel32_environment.cpp
/// @brief The process environment block and the path APIs that read it.
///
/// GetEnvironmentVariable / SetEnvironmentVariable and the environment strings
/// block, ExpandEnvironmentStrings, and the path queries a guest uses to find
/// itself: GetModuleFileName's directory, GetTempPath, GetFullPathName,
/// GetCurrentDirectory, SearchPath, GetDiskFreeSpace.
///
/// Environment values are guest-visible strings in a capsule, not host ones, so
/// %-token expansion happens here rather than through getenv.
///
/// Split out of kernel32_ext.cpp. Contiguous and closed: the one anonymous
/// helper it carries, expand_environment_percent_tokens, is used nowhere else,
/// and nothing here needed a file-local helper from outside the region.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/system/userenv/profile_environment.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/win_path.hpp"
#include "capsule/vfs/win32_full_path.hpp"
#include "capsule/vfs/path_encoding.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/system/time.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <sys/statvfs.h>

namespace vitreo::winapi {

// =============================================================================
// Environment / Paths
// =============================================================================

namespace {

std::string expand_environment_percent_tokens(
    const std::string& input, EnvironmentManager* environment_manager) {
    return winapi::expand_environment_percent_tokens(
        input, [environment_manager](std::string_view name)
                   -> std::optional<std::string> {
            if (!environment_manager) return std::nullopt;
            auto result = environment_manager->get_variable(name);
            if (result.is_err()) return std::nullopt;
            return result.value();
        });
}

// GetCurrentDirectoryW reports TCHAR (UTF-16) units, not host UTF-8 bytes.
u32 utf8_utf16_units(std::string_view s) {
    u32 units = 0;
    usize i = 0;
    while (i < s.size()) {
        const u8 b = static_cast<u8>(s[i]);
        u32 cp = 0;
        if ((b & 0x80) == 0) {
            cp = b;
            i += 1;
        } else if ((b & 0xE0) == 0xC0 && i + 1 < s.size()) {
            cp = (static_cast<u32>(b & 0x1F) << 6) |
                 (static_cast<u8>(s[i + 1]) & 0x3F);
            i += 2;
        } else if ((b & 0xF0) == 0xE0 && i + 2 < s.size()) {
            cp = (static_cast<u32>(b & 0x0F) << 12) |
                 ((static_cast<u8>(s[i + 1]) & 0x3F) << 6) |
                 (static_cast<u8>(s[i + 2]) & 0x3F);
            i += 3;
        } else if ((b & 0xF8) == 0xF0 && i + 3 < s.size()) {
            cp = (static_cast<u32>(b & 0x07) << 18) |
                 ((static_cast<u8>(s[i + 1]) & 0x3F) << 12) |
                 ((static_cast<u8>(s[i + 2]) & 0x3F) << 6) |
                 (static_cast<u8>(s[i + 3]) & 0x3F);
            i += 4;
        } else {
            ++i;
            continue;
        }
        units += (cp >= 0x10000u) ? 2u : 1u;
    }
    return units;
}

} // namespace

void Kernel32ExtManager::impl_ExpandEnvironmentStringsA(Marshaler& m) {
    // DWORD ExpandEnvironmentStringsA(LPCSTR lpSrc, LPSTR lpDst, DWORD nSize)
    GuestAddr src_addr = m.arg_ptr(0);
    GuestAddr dst_addr = m.arg_ptr(1);
    u32 n_size = static_cast<u32>(m.arg(2));

    auto src_res = m.read_ansi_string(src_addr);
    if (src_res.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const std::string& input = src_res.value();
    std::string expanded =
        expand_environment_percent_tokens(input, m_environment_manager);

    u32 required = static_cast<u32>(expanded.size()) + 1; // including null

    if (dst_addr != 0 && n_size > 0) {
        u32 copy_len = (static_cast<u32>(expanded.size()) < n_size)
                       ? static_cast<u32>(expanded.size())
                       : (n_size - 1);
        for (u32 j = 0; j < copy_len; ++j) {
            (void)m_memory.write_u8(dst_addr + j,
                                   static_cast<u8>(static_cast<unsigned char>(expanded[j])));
        }
        (void)m_memory.write_u8(dst_addr + copy_len, 0);
    }

    VITREO_TRACE("[kernel32_ext] ExpandEnvironmentStringsA('%s') -> '%s' (req=%u)\n",
                 input.c_str(), expanded.c_str(), required);
    m.set_return(required);
}

void Kernel32ExtManager::impl_ExpandEnvironmentStringsW(Marshaler& m) {
    // DWORD ExpandEnvironmentStringsW(LPCWSTR lpSrc, LPWSTR lpDst, DWORD nSize)
    GuestAddr src_addr = m.arg_ptr(0);
    GuestAddr dst_addr = m.arg_ptr(1);
    u32 n_size = static_cast<u32>(m.arg(2));

    // Read the input string (ASCII from wide chars)
    std::string input = read_wide_ascii_z(src_addr);
    std::string expanded =
        expand_environment_percent_tokens(input, m_environment_manager);

    u32 required = static_cast<u32>(expanded.size()) + 1; // including null

    if (dst_addr != 0 && n_size > 0) {
        u32 copy_len = (static_cast<u32>(expanded.size()) < n_size)
                       ? static_cast<u32>(expanded.size())
                       : (n_size - 1);
        for (u32 j = 0; j < copy_len; ++j) {
            (void)m_memory.write_u16(dst_addr + j * 2,
                                     static_cast<u16>(static_cast<unsigned char>(expanded[j])));
        }
        (void)m_memory.write_u16(dst_addr + copy_len * 2, 0);
    }

    VITREO_TRACE("[kernel32_ext] ExpandEnvironmentStringsW('%s') -> '%s' (req=%u)\n",
                 input.c_str(), expanded.c_str(), required);
    m.set_return(required);
}

void Kernel32ExtManager::impl_GetFullPathNameW(Marshaler& m) {
    // DWORD GetFullPathNameW(LPCWSTR lpFileName, DWORD nBufferLength,
    //                        LPWSTR lpBuffer, LPWSTR* lpFilePart)
    // Normalization per Learn (Naming Files) measured shape by shape in
    // files_ns_probe.expected (gfp_*). Names are carried as UTF-8 and written
    // back as UTF-16 unchanged ("GetFullPathName does not convert the
    // specified file name"), surrogate pairs included.
    const GuestAddr filename_addr = m.arg_ptr(0);
    const u32 buf_len = static_cast<u32>(m.arg(1));
    const GuestAddr buffer_addr = m.arg_ptr(2);
    const GuestAddr filepart_ptr = m.arg_ptr(3);

    auto input = m.read_wide_string(filename_addr);
    if (input.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    auto full = capsule::win32_full_path(input.value(), m_current_directory);
    if (!full) {
        set_last_error(error_codes::ERROR_INVALID_NAME);  // gfp_empty
        m.set_return(0);
        return;
    }
    const std::vector<u16> units = utf8_to_utf16(full->path);
    const u32 len = static_cast<u32>(units.size());
    if (buffer_addr == 0 || buf_len <= len) {
        // Too small: required size including the terminator, and the file
        // part pointer is cleared (gfp_small_buffer / gfp_exact_buffer).
        if (filepart_ptr != 0) (void)m.write_ptr(filepart_ptr, 0);
        m.set_return(len + 1);
        return;
    }
    for (u32 i = 0; i < len; ++i) {
        (void)m.write_u16(buffer_addr + i * 2, units[i]);
    }
    (void)m.write_u16(buffer_addr + len * 2, 0);
    if (filepart_ptr != 0) {
        // LPWSTR* is pointer-sized: the full 8 bytes on x64.
        if (full->file_part == std::string::npos) {
            (void)m.write_ptr(filepart_ptr, 0);
        } else {
            const u32 off = static_cast<u32>(
                utf8_to_utf16(std::string_view(full->path).substr(0, full->file_part)).size());
            (void)m.write_ptr(filepart_ptr, buffer_addr + static_cast<u64>(off) * 2);
        }
    }
    m.set_return(len);
}

void Kernel32ExtManager::impl_GetShortPathNameW(Marshaler& m) {
    // DWORD GetShortPathNameW(LPCWSTR lpszLongPath, LPWSTR lpszShortPath, DWORD cchBuffer)
    GuestAddr long_path = m.arg_ptr(0);
    GuestAddr short_path = m.arg_ptr(1);
    u32 cch_buffer = static_cast<u32>(m.arg(2));

    u32 src_len = guest_wstrlen(long_path);

    if (short_path != 0 && cch_buffer > src_len) {
        for (u32 i = 0; i < src_len; ++i) {
            auto ch = m_memory.read_u16(long_path + i * 2);
            (void)m_memory.write_u16(short_path + i * 2, ch.is_ok() ? ch.value() : 0);
        }
        (void)m_memory.write_u16(short_path + src_len * 2, 0);
    }

    m.set_return(src_len);
}

// GetTempFileName (Learn GetTempFileNameW): "<path>\<pre><uuuu>.tmp". What
// Learn leaves open is measured on Windows 11, x86 and x64 alike
// (tools/oracle_probe/gettempfilename_probe{,.x86}.expected):
//  - One trailing '\' is dropped from lpPathName ('/' is not a separator)
//    and exactly one '\' is put back; a relative path stays relative. NULL is
//    the empty path. More than 247 characters left -> ERROR_BUFFER_OVERFLOW
//    with the buffer untouched (Learn's MAX_PATH-14 is one short).
//  - The W buffer then holds the stripped path. An empty one ("", NULL,
//    "\") skips the directory check and sets ERROR_PATH_NOT_FOUND although
//    the call goes on to succeed ("\pre12.tmp"); any other path that is not
//    an existing directory fails ERROR_DIRECTORY, whatever uUnique is.
//  - The prefix is the first three UTF-16 units, unmasked; NULL is "".
//  - Digits are "%X" of uUnique's low 16 bits: uppercase, unpadded; the
//    extension is lowercase. If those bits are nonzero that number is used
//    and returned, nothing is created and the last error is left alone.
//  - Otherwise the first number tried is (GetTickCount() + n) & 0xFFFF, n
//    counting this process's earlier generating calls that passed the
//    directory check. An existing file or directory moves on by one, 0 is
//    skipped, and after all 65,535 numbers the call fails ERROR_FILE_EXISTS.
//    Any other CreateFile error (ERROR_ACCESS_DENIED, ERROR_PATH_NOT_FOUND
//    from a prefix with '\') fails at once. A failed call leaves the last
//    candidate in the W buffer; success creates the empty file, closes it and
//    leaves ERROR_SUCCESS.
//  - The A form converts through CP_ACP and writes nothing on failure.
Kernel32ExtManager::TempFileNameResult Kernel32ExtManager::temp_file_name(
    std::u16string path, std::u16string_view prefix, u32 unique) {
    constexpr usize kMaxDirectoryChars = 247;
    TempFileNameResult result;
    if (!path.empty() && path.back() == u'\\') path.pop_back();
    if (path.size() > kMaxDirectoryChars) {
        set_last_error(error_codes::ERROR_BUFFER_OVERFLOW);
        return result;
    }
    result.buffer = path;
    result.buffer_written = true;

    auto to_utf8 = [](const std::u16string& units) {
        return utf16_to_utf8(reinterpret_cast<const u16*>(units.data()),
                             static_cast<int>(units.size()));
    };
    if (path.empty()) {
        set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
    } else {
        auto full = capsule::win32_full_path(to_utf8(path), m_current_directory);
        bool is_directory = false;
        if (full) {
            auto info = m_vfs.stat(full->path);
            is_directory = info.is_ok() &&
                           capsule::has_attribute(info.value().attributes,
                                                  capsule::FileAttributes::Directory);
        }
        if (!is_directory) {
            set_last_error(error_codes::ERROR_DIRECTORY);
            return result;
        }
    }

    std::u16string stem = path;
    stem.push_back(u'\\');
    stem.append(prefix.substr(0, 3));
    auto name_for = [&](u32 value) {
        char digits[8];
        std::snprintf(digits, sizeof(digits), "%X", value);
        std::u16string name = stem;
        for (const char* d = digits; *d != '\0'; ++d) name.push_back(static_cast<char16_t>(*d));
        name.append(u".tmp");
        return name;
    };

    unique &= 0xFFFFu;
    if (unique != 0) {
        result.buffer = name_for(unique);
        result.value = unique;
        return result;
    }

    const u32 start = (tick_count32() + m_temp_counter.fetch_add(1)) & 0xFFFFu;
    u32 value = start != 0 ? start : 1;
    for (u32 tries = 0; tries < 0xFFFFu; ++tries) {
        result.buffer = name_for(value);
        auto full = capsule::win32_full_path(to_utf8(result.buffer), m_current_directory);
        if (!full) {
            set_last_error(error_codes::ERROR_INVALID_NAME);
            return result;
        }
        auto h = m_vfs.open(full->path, capsule::AccessMode::ReadWrite,
                            capsule::ShareMode::None,
                            capsule::CreationDisposition::CreateNew);
        if (h.is_ok()) {
            (void)m_vfs.close(h.value());
            set_last_error(error_codes::ERROR_SUCCESS);
            result.value = value;
            return result;
        }
        if (h.error() != capsule::VfsError::PathAlreadyExists) {
            set_last_error(vfs_error_to_win32(h.error()));
            return result;
        }
        value = value == 0xFFFFu ? 1 : value + 1;
    }
    set_last_error(error_codes::ERROR_FILE_EXISTS);
    return result;
}

namespace {

/// Raw UTF-16 units of a NUL-terminated guest string; NULL reads as empty.
/// False when the memory cannot be read (Windows faults there).
bool read_guest_units(GuestMemory& memory, GuestAddr addr, usize limit,
                      std::u16string& out) {
    out.clear();
    if (addr == 0) return true;
    for (usize i = 0; i < limit; ++i) {
        auto unit = memory.read_u16(addr + i * 2);
        if (unit.is_err()) return false;
        if (unit.value() == 0) return true;
        out.push_back(static_cast<char16_t>(unit.value()));
    }
    return true;
}

}  // namespace

void Kernel32ExtManager::impl_GetTempFileNameW(Marshaler& m) {
    // UINT GetTempFileNameW(LPCWSTR lpPathName, LPCWSTR lpPrefixString,
    //                       UINT uUnique, LPWSTR lpTempFileName)
    const GuestAddr path_addr = m.arg_ptr(0);
    const GuestAddr prefix_addr = m.arg_ptr(1);
    const u32 unique = m.arg32(2);
    const GuestAddr out_addr = m.arg_ptr(3);

    std::u16string path, prefix;
    if (!read_guest_units(m_memory, path_addr, 0x8000, path)) {
        (void)m.raise_access_violation(false, path_addr);
        m.set_return(0u);
        return;
    }
    if (!read_guest_units(m_memory, prefix_addr, 3, prefix)) {
        (void)m.raise_access_violation(false, prefix_addr);
        m.set_return(0u);
        return;
    }
    const TempFileNameResult result = temp_file_name(std::move(path), prefix, unique);
    if (result.buffer_written) {
        for (usize i = 0; i <= result.buffer.size(); ++i) {
            const u16 unit = i < result.buffer.size() ? static_cast<u16>(result.buffer[i]) : 0;
            if (m_memory.write_u16(out_addr + i * 2, unit).is_err()) {
                (void)m.raise_access_violation(true, out_addr + i * 2);
                m.set_return(0u);
                return;
            }
        }
    }
    m.set_return(result.value);
}

std::string Kernel32ExtManager::resolve_temp_path() {
    // Learn GetTempPathW: the first of TMP, TEMP, USERPROFILE, then the
    // Windows directory; the result ends in a backslash. Windows does not
    // verify it exists, but a Windows profile always has its temp directory,
    // so the capsule's is created on first use.
    std::string temp_path;
    for (const char* name : {"TMP", "TEMP", "USERPROFILE"}) {
        if (m_environment_manager == nullptr) break;
        auto value = m_environment_manager->get_variable(name);
        if (value.is_ok() && !value.value().empty()) {
            temp_path = value.value();
            break;
        }
    }
    if (temp_path.empty()) temp_path = "C:\\Windows";
    temp_path = normalize_dir_path(temp_path);

    auto parsed = capsule::parse_windows_path(temp_path);
    if (parsed.drive_letter) {
        std::string accumulated;
        std::string rel = parsed.relative_path;
        usize start = 0;
        while (start < rel.size()) {
            usize sep = rel.find('/', start);
            const std::string component =
                rel.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
            start = sep == std::string::npos ? rel.size() : sep + 1;
            if (component.empty()) continue;
            if (!accumulated.empty()) accumulated.push_back('\\');
            accumulated += component;
            std::string partial(1, capsule::normalize_drive_letter(parsed.drive_letter));
            partial += ":\\";
            partial += accumulated;
            (void)m_vfs.create_dir(partial);
        }
    }
    return temp_path;
}

void Kernel32ExtManager::impl_GetTempPathW(Marshaler& m) {
    // DWORD GetTempPathW(DWORD nBufferLength, LPWSTR lpBuffer)
    u32 n_buffer_length = static_cast<u32>(m.arg(0));
    GuestAddr buf = m.arg_ptr(1);

    const std::vector<u16> temp_path = utf8_to_utf16(resolve_temp_path());
    const u32 required_chars = static_cast<u32>(temp_path.size() + 1); // incl null

    if (buf == 0 || n_buffer_length == 0) {
        m.set_return(required_chars);
        return;
    }

    if (n_buffer_length < required_chars) {
        // Per Win32 docs, return required size INCLUDING null terminator.
        m.set_return(required_chars);
        return;
    }

    for (u32 i = 0; i < static_cast<u32>(temp_path.size()); ++i) {
        (void)m_memory.write_u16(buf + i * 2, temp_path[i]);
    }
    (void)m_memory.write_u16(buf + static_cast<u32>(temp_path.size()) * 2, 0);

    // Success: return length EXCLUDING null terminator.
    m.set_return(static_cast<u32>(temp_path.size()));
}

void Kernel32ExtManager::impl_GetWindowsDirectoryW(Marshaler& m) {
    // UINT GetWindowsDirectoryW(LPWSTR lpBuffer, UINT uSize)
    GuestAddr buf = m.arg_ptr(0);
    u32 size = static_cast<u32>(m.arg(1));

    const char* win_dir = "C:\\Windows";
    u32 len = 0;
    while (win_dir[len]) ++len;

    const u32 required_chars = len + 1; // incl null
    if (buf == 0 || size == 0) {
        m.set_return(required_chars);
        return;
    }
    if (size < required_chars) {
        m.set_return(required_chars);
        return;
    }

    (void)write_wide_literal(buf, win_dir);
    m.set_return(len);
}

void Kernel32ExtManager::impl_GetSystemWow64DirectoryW(Marshaler& m) {
    // UINT GetSystemWow64DirectoryW(LPWSTR lpBuffer, UINT uSize)
    GuestAddr buf = m.arg_ptr(0);
    const u32 size = static_cast<u32>(m.arg(1));

    constexpr char wow64_dir[] = "C:\\Windows\\SysWOW64";
    constexpr u32 len = sizeof(wow64_dir) - 1;
    constexpr u32 required_chars = sizeof(wow64_dir);

    if (buf == 0 || size < required_chars) {
        m.set_return(required_chars);
        return;
    }

    (void)write_wide_literal(buf, wow64_dir);
    m.set_return(len);
}

void Kernel32ExtManager::impl_GetSystemWow64DirectoryA(Marshaler& m) {
    // UINT GetSystemWow64DirectoryA(LPSTR lpBuffer, UINT uSize)
    // Same contract as the W form: on 64-bit Windows (the only OS this
    // runtime presents) the path is SysWOW64 for both guest bitnesses.
    // A short or null buffer returns the size including the terminator.
    GuestAddr buf = m.arg_ptr(0);
    const u32 size = static_cast<u32>(m.arg(1));

    constexpr char wow64_dir[] = "C:\\Windows\\SysWOW64";
    constexpr u32 len = sizeof(wow64_dir) - 1;
    constexpr u32 required_chars = sizeof(wow64_dir);

    if (buf == 0 || size < required_chars) {
        m.set_return(required_chars);
        return;
    }
    (void)m.write_ansi_string(buf, wow64_dir);
    m.set_return(len);
}

void Kernel32ExtManager::impl_SetCurrentDirectoryW(Marshaler& m) {
    // BOOL SetCurrentDirectoryW(LPCWSTR lpPathName)
    GuestAddr path_ptr = m.arg_ptr(0);
    if (path_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    auto directory = m.read_wide_string(path_ptr);
    if (directory.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    m.set_return(change_current_directory(directory.value()) ? 1 : 0);
}

bool Kernel32ExtManager::change_current_directory(const std::string& directory) {
    // Learn (SetCurrentDirectory): the value is "the path to the new current
    // directory"; Windows fails unless it names an existing directory
    // (files_ns_probe: missing -> 2, missing parent -> 3, a file -> 267,
    // empty -> 123).
    auto full = capsule::win32_full_path(directory, m_current_directory);
    if (!full) {
        set_last_error(error_codes::ERROR_INVALID_NAME);
        return false;
    }
    auto info = m_vfs.stat(full->path);
    if (info.is_err()) {
        set_last_error(vfs_error_to_win32(info.error()));
        return false;
    }
    if (!capsule::has_attribute(info.value().attributes,
                                capsule::FileAttributes::Directory)) {
        set_last_error(error_codes::ERROR_DIRECTORY);
        return false;
    }
    set_current_directory(full->path);
    VITREO_TRACE("[k32ext] SetCurrentDirectory('%s')\n", m_current_directory.c_str());
    return true;
}

void Kernel32ExtManager::impl_GetCurrentDirectoryW(Marshaler& m) {
    // DWORD GetCurrentDirectoryW(DWORD nBufferLength, LPWSTR lpBuffer)
    // Learn: nBufferLength / return are UTF-16 code units, not host bytes.
    u32 n = static_cast<u32>(m.arg(0));
    GuestAddr buf = m.arg_ptr(1);

    const u32 needed_chars = utf8_utf16_units(m_current_directory);
    if (buf == 0 || n == 0) {
        m.set_return(needed_chars + 1);
        return;
    }

    if (n <= needed_chars) {
        m.set_return(needed_chars + 1);
        return;
    }

    (void)m.write_wide_string(buf, m_current_directory);
    m.set_return(needed_chars);
}

void Kernel32ExtManager::impl_SearchPathW(Marshaler& m) {
    impl_SearchPathCommon(m, true);
}

void Kernel32ExtManager::impl_SearchPathA(Marshaler& m) {
    impl_SearchPathCommon(m, false);
}

void Kernel32ExtManager::impl_SetSearchPathMode(Marshaler& m) {
    // BOOL SetSearchPathMode(DWORD Flags) (Learn), measured x86 == x64 in
    // setsearchpathmode_probe{32,64}.expected: only ENABLE (0x1), DISABLE
    // (0x10000) and ENABLE|PERMANENT (0x8001) are valid, anything else is
    // ERROR_INVALID_PARAMETER; repeating a mode succeeds; once permanent,
    // ENABLE|PERMANENT still succeeds while ENABLE and DISABLE fail with
    // ERROR_ACCESS_DENIED.
    constexpr u32 kEnable = 0x00000001u;
    constexpr u32 kDisable = 0x00010000u;
    constexpr u32 kPermanent = 0x00008000u;
    const u32 flags = static_cast<u32>(m.arg(0));
    if (flags != kEnable && flags != kDisable && flags != (kEnable | kPermanent)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (m_search_path_permanent.load() && flags != (kEnable | kPermanent)) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        m.set_return(0);
        return;
    }
    m_search_path_mode.store(flags == kDisable ? 2u : 1u);
    if (flags & kPermanent) m_search_path_permanent.store(true);
    m.set_return(1);
}

void Kernel32ExtManager::impl_SearchPathCommon(Marshaler& m, bool wide) {
    // DWORD SearchPathA/W(LPCTSTR lpPath, LPCTSTR lpFileName,
    //                     LPCTSTR lpExtension, DWORD nBufferLength,
    //                     LPTSTR lpBuffer, LPTSTR* lpFilePart)
    const GuestAddr path_ptr = m.arg_ptr(0);
    const GuestAddr filename_ptr = m.arg_ptr(1);
    const GuestAddr extension_ptr = m.arg_ptr(2);
    const u32 buffer_length = static_cast<u32>(m.arg(3));
    const GuestAddr buffer_ptr = m.arg_ptr(4);
    const GuestAddr file_part_ptr = m.arg_ptr(5);

    auto read_string = [&](GuestAddr ptr) -> std::optional<std::string> {
        if (ptr == 0) return std::nullopt;
        auto value = wide ? m.read_wide_string(ptr, 32768)
                          : m.read_ansi_string(ptr, 32768);
        if (value.is_err()) return std::nullopt;
        return std::move(value.value());
    };

    const auto filename_value = read_string(filename_ptr);
    if (!filename_value.has_value() || filename_value->empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        if (file_part_ptr != 0) (void)m.write_ptr(file_part_ptr, 0);
        m.set_return(0);
        return;
    }

    std::string filename = *filename_value;
    const auto extension = read_string(extension_ptr);
    const usize last_separator = filename.find_last_of("\\/");
    const usize last_dot = filename.find_last_of('.');
    const bool has_extension =
        last_dot != std::string::npos &&
        (last_separator == std::string::npos || last_dot > last_separator);
    if (!has_extension && extension.has_value() && !extension->empty()) {
        filename += *extension;
    }

    std::vector<std::string> search_directories;
    const auto explicit_path = read_string(path_ptr);
    if (path_ptr != 0) {
        usize begin = 0;
        do {
            const usize end = explicit_path.has_value()
                ? explicit_path->find(';', begin)
                : std::string::npos;
            std::string directory =
                explicit_path.has_value()
                    ? explicit_path->substr(
                          begin, end == std::string::npos
                                     ? std::string::npos
                                     : end - begin)
                    : std::string{};
            search_directories.push_back(
                directory.empty() ? m_current_directory : std::move(directory));
            if (end == std::string::npos) break;
            begin = end + 1;
        } while (true);
    } else {
        // The default (lpPath == NULL) path begins at the directory the
        // application was loaded from, before the current directory and the
        // process PATH. Installers rely on this to find helper DLLs that sit
        // beside the .exe but are on no other search list.
        if (m_module_manager != nullptr) {
            const std::string_view executable =
                m_module_manager->main_module_path();
            const usize separator = executable.find_last_of("\\/");
            if (separator != std::string_view::npos && separator > 0) {
                search_directories.emplace_back(executable.substr(0, separator));
            }
        }
        // Learn (SearchPath remarks): safe process search mode searches the
        // system directories before the current directory; without it the
        // current directory comes first. SetSearchPathMode overrides the
        // registry default SafeProcessSearchMode = 0
        // (setsearchpathmode_probe: xcopy.exe found in the current
        // directory unsafe, in System32 safe). PATH follows either way.
        const bool safe = m_search_path_mode.load() == 1u;
        if (!safe) search_directories.push_back(m_current_directory);
        search_directories.emplace_back("C:\\Windows\\System32");
        search_directories.emplace_back("C:\\Windows\\System");
        search_directories.emplace_back("C:\\Windows");
        if (safe) search_directories.push_back(m_current_directory);
        std::string path_value = "C:\\Windows\\System32;C:\\Windows";
        if (m_environment_manager != nullptr) {
            auto process_path = m_environment_manager->get_variable("PATH");
            if (process_path.is_ok()) path_value = process_path.value();
        }
        usize begin = 0;
        do {
            const usize end = path_value.find(';', begin);
            std::string directory =
                path_value.substr(
                    begin, end == std::string::npos
                               ? std::string::npos
                               : end - begin);
            search_directories.push_back(
                directory.empty() ? m_current_directory : std::move(directory));
            if (end == std::string::npos) break;
            begin = end + 1;
        } while (true);
    }

    std::optional<std::string> found_path;
    const bool filename_has_path =
        filename.find_first_of("\\/") != std::string::npos ||
        (filename.size() >= 2 && capsule::is_valid_drive_letter(filename[0]) &&
         filename[1] == ':');
    // Probing candidates records a miss in the last error for every
    // directory that lacks the file; a search that succeeds leaves the
    // caller's value untouched (measured on Windows 11:
    // last_error_on_success_probe.expected), so restore it below.
    const u32 entry_error = get_last_error();
    auto try_candidate = [&](std::string_view candidate) {
        if (found_path.has_value()) return;
        const std::string absolute =
            capsule::make_absolute_win_path(candidate, m_current_directory);
        if (absolute.empty()) return;
        if (m_file_search_manager != nullptr) {
            const u32 attrs =
                m_file_search_manager->get_file_attributes(absolute);
            if (attrs != INVALID_FILE_ATTRIBUTES) {
                found_path = absolute;
            }
            return;
        }
        auto info = m_vfs.stat(absolute);
        if (info.is_ok()) {
            found_path = absolute;
        }
    };

    if (filename_has_path) {
        try_candidate(filename);
    } else {
        for (const auto& directory : search_directories) {
            std::string candidate = directory;
            if (!candidate.empty() && candidate.back() != '\\' &&
                candidate.back() != '/') {
                candidate.push_back('\\');
            }
            candidate += filename;
            try_candidate(candidate);
        }
    }

    // Thunk-backed Windows DLLs have Windows-visible System32 paths even
    // though no host file exists. Preserve that observable filesystem
    // presence for already loaded modules after the real VFS search.
    if (!found_path.has_value() && !filename_has_path &&
        path_ptr == 0 && m_module_manager != nullptr) {
        const GuestAddr module = m_module_manager->get_module_handle(filename);
        if (module != 0) {
            std::array<char, 32768> module_path{};
            const u32 length = m_module_manager->get_module_filename(
                module, module_path.data(),
                static_cast<u32>(module_path.size()));
            if (length > 0 && length < module_path.size()) {
                found_path = std::string(module_path.data(), length);
            }
        }
    }

    if (!found_path.has_value()) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        if (file_part_ptr != 0) (void)m.write_ptr(file_part_ptr, 0);
        VITREO_TRACE(
            "[kernel32_ext] SearchPath%c path=\"%s\" file=\"%s\" -> not found\n",
            wide ? 'W' : 'A',
            explicit_path.has_value() ? explicit_path->c_str() : "(default)",
            filename.c_str());
        m.set_return(0);
        return;
    }

    set_last_error(entry_error);
    const u32 result_length = static_cast<u32>(found_path->size());
    const u32 required_length = result_length + 1;
    if (buffer_ptr == 0 || buffer_length < required_length) {
        if (file_part_ptr != 0) (void)m.write_ptr(file_part_ptr, 0);
        VITREO_TRACE(
            "[kernel32_ext] SearchPath%c file=\"%s\" -> \"%s\" required=%u\n",
            wide ? 'W' : 'A', filename.c_str(), found_path->c_str(),
            required_length);
        m.set_return(required_length);
        return;
    }

    const auto write_result =
        wide ? m.write_wide_string(buffer_ptr, *found_path)
             : m.write_ansi_string(buffer_ptr, *found_path);
    if (write_result.is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        if (file_part_ptr != 0) (void)m.write_ptr(file_part_ptr, 0);
        m.set_return(0);
        return;
    }

    if (file_part_ptr != 0) {
        const usize filename_offset = found_path->find_last_of("\\/");
        const u64 character_offset =
            filename_offset == std::string::npos ? 0 : filename_offset + 1;
        const u64 byte_offset = wide ? character_offset * 2 : character_offset;
        (void)m.write_ptr(file_part_ptr, buffer_ptr + byte_offset);
    }
    VITREO_TRACE(
        "[kernel32_ext] SearchPath%c file=\"%s\" -> \"%s\"\n",
        wide ? 'W' : 'A', filename.c_str(), found_path->c_str());
    m.set_return(result_length);
}


} // namespace vitreo::winapi
