// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file shader_cache.hpp
/// @brief Disk-backed shader cache keyed by the Vitreo build, a caller salt
///        (compile options) and the input bytecode.

#include "shader_types.hpp"
#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <dlfcn.h>
#include <mach-o/loader.h>

namespace vitreo::graphics {

// =============================================================================
// Build identity
// =============================================================================

/// LC_UUID of the Mach-O image this code is linked into (loader.h,
/// uuid_command). The linker writes a fresh UUID for every distinct link, so
/// any cache derived from Vitreo's translators is valid only under it.
struct BuildUuid {
    std::array<u8, 16> bytes{};
    bool ok{false};  ///< false: no MH_MAGIC_64 image or no LC_UUID; never faked
};

/// Header-only so that every static library linked into one image (graphics
/// shader, graphics metal) resolves the same image without a link dependency.
[[nodiscard]] inline const BuildUuid& host_build_uuid() noexcept {
    static const BuildUuid id = [] {
        BuildUuid out;
        Dl_info info{};
        if (dladdr(reinterpret_cast<const void*>(&host_build_uuid), &info) == 0 ||
            info.dli_fbase == nullptr) {
            return out;
        }
        const auto* mh = static_cast<const mach_header_64*>(info.dli_fbase);
        if (mh->magic != MH_MAGIC_64) return out;
        const auto* cmd = reinterpret_cast<const u8*>(mh + 1);
        for (u32 i = 0; i < mh->ncmds; ++i) {
            const auto* lc = reinterpret_cast<const load_command*>(cmd);
            if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(uuid_command)) {
                std::memcpy(out.bytes.data(),
                            reinterpret_cast<const uuid_command*>(lc)->uuid, 16);
                out.ok = true;
                break;
            }
            cmd += lc->cmdsize;
        }
        return out;
    }();
    return id;
}

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

    /// Look up under `salt`: every input that changes the translation besides
    /// the bytecode (the translator's compile options). Different salt,
    /// different entry. Always misses when host_build_uuid() is unavailable.
    [[nodiscard]] bool lookup(const u8* input_data, usize input_size,
                               std::string_view salt,
                               std::vector<u8>& out_msl) const;

    /// Store a compiled shader in the cache.
    /// @param input_data  Raw input shader bytecode/source
    /// @param input_size  Size in bytes
    /// @param msl_source  Compiled MSL source text (null-terminated)
    void store(const u8* input_data, usize input_size,
               const std::vector<u8>& msl_source);

    /// Store under `salt`. The disk write is atomic (temp file + rename), so a
    /// concurrent reader sees the old entry, the new one, or none. A no-op
    /// when host_build_uuid() is unavailable.
    void store(const u8* input_data, usize input_size, std::string_view salt,
               const std::vector<u8>& msl_source);

    /// Clear all cached shaders, including other builds' directories.
    void clear();

    /// Get the number of entries in the cache.
    [[nodiscard]] usize size() const;

    /// Hex SHA-256 over "vitreo-shader-cache\x01" || build uuid (16) ||
    /// u64 salt length (little-endian) || salt || data.
    [[nodiscard]] static std::string hash_input(const u8* data, usize size,
                                                std::string_view salt);
    /// hash_input with the empty salt.
    [[nodiscard]] static std::string hash_input(const u8* data, usize size);

private:
    std::string m_cache_dir;
    mutable std::mutex m_mutex;
    mutable std::unordered_map<std::string, std::vector<u8>> m_memory_cache;

    /// <dir>/<uuidhex32>/<h[0..1]>/<h>.msl
    [[nodiscard]] std::string cache_path(const std::string& hash) const;
    [[nodiscard]] bool read_file(const std::string& path, std::vector<u8>& out) const;
    bool write_file(const std::string& path, const std::vector<u8>& data);
};

} // namespace vitreo::graphics
