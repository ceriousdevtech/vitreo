// SPDX-License-Identifier: LGPL-2.1-only

/// @file path_volume_ext.cpp
/// @brief Wave 3 Group C: file/volume/path stubs.
///
/// Path and volume helpers from the Wave 3 banner.
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
#include "capsule/vfs/win32_full_path.hpp"
#include "capsule/vfs/path_encoding.hpp"
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

// ---- Group C: File/Volume/Path ----
void Kernel32ExtManager::impl_FindFirstFileExA(Marshaler& m) {
    // FindFirstFileExW's contract (see impl_FindFirstFileExW) on a CP_ACP
    // pattern, filling WIN32_FIND_DATAA; INVALID_HANDLE_VALUE is pointer-wide.
    const u64 invalid = m.cpu_is_64bit() ? ~0ull : 0xFFFFFFFFull;
    const GuestAddr filename = m.arg_ptr(0);
    const u32 info_level = static_cast<u32>(m.arg(1));
    const GuestAddr data = m.arg_ptr(2);
    const u32 search_op = static_cast<u32>(m.arg(3));
    (void)m.arg_ptr(4);
    const u32 flags = m.arg32(5);
    if (info_level > 1 || search_op > 2 || (flags & ~0x7u) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(invalid); return;
    }
    if (search_op == 2) {
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(invalid); return;
    }
    if (!m_file_search_manager) {
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        m.set_return(invalid); return;
    }
    auto path = m.read_ansi_string(filename);
    if (path.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(invalid); return;
    }
    FindData fd{};
    u64 h = m_file_search_manager->find_first_file(capsule::ansi_to_utf8(path.value()), fd);
    if (h == 0) { m.set_return(invalid); return; }
    write_find_data_a(m, data, fd);
    m.set_return(h);
}
void Kernel32ExtManager::impl_FindFirstVolumeW(Marshaler& m) {
    GuestAddr buf = m.arg_ptr(0);
    u32 len = static_cast<u32>(m.arg(1));
    const char* name = "\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\";
    if (buf && len) {
        u32 i = 0;
        for (; name[i] && i + 1 < len; ++i)
            (void)m.write_u16(buf + i * 2, static_cast<u16>(static_cast<unsigned char>(name[i])));
        (void)m.write_u16(buf + i * 2, 0);
    }
    m.set_return(0x56010000u);
}
void Kernel32ExtManager::impl_FindNextVolumeW(Marshaler& m) {
    (void)m.arg(0); (void)m.arg_ptr(1); (void)m.arg(2);
    set_last_error(error_codes::ERROR_NO_MORE_FILES);
    m.set_return(0);
}
void Kernel32ExtManager::impl_FindVolumeClose(Marshaler& m) {
    (void)m.arg(0); m.set_return(1);
}
namespace {

bool drive_is_optical(capsule::Vfs& vfs, char letter) {
    auto resolved = vfs.resolve_components(std::string(1, letter) + ":\\");
    return resolved.is_ok() && resolved.value().first->is_optical();
}

}  // namespace

/// The NT device an MS-DOS drive name links to, or nullopt for a letter
/// with no drive. On Windows (query_dos_device_probe) C: is
/// \Device\HarddiskVolume3 and the CD-ROM drives are \Device\CdRom0 and
/// \Device\CdRom1 in letter order; Vitreo lays its drives out the same
/// way: C: is HarddiskVolume3, the other fixed drives follow it in letter
/// order, and optical drives are CdRom0, CdRom1, ... in letter order.
std::optional<std::string> nt_device_for_drive(capsule::Vfs& vfs, char letter) {
    letter = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
    if (letter < 'A' || letter > 'Z' || !vfs.is_drive_mounted(letter)) {
        return std::nullopt;
    }
    std::vector<char> drives = vfs.mounted_drives();
    std::sort(drives.begin(), drives.end());
    const bool optical = drive_is_optical(vfs, letter);
    if (!optical && letter == 'C') return std::string("\\Device\\HarddiskVolume3");
    u32 before = 0;
    for (char drive : drives) {
        if (drive >= letter) break;
        if (drive_is_optical(vfs, drive) == optical && (optical || drive != 'C')) {
            ++before;
        }
    }
    return optical ? "\\Device\\CdRom" + std::to_string(before)
                   : "\\Device\\HarddiskVolume" + std::to_string(4 + before);
}

std::optional<std::string> win32_to_nt_device_path(capsule::Vfs& vfs,
                                                   std::string_view path) {
    if (path.size() < 2 || path[1] != ':') return std::nullopt;
    auto device = nt_device_for_drive(vfs, path[0]);
    if (!device) return std::nullopt;
    std::string out = *device;
    for (char c : path.substr(2)) out.push_back(c == '/' ? '\\' : c);
    return out;
}

namespace {

/// QueryDosDevice's answer: the characters it stores (each target string
/// and the list's final terminator) or the Win32 error.
struct DosDeviceAnswer {
    std::string chars;
    u32 error = 0;
};

/// @p name nullopt is the NULL lpDeviceName: every MS-DOS device name.
/// Vitreo's are its drive letters and NUL (Windows lists more). Every rule
/// is query_dos_device_probe.expected (x86 == x64).
DosDeviceAnswer query_dos_device(capsule::Vfs& vfs,
                                 const std::optional<std::string>& name) {
    std::vector<char> drives = vfs.mounted_drives();
    std::sort(drives.begin(), drives.end());
    const auto target_of = [&](std::string_view device) -> std::optional<std::string> {
        std::string upper(device);
        for (char& c : upper) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        if (upper == "NUL") return std::string("\\Device\\Null");
        if (upper.size() == 2 && upper[1] == ':') {
            return nt_device_for_drive(vfs, upper[0]);
        }
        return std::nullopt;
    };
    DosDeviceAnswer answer;
    if (!name.has_value()) {
        for (char drive : drives) {
            answer.chars += std::string(1, drive) + ":";
            answer.chars.push_back('\0');
        }
        answer.chars += "NUL";
        answer.chars.push_back('\0');
        answer.chars.push_back('\0');
        return answer;
    }
    // The name is looked up in the object directory: an empty name, or a
    // path beneath a device's link, is ERROR_INVALID_HANDLE ("empty",
    // "C:\\", "NUL\\"); beneath a name that is not there it is
    // ERROR_PATH_NOT_FOUND ("unmapped Q:\\"); a missing name is
    // ERROR_FILE_NOT_FOUND ("C", "unmapped Q:").
    if (name->empty()) {
        answer.error = error_codes::ERROR_INVALID_HANDLE;
        return answer;
    }
    const usize separator = name->find('\\');
    if (separator != std::string::npos) {
        answer.error = target_of(std::string_view(*name).substr(0, separator))
                           ? error_codes::ERROR_INVALID_HANDLE
                           : error_codes::ERROR_PATH_NOT_FOUND;
        return answer;
    }
    const auto target = target_of(*name);
    if (!target.has_value()) {
        answer.error = error_codes::ERROR_FILE_NOT_FOUND;
        return answer;
    }
    answer.chars = *target;
    answer.chars.push_back('\0');
    answer.chars.push_back('\0');
    return answer;
}

// GetFinalPathNameByHandle dwFlags (Learn). FILE_NAME_OPENED (0x8) and every
// bit above the volume field are accepted and change nothing: on NTFS both
// name forms report the stored spelling (final_path_probe file_lc, badflags
// 0x10/0x100/0x80000000).
constexpr u32 kVolumeNameDos = 0x0;
constexpr u32 kVolumeNameGuid = 0x1;
constexpr u32 kVolumeNameNt = 0x2;
constexpr u32 kVolumeNameNone = 0x4;
constexpr u32 kVolumeNameMask = 0x7;

/// The final path of `handle` in the form `flags` selects, as UTF-8, or
/// nullopt with the last error set. Shared by the W and A exports; every rule
/// is from tools/oracle_probe/final_path_probe.expected (x86 == x64).
std::optional<std::string> final_path_by_handle(FileManager* files, capsule::Vfs& vfs,
                                                Handle handle, u32 flags) {
    // The volume field is validated before the handle is looked at
    // (bogus_badflags / event_badflags / nul_badflags all give 87).
    const u32 volume = flags & kVolumeNameMask;
    if (volume != kVolumeNameDos && volume != kVolumeNameGuid &&
        volume != kVolumeNameNt && volume != kVolumeNameNone) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return std::nullopt;
    }
    if (files == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return std::nullopt;
    }
    switch (files->handle_table().get_type(handle)) {
        case HandleType::File:
            break;
        case HandleType::Pipe:
            // NPFS objects cannot be named this way: a named pipe (either end)
            // fails ERROR_INVALID_FUNCTION, an anonymous one
            // ERROR_BAD_PATHNAME (named_pipe_server / _client, anon_pipe).
            set_last_error(files->named_pipe_file_name(handle).has_value()
                               ? error_codes::ERROR_INVALID_FUNCTION
                               : error_codes::ERROR_BAD_PATHNAME);
            return std::nullopt;
        case HandleType::Mailslot:
        case HandleType::ConsoleInput:
        case HandleType::ConsoleOutput:
        case HandleType::Device:
            // mailslot, conout, volume (\\.\C:).
            set_last_error(error_codes::ERROR_INVALID_FUNCTION);
            return std::nullopt;
        default:
            // NULL, INVALID_HANDLE_VALUE, closed, and non-file objects.
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return std::nullopt;
    }
    const capsule::FileHandle vfs_handle = files->vfs_handle_for(handle);
    if (vfs_handle == capsule::INVALID_FILE_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return std::nullopt;
    }
    if (files->is_character_file(handle)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);  // nul_device
        return std::nullopt;
    }
    if (volume == kVolumeNameGuid) {
        // The GUID form names the volume by its \\?\Volume{GUID}, and
        // GetVolumeNameForVolumeMountPointW has no GUIDs for it to agree
        // with. Learn's failure for a volume without that name is
        // ERROR_PATH_NOT_FOUND.
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::fprintf(stderr,
                "[vitreo] UNIMPLEMENTED kernel32!GetFinalPathNameByHandle: "
                "VOLUME_NAME_GUID (no volume GUIDs)\n");
        }
        set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        return std::nullopt;
    }
    auto guest = vfs.final_path_for_handle(vfs_handle);
    if (guest.is_err()) {
        // PathNotFound: the object resolved outside every drive's root, the
        // documented "no drive letter" failure.
        set_last_error(vfs_error_to_win32(guest.error()));
        return std::nullopt;
    }
    const std::string& path = guest.value();  // "C:\\dir\\name", root "C:\\"
    if (volume == kVolumeNameNt) {
        // The drive's NT device, the one QueryDosDevice reports, then the
        // path; the last error is left alone (file / dir / root flags=2).
        const auto device = nt_device_for_drive(vfs, path[0]);
        if (!device.has_value()) {
            set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
            return std::nullopt;
        }
        return *device + path.substr(2);
    }
    if (volume == kVolumeNameDos) {
        // The DOS form clears the last error on its way to an answer, even
        // when the buffer then proves too small (file / sizes_dos rows);
        // VOLUME_NAME_NONE leaves it alone.
        set_last_error(0);
        return "\\\\?\\" + path;
    }
    return path.substr(2);  // VOLUME_NAME_NONE: "\\dir\\name", root "\\"
}

}  // namespace

void Kernel32ExtManager::impl_GetFinalPathNameByHandleW(Marshaler& m) {
    // DWORD GetFinalPathNameByHandleW(HANDLE hFile, LPWSTR lpszFilePath,
    //                                 DWORD cchFilePath, DWORD dwFlags)
    const Handle handle = static_cast<Handle>(m.arg(0));
    const GuestAddr buffer = m.arg_ptr(1);
    const u32 cch = m.arg32(2);
    const u32 flags = m.arg32(3);
    auto path = final_path_by_handle(m_file_manager, m_vfs, handle, flags);
    if (!path.has_value()) {
        m.set_return(0u);
        return;
    }
    std::vector<u16> wide = utf8_to_utf16(*path);
    const u32 length = static_cast<u32>(wide.size());
    // Learn: success returns the length without the NUL; a buffer that cannot
    // hold the string plus its NUL returns the size that can, and is left
    // untouched (sizes_dos cch 0..len, null_buf_cch0: ERROR_NOT_ENOUGH_MEMORY).
    if (cch <= length) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        m.set_return(length + 1);
        return;
    }
    wide.push_back(0);
    if (m_memory.write_bytes(buffer, reinterpret_cast<const Byte*>(wide.data()),
                             wide.size() * sizeof(u16)).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0u);
        return;
    }
    VITREO_TRACE("[file] GetFinalPathNameByHandleW(0x%x, flags=0x%x) -> \"%s\"\n",
                 static_cast<unsigned>(handle), flags, path->c_str());
    m.set_return(length);
}

void Kernel32ExtManager::impl_GetFinalPathNameByHandleA(Marshaler& m) {
    // DWORD GetFinalPathNameByHandleA(HANDLE hFile, LPSTR lpszFilePath,
    //                                 DWORD cchFilePath, DWORD dwFlags)
    const Handle handle = static_cast<Handle>(m.arg(0));
    const GuestAddr buffer = m.arg_ptr(1);
    const u32 cch = m.arg32(2);
    const u32 flags = m.arg32(3);
    auto path = final_path_by_handle(m_file_manager, m_vfs, handle, flags);
    if (!path.has_value()) {
        m.set_return(0u);
        return;
    }
    // CP_ACP, unmappable characters as '?' (A_cjk).
    std::string ansi = utf8_to_acp(*path);
    const u32 length = static_cast<u32>(ansi.size());
    // The ANSI export differs from the W one when the buffer is short: it
    // returns the length WITHOUT the NUL, writes nothing and sets no error of
    // its own (A_file_null_cch0 / A_file_exact / A_file_short). Callers that
    // test `ret < cch` for success still see a failure.
    if (cch <= length) {
        m.set_return(length);
        return;
    }
    ansi.push_back('\0');
    if (m_memory.write_bytes(buffer, reinterpret_cast<const Byte*>(ansi.data()),
                             ansi.size()).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0u);
        return;
    }
    m.set_return(length);
}
void Kernel32ExtManager::impl_GetFullPathNameA(Marshaler& m) {
    // GetFullPathNameW's contract on CP_ACP strings; sizes are in chars.
    const GuestAddr filename = m.arg_ptr(0);
    const u32 buflen = static_cast<u32>(m.arg(1));
    const GuestAddr buf = m.arg_ptr(2);
    const GuestAddr file_part = m.arg_ptr(3);
    auto path = m.read_ansi_string(filename);
    if (path.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    auto full = capsule::win32_full_path(capsule::ansi_to_utf8(path.value()),
                                         m_current_directory);
    if (!full) {
        set_last_error(error_codes::ERROR_INVALID_NAME);
        m.set_return(0);
        return;
    }
    const std::string ansi = utf8_to_acp(full->path);
    const u32 need = static_cast<u32>(ansi.size());
    if (buf == 0 || buflen <= need) {
        if (file_part != 0) (void)m.write_ptr(file_part, 0);
        m.set_return(need + 1);
        return;
    }
    for (u32 i = 0; i < need; ++i) (void)m.write_u8(buf + i, static_cast<u8>(ansi[i]));
    (void)m.write_u8(buf + need, 0);
    if (file_part != 0) {
        if (full->file_part == std::string::npos) {
            (void)m.write_ptr(file_part, 0);
        } else {
            const u64 off = utf8_to_acp(
                std::string_view(full->path).substr(0, full->file_part)).size();
            (void)m.write_ptr(file_part, buf + off);
        }
    }
    m.set_return(need);
}
void Kernel32ExtManager::impl_GetTempFileNameA(Marshaler& m) {
    // UINT GetTempFileNameA(LPCSTR, LPCSTR, UINT, LPSTR): the W contract
    // (temp_file_name) through CP_ACP. Unlike W it writes the buffer only on
    // success (gettempfilename_probe: "A path=missing_u0 ... out=untouched").
    const GuestAddr path_p = m.arg_ptr(0);
    const GuestAddr prefix_p = m.arg_ptr(1);
    const u32 unique = m.arg32(2);
    const GuestAddr buf = m.arg_ptr(3);
    auto read_acp = [&](GuestAddr addr, std::u16string& out) {
        out.clear();
        if (addr == 0) return true;
        auto bytes = m.read_ansi_string(addr);
        if (bytes.is_err()) return false;
        for (u16 unit : utf8_to_utf16(capsule::ansi_to_utf8(bytes.value()))) {
            out.push_back(static_cast<char16_t>(unit));
        }
        return true;
    };
    std::u16string path, prefix;
    if (!read_acp(path_p, path)) {
        (void)m.raise_access_violation(false, path_p);
        m.set_return(0u);
        return;
    }
    if (!read_acp(prefix_p, prefix)) {
        (void)m.raise_access_violation(false, prefix_p);
        m.set_return(0u);
        return;
    }
    const TempFileNameResult result = temp_file_name(std::move(path), prefix, unique);
    if (result.value == 0) {
        m.set_return(0u);
        return;
    }
    const std::string ansi = utf8_to_acp(utf16_to_utf8(
        reinterpret_cast<const u16*>(result.buffer.data()),
        static_cast<int>(result.buffer.size())));
    for (usize i = 0; i <= ansi.size(); ++i) {
        const u8 byte = i < ansi.size() ? static_cast<u8>(ansi[i]) : 0;
        if (m.write_u8(buf + i, byte).is_err()) {
            (void)m.raise_access_violation(true, buf + i);
            m.set_return(0u);
            return;
        }
    }
    m.set_return(result.value);
}
void Kernel32ExtManager::impl_QueryDosDeviceW(Marshaler& m) {
    // DWORD QueryDosDeviceW(LPCWSTR lpDeviceName, LPWSTR lpTargetPath,
    //                       DWORD ucchMax)
    query_dos_device_export(m, /*wide=*/true);
}

void Kernel32ExtManager::impl_QueryDosDeviceA(Marshaler& m) {
    // DWORD QueryDosDeviceA(LPCSTR, LPSTR, DWORD): the same answer in bytes.
    query_dos_device_export(m, /*wide=*/false);
}

void Kernel32ExtManager::query_dos_device_export(Marshaler& m, bool wide) {
    const GuestAddr name_ptr = m.arg_ptr(0);
    const GuestAddr target = m.arg_ptr(1);
    const u32 max = m.arg32(2);
    std::optional<std::string> name;
    if (name_ptr != 0) {
        auto read = wide ? m.read_wide_string(name_ptr)
                         : m.read_ansi_string(name_ptr);
        name = read.is_ok() ? read.value() : std::string();
    }
    const DosDeviceAnswer answer = query_dos_device(m_vfs, name);
    if (answer.error != 0) {
        set_last_error(answer.error);
        m.set_return(0u);
        return;
    }
    // The names are ASCII. Success returns the characters stored and leaves
    // the last error alone; a buffer one character short still receives the
    // string before the call fails ("C: one short"), a shorter one nothing.
    const u32 count = static_cast<u32>(answer.chars.size());
    const u32 stored = max >= count ? count : (max + 1 == count ? max : 0);
    for (u32 i = 0; i < stored; ++i) {
        const u8 c = static_cast<u8>(answer.chars[i]);
        (void)(wide ? m.write_u16(target + i * 2, c) : m.write_u8(target + i, c));
    }
    if (stored < count) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0u);
        return;
    }
    m.set_return(count);
}
void Kernel32ExtManager::impl_LockFileEx(Marshaler& m) {
    // BOOL LockFileEx(HANDLE, DWORD dwFlags, DWORD dwReserved,
    //                 DWORD nNumberOfBytesToLockLow, DWORD nNumberOfBytesToLockHigh,
    //                 LPOVERLAPPED)
    // Learn: LOCKFILE_EXCLUSIVE_LOCK vs shared; LOCKFILE_FAIL_IMMEDIATELY
    // (we never wait). Offset comes from OVERLAPPED (files_core_probe
    // LockFileEx_shared / _excl_other).
    constexpr u32 kExclusive = 0x00000002;  // LOCKFILE_EXCLUSIVE_LOCK
    const Handle handle = static_cast<Handle>(m.arg(0));
    const u32 flags = static_cast<u32>(m.arg(1));
    (void)m.arg(2);  // reserved
    const u64 length = static_cast<u64>(m.arg32(3)) |
                       (static_cast<u64>(m.arg32(4)) << 32);
    const GuestAddr ov = m.arg_ptr(5);
    if (ov == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const GuestAddr off_field = ov + (m.cpu_is_64bit() ? 16u : 8u);
    const u32 off_lo = m.read_u32(off_field).value_or(0);
    const u32 off_hi = m.read_u32(off_field + 4).value_or(0);
    const u64 offset = static_cast<u64>(off_lo) | (static_cast<u64>(off_hi) << 32);
    if (handle == 0 || m_file_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    const capsule::FileHandle vfs = m_file_manager->vfs_handle_for(handle);
    if (vfs == capsule::INVALID_FILE_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    auto r = m_vfs.lock_range(vfs, offset, length, (flags & kExclusive) != 0);
    if (r.is_err()) {
        set_last_error(vfs_error_to_win32(r.error()));
        m.set_return(0);
        return;
    }
    m.set_return(1);
}
void Kernel32ExtManager::impl_UnlockFileEx(Marshaler& m) {
    // BOOL UnlockFileEx(HANDLE, DWORD dwReserved,
    //                   DWORD nNumberOfBytesToUnlockLow,
    //                   DWORD nNumberOfBytesToUnlockHigh, LPOVERLAPPED)
    const Handle handle = static_cast<Handle>(m.arg(0));
    (void)m.arg(1);
    const u64 length = static_cast<u64>(m.arg32(2)) |
                       (static_cast<u64>(m.arg32(3)) << 32);
    const GuestAddr ov = m.arg_ptr(4);
    if (ov == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const GuestAddr off_field = ov + (m.cpu_is_64bit() ? 16u : 8u);
    const u32 off_lo = m.read_u32(off_field).value_or(0);
    const u32 off_hi = m.read_u32(off_field + 4).value_or(0);
    const u64 offset = static_cast<u64>(off_lo) | (static_cast<u64>(off_hi) << 32);
    if (handle == 0 || m_file_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    const capsule::FileHandle vfs = m_file_manager->vfs_handle_for(handle);
    if (vfs == capsule::INVALID_FILE_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    auto r = m_vfs.unlock_range(vfs, offset, length);
    if (r.is_err()) {
        set_last_error(vfs_error_to_win32(r.error()));
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

} // namespace vitreo::winapi
