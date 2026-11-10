// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/win32_full_path.hpp"
#include "capsule/vfs/win32_namespace_path.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include <cstdio>
#include <cctype>
#include <vector>
#include <mutex>
namespace vitreo::winapi {
// Generated from SDK-compatible windows.h MAX_PATH; independently checked by
// Windows namespace-probe raw UTF16 boundary rows (259 succeeds, 260 fails).
inline constexpr usize kLegacyDiskPathUnits = 260;
inline u32 disk_space_unsupported() {
    static std::once_flag once;
    std::call_once(once, [] { std::fprintf(stderr,
        "[vitreo] UNIMPLEMENTED kernel32!GetDiskFreeSpaceA/W/ExA/ExW: volume capacity/geometry or UNC provider unavailable\n"); });
    return error_codes::ERROR_CALL_NOT_IMPLEMENTED;
}
inline bool read_disk_space_path(Marshaler& m, bool wide, std::string& path) {
    if (wide) {
        auto value = m.read_wide_string(m.arg_ptr(0), 0);
        if (value.is_err()) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); return false; }
        path = std::move(value.value()); return true;
    }
    auto value = m.read_ansi_string(m.arg_ptr(0), 0);
    if (value.is_err()) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); return false; }
    if (value.value().empty()) { path.clear(); return true; }
    const auto* bytes = reinterpret_cast<const u8*>(value.value().data());
    auto converted = codepage::multi_byte_to_wide(codepage::kCpAcp, 0, bytes, value.value().size(), nullptr, 0);
    if (!converted.ok()) { set_last_error(converted.error); return false; }
    std::vector<u16> units(converted.count);
    converted = codepage::multi_byte_to_wide(codepage::kCpAcp, 0, bytes, value.value().size(), units.data(), units.size());
    if (!converted.ok()) { set_last_error(converted.error); return false; }
    path = utf16_to_utf8(units.data(), static_cast<int>(units.size())); return true;
}
struct DiskDirectoryVolume {
    capsule::VolumeInfo volume;
    bool optical{};
    bool preserve_outputs{};
    u32 error{};
};
inline DiskDirectoryVolume query_disk_directory_volume(capsule::Vfs& vfs,
    const std::string& current_directory, const std::string* path_arg, bool extended) {

    DiskDirectoryVolume result;
    std::string path;
    if (!path_arg) {
        if (current_directory.size() >= 2 && current_directory[1] == ':')
            path = current_directory.substr(0, 2) + "\\";
    } else path = *path_arg;
    const auto reject = [&](u32 error, bool preserve = false) {
        result.error = error; result.preserve_outputs = preserve; return result;
    };
    if (path.empty()) return reject(error_codes::ERROR_PATH_NOT_FOUND, true);
    using NS = capsule::Win32NamespacePath;
    auto ns = capsule::win32_namespace_path(path);
    // Legacy input-length validation precedes lookup, and counts UTF-16 units,
    // not bytes in the host UTF-8 spelling (Windows namespace oracle).
    const auto units = utf8_to_utf16(path);
    if (!ns.literal() && units.size() >= kLegacyDiskPathUnits)
        return reject(error_codes::ERROR_PATH_NOT_FOUND, true);
    if (ns.prefixed_dos()) {
        if (ns.body.size() == 2) return reject(error_codes::ERROR_INVALID_FUNCTION, true);
        if (ns.body.size() < 3 || ns.body[2] != '\\')
            return reject(extended ? error_codes::ERROR_PATH_NOT_FOUND : error_codes::ERROR_FILE_NOT_FOUND);
    }
    if (ns.kind == NS::Kind::Network) {
        if (path.starts_with(R"(\\\)")) return reject(error_codes::ERROR_BAD_PATHNAME);
        if (ns.body.find('\\') == std::string_view::npos)
            return reject(error_codes::ERROR_BAD_PATHNAME);
        return reject(disk_space_unsupported());
    }
    if (ns.kind == NS::Kind::OtherDevice) {
        auto name = std::string(ns.body);
        for (auto& ch : name) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (name.empty() || name == "GLOBALROOT\\") return reject(error_codes::ERROR_INVALID_NAME);
        if (name == "UNC" || name == "NUL" || name.starts_with("PHYSICALDRIVE"))
            return reject(error_codes::ERROR_INVALID_FUNCTION, true);
        if (name.starts_with("VOLUME{")) {
            // A mounted volume-name provider is distinct from a DOS drive.
            // Reject malformed syntax; never alias a valid GUID to drive C.
            const auto close = name.find('}');
            const std::string_view shape = "00000000-0000-0000-0000-000000000000";
            bool valid = close != std::string::npos && close == 7 + shape.size() &&
                close + 1 < name.size() && name[close+1] == '\\';
            for (usize i = 0; valid && i < shape.size(); ++i)
                valid = shape[i] == '-' ? name[7+i] == '-' : std::isxdigit(static_cast<unsigned char>(name[7+i])) != 0;
            return reject(valid ? disk_space_unsupported() : error_codes::ERROR_PATH_NOT_FOUND);
        }
        if (name.starts_with("GLOBALROOT\\")) return reject(disk_space_unsupported());
        return reject(extended ? error_codes::ERROR_PATH_NOT_FOUND :
            name.find('\\') == std::string::npos ? error_codes::ERROR_FILE_NOT_FOUND : error_codes::ERROR_PATH_NOT_FOUND);
    }
    if (!ns.prefixed_dos()) {
        const auto full = capsule::win32_full_path(path, current_directory);
        if (!full) return reject(error_codes::ERROR_INVALID_NAME);
        path = full->path;
        if (capsule::win32_namespace_path(path).kind == NS::Kind::OtherDevice)
            return reject(error_codes::ERROR_INVALID_FUNCTION, true);
        // NUL components remain DOS devices in ordinary paths; verbatim names
        // are filesystem entries. CON within a directory is not NUL's alias.
        std::string upper(path);
        for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        for (usize at = 0; at < upper.size();) {
            const auto end = upper.find('\\', at);
            if (std::string_view(upper).substr(at, end == std::string::npos ? end : end-at) == "NUL")
                return reject(error_codes::ERROR_INVALID_FUNCTION, true);
            if (end == std::string::npos) break;
            at = end + 1;
        }
    }

    const auto resolved = vfs.resolve_components(path);
    if (resolved.is_err()) {
        const auto error = resolved.error();
        if (error == capsule::VfsError::NotSupported) return reject(disk_space_unsupported());
        return reject(error == capsule::VfsError::DriveNotMounted ? error_codes::ERROR_PATH_NOT_FOUND : vfs_error_to_win32(error));
    }
    const auto info = vfs.stat(path);
    if (info.is_err()) {
        result.error = extended && (info.error() == capsule::VfsError::FileNotFound ||
                                    info.error() == capsule::VfsError::PathNotFound)
            ? error_codes::ERROR_PATH_NOT_FOUND : vfs_error_to_win32(info.error());

        return result;
    }
    if (!capsule::has_attribute(info.value().attributes, capsule::FileAttributes::Directory)) {
        result.error = error_codes::ERROR_DIRECTORY;
         return result;
    }
    result.optical = resolved.value().first->is_optical();
    const bool provider_ok = resolved.value().first->volume_info(result.volume);
    if (!provider_ok) result.error = disk_space_unsupported();

    return result;
}
} // namespace vitreo::winapi
