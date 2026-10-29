// SPDX-License-Identifier: LGPL-2.1-only
/// @file scm_status_file.cpp
/// @brief ScmManager status-file persistence (the capsule-local service status
///        record).

#include "winapi/advapi32/services/scm_internal.hpp"

#include <filesystem>
#include <fstream>

namespace vitreo::winapi {

using namespace scm_detail;

namespace {

constexpr u32 kStatusMagic = 0x56545356;  // "VTSV"
constexpr u32 kStatusVersion = 1;

/// On-disk service status record. Fixed layout, written whole.
struct StatusRecord {
    u32 magic;
    u32 version;
    u32 pid;
    u32 service_type;
    u32 current_state;
    u32 controls_accepted;
    u32 win32_exit_code;
    u32 service_specific_exit_code;
    u32 check_point;
    u32 wait_hint;
};

} // namespace

// ---- Status file ------------------------------------------------------------

bool ScmManager::write_status_file(const std::wstring& name,
                                   const ServiceStatus& status) const {
    const std::string path = status_file_path(name);
    if (path.empty()) return false;

    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec) {
        VITREO_TRACE("[scm] status dir create failed: %s\n", ec.message().c_str());
        return false;
    }

    StatusRecord record{};
    record.magic = kStatusMagic;
    record.version = kStatusVersion;
    record.pid = status.process_id;
    record.service_type = status.service_type;
    record.current_state = status.current_state;
    record.controls_accepted = status.controls_accepted;
    record.win32_exit_code = status.win32_exit_code;
    record.service_specific_exit_code = status.service_specific_exit_code;
    record.check_point = status.check_point;
    record.wait_hint = status.wait_hint;

    // Write to a temp file and rename, so a concurrent reader never sees a
    // half-written record.
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(&record), sizeof(record));
        if (!out) return false;
    }
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

std::optional<ServiceStatus> ScmManager::read_status_file(
    const std::wstring& name) const {
    const std::string path = status_file_path(name);
    if (path.empty()) return std::nullopt;

    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;

    StatusRecord record{};
    in.read(reinterpret_cast<char*>(&record), sizeof(record));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(record))) {
        return std::nullopt;
    }
    if (record.magic != kStatusMagic || record.version != kStatusVersion) {
        return std::nullopt;
    }

    ServiceStatus status;
    status.process_id = record.pid;
    status.service_type = record.service_type;
    status.current_state = record.current_state;
    status.controls_accepted = record.controls_accepted;
    status.win32_exit_code = record.win32_exit_code;
    status.service_specific_exit_code = record.service_specific_exit_code;
    status.check_point = record.check_point;
    status.wait_hint = record.wait_hint;
    return status;
}

void ScmManager::remove_status_file(const std::wstring& name) const {
    const std::string path = status_file_path(name);
    if (path.empty()) return;
    std::error_code ec;
    fs::remove(path, ec);
    const std::string handshake = dispatcher_handshake_path(path);
    if (!handshake.empty()) fs::remove(handshake, ec);
}

} // namespace vitreo::winapi
