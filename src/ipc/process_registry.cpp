// SPDX-License-Identifier: LGPL-2.1-only
/// @file process_registry.cpp

#include "ipc/process_registry.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

#include <csignal>
#include <unistd.h>

namespace vitreo::ipc {

namespace fs = std::filesystem;

namespace {

constexpr u32 kRegistryMagic = 0x56545052; // "VTPR"
constexpr u32 kRegistryVersion = 3;
constexpr usize kMaxImageName = 260;
constexpr usize kMaxImagePath = 32768;

struct RegistryRecord {
    u32 magic;
    u32 version;
    u32 pid;
    u32 parent_pid;
    u32 image_name_size;
    u32 image_path_size;
};

std::string registry_directory(const std::string& capsule_root) {
    return capsule_root + "/runtime/processes";
}

bool process_is_live(pid_t pid) {
    if (pid <= 0) return false;
    if (::kill(pid, 0) == 0) return true;
    return errno == EPERM;
}

} // namespace

ProcessRegistration::ProcessRegistration(const std::string& capsule_root,
                                         const std::string& image_name,
                                         const std::string& image_path,
                                         u32 primary_thread_id) {
    if (capsule_root.empty() || image_name.empty()) return;

    const std::string directory = registry_directory(capsule_root);
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) {
        VITREO_TRACE("[process_registry] create failed: path=%s error=%s\n",
                     directory.c_str(), ec.message().c_str());
        return;
    }

    const u32 pid = static_cast<u32>(::getpid());
    const u32 parent_pid = static_cast<u32>(::getppid());
    const std::string name = image_name.substr(0, kMaxImageName - 1);
    const std::string path = (image_path.empty() ? image_name : image_path)
                                 .substr(0, kMaxImagePath - 1);
    const std::string final_path = directory + "/" + std::to_string(pid);
    const std::string temp_path = final_path + ".tmp";

    RegistryRecord record{
        kRegistryMagic,
        kRegistryVersion,
        pid,
        parent_pid,
        static_cast<u32>(name.size()),
        static_cast<u32>(path.size()),
    };

    {
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(reinterpret_cast<const char*>(&record), sizeof(record));
        out.write(reinterpret_cast<const char*>(&primary_thread_id), sizeof(primary_thread_id));
        out.write(name.data(), static_cast<std::streamsize>(name.size()));
        out.write(path.data(), static_cast<std::streamsize>(path.size()));
        out.close();
        if (!out) {
            fs::remove(temp_path, ec);
            return;
        }
    }

    fs::rename(temp_path, final_path, ec);
    if (ec) {
        fs::remove(temp_path, ec);
        return;
    }

    m_path = final_path;
    VITREO_TRACE("[process_registry] registered pid=%u parent=%u image='%s' path='%s'\n",
                 pid, parent_pid, name.c_str(), path.c_str());
}

ProcessRegistration::~ProcessRegistration() {
    if (m_path.empty()) return;
    std::error_code ec;
    fs::remove(m_path, ec);
}

ProcessRegistration::ProcessRegistration(ProcessRegistration&& other) noexcept
    : m_path(std::move(other.m_path)) {
    other.m_path.clear();
}

ProcessRegistration& ProcessRegistration::operator=(
    ProcessRegistration&& other) noexcept {
    if (this == &other) return *this;
    if (!m_path.empty()) {
        std::error_code ec;
        fs::remove(m_path, ec);
    }
    m_path = std::move(other.m_path);
    other.m_path.clear();
    return *this;
}

std::vector<ProcessSnapshotEntry> list_processes(
    const std::string& capsule_root) {
    std::vector<ProcessSnapshotEntry> result;
    const std::string directory = registry_directory(capsule_root);
    std::error_code ec;
    fs::directory_iterator it(directory, ec);
    if (ec) return result;

    for (const auto& item : it) {
        if (!item.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }

        RegistryRecord record{};
        u32 primary_thread_id = 0;
        std::string image_name;
        std::string image_path;
        std::ifstream in(item.path(), std::ios::binary);
        if (in) {
            in.read(reinterpret_cast<char*>(&record), sizeof(record));
            if (in && record.magic == kRegistryMagic &&
                (record.version == kRegistryVersion || record.version == 2) && record.pid != 0 &&
                record.image_name_size < kMaxImageName &&
                record.image_path_size < kMaxImagePath) {
                if (record.version == kRegistryVersion) {
                    in.read(reinterpret_cast<char*>(&primary_thread_id), sizeof(primary_thread_id));
                }
                image_name.resize(record.image_name_size);
                in.read(image_name.data(),
                        static_cast<std::streamsize>(image_name.size()));
                image_path.resize(record.image_path_size);
                in.read(image_path.data(),
                        static_cast<std::streamsize>(image_path.size()));
            }
        }

        if (!in || image_name.empty() || image_path.empty() ||
            !process_is_live(static_cast<pid_t>(record.pid))) {
            fs::remove(item.path(), ec);
            ec.clear();
            continue;
        }

        result.push_back(ProcessSnapshotEntry{
            record.pid,
            record.parent_pid,
            std::move(image_name),
            std::move(image_path),
            primary_thread_id,
        });
    }

    std::sort(result.begin(), result.end(),
              [](const ProcessSnapshotEntry& lhs,
                 const ProcessSnapshotEntry& rhs) { return lhs.pid < rhs.pid; });
    return result;
}

std::optional<u32> primary_thread_owner(u32 thread_id) {
    const char* scope = std::getenv("VITREO_IPC_SCOPE");
    if (thread_id == 0 || scope == nullptr || *scope == '\0') return std::nullopt;
    for (const auto& process : list_processes(scope)) {
        if (process.primary_thread_id == thread_id) return process.pid;
    }
    return std::nullopt;
}

} // namespace vitreo::ipc
