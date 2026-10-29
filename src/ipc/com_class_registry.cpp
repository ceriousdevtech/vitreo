// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_class_registry.cpp

#include "ipc/com_class_registry.hpp"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <utility>
#include <vector>

#include <csignal>
#include <unistd.h>

namespace vitreo::ipc {

namespace fs = std::filesystem;

namespace {

constexpr u32 kRegistryMagic = 0x5654434f; // "VTCO"
constexpr u32 kRegistryVersion = 2;
constexpr u32 kRotMagic = 0x5654524f; // "VTRO"
constexpr u32 kRotVersion = 1;

struct RegistryRecord {
    u32 magic;
    u32 version;
    u32 owner_pid;
    u32 cookie;
    u64 class_object;
    u32 context;
    u32 flags;
    u32 suspended;
    u32 pointer_size;
};

static_assert(sizeof(RegistryRecord) == 40);

struct RotRecord {
    u32 magic;
    u32 version;
    u32 owner_pid;
    u32 cookie;
    u64 object;
    u32 pointer_size;
    u32 moniker_size;
};

static_assert(sizeof(RotRecord) == 32);

std::string class_id_text(const ComClassId& clsid) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(clsid.size() * 2);
    for (const u8 byte : clsid) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

std::string class_directory(const std::string& capsule_root,
                            const ComClassId& clsid) {
    return capsule_root + "/runtime/com/classes/" + class_id_text(clsid);
}

u64 rot_moniker_hash(std::string_view moniker) {
    u64 hash = 14695981039346656037ull;
    for (const char character : moniker) {
        const auto byte = static_cast<unsigned char>(character);
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string rot_directory(const std::string& capsule_root,
                          std::string_view moniker) {
    char hash[17]{};
    (void)std::snprintf(hash, sizeof(hash), "%016llx",
                        static_cast<unsigned long long>(
                            rot_moniker_hash(moniker)));
    return capsule_root + "/runtime/com/rot/" + hash;
}

bool process_is_live(pid_t pid) {
    if (pid <= 0) return false;
    if (::kill(pid, 0) == 0) return true;
    return errno == EPERM;
}

} // namespace

ComClassRegistration::ComClassRegistration(
    const std::string& capsule_root, const ComClassId& clsid, u32 cookie,
    u64 class_object, u32 context, u32 flags, u32 pointer_size,
    bool suspended) {
    if (capsule_root.empty() || cookie == 0 || class_object == 0 ||
        (pointer_size != 4 && pointer_size != 8)) {
        return;
    }

    const std::string directory = class_directory(capsule_root, clsid);
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) return;

    m_snapshot = ComClassSnapshot{
        static_cast<u32>(::getpid()), cookie, class_object, context, flags,
        pointer_size};
    m_path = directory + "/" + std::to_string(m_snapshot.owner_pid) + "-" +
             std::to_string(cookie);
    m_temp_path = m_path + ".tmp";
    if (!write_record(suspended)) {
        m_path.clear();
        m_temp_path.clear();
    }
}

ComClassRegistration::~ComClassRegistration() {
    if (m_path.empty()) return;
    std::error_code ec;
    fs::remove(m_path, ec);
    fs::remove(m_temp_path, ec);
}

ComClassRegistration::ComClassRegistration(
    ComClassRegistration&& other) noexcept
    : m_path(std::move(other.m_path))
    , m_temp_path(std::move(other.m_temp_path))
    , m_snapshot(other.m_snapshot) {
    other.m_path.clear();
    other.m_temp_path.clear();
}

ComClassRegistration& ComClassRegistration::operator=(
    ComClassRegistration&& other) noexcept {
    if (this == &other) return *this;
    if (!m_path.empty()) {
        std::error_code ec;
        fs::remove(m_path, ec);
        fs::remove(m_temp_path, ec);
    }
    m_path = std::move(other.m_path);
    m_temp_path = std::move(other.m_temp_path);
    m_snapshot = other.m_snapshot;
    other.m_path.clear();
    other.m_temp_path.clear();
    return *this;
}

bool ComClassRegistration::write_record(bool suspended) {
    if (m_path.empty()) return false;
    const RegistryRecord record{
        kRegistryMagic, kRegistryVersion, m_snapshot.owner_pid,
        m_snapshot.cookie, m_snapshot.class_object, m_snapshot.context,
        m_snapshot.flags, suspended ? 1u : 0u, m_snapshot.pointer_size};
    {
        std::ofstream out(m_temp_path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(&record), sizeof(record));
        out.close();
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(m_temp_path, m_path, ec);
    if (!ec) return true;
    fs::remove(m_path, ec);
    ec.clear();
    fs::rename(m_temp_path, m_path, ec);
    return !ec;
}

bool ComClassRegistration::set_suspended(bool suspended) {
    return write_record(suspended);
}

bool find_com_class(const std::string& capsule_root, const ComClassId& clsid,
                    u32 requested_context, ComClassSnapshot& result) {
    const std::string directory = class_directory(capsule_root, clsid);
    std::error_code ec;
    fs::directory_iterator it(directory, ec);
    if (ec) return false;

    for (const auto& item : it) {
        if (!item.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        RegistryRecord record{};
        std::ifstream in(item.path(), std::ios::binary);
        in.read(reinterpret_cast<char*>(&record), sizeof(record));
        if (!in || record.magic != kRegistryMagic ||
            record.version != kRegistryVersion || record.owner_pid == 0 ||
            record.cookie == 0 || record.class_object == 0 ||
            (record.pointer_size != 4 && record.pointer_size != 8) ||
            !process_is_live(static_cast<pid_t>(record.owner_pid))) {
            fs::remove(item.path(), ec);
            ec.clear();
            continue;
        }
        if (record.suspended != 0 ||
            (record.context & requested_context) == 0) {
            continue;
        }
        result = ComClassSnapshot{
            record.owner_pid, record.cookie, record.class_object,
            record.context, record.flags, record.pointer_size};
        return true;
    }
    return false;
}

RotObjectRegistration::RotObjectRegistration(
    const std::string& capsule_root, std::string_view moniker, u32 cookie,
    u64 object, u32 pointer_size) {
    if (capsule_root.empty() || moniker.empty() || cookie == 0 || object == 0 ||
        (pointer_size != 4 && pointer_size != 8) ||
        moniker.size() > 0x10000u) {
        return;
    }
    const std::string directory = rot_directory(capsule_root, moniker);
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) return;

    m_path = directory + "/" + std::to_string(::getpid()) + "-" +
             std::to_string(cookie);
    m_temp_path = m_path + ".tmp";
    const RotRecord record{
        kRotMagic, kRotVersion, static_cast<u32>(::getpid()), cookie, object,
        pointer_size, static_cast<u32>(moniker.size())};
    {
        std::ofstream out(m_temp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            m_path.clear();
            m_temp_path.clear();
            return;
        }
        out.write(reinterpret_cast<const char*>(&record), sizeof(record));
        out.write(moniker.data(), static_cast<std::streamsize>(moniker.size()));
        out.close();
        if (!out) {
            fs::remove(m_temp_path, ec);
            m_path.clear();
            m_temp_path.clear();
            return;
        }
    }
    fs::rename(m_temp_path, m_path, ec);
    if (ec) {
        fs::remove(m_path, ec);
        ec.clear();
        fs::rename(m_temp_path, m_path, ec);
    }
    if (ec) {
        fs::remove(m_temp_path, ec);
        m_path.clear();
        m_temp_path.clear();
    }
}

RotObjectRegistration::~RotObjectRegistration() {
    if (m_path.empty()) return;
    std::error_code ec;
    fs::remove(m_path, ec);
    fs::remove(m_temp_path, ec);
}

bool find_rot_object(const std::string& capsule_root, std::string_view moniker,
                     RotObjectSnapshot& result) {
    if (capsule_root.empty() || moniker.empty()) return false;
    const std::string directory = rot_directory(capsule_root, moniker);
    std::error_code ec;
    fs::directory_iterator it(directory, ec);
    if (ec) return false;

    for (const auto& item : it) {
        if (!item.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        RotRecord record{};
        std::ifstream in(item.path(), std::ios::binary);
        in.read(reinterpret_cast<char*>(&record), sizeof(record));
        if (!in || record.magic != kRotMagic ||
            record.version != kRotVersion || record.owner_pid == 0 ||
            record.cookie == 0 || record.object == 0 ||
            (record.pointer_size != 4 && record.pointer_size != 8) ||
            record.moniker_size == 0 || record.moniker_size > 0x10000u ||
            !process_is_live(static_cast<pid_t>(record.owner_pid))) {
            fs::remove(item.path(), ec);
            ec.clear();
            continue;
        }
        std::string published(record.moniker_size, '\0');
        in.read(published.data(),
                static_cast<std::streamsize>(published.size()));
        if (!in || published != moniker) continue;
        result = RotObjectSnapshot{
            record.owner_pid, record.cookie, record.object,
            record.pointer_size};
        return true;
    }
    return false;
}

} // namespace vitreo::ipc
