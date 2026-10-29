/// @file file_search.cpp
/// @brief Implementation of directory search and file attribute APIs (WIN32-081)

#include "file_search.hpp"
#include "file.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/win32_full_path.hpp"
#include "capsule/vfs/path_encoding.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/filesystem/short_name_83.hpp"

#include <cwctype>
#include <xlocale.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string_view>
#include <sys/stat.h>

namespace vitreo::winapi {

// Windows epoch offset: 1601-01-01 to 1970-01-01 in 100-ns intervals
static constexpr u64 EPOCH_OFFSET = 116444736000000000ULL;

/// Convert a POSIX timespec to Windows FILETIME (100-ns since 1601-01-01)
static u64 timespec_to_filetime(const struct timespec& ts) {
    u64 sec  = static_cast<u64>(ts.tv_sec) * 10000000ULL;
    u64 nsec = static_cast<u64>(ts.tv_nsec) / 100ULL;
    return sec + nsec + EPOCH_OFFSET;
}

// =============================================================================
// Pattern Matching
// =============================================================================

namespace {

/// UTF-8 -> upper-cased UTF-32. Win32 name comparison is case-insensitive
/// over Unicode (files_meta_probe exact_unicode: "CAFÉ.TXT" finds "café.txt").
std::u32string upcase_u32(std::string_view utf8) {
    static locale_t loc = ::newlocale(LC_CTYPE_MASK, "en_US.UTF-8", nullptr);
    std::u32string out;
    out.reserve(utf8.size());
    usize i = 0;
    while (i < utf8.size()) {
        const auto b = static_cast<unsigned char>(utf8[i]);
        char32_t cp = b;
        usize n = 1;
        if (b >= 0xF0 && i + 3 < utf8.size() + 0) {
            cp = ((b & 0x07u) << 18) | ((utf8[i + 1] & 0x3Fu) << 12) |
                 ((utf8[i + 2] & 0x3Fu) << 6) | (utf8[i + 3] & 0x3Fu);
            n = 4;
        } else if (b >= 0xE0 && i + 2 < utf8.size()) {
            cp = ((b & 0x0Fu) << 12) | ((utf8[i + 1] & 0x3Fu) << 6) |
                 (utf8[i + 2] & 0x3Fu);
            n = 3;
        } else if (b >= 0xC0 && i + 1 < utf8.size()) {
            cp = ((b & 0x1Fu) << 6) | (utf8[i + 1] & 0x3Fu);
            n = 2;
        }
        i += n;
        if (loc != nullptr) {
            cp = static_cast<char32_t>(::towupper_l(static_cast<wint_t>(cp), loc));
        } else if (cp < 0x80) {
            cp = static_cast<char32_t>(std::toupper(static_cast<int>(cp)));
        }
        out.push_back(cp);
    }
    return out;
}

// DOS wildcard code points that Win32 hands the file system (Learn archive,
// J. Kuhne, "Wildcards in Windows"): DOS_STAR '<', DOS_QM '>', DOS_DOT '"'.
constexpr char32_t kDosStar = U'<';
constexpr char32_t kDosQm = U'>';
constexpr char32_t kDosDot = U'"';

/// FindFirstFileEx's translation of a Win32 pattern: "*.*" is "*"; a '.'
/// right after '*' at the very end makes that '*' a DOS_STAR ("*." = names
/// without an extension); a '.' followed by '?' or '*' is DOS_DOT; every '?'
/// is DOS_QM. Verified against files_meta_probe (star_dot, noext_dot_star,
/// qqqqq, star_dot_tx_q ...).
std::u32string translate_win32_expression(const std::u32string& e) {
    if (e == U"*" || e == U"*.*") return U"*";
    std::u32string out;
    for (usize i = 0; i < e.size(); ++i) {
        const char32_t c = e[i];
        if (c == U'.') {
            if (i >= 1 && i + 1 == e.size() && e[i - 1] == U'*') {
                out.back() = kDosStar;
            } else if (i + 1 < e.size() && (e[i + 1] == U'?' || e[i + 1] == U'*')) {
                out.push_back(kDosDot);
            } else {
                out.push_back(U'.');
            }
        } else if (c == U'?') {
            out.push_back(kDosQm);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/// FsRtlIsNameInExpression semantics over the translated expression.
bool dos_match(const std::u32string& e, const std::u32string& n) {
    const usize E = e.size(), N = n.size();
    // Index of the last '.' in the name: DOS_STAR may not consume it.
    usize last_dot = std::u32string::npos;
    for (usize i = 0; i < N; ++i) if (n[i] == U'.') last_dot = i;
    std::vector<signed char> memo((E + 1) * (N + 1), -1);
    auto rec = [&](auto&& self, usize ei, usize ni) -> bool {
        signed char& slot = memo[ei * (N + 1) + ni];
        if (slot >= 0) return slot != 0;
        bool r = false;
        if (ei == E) {
            r = (ni == N);
        } else {
            const char32_t c = e[ei];
            if (c == U'*') {
                r = self(self, ei + 1, ni) || (ni < N && self(self, ei, ni + 1));
            } else if (c == kDosStar) {
                r = self(self, ei + 1, ni) ||
                    (ni < N && ni != last_dot && self(self, ei, ni + 1));
            } else if (c == kDosQm) {
                if (ni == N || n[ni] == U'.') {
                    usize ej = ei;
                    while (ej < E && e[ej] == kDosQm) ++ej;
                    r = self(self, ej, ni);
                } else {
                    r = self(self, ei + 1, ni + 1);
                }
            } else if (c == kDosDot) {
                if (ni == N) r = self(self, ei + 1, ni);
                else if (n[ni] == U'.') r = self(self, ei + 1, ni + 1);
            } else if (c == U'?') {
                r = ni < N && self(self, ei + 1, ni + 1);
            } else {
                r = ni < N && n[ni] == c && self(self, ei + 1, ni + 1);
            }
        }
        slot = r ? 1 : 0;
        return r;
    };
    return rec(rec, 0, 0);
}

}  // namespace

bool FileSearchManager::match_pattern(const std::string& pattern,
                                      const std::string& name) {
    const std::u32string expr = translate_win32_expression(upcase_u32(pattern));
    // The "." and ".." entries are matched as an empty name: they appear for
    // "*", "*.*", "*." and "?????" but not for "a.*" or "?.txt"
    // (files_meta_probe star_dot / qqqqq / a_dot_star).
    if (name == "." || name == "..") return dos_match(expr, std::u32string());
    return dos_match(expr, upcase_u32(name));
}

// =============================================================================
// stat helper
// =============================================================================

bool FileSearchManager::stat_to_find_data(const std::string& path,
                                          const std::string& name,
                                          FindData& data) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }

    data.file_name = name;
    data.file_size = static_cast<u64>(st.st_size);

    if (S_ISDIR(st.st_mode)) {
        data.attributes = FILE_ATTRIBUTE_DIRECTORY;
    } else {
        data.attributes = FILE_ATTRIBUTE_ARCHIVE;
    }

    // macOS uses st_mtimespec, st_atimespec, st_birthtimespec
#if defined(__APPLE__)
    data.last_write_time  = timespec_to_filetime(st.st_mtimespec);
    data.last_access_time = timespec_to_filetime(st.st_atimespec);
    data.creation_time    = timespec_to_filetime(st.st_birthtimespec);
#else
    data.last_write_time  = timespec_to_filetime(st.st_mtim);
    data.last_access_time = timespec_to_filetime(st.st_atim);
    data.creation_time    = data.last_write_time; // fallback
#endif

    // Mark hidden if name starts with '.'
    if (!name.empty() && name[0] == '.') {
        data.attributes |= FILE_ATTRIBUTE_HIDDEN;
    }

    // Read-only check
    if (!(st.st_mode & S_IWUSR)) {
        data.attributes |= FILE_ATTRIBUTE_READONLY;
    }

    return true;
}

// =============================================================================
// FileSearchManager Implementation
// =============================================================================

FileSearchManager::FileSearchManager() = default;

namespace {

/// GetFileAttributes(Ex) reject wildcards with ERROR_INVALID_NAME. FindFirstFile
/// is the API that accepts `*`/`?`. The `\\?\` / `\\.\` prefixes contain a
/// literal `?` and must not be treated as wildcards.
[[nodiscard]] bool path_has_attribute_wildcards(
    std::string_view path) noexcept {
    std::string_view scan = path;
    if (scan.size() >= 4 && scan[0] == '\\' && scan[1] == '\\' &&
        (scan[2] == '?' || scan[2] == '.') && scan[3] == '\\') {
        scan.remove_prefix(4);
        if (scan.size() >= 4 &&
            (scan[0] == 'U' || scan[0] == 'u') &&
            (scan[1] == 'N' || scan[1] == 'n') &&
            (scan[2] == 'C' || scan[2] == 'c') &&
            scan[3] == '\\') {
            scan.remove_prefix(4);
        }
    } else if (scan.size() >= 4 && scan[0] == '\\' && scan[1] == '?' &&
               scan[2] == '?' && scan[3] == '\\') {
        scan.remove_prefix(4);
    }
    return scan.find_first_of("*?") != std::string_view::npos;
}

} // namespace

u64 FileSearchManager::find_first_file(const std::string& pattern,
                                       FindData& data) {
    diagnostics::phase_note_find();
    if (pattern.empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    // A pattern ending in a separator names no file (files_meta_probe
    // dir_trailing_backslash -> ERROR_FILE_NOT_FOUND).
    if (pattern.back() == '\\' || pattern.back() == '/') {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        return 0;
    }
    // Resolve the directory part the way every Win32 path is resolved
    // (relative and drive-relative names against the current directory,
    // "." / "..", separators), then split off the final component, which is
    // the expression (files_meta_probe drive_relative "C:*.txt", relative).
    std::string dir_path;
    std::string file_pattern;
    bool dir_is_root = false;
    if (m_vfs != nullptr) {
        auto full = capsule::win32_full_path(pattern, m_vfs->cwd());
        if (!full || full->file_part == std::string::npos) {
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
            return 0;
        }
        dir_path = full->path.substr(0, full->file_part);
        file_pattern = full->path.substr(full->file_part);
        // A drive root lists no "." / ".." entries (files_meta_probe
        // root_star_dot_entries = 0); every other directory does.
        dir_is_root = dir_path.size() == 3 && dir_path[1] == ':';
        if (!dir_is_root && dir_path.size() > 1 && dir_path.back() == '\\') {
            dir_path.pop_back();
        }
    } else {
        // Host-path mode (no VFS attached; unit tests only).
        const auto sep = pattern.find_last_of("/\\");
        dir_path = sep == std::string::npos ? std::string(".") : pattern.substr(0, sep);
        if (dir_path.empty()) dir_path = "/";
        file_pattern = sep == std::string::npos ? pattern : pattern.substr(sep + 1);
    }

    SearchContext ctx;
    ctx.current_index = 0;

    // Use VFS to list directory contents
    if (m_vfs) {
        auto list_result = m_vfs->list_dir(dir_path);
        if (list_result.is_err()) {
            set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
            return 0;
        }

        // Windows always returns "." and ".." first in directory listings.
        // VFS omits these, so inject synthetic entries when the wildcard
        // pattern matches them (e.g. "*.*", "*", ".", "..").
        auto maybe_add_dot = [&](const std::string& dot_name) {
            if (match_pattern(file_pattern, dot_name)) {
                FindData fd{};
                fd.file_name = dot_name;
                fd.attributes = FILE_ATTRIBUTE_DIRECTORY;
                // Use creation/access/write times from the directory itself
                auto dir_info = m_vfs->stat(dir_path);
                if (dir_info.is_ok()) {
                    fd.creation_time    = dir_info.value().creation_time;
                    fd.last_access_time = dir_info.value().last_access_time;
                    fd.last_write_time  = dir_info.value().last_write_time;
                }
                ctx.entries.push_back(fd);
            }
        };
        if (!dir_is_root) {
            maybe_add_dot(".");
            maybe_add_dot("..");
        }

        for (const auto& entry : list_result.value()) {
            if (match_pattern(file_pattern, entry.name)) {
                FindData fd{};
                fd.attributes = static_cast<u32>(entry.info.attributes);
                fd.creation_time = entry.info.creation_time;
                fd.last_access_time = entry.info.last_access_time;
                fd.last_write_time = entry.info.last_write_time;
                fd.file_size = entry.info.size;
                fd.file_name = entry.name;
                ctx.entries.push_back(fd);
            }
        }
    } else {
        // Fallback: raw opendir (only for host paths)
        DIR* dir = ::opendir(dir_path.c_str());
        if (!dir) {
            set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
            return 0;
        }

        struct dirent* direntry = nullptr;
        while ((direntry = ::readdir(dir)) != nullptr) {
            std::string name(direntry->d_name);
            if (name == "." || name == "..") continue;
            if (match_pattern(file_pattern, name)) {
                std::string full_path = dir_path + "/" + name;
                FindData fd{};
                if (stat_to_find_data(full_path, name, fd)) {
                    ctx.entries.push_back(fd);
                }
            }
        }
        ::closedir(dir);
    }

    if (ctx.entries.empty()) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        return 0;
    }

    // Sort entries by name for deterministic iteration, but keep "." and ".."
    // at the front in that order (matching Windows behaviour).
    std::sort(ctx.entries.begin(), ctx.entries.end(),
              [](const FindData& a, const FindData& b) {
                  bool a_dot = (a.file_name == "." || a.file_name == "..");
                  bool b_dot = (b.file_name == "." || b.file_name == "..");
                  if (a_dot != b_dot) return a_dot; // dots come first
                  if (a_dot && b_dot) return a.file_name.size() < b.file_name.size(); // "." before ".."
                  // NTFS returns entries in upcase-table order.
                  return upcase_u32(a.file_name) < upcase_u32(b.file_name);
              });

    data = ctx.entries[0];
    ctx.current_index = 1;

    std::lock_guard<std::mutex> lock(m_searches_mutex);
    u64 handle = m_next_handle++;
    m_searches[handle] = std::move(ctx);
    return handle;
}

bool FileSearchManager::find_next_file(u64 handle, FindData& data) {
    diagnostics::phase_note_find();
    std::lock_guard<std::mutex> lock(m_searches_mutex);
    auto it = m_searches.find(handle);
    if (it == m_searches.end() || it->second.stream_search) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }

    auto& ctx = it->second;
    if (ctx.current_index >= ctx.entries.size()) {
        set_last_error(error_codes::ERROR_NO_MORE_FILES);
        return false;
    }

    data = ctx.entries[ctx.current_index++];
    return true;
}

// FindFirstStreamW: files have an unnamed ::$DATA stream; directories do not.
// The capsule VFS currently exposes no named data streams (matching its
// FileStreamInfo implementation). Keep stream and directory searches distinct.
u64 FileSearchManager::find_first_stream(const std::string& path, FindData& data) {
    if (!get_file_attributes_ex(path, data)) return 0;
    if ((data.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        set_last_error(error_codes::ERROR_HANDLE_EOF);
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_searches_mutex);
    const u64 handle = m_next_handle++;
    m_searches.emplace(handle, SearchContext{{}, 0, true});
    return handle;
}

bool FileSearchManager::find_next_stream(u64 handle) {
    std::lock_guard<std::mutex> lock(m_searches_mutex);
    const auto it = m_searches.find(handle);
    set_last_error(it == m_searches.end() || !it->second.stream_search
                       ? error_codes::ERROR_INVALID_HANDLE
                       : error_codes::ERROR_HANDLE_EOF);
    return false;
}

bool FileSearchManager::find_close(u64 handle) {
    std::lock_guard<std::mutex> lock(m_searches_mutex);
    auto it = m_searches.find(handle);
    if (it == m_searches.end()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    m_searches.erase(it);
    return true;
}

u32 FileSearchManager::get_file_attributes(const std::string& path) const {
    if (path.empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return INVALID_FILE_ATTRIBUTES;
    }
    // Windows returns ERROR_INVALID_NAME (123) for wildcard paths.
    // GetFileAttributesW(L"???.???") must set that error; ERROR_FILE_NOT_FOUND
    // (2) is a different result and must not be substituted.
    if (path_has_attribute_wildcards(path)) {
        set_last_error(error_codes::ERROR_INVALID_NAME);
        return INVALID_FILE_ATTRIBUTES;
    }

    // Use VFS to translate Windows path and stat
    if (m_vfs) {
        auto info_result = m_vfs->stat(path);
        if (info_result.is_ok()) {
            return static_cast<u32>(info_result.value().attributes);
        }
        // PathNotFound (missing parent) vs FileNotFound (missing leaf):
        // files_meta_probe GetFileAttributesW_missing_dir / _missing_file.
        set_last_error(vfs_error_to_win32(info_result.error()));
        return INVALID_FILE_ATTRIBUTES;
    }

    // Fallback: raw stat (only works for host paths)
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        return INVALID_FILE_ATTRIBUTES;
    }

    u32 attrs = 0;
    if (S_ISDIR(st.st_mode)) {
        attrs |= FILE_ATTRIBUTE_DIRECTORY;
    } else {
        attrs |= FILE_ATTRIBUTE_ARCHIVE;
    }

    if (!(st.st_mode & S_IWUSR)) {
        attrs |= FILE_ATTRIBUTE_READONLY;
    }

    // Check basename for hidden (Unix dotfile convention)
    auto sep = path.rfind('/');
    std::string name = (sep != std::string::npos) ? path.substr(sep + 1) : path;
    if (!name.empty() && name[0] == '.') {
        attrs |= FILE_ATTRIBUTE_HIDDEN;
    }

    return attrs;
}

bool FileSearchManager::get_file_attributes_ex(const std::string& path,
                                               FindData& data) const {
    if (path.empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    if (path_has_attribute_wildcards(path)) {
        set_last_error(error_codes::ERROR_INVALID_NAME);
        return false;
    }

    // Use VFS if available
    if (m_vfs) {
        auto info_result = m_vfs->stat(path);
        if (info_result.is_err()) {
            // PathNotFound vs FileNotFound (files_meta_probe
            // GetFileAttributesExW_missing_dir / _missing_file).
            set_last_error(vfs_error_to_win32(info_result.error()));
            return false;
        }
        const auto& info = info_result.value();
        data.attributes = static_cast<u32>(info.attributes);
        data.creation_time = info.creation_time;
        data.last_access_time = info.last_access_time;
        data.last_write_time = info.last_write_time;
        data.file_size = info.size;
        // Extract filename from path
        std::string norm = path;
        for (char& c : norm) { if (c == '\\') c = '/'; }
        auto sep = norm.rfind('/');
        data.file_name = (sep != std::string::npos) ? norm.substr(sep + 1) : norm;
        return true;
    }

    // Fallback: raw stat
    auto sep = path.rfind('/');
    std::string name = (sep != std::string::npos) ? path.substr(sep + 1) : path;

    if (!stat_to_find_data(path, name, data)) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        return false;
    }
    return true;
}

bool FileSearchManager::get_compressed_file_size(const std::string& path,
                                                  u64& size) const {
    FindData data{};
    if (!get_file_attributes_ex(path, data)) {
        return false;
    }

    // The capsule VFS does not transparently compress files, so the
    // allocation-visible compressed size is the ordinary file size.
    size = data.file_size;
    return true;
}

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

// WIN32_FIND_DATAW layout:
// offset 0:   DWORD  dwFileAttributes       (4 bytes)
// offset 4:   FILETIME ftCreationTime       (8 bytes)
// offset 12:  FILETIME ftLastAccessTime     (8 bytes)
// offset 20:  FILETIME ftLastWriteTime      (8 bytes)
// offset 28:  DWORD  nFileSizeHigh          (4 bytes)
// offset 32:  DWORD  nFileSizeLow           (4 bytes)
// offset 36:  DWORD  dwReserved0            (4 bytes)
// offset 40:  DWORD  dwReserved1            (4 bytes)
// offset 44:  WCHAR  cFileName[260]         (520 bytes)
// offset 564: WCHAR  cAlternateFileName[14] (28 bytes)
// Total: 592 bytes
static constexpr u32 SIZEOF_WIN32_FIND_DATAW = 592;
static constexpr u32 MAX_PATH_CHARS = 260;  // cFileName[MAX_PATH]

// WIN32_FIND_DATAA: same layout but CHAR instead of WCHAR
// offset 44:  CHAR   cFileName[260]         (260 bytes)
// offset 304: CHAR   cAlternateFileName[14] (14 bytes)
// Total: 318 bytes
static constexpr u32 SIZEOF_WIN32_FIND_DATAA = 318;

// Win32 INVALID_HANDLE_VALUE is (HANDLE)(LONG_PTR)-1. PE32+ guests compare the
// full 64-bit register; zero-extending 0xFFFFFFFF makes the compare fail.
static u64 invalid_handle_value(const Marshaler& m) noexcept {
    return m.cpu_is_64bit() ? 0xFFFFFFFFFFFFFFFFULL : 0xFFFFFFFFu;
}

/// cAlternateFileName holds the generated 8.3 name when the long name is not
/// already DOS 8.3 (files_meta_probe long_name_file.text -> LONG_N~1.TEX).
void write_find_data_w(Marshaler& m, GuestAddr ptr, const FindData& data) {
    if (ptr == 0) return;
    for (u32 i = 0; i < SIZEOF_WIN32_FIND_DATAW; ++i) {
        (void)m.write_u8(ptr + i, 0);
    }
    (void)m.write_u32(ptr + 0, data.attributes);
    (void)m.write_u32(ptr + 4,  static_cast<u32>(data.creation_time & 0xFFFFFFFF));
    (void)m.write_u32(ptr + 8,  static_cast<u32>(data.creation_time >> 32));
    (void)m.write_u32(ptr + 12, static_cast<u32>(data.last_access_time & 0xFFFFFFFF));
    (void)m.write_u32(ptr + 16, static_cast<u32>(data.last_access_time >> 32));
    (void)m.write_u32(ptr + 20, static_cast<u32>(data.last_write_time & 0xFFFFFFFF));
    (void)m.write_u32(ptr + 24, static_cast<u32>(data.last_write_time >> 32));
    (void)m.write_u32(ptr + 28, static_cast<u32>(data.file_size >> 32));   // nFileSizeHigh
    (void)m.write_u32(ptr + 32, static_cast<u32>(data.file_size & 0xFFFFFFFF)); // nFileSizeLow
    const std::vector<u16> name = utf8_to_utf16(data.file_name);
    const u32 n = static_cast<u32>(std::min<usize>(name.size(), MAX_PATH_CHARS - 1));
    for (u32 i = 0; i < n; ++i) {
        (void)m.write_u16(ptr + 44 + i * 2, name[i]);
    }
    (void)m.write_u16(ptr + 44 + n * 2, 0);
    // cAlternateFileName is WCHAR[14] at offset 44 + MAX_PATH*2 = 44+520 = 564.
    const std::vector<u16> alt = short_name_83::generate(data.file_name);
    const u32 an = static_cast<u32>(std::min<usize>(alt.size(), 13));
    for (u32 i = 0; i < an; ++i) {
        (void)m.write_u16(ptr + 564 + i * 2, alt[i]);
    }
}

/// Write FindData into guest WIN32_FIND_DATAA: the name in CP_ACP.
void write_find_data_a(Marshaler& m, GuestAddr ptr, const FindData& data) {
    if (ptr == 0) return;
    for (u32 i = 0; i < SIZEOF_WIN32_FIND_DATAA; ++i) {
        (void)m.write_u8(ptr + i, 0);
    }
    (void)m.write_u32(ptr + 0, data.attributes);
    (void)m.write_u32(ptr + 4,  static_cast<u32>(data.creation_time & 0xFFFFFFFF));
    (void)m.write_u32(ptr + 8,  static_cast<u32>(data.creation_time >> 32));
    (void)m.write_u32(ptr + 12, static_cast<u32>(data.last_access_time & 0xFFFFFFFF));
    (void)m.write_u32(ptr + 16, static_cast<u32>(data.last_access_time >> 32));
    (void)m.write_u32(ptr + 20, static_cast<u32>(data.last_write_time & 0xFFFFFFFF));
    (void)m.write_u32(ptr + 24, static_cast<u32>(data.last_write_time >> 32));
    (void)m.write_u32(ptr + 28, static_cast<u32>(data.file_size >> 32));
    (void)m.write_u32(ptr + 32, static_cast<u32>(data.file_size & 0xFFFFFFFF));
    const std::string name = utf8_to_acp(data.file_name);
    const u32 n = static_cast<u32>(std::min<usize>(name.size(), MAX_PATH_CHARS - 1));
    for (u32 i = 0; i < n; ++i) {
        (void)m.write_u8(ptr + 44 + i, static_cast<u8>(name[i]));
    }
    (void)m.write_u8(ptr + 44 + n, 0);
}

void impl_FindFirstFileA(Marshaler& m, FileSearchManager& mgr) {
    const u64 invalid = invalid_handle_value(m);
    auto pattern_result = m.read_ansi_string(m.arg_ptr(0));
    if (!pattern_result.is_ok()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(invalid);
        return;
    }
    VITREO_TRACE( "[file_search] FindFirstFileA: pattern=\"%s\"\n",
                 pattern_result.value().c_str());
    FindData data{};
    u64 h = mgr.find_first_file(capsule::ansi_to_utf8(pattern_result.value()), data);
    if (h == 0) {
        VITREO_TRACE( "[file_search] FindFirstFileA: no match -> INVALID_HANDLE_VALUE\n");
        m.set_return(invalid);
        return;
    }
    VITREO_TRACE( "[file_search] FindFirstFileA: handle=%llu first='%s' attrs=0x%x\n",
                 static_cast<unsigned long long>(h), data.file_name.c_str(), data.attributes);
    write_find_data_a(m, m.arg_ptr(1), data);
    m.set_return(h);
}

void impl_FindFirstFileW(Marshaler& m, FileSearchManager& mgr) {
    const u64 invalid = invalid_handle_value(m);
    auto pattern_result = m.read_wide_string(m.arg_ptr(0));
    if (!pattern_result.is_ok()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(invalid);
        return;
    }
    VITREO_TRACE( "[file_search] FindFirstFileW: pattern=\"%s\"\n",
                 pattern_result.value().c_str());
    FindData data{};
    u64 h = mgr.find_first_file(pattern_result.value(), data);
    if (h == 0) {
        VITREO_TRACE( "[file_search] FindFirstFileW: no match -> INVALID_HANDLE_VALUE\n");
        m.set_return(invalid);
        return;
    }
    VITREO_TRACE( "[file_search] FindFirstFileW: handle=%llu first='%s' attrs=0x%x"
                 " ctime=0x%llx atime=0x%llx mtime=0x%llx size=%llu\n",
                 static_cast<unsigned long long>(h), data.file_name.c_str(), data.attributes,
                 static_cast<unsigned long long>(data.creation_time),
                 static_cast<unsigned long long>(data.last_access_time),
                 static_cast<unsigned long long>(data.last_write_time),
                 static_cast<unsigned long long>(data.file_size));
    write_find_data_w(m, m.arg_ptr(1), data);
    m.set_return(h);
}

void impl_FindNextFileA(Marshaler& m, FileSearchManager& mgr) {
    u64 handle = m.arg(0);
    FindData data{};
    bool ok = mgr.find_next_file(handle, data);
    if (ok) {
        VITREO_TRACE( "[file_search] FindNextFileA: handle=%llu -> '%s' attrs=0x%x\n",
                     static_cast<unsigned long long>(handle), data.file_name.c_str(), data.attributes);
        write_find_data_a(m, m.arg_ptr(1), data);
    } else {
        VITREO_TRACE( "[file_search] FindNextFileA: handle=%llu -> no more\n",
                     static_cast<unsigned long long>(handle));
    }
    m.set_return(ok ? 1u : 0u);
}

void impl_FindNextFileW(Marshaler& m, FileSearchManager& mgr) {
    u64 handle = m.arg(0);
    FindData data{};
    bool ok = mgr.find_next_file(handle, data);
    if (ok) {
        VITREO_TRACE( "[file_search] FindNextFileW: handle=%llu -> '%s' attrs=0x%x\n",
                     static_cast<unsigned long long>(handle), data.file_name.c_str(), data.attributes);
        write_find_data_w(m, m.arg_ptr(1), data);
    } else {
        VITREO_TRACE( "[file_search] FindNextFileW: handle=%llu -> no more\n",
                     static_cast<unsigned long long>(handle));
    }
    m.set_return(ok ? 1u : 0u);
}

// Microsoft Learn: fileapi/FindFirstStreamW and WIN32_FIND_STREAM_DATA.
// winbase.h: LARGE_INTEGER StreamSize; WCHAR cStreamName[MAX_PATH + 36].
// This layout is identical for PE32 and PE32+; handles are pointer-sized.
void impl_FindFirstStreamW(Marshaler& m, FileSearchManager& mgr) {
    const auto invalid = invalid_handle_value(m);
    if (m.arg(1) != 0 || m.arg(3) != 0 || m.arg_ptr(2) == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(invalid);
        return;
    }
    auto path = m.read_wide_string(m.arg_ptr(0));
    if (path.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(invalid);
        return;
    }
    FindData data{};
    const auto handle = mgr.find_first_stream(path.value(), data);
    if (handle == 0) {
        m.set_return(invalid);
        return;
    }
    std::array<u8, 8 + (260 + 36) * 2> output{};
    for (usize i = 0; i < 8; ++i)
        output[i] = static_cast<u8>(data.file_size >> (i * 8));
    constexpr std::u16string_view name = u"::$DATA";
    for (usize i = 0; i < name.size(); ++i)
        output[8 + i * 2] = static_cast<u8>(name[i]);
    if (m.memory().write_bytes(m.arg_ptr(2), output.data(), output.size()).is_err()) {
        (void)mgr.find_close(handle);
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(invalid);
        return;
    }
    m.set_return(handle);
}

void impl_FindClose(Marshaler& m, FileSearchManager& mgr) {
    u64 handle = m.arg(0);
    bool ok = mgr.find_close(handle);
    m.set_return(ok ? 1u : 0u);
}

void impl_GetFileAttributesA(Marshaler& m, FileSearchManager& mgr) {
    auto path_result = m.read_ansi_string(m.arg_ptr(0));
    if (!path_result.is_ok()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(static_cast<u64>(INVALID_FILE_ATTRIBUTES));
        return;
    }
    u32 attrs = mgr.get_file_attributes(path_result.value());
    VITREO_TRACE( "[file_search] GetFileAttributesA: path=\"%s\" -> 0x%x\n",
                 path_result.value().c_str(), attrs);
    m.set_return(static_cast<u64>(attrs));
}

void impl_GetFileAttributesW(Marshaler& m, FileSearchManager& mgr) {
    auto path_result = m.read_wide_string(m.arg_ptr(0));
    if (!path_result.is_ok()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(static_cast<u64>(INVALID_FILE_ATTRIBUTES));
        return;
    }
    u32 attrs = mgr.get_file_attributes(path_result.value());
    VITREO_TRACE( "[file_search] GetFileAttributesW: path=\"%s\" -> 0x%x\n",
                 path_result.value().c_str(), attrs);
    m.set_return(static_cast<u64>(attrs));
}

void impl_GetFileAttributesExW(Marshaler& m, FileSearchManager& mgr) {
    auto path_result = m.read_wide_string(m.arg_ptr(0));
    if (!path_result.is_ok()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    // Learn: GetFileExInfoStandard is the only defined level; any other
    // value fails ERROR_INVALID_PARAMETER (files_meta_probe
    // GetFileAttributesExW_level1).
    const u32 info_level = static_cast<u32>(m.arg(1));
    if (info_level != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    GuestAddr data_ptr = m.arg_ptr(2);
    FindData data{};
    bool ok = mgr.get_file_attributes_ex(path_result.value(), data);
    VITREO_TRACE("[file_search] GetFileAttributesExW: path=\"%s\" -> %s\n",
                 path_result.value().c_str(), ok ? "ok" : "fail");
    if (ok && data_ptr != 0) {
        // WIN32_FILE_ATTRIBUTE_DATA layout (36 bytes):
        // offset 0:  DWORD dwFileAttributes
        // offset 4:  FILETIME ftCreationTime
        // offset 12: FILETIME ftLastAccessTime
        // offset 20: FILETIME ftLastWriteTime
        // offset 28: DWORD nFileSizeHigh
        // offset 32: DWORD nFileSizeLow
        (void)m.write_u32(data_ptr + 0, data.attributes);
        (void)m.write_u32(data_ptr + 4,  static_cast<u32>(data.creation_time & 0xFFFFFFFF));
        (void)m.write_u32(data_ptr + 8,  static_cast<u32>(data.creation_time >> 32));
        (void)m.write_u32(data_ptr + 12, static_cast<u32>(data.last_access_time & 0xFFFFFFFF));
        (void)m.write_u32(data_ptr + 16, static_cast<u32>(data.last_access_time >> 32));
        (void)m.write_u32(data_ptr + 20, static_cast<u32>(data.last_write_time & 0xFFFFFFFF));
        (void)m.write_u32(data_ptr + 24, static_cast<u32>(data.last_write_time >> 32));
        (void)m.write_u32(data_ptr + 28, static_cast<u32>(data.file_size >> 32));
        (void)m.write_u32(data_ptr + 32, static_cast<u32>(data.file_size & 0xFFFFFFFF));
    }
    m.set_return(ok ? 1u : 0u);
}

void impl_GetFileAttributesExA(Marshaler& m, FileSearchManager& mgr) {
    auto path_result = m.read_ansi_string(m.arg_ptr(0));
    if (!path_result.is_ok()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    const u32 info_level = static_cast<u32>(m.arg(1));
    if (info_level != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    GuestAddr data_ptr = m.arg_ptr(2);
    FindData data{};
    bool ok = mgr.get_file_attributes_ex(path_result.value(), data);
    if (ok && data_ptr != 0) {
        (void)m.write_u32(data_ptr + 0, data.attributes);
        (void)m.write_u32(data_ptr + 4,  static_cast<u32>(data.creation_time & 0xFFFFFFFF));
        (void)m.write_u32(data_ptr + 8,  static_cast<u32>(data.creation_time >> 32));
        (void)m.write_u32(data_ptr + 12, static_cast<u32>(data.last_access_time & 0xFFFFFFFF));
        (void)m.write_u32(data_ptr + 16, static_cast<u32>(data.last_access_time >> 32));
        (void)m.write_u32(data_ptr + 20, static_cast<u32>(data.last_write_time & 0xFFFFFFFF));
        (void)m.write_u32(data_ptr + 24, static_cast<u32>(data.last_write_time >> 32));
        (void)m.write_u32(data_ptr + 28, static_cast<u32>(data.file_size >> 32));
        (void)m.write_u32(data_ptr + 32, static_cast<u32>(data.file_size & 0xFFFFFFFF));
    }
    VITREO_TRACE("[file_search] GetFileAttributesExA: path=\"%s\" -> %s\n",
                 path_result.value().c_str(), ok ? "TRUE" : "FALSE");
    m.set_return(ok ? 1u : 0u);
}

static void finish_get_compressed_file_size(Marshaler& m,
                                             FileSearchManager& mgr,
                                             const std::string& path) {
    const GuestAddr high_ptr = m.arg_ptr(1);
    u64 size = 0;
    if (!mgr.get_compressed_file_size(path, size)) {
        VITREO_TRACE("[file_search] GetCompressedFileSize: path=\"%s\" -> INVALID_FILE_SIZE gle=%u\n",
                     path.c_str(), get_last_error());
        m.set_return(0xFFFFFFFFu);
        return;
    }

    if (high_ptr != 0 && m.write_u32(high_ptr, static_cast<u32>(size >> 32)).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0xFFFFFFFFu);
        return;
    }

    // Success leaves the last error untouched (measured on Windows 11 for an
    // ordinary size: last_error_on_success_probe.expected). Learn: when the
    // low half collides with INVALID_FILE_SIZE the caller tells success from
    // failure by GetLastError() == NO_ERROR, so that case must clear it.
    if (static_cast<u32>(size) == 0xFFFFFFFFu) {
        set_last_error(error_codes::ERROR_SUCCESS);
    }
    VITREO_TRACE("[file_search] GetCompressedFileSize: path=\"%s\" -> %llu\n",
                 path.c_str(), static_cast<unsigned long long>(size));
    m.set_return(static_cast<u32>(size));
}

void impl_GetCompressedFileSizeW(Marshaler& m, FileSearchManager& mgr) {
    auto path_result = m.read_wide_string(m.arg_ptr(0));
    if (path_result.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0xFFFFFFFFu);
        return;
    }
    finish_get_compressed_file_size(m, mgr, path_result.value());
}

void impl_GetCompressedFileSizeA(Marshaler& m, FileSearchManager& mgr) {
    auto path_result = m.read_ansi_string(m.arg_ptr(0));
    if (path_result.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0xFFFFFFFFu);
        return;
    }
    finish_get_compressed_file_size(m, mgr, path_result.value());
}

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports create_file_search_thunks(FileSearchManager& mgr) {
    DllExports exports;
    exports.entries.reserve(12);

    exports.entries.push_back(ExportEntry{
        "FindFirstFileW", 1,
        [&mgr](Marshaler& m) { impl_FindFirstFileW(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "FindFirstFileA", 2,
        [&mgr](Marshaler& m) { impl_FindFirstFileA(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "FindNextFileW", 3,
        [&mgr](Marshaler& m) { impl_FindNextFileW(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "FindNextFileA", 4,
        [&mgr](Marshaler& m) { impl_FindNextFileA(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "FindClose", 5,
        [&mgr](Marshaler& m) { impl_FindClose(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "GetFileAttributesW", 6,
        [&mgr](Marshaler& m) { impl_GetFileAttributesW(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "GetFileAttributesA", 7,
        [&mgr](Marshaler& m) { impl_GetFileAttributesA(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "GetFileAttributesExW", 8,
        [&mgr](Marshaler& m) { impl_GetFileAttributesExW(m, mgr); },
        12
    });

    exports.entries.push_back(ExportEntry{
        "GetFileAttributesExA", 9,
        [&mgr](Marshaler& m) { impl_GetFileAttributesExA(m, mgr); },
        12
    });

    exports.entries.push_back(ExportEntry{
        "GetCompressedFileSizeW", 10,
        [&mgr](Marshaler& m) { impl_GetCompressedFileSizeW(m, mgr); },
        8
    });

    exports.entries.push_back(ExportEntry{
        "GetCompressedFileSizeA", 11,
        [&mgr](Marshaler& m) { impl_GetCompressedFileSizeA(m, mgr); },
        8
    });

    exports.entries.push_back(ExportEntry{
        "FindFirstStreamW", 0,
        [&mgr](Marshaler& m) { impl_FindFirstStreamW(m, mgr); }, 16
    });
    exports.entries.push_back(ExportEntry{
        "FindNextStreamW", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.find_next_stream(m.arg(0)) ? 1u : 0u); }, 8
    });

    // GetFileAttributesTransactedW (4 args, stdcall_bytes=16)
    // BOOL GetFileAttributesTransactedW(LPCWSTR, GET_FILEEX_INFO_LEVELS,
    //   LPVOID, HANDLE hTransaction)
    // Prefix-compatible with GetFileAttributesExW; transaction ignored.
    exports.entries.push_back(ExportEntry{
        "GetFileAttributesTransactedW", 12,
        [&mgr](Marshaler& m) { impl_GetFileAttributesExW(m, mgr); },
        16
    });

    // FindFirstFileTransactedW (7 args, stdcall_bytes=28)
    // HANDLE FindFirstFileTransactedW(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID,
    //   FINDEX_SEARCH_OPS, LPVOID, DWORD dwAdditionalFlags, HANDLE hTransaction)
    // NOTE: FindFirstFileExW lives in kernel32_ext (Kernel32ExtManager). We
    // register the transacted variant there so it can reach the impl.

    return exports;
}

} // namespace vitreo::winapi
