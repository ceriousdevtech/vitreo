#pragma once

/// @file file_search.hpp
/// @brief Directory search and file attributes (WIN32-081)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vitreo::capsule { class Vfs; }

namespace vitreo::winapi {

class Marshaler;

// =============================================================================
// File Attribute Constants
// =============================================================================

constexpr u32 FILE_ATTRIBUTE_READONLY  = 0x01;
constexpr u32 FILE_ATTRIBUTE_HIDDEN    = 0x02;
constexpr u32 FILE_ATTRIBUTE_DIRECTORY = 0x10;
constexpr u32 FILE_ATTRIBUTE_ARCHIVE   = 0x20;
constexpr u32 FILE_ATTRIBUTE_NORMAL    = 0x80;
constexpr u32 INVALID_FILE_ATTRIBUTES  = 0xFFFFFFFF;

// =============================================================================
// FindData structure
// =============================================================================

struct FindData {
    u32 attributes;
    u64 creation_time;
    u64 last_access_time;
    u64 last_write_time;
    u64 file_size;
    std::string file_name;
};

// =============================================================================
// FileSearchManager
// =============================================================================

class FileSearchManager {
public:
    FileSearchManager();

    /// Set VFS for path translation (must be called before use)
    void set_vfs(capsule::Vfs* vfs) { m_vfs = vfs; }

    /// FindFirstFile — returns search handle (0 = invalid)
    [[nodiscard]] u64 find_first_file(const std::string& pattern, FindData& data);

    /// FindNextFile
    [[nodiscard]] bool find_next_file(u64 handle, FindData& data);

    /// Enumerate the data streams exposed by the capsule filesystem.
    [[nodiscard]] u64 find_first_stream(const std::string& path, FindData& data);
    [[nodiscard]] bool find_next_stream(u64 handle);

    /// FindClose
    bool find_close(u64 handle);

    /// GetFileAttributes
    [[nodiscard]] u32 get_file_attributes(const std::string& path) const;

    /// GetFileAttributesEx
    [[nodiscard]] bool get_file_attributes_ex(const std::string& path, FindData& data) const;

    /// Return the compressed size exposed by GetCompressedFileSize.
    [[nodiscard]] bool get_compressed_file_size(const std::string& path, u64& size) const;

private:
    struct SearchContext {
        std::vector<FindData> entries;
        usize current_index;
        bool stream_search = false;
    };

    capsule::Vfs* m_vfs = nullptr;
    // FindFirstFile / FindNextFile / FindClose are ordinary thread-safe Win32
    // APIs: different threads may drive different search handles at the same
    // time, and the handle table itself is process-wide. Guard the table so a
    // concurrent insert/erase cannot corrupt it. The lock covers only the
    // table, never the directory walk.
    mutable std::mutex m_searches_mutex;
    std::unordered_map<u64, SearchContext> m_searches;
    u64 m_next_handle = 1;

    /// Simple glob matching (* and ? with case-insensitive compare)
    static bool match_pattern(const std::string& pattern, const std::string& name);

    /// Fill a FindData from a host stat result
    static bool stat_to_find_data(const std::string& path,
                                  const std::string& name,
                                  FindData& data);
};

/// Fill a guest WIN32_FIND_DATAW (shared by FindFirstFileW/ExW/NextFileW).
void write_find_data_w(Marshaler& m, GuestAddr ptr, const FindData& data);
/// Fill a guest WIN32_FIND_DATAA (name in CP_ACP).
void write_find_data_a(Marshaler& m, GuestAddr ptr, const FindData& data);

/// Register file-search thunks
DllExports create_file_search_thunks(FileSearchManager& mgr);

} // namespace vitreo::winapi
