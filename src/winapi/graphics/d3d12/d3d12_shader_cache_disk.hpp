// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d12_shader_cache_disk.hpp
/// @brief The file behind a D3D12_SHADER_CACHE_MODE_DISK shader cache session.
///
/// Learn (D3D12_SHADER_CACHE_SESSION_DESC, D3D12_SHADER_CACHE_FLAGS) and the
/// DirectX-Specs shader cache spec: a disk cache is "backed by files on
/// disk, that will persist from run to run, unless cleared" -- by
/// SetDeleteOnDestroy and the last session's release, or by a Version
/// mismatch, which wipes and re-creates it. MaximumValueFileSizeBytes (0:
/// 128 MiB) bounds the file; a StoreValue that would pass it is
/// DXGI_ERROR_CACHE_FULL. The on-disk layout is Vitreo's own (Windows' is
/// undocumented): a header, then appended (key, value) records.
///
/// Several processes may open one cache: every access takes an flock, a
/// lookup that misses reads what others appended since, and a store first
/// catches up so a key another process stored is DXGI_ERROR_ALREADY_EXISTS.

#include <vitreo/common.hpp>

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vitreo::winapi::d3d12 {

class ShaderCacheFile {
public:
    using Values = std::map<std::vector<u8>, std::vector<u8>>;

    /// Opens (creating it and its directory) the cache file at @p path,
    /// whose records may total at most @p limit_bytes. Null when the file
    /// cannot be created.
    [[nodiscard]] static std::unique_ptr<ShaderCacheFile> open(
        const std::string& path, u64 limit_bytes);
    ~ShaderCacheFile();
    ShaderCacheFile(const ShaderCacheFile&) = delete;
    ShaderCacheFile& operator=(const ShaderCacheFile&) = delete;

    /// Reads the header and every record into @p values. A file whose
    /// Version is not @p version, or that is not a cache file, is wiped and
    /// re-created at @p version (Learn: "it will be wiped and re-created").
    void load(u64 version, Values& values);
    /// Wipes the file and starts it again at @p version.
    void reset(u64 version);
    /// Reads records other processes appended since the last read.
    void catch_up(Values& values);
    /// Appends one record: S_OK, DXGI_ERROR_ALREADY_EXISTS (the key is in
    /// the file, perhaps from another process; @p values then has it),
    /// DXGI_ERROR_CACHE_FULL (the file would pass its limit), or E_FAIL.
    [[nodiscard]] u32 append(const std::vector<u8>& key,
                             const std::vector<u8>& value, Values& values);
    /// Closes and deletes the file (SetDeleteOnDestroy).
    void remove();

private:
    ShaderCacheFile(int fd, std::string path, u64 limit_bytes);
    void read_records_locked(Values& values);
    void write_header_locked(u64 version);

    int m_fd{-1};
    std::string m_path;
    u64 m_limit{0};
    u64 m_read_offset{0};  ///< bytes of the file already read into values
};

/// "<IDENTIFIER>[_<driver>].vxshcache": the identifier as Windows spells a
/// GUID (upper case, no braces), the driver tag with DRIVER_VERSIONED.
[[nodiscard]] std::string shader_cache_file_name(const std::array<u8, 16>& id,
                                                 const std::string& driver_tag);

}  // namespace vitreo::winapi::d3d12
