// SPDX-License-Identifier: LGPL-2.1-only

/// @file shader_cache.cpp
/// @brief Disk-backed shader cache implementation.

#include "shader_cache.hpp"
#include <vitreo/common.hpp>

#include <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fstream>
#include <filesystem>

namespace vitreo::graphics {

ShaderCache::ShaderCache(std::string cache_dir)
    : m_cache_dir(std::move(cache_dir))
{
    // Ensure cache directory exists
    std::filesystem::create_directories(m_cache_dir);
}

std::string ShaderCache::hash_input(const u8* data, usize size) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data, static_cast<CC_LONG>(size), digest);

    // Convert to hex string
    char hex[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return std::string(hex, CC_SHA256_DIGEST_LENGTH * 2);
}

std::string ShaderCache::cache_path(const std::string& hash) const {
    // Use first 2 chars as subdirectory to avoid too many files in one dir
    return m_cache_dir + "/" + hash.substr(0, 2) + "/" + hash + ".msl";
}

bool ShaderCache::lookup(const u8* input_data, usize input_size,
                          std::vector<u8>& out_msl) const
{
    if (!input_data || input_size == 0) return false;

    auto hash = hash_input(input_data, input_size);

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
    if (!input_data || input_size == 0 || msl_source.empty()) return;

    auto hash = hash_input(input_data, input_size);

    std::lock_guard lock(m_mutex);

    // Store in memory
    m_memory_cache[hash] = msl_source;

    // Store on disk
    auto path = cache_path(hash);
    auto dir = path.substr(0, path.rfind('/'));
    std::filesystem::create_directories(dir);
    write_file(path, msl_source);
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
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    return file.good();
}

} // namespace vitreo::graphics
