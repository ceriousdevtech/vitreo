// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/win_path.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/filesystem/drive_type_contract.inc"
#include <algorithm>

namespace vitreo::winapi {

// Learn GetDriveType + drive_type{32,64}.expected: validate the actual
// directory before reporting a volume type; preserve last-error on misses.
inline u32 query_drive_type(Marshaler& m, capsule::Vfs& vfs,
                           const std::string& current_directory, bool wide) {
    namespace sdk = ::drive_type_sdk;
    std::string path;
    if (m.arg_ptr(0) == 0) {
        path = current_directory.size() >= 2 && current_directory[1] == ':'
            ? current_directory.substr(0, 2) + "\\" : "C:\\";
    } else if (wide) {
        auto read = m.read_wide_string(m.arg_ptr(0), 0);
        if (read.is_err()) return sdk::DRIVE_NO_ROOT_DIR;
        path = std::move(read.value());
    } else {
        auto read = m.read_ansi_string(m.arg_ptr(0), 0);
        if (read.is_err()) return sdk::DRIVE_NO_ROOT_DIR;
        if (!read.value().empty()) {
            const auto* bytes = reinterpret_cast<const u8*>(read.value().data());
            auto converted = codepage::multi_byte_to_wide(codepage::kCpAcp, 0,
                bytes, read.value().size(), nullptr, 0);
            if (!converted.ok()) return sdk::DRIVE_UNKNOWN;
            std::vector<u16> units(converted.count);
            converted = codepage::multi_byte_to_wide(codepage::kCpAcp, 0,
                bytes, read.value().size(), units.data(), units.size());
            if (!converted.ok()) return sdk::DRIVE_UNKNOWN;
            path = utf16_to_utf8(units.data(), static_cast<int>(units.size()));
        }
    }
    if (path.empty()) return sdk::DRIVE_NO_ROOT_DIR;
    std::replace(path.begin(), path.end(), '/', '\\');
    if (path.starts_with("\\\\?\\") || path.starts_with("\\\\.\\")) path.erase(0, 4);
    const bool drive_only = path.size() == 2 && path[1] == ':';
    if (drive_only) path += '\\';
    const bool trailing = path.back() == '\\';
    const std::string absolute = capsule::make_absolute_win_path(path, current_directory);
    if (absolute.empty()) return sdk::DRIVE_NO_ROOT_DIR;
    const bool root = absolute.size() == 3 && absolute[1] == ':';
    if (!root && !trailing) return sdk::DRIVE_NO_ROOT_DIR;
    const auto info = vfs.stat(absolute);
    if (info.is_err() ||
        (info.value().attributes & capsule::FileAttributes::Directory) == capsule::FileAttributes::None) {
        return sdk::DRIVE_NO_ROOT_DIR;
    }
    auto resolved = vfs.resolve_components(absolute.substr(0, 3));
    if (resolved.is_err()) return sdk::DRIVE_NO_ROOT_DIR;
    if (!root) set_last_error(sdk::ERROR_NOT_A_REPARSE_POINT);
    return resolved.value().first->is_optical() ? sdk::DRIVE_CDROM : sdk::DRIVE_FIXED;
}

} // namespace vitreo::winapi
