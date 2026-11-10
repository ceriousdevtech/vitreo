// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_shader_cache_disk.cpp
/// @brief ShaderCacheFile: the persistent store of a disk shader cache.

#include "winapi/graphics/d3d12/d3d12_shader_cache_disk.hpp"
#include "winapi/graphics/d3d12/d3d12_constants.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vitreo::winapi::d3d12 {

namespace {

// Header: magic, layout version, the session Version (Learn: the cache is
// wiped when it does not match). Record: key size, value size, key, value.
constexpr u32 kMagic = 0x43535856u;  // "VXSC"
constexpr u32 kLayout = 1;
constexpr u64 kHeaderBytes = 16;
constexpr u64 kRecordHeaderBytes = 8;

/// Holds an flock for one scope: every access, as other processes may write.
class FileLock {
public:
    explicit FileLock(int fd) : m_fd(fd) { (void)::flock(m_fd, LOCK_EX); }
    ~FileLock() { (void)::flock(m_fd, LOCK_UN); }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

private:
    int m_fd;
};

bool read_exact(int fd, u64 offset, void* out, u64 bytes) {
    auto* p = static_cast<u8*>(out);
    while (bytes != 0) {
        const ssize_t got = ::pread(fd, p, static_cast<size_t>(bytes),
                                    static_cast<off_t>(offset));
        if (got <= 0) return false;
        p += got;
        offset += static_cast<u64>(got);
        bytes -= static_cast<u64>(got);
    }
    return true;
}

bool write_exact(int fd, u64 offset, const void* in, u64 bytes) {
    const auto* p = static_cast<const u8*>(in);
    while (bytes != 0) {
        const ssize_t put = ::pwrite(fd, p, static_cast<size_t>(bytes),
                                     static_cast<off_t>(offset));
        if (put <= 0) return false;
        p += put;
        offset += static_cast<u64>(put);
        bytes -= static_cast<u64>(put);
    }
    return true;
}

u64 file_size(int fd) {
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < 0) return 0;
    return static_cast<u64>(st.st_size);
}

}  // namespace

std::string shader_cache_file_name(const std::array<u8, 16>& id,
                                   const std::string& driver_tag) {
    // GUID bytes are Data1 (LE u32), Data2, Data3 (LE u16), Data4[8].
    const u32 d1 = static_cast<u32>(id[0]) | (static_cast<u32>(id[1]) << 8) |
                   (static_cast<u32>(id[2]) << 16) | (static_cast<u32>(id[3]) << 24);
    const u32 d2 = static_cast<u32>(id[4]) | (static_cast<u32>(id[5]) << 8);
    const u32 d3 = static_cast<u32>(id[6]) | (static_cast<u32>(id[7]) << 8);
    char name[64];
    std::snprintf(name, sizeof(name),
                  "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", d1, d2, d3,
                  id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
    std::string out = name;
    if (!driver_tag.empty()) out += "_" + driver_tag;
    out += ".vxshcache";
    return out;
}

ShaderCacheFile::ShaderCacheFile(int fd, std::string path, u64 limit_bytes)
    : m_fd(fd), m_path(std::move(path)), m_limit(limit_bytes) {}

ShaderCacheFile::~ShaderCacheFile() {
    if (m_fd >= 0) ::close(m_fd);
}

std::unique_ptr<ShaderCacheFile> ShaderCacheFile::open(const std::string& path,
                                                       u64 limit_bytes) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return nullptr;
    return std::unique_ptr<ShaderCacheFile>(
        new ShaderCacheFile(fd, path, limit_bytes));
}

void ShaderCacheFile::write_header_locked(u64 version) {
    u8 header[kHeaderBytes];
    std::memcpy(header, &kMagic, 4);
    std::memcpy(header + 4, &kLayout, 4);
    std::memcpy(header + 8, &version, 8);
    (void)::ftruncate(m_fd, 0);
    (void)write_exact(m_fd, 0, header, kHeaderBytes);
    m_read_offset = kHeaderBytes;
}

void ShaderCacheFile::load(u64 version, Values& values) {
    FileLock lock(m_fd);
    values.clear();
    u8 header[kHeaderBytes];
    u32 magic = 0;
    u32 layout = 0;
    u64 stored = 0;
    if (read_exact(m_fd, 0, header, kHeaderBytes)) {
        std::memcpy(&magic, header, 4);
        std::memcpy(&layout, header + 4, 4);
        std::memcpy(&stored, header + 8, 8);
    }
    if (magic != kMagic || layout != kLayout || stored != version) {
        write_header_locked(version);
        return;
    }
    m_read_offset = kHeaderBytes;
    read_records_locked(values);
}

void ShaderCacheFile::reset(u64 version) {
    FileLock lock(m_fd);
    write_header_locked(version);
}

void ShaderCacheFile::read_records_locked(Values& values) {
    const u64 end = file_size(m_fd);
    while (m_read_offset + kRecordHeaderBytes <= end) {
        u32 sizes[2] = {};
        if (!read_exact(m_fd, m_read_offset, sizes, kRecordHeaderBytes)) break;
        const u64 body = static_cast<u64>(sizes[0]) + sizes[1];
        if (sizes[0] == 0 || sizes[1] == 0 ||
            m_read_offset + kRecordHeaderBytes + body > end) {
            break;  // a record still being written, or a torn tail
        }
        std::vector<u8> key(sizes[0]);
        std::vector<u8> value(sizes[1]);
        const u64 at = m_read_offset + kRecordHeaderBytes;
        if (!read_exact(m_fd, at, key.data(), key.size()) ||
            !read_exact(m_fd, at + key.size(), value.data(), value.size())) {
            break;
        }
        values.emplace(std::move(key), std::move(value));
        m_read_offset = at + body;
    }
}

void ShaderCacheFile::catch_up(Values& values) {
    FileLock lock(m_fd);
    read_records_locked(values);
}

u32 ShaderCacheFile::append(const std::vector<u8>& key,
                            const std::vector<u8>& value, Values& values) {
    FileLock lock(m_fd);
    read_records_locked(values);
    if (values.count(key) != 0) return DXGI_ERROR_ALREADY_EXISTS;
    const u64 end = std::max(file_size(m_fd), m_read_offset);
    const u64 record = kRecordHeaderBytes + key.size() + value.size();
    if (end + record > m_limit) return DXGI_ERROR_CACHE_FULL;
    std::vector<u8> bytes(static_cast<usize>(record));
    const u32 sizes[2] = {static_cast<u32>(key.size()),
                          static_cast<u32>(value.size())};
    std::memcpy(bytes.data(), sizes, kRecordHeaderBytes);
    std::memcpy(bytes.data() + kRecordHeaderBytes, key.data(), key.size());
    std::memcpy(bytes.data() + kRecordHeaderBytes + key.size(), value.data(),
                value.size());
    if (!write_exact(m_fd, end, bytes.data(), bytes.size())) {
        (void)::ftruncate(m_fd, static_cast<off_t>(end));
        return E_FAIL;
    }
    m_read_offset = end + record;
    values.emplace(key, value);
    return S_OK;
}

void ShaderCacheFile::remove() {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    (void)::unlink(m_path.c_str());
}

}  // namespace vitreo::winapi::d3d12
