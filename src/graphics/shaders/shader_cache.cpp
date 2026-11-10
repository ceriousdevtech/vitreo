// SPDX-License-Identifier: LGPL-2.1-only

/// @file shader_cache.cpp
/// @brief Disk-backed shader cache implementation.

#include "shader_cache.hpp"
#include <vitreo/common.hpp>

#include <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <fstream>
#include <filesystem>

namespace vitreo::graphics {

ShaderCache::ShaderCache(std::string cache_dir)
    : m_cache_dir(std::move(cache_dir))
{
    // Ensure cache directory exists
    std::filesystem::create_directories(m_cache_dir);
}

namespace {

std::string to_hex(const unsigned char* p, usize n) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(n * 2, '0');
    for (usize i = 0; i < n; ++i) {
        out[2 * i] = kDigits[p[i] >> 4];
        out[2 * i + 1] = kDigits[p[i] & 0xF];
    }
    return out;
}

}  // namespace

std::string ShaderCache::hash_input(const u8* data, usize size,
                                    std::string_view salt) {
    static constexpr char kTag[] = "vitreo-shader-cache\x01";
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    CC_SHA256_Update(&ctx, kTag, static_cast<CC_LONG>(sizeof kTag - 1));
    const BuildUuid& id = host_build_uuid();
    CC_SHA256_Update(&ctx, id.bytes.data(), static_cast<CC_LONG>(id.bytes.size()));
    u8 len[8];
    const u64 salt_len = salt.size();
    for (int i = 0; i < 8; ++i) len[i] = static_cast<u8>(salt_len >> (8 * i));
    CC_SHA256_Update(&ctx, len, sizeof len);
    if (!salt.empty()) {
        CC_SHA256_Update(&ctx, salt.data(), static_cast<CC_LONG>(salt.size()));
    }
    // CC_LONG is 32-bit: feed large inputs in chunks.
    usize off = 0;
    while (off < size) {
        const usize n = std::min<usize>(size - off, usize{1} << 30);
        CC_SHA256_Update(&ctx, data + off, static_cast<CC_LONG>(n));
        off += n;
    }
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &ctx);
    return to_hex(digest, CC_SHA256_DIGEST_LENGTH);
}

std::string ShaderCache::hash_input(const u8* data, usize size) {
    return hash_input(data, size, std::string_view{});
}

std::string ShaderCache::cache_path(const std::string& hash) const {
    // <dir>/<build uuid>/<first 2 hash chars>/<hash>.msl: other builds' files
    // live in sibling directories and are never read.
    const BuildUuid& id = host_build_uuid();
    return m_cache_dir + "/" + to_hex(id.bytes.data(), id.bytes.size()) + "/" +
           hash.substr(0, 2) + "/" + hash + ".msl";
}

bool ShaderCache::lookup(const u8* input_data, usize input_size,
                          std::vector<u8>& out_msl) const
{
    return lookup(input_data, input_size, std::string_view{}, out_msl);
}

bool ShaderCache::lookup(const u8* input_data, usize input_size,
                          std::string_view salt,
                          std::vector<u8>& out_msl) const
{
    if (!input_data || input_size == 0) return false;
    // Without the build identity no entry can be told apart from another
    // build's, so nothing is trusted.
    if (!host_build_uuid().ok) return false;

    auto hash = hash_input(input_data, input_size, salt);

    std::lock_guard lock(m_mutex);

    // Check memory cache first
    auto it = m_memory_cache.find(hash);
    if (it != m_memory_cache.end()) {
        out_msl = it->second;
        return true;
    }

    // Check disk cache
    auto path = cache_path(hash);
    if (read_file(path, out_msl)) {
        m_memory_cache[hash] = out_msl;
        return true;
    }

    return false;
}

void ShaderCache::store(const u8* input_data, usize input_size,
                         const std::vector<u8>& msl_source)
{
    store(input_data, input_size, std::string_view{}, msl_source);
}

void ShaderCache::store(const u8* input_data, usize input_size,
                         std::string_view salt,
                         const std::vector<u8>& msl_source)
{
    if (!input_data || input_size == 0 || msl_source.empty()) return;
    if (!host_build_uuid().ok) return;

    auto hash = hash_input(input_data, input_size, salt);

    std::lock_guard lock(m_mutex);

    // Store in memory
    m_memory_cache[hash] = msl_source;

    // Store on disk
    auto path = cache_path(hash);
    auto dir = path.substr(0, path.rfind('/'));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!ec) (void)write_file(path, msl_source);
}

void ShaderCache::clear() {
    std::lock_guard lock(m_mutex);
    m_memory_cache.clear();

    // Remove all files in cache directory
    std::filesystem::remove_all(m_cache_dir);
    std::filesystem::create_directories(m_cache_dir);
}

usize ShaderCache::size() const {
    std::lock_guard lock(m_mutex);
    return m_memory_cache.size();
}

bool ShaderCache::read_file(const std::string& path,
                             std::vector<u8>& out) const
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    auto file_size = file.tellg();
    if (file_size <= 0) return false;

    out.resize(static_cast<usize>(file_size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(out.data()),
              static_cast<std::streamsize>(file_size));
    return file.good();
}

bool ShaderCache::write_file(const std::string& path,
                              const std::vector<u8>& data)
{
    // Temp file in the destination directory (same filesystem), fully
    // written and fsync'd, then rename(2) over the final name: rename is
    // atomic, so a reader in another process never sees a torn entry.
    const std::string dir = path.substr(0, path.rfind('/'));
    std::string tmpl = dir + "/.tmp-XXXXXX";
    const int fd = ::mkstemp(tmpl.data());
    if (fd < 0) return false;
    bool ok = true;
    usize off = 0;
    while (ok && off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
        } else {
            off += static_cast<usize>(n);
        }
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;
    if (ok && ::rename(tmpl.c_str(), path.c_str()) != 0) ok = false;
    if (!ok) ::unlink(tmpl.c_str());
    return ok;
}

} // namespace vitreo::graphics
