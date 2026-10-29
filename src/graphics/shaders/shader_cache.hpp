// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file shader_cache.hpp
/// @brief Disk-backed shader cache keyed by SHA-256 hash of input bytecode.

#include "shader_types.hpp"
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo::graphics {

// =============================================================================
// Shader Cache
// =============================================================================

class ShaderCache {
public:
    /// Create a shader cache backed by the given directory.
    /// @param cache_dir  Directory for cached MSL files (e.g., ~/.vitreo/shader_cache/)
    explicit ShaderCache(std::string cache_dir);

    /// Look up a cached shader by the hash of its input bytecode.
    /// @param input_data  Raw input shader bytecode/source
    /// @param input_size  Size in bytes
    /// @param out_msl     Output: cached MSL source text (if found)
    /// @return true if cache hit
    [[nodiscard]] bool lookup(const u8* input_data, usize input_size,
                               std::vector<u8>& out_msl) const;

    /// Store a compiled shader in the cache.
    /// @param input_data  Raw input shader bytecode/source
    /// @param input_size  Size in bytes
    /// @param msl_source  Compiled MSL source text (null-terminated)
    void store(const u8* input_data, usize input_size,
               const std::vector<u8>& msl_source);

    /// Clear all cached shaders.
    void clear();

    /// Get the number of entries in the cache.
    [[nodiscard]] usize size() const;

    /// Compute SHA-256 hash of input data, return as hex string.
    [[nodiscard]] static std::string hash_input(const u8* data, usize size);

private:
    std::string m_cache_dir;
    mutable std::mutex m_mutex;
    mutable std::unordered_map<std::string, std::vector<u8>> m_memory_cache;

    [[nodiscard]] std::string cache_path(const std::string& hash) const;
    [[nodiscard]] bool read_file(const std::string& path, std::vector<u8>& out) const;
    bool write_file(const std::string& path, const std::vector<u8>& data);
};

} // namespace vitreo::graphics
