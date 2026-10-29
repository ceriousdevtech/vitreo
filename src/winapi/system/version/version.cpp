/// @file version.cpp
/// @brief Implementation of version.dll APIs (WIN32-087)

#include "version.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/common/synthetic_version_resource.hpp"

#include <algorithm>
#include <cstdio>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace vitreo::winapi {

// =============================================================================
// VersionManager Implementation
// =============================================================================

namespace {

/// Read a little-endian value from a byte buffer with bounds checking.
template <typename T>
[[nodiscard]] bool le_read(const std::vector<u8>& d, usize off, T& out) {
    if (off + sizeof(T) > d.size()) return false;
    T v = 0;
    for (usize i = 0; i < sizeof(T); ++i) {
        v |= static_cast<T>(static_cast<T>(d[off + i]) << (8 * i));
    }
    out = v;
    return true;
}

[[nodiscard]] std::optional<u16> read_u16(
    std::span<const u8> bytes, usize offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(u16)) {
        return std::nullopt;
    }
    return static_cast<u16>(
        static_cast<u16>(bytes[offset]) |
        static_cast<u16>(static_cast<u16>(bytes[offset + 1]) << 8));
}

[[nodiscard]] std::optional<u32> read_u32(
    std::span<const u8> bytes, usize offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(u32)) {
        return std::nullopt;
    }
    return static_cast<u32>(bytes[offset]) |
           (static_cast<u32>(bytes[offset + 1]) << 8) |
           (static_cast<u32>(bytes[offset + 2]) << 16) |
           (static_cast<u32>(bytes[offset + 3]) << 24);
}

[[nodiscard]] std::string join_windows_path(
    std::string_view directory, std::string_view filename) {
    if (directory.empty()) return std::string(filename);
    std::string path(directory);
    if (path.back() != '\\' && path.back() != '/') path.push_back('\\');
    path.append(filename);
    return path;
}

[[nodiscard]] bool read_vfs_file(
    capsule::Vfs& vfs,
    std::string_view path,
    std::vector<u8>& bytes,
    capsule::VfsError& error) {
    using capsule::AccessMode;
    using capsule::CreationDisposition;
    using capsule::ShareMode;

    auto opened = vfs.open(
        path, AccessMode::Read, ShareMode::Read,
        CreationDisposition::OpenExisting);
    if (opened.is_err()) {
        error = opened.error();
        return false;
    }

    bytes.clear();
    if (auto info = vfs.stat_handle(opened.value()); info.is_ok() &&
        info.value().size <= std::numeric_limits<usize>::max()) {
        bytes.reserve(static_cast<usize>(info.value().size));
    }

    std::array<u8, 64 * 1024> chunk{};
    bool ok = true;
    for (;;) {
        auto read = vfs.read(opened.value(), chunk);
        if (read.is_err()) {
            error = read.error();
            ok = false;
            break;
        }
        if (read.value() == 0) break;
        bytes.insert(
            bytes.end(), chunk.begin(),
            chunk.begin() + static_cast<std::ptrdiff_t>(read.value()));
    }
    (void)vfs.close(opened.value());
    return ok;
}

[[nodiscard]] bool write_vfs_file(
    capsule::Vfs& vfs,
    std::string_view path,
    std::span<const u8> bytes,
    capsule::VfsError& error) {
    using capsule::AccessMode;
    using capsule::CreationDisposition;
    using capsule::ShareMode;

    auto opened = vfs.open(
        path, AccessMode::Write, ShareMode::None,
        CreationDisposition::CreateAlways);
    if (opened.is_err()) {
        error = opened.error();
        return false;
    }

    usize offset = 0;
    bool ok = true;
    while (offset < bytes.size()) {
        auto written = vfs.write(opened.value(), bytes.subspan(offset));
        if (written.is_err()) {
            error = written.error();
            ok = false;
            break;
        }
        if (written.value() == 0) {
            error = capsule::VfsError::WriteError;
            ok = false;
            break;
        }
        offset += written.value();
    }
    (void)vfs.close(opened.value());
    if (!ok) (void)vfs.remove(path);
    return ok;
}

/// Expand the legacy Microsoft SZDD stream consumed by VerInstallFile/LZ32.
/// The format is a 4 KiB LZSS window initialized with spaces. Control bits are
/// consumed least-significant first; a set bit is a literal and a clear bit is
/// a 12-bit window offset plus a 4-bit length.
[[nodiscard]] bool expand_szdd(
    std::span<const u8> source,
    std::vector<u8>& output,
    bool& was_compressed) {
    constexpr std::array<u8, 8> kSignature{
        'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33};
    was_compressed =
        source.size() >= kSignature.size() &&
        std::equal(kSignature.begin(), kSignature.end(), source.begin());
    if (!was_compressed) {
        output.assign(source.begin(), source.end());
        return true;
    }
    if (source.size() < 14 || source[8] != 'A') return false;

    const auto expanded_size = read_u32(source, 10);
    constexpr u32 kMaximumExpandedSize = 512u * 1024u * 1024u;
    if (!expanded_size || *expanded_size > kMaximumExpandedSize) return false;

    output.clear();
    output.reserve(*expanded_size);
    std::array<u8, 4096> window{};
    window.fill(0x20);
    usize window_position = 0xFF0;
    usize input_position = 14;

    while (output.size() < *expanded_size) {
        if (input_position >= source.size()) return false;
        const u8 control = source[input_position++];
        for (u8 bit = 0; bit < 8 && output.size() < *expanded_size; ++bit) {
            if ((control & static_cast<u8>(1u << bit)) != 0) {
                if (input_position >= source.size()) return false;
                const u8 value = source[input_position++];
                output.push_back(value);
                window[window_position] = value;
                window_position = (window_position + 1) & 0xFFFu;
                continue;
            }

            if (input_position + 1 >= source.size()) return false;
            const u8 low = source[input_position++];
            const u8 high_length = source[input_position++];
            const usize match_position =
                static_cast<usize>(low) |
                (static_cast<usize>(high_length & 0xF0u) << 4);
            const usize match_length =
                static_cast<usize>(high_length & 0x0Fu) + 3;
            for (usize index = 0;
                 index < match_length && output.size() < *expanded_size;
                 ++index) {
                const u8 value = window[(match_position + index) & 0xFFFu];
                output.push_back(value);
                window[window_position] = value;
                window_position = (window_position + 1) & 0xFFFu;
            }
        }
    }
    return true;
}

[[nodiscard]] u32 destination_failure_flags(capsule::VfsError error) {
    using namespace version_install_flags;
    switch (error) {
        case capsule::VfsError::SharingViolation:
            return VIF_CANNOTCREATE | VIF_SHARINGVIOLATION;
        case capsule::VfsError::AccessDenied:
            return VIF_CANNOTCREATE | VIF_ACCESSVIOLATION;
        case capsule::VfsError::DiskFull:
            return VIF_CANNOTCREATE | VIF_OUTOFSPACE;
        case capsule::VfsError::OutOfMemory:
            return VIF_CANNOTCREATE | VIF_OUTOFMEMORY;
        default:
            return VIF_CANNOTCREATE;
    }
}

[[nodiscard]] constexpr usize align4(usize value) {
    return (value + 3u) & ~usize{3u};
}

struct VersionBlock {
    usize offset{0};
    usize end{0};
    usize value_offset{0};
    usize children_offset{0};
    u16 value_length{0};
    u16 type{0};
    std::string key;
    usize key_end{0};  ///< just past the key's NUL, before alignment
};

[[nodiscard]] std::optional<VersionBlock> parse_version_block(
    std::span<const u8> bytes, usize offset, usize containing_end) {
    if (containing_end > bytes.size() || offset > containing_end ||
        containing_end - offset < 6) {
        return std::nullopt;
    }

    const auto length = read_u16(bytes, offset);
    const auto value_length = read_u16(bytes, offset + 2);
    const auto type = read_u16(bytes, offset + 4);
    if (!length || !value_length || !type || *length < 6 ||
        static_cast<usize>(*length) > containing_end - offset) {
        return std::nullopt;
    }
    const usize end = offset + *length;

    std::string key;
    usize cursor = offset + 6;
    bool terminated = false;
    while (cursor + 1 < end) {
        const auto unit = read_u16(bytes, cursor);
        if (!unit) return std::nullopt;
        cursor += 2;
        if (*unit == 0) {
            terminated = true;
            break;
        }
        if (*unit > 0x7Fu) return std::nullopt;
        key.push_back(static_cast<char>(*unit));
    }
    if (!terminated) return std::nullopt;

    const usize value_offset = align4(cursor);
    if (value_offset > end) return std::nullopt;
    usize value_bytes =
        *type == 1
            ? static_cast<usize>(*value_length) * sizeof(u16)
            : static_cast<usize>(*value_length);
    // Some resource compilers count a string value in BYTES rather than
    // characters; Windows still reads such blocks, so the value is bounded
    // by the block instead of rejecting it.
    if (value_bytes > end - value_offset) value_bytes = end - value_offset;

    return VersionBlock{
        offset,
        end,
        value_offset,
        align4(value_offset + value_bytes),
        *value_length,
        *type,
        std::move(key),
        cursor,
    };
}

[[nodiscard]] bool ascii_iequals(
    std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(
               left.begin(), left.end(), right.begin(),
               [](char lhs, char rhs) {
                   return std::tolower(
                              static_cast<unsigned char>(lhs)) ==
                          std::tolower(
                              static_cast<unsigned char>(rhs));
               });
}

[[nodiscard]] std::vector<std::string_view> split_query_path(
    std::string_view path) {
    std::vector<std::string_view> segments;
    usize offset = 0;
    while (offset < path.size()) {
        while (offset < path.size() &&
               (path[offset] == '\\' || path[offset] == '/')) {
            ++offset;
        }
        const usize begin = offset;
        while (offset < path.size() &&
               path[offset] != '\\' && path[offset] != '/') {
            ++offset;
        }
        if (offset > begin) {
            segments.push_back(path.substr(begin, offset - begin));
        }
    }
    return segments;
}

[[nodiscard]] std::optional<VersionBlock> find_child_block(
    std::span<const u8> bytes,
    const VersionBlock& parent,
    std::string_view wanted_key) {
    usize child_offset = parent.children_offset;
    while (child_offset < parent.end) {
        auto child =
            parse_version_block(bytes, child_offset, parent.end);
        if (!child) return std::nullopt;
        if (ascii_iequals(child->key, wanted_key)) return child;
        const usize next = align4(child->end);
        if (next <= child_offset) return std::nullopt;
        child_offset = next;
    }
    return std::nullopt;
}


[[nodiscard]] std::string normalize_windows_path(std::string_view path) {
    std::string normalized(path);
    std::transform(
        normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char character) {
            if (character == '/') return '\\';
            return static_cast<char>(std::tolower(character));
        });
    return normalized;
}

[[nodiscard]] bool is_explicit_windows_path(std::string_view path) {
    return path.find('\\') != std::string_view::npos ||
           path.find('/') != std::string_view::npos ||
           path.find(':') != std::string_view::npos;
}

} // namespace

namespace {
thread_local u32 t_version_last_error = 0;
constexpr u32 ERROR_RESOURCE_DATA_NOT_FOUND_CODE = 1812;
}

u32 VersionManager::last_error() const { return t_version_last_error; }

std::string VersionManager::search_file(const std::string& name) const {
    if (m_vfs == nullptr) return {};
    std::vector<std::string> dirs;
    if (m_module_manager != nullptr) {
        const std::string exe(m_module_manager->main_module_path());
        if (const usize slash = exe.find_last_of("\\/"); slash != std::string::npos) {
            dirs.push_back(exe.substr(0, slash));
        }
    }
    dirs.push_back("C:\\Windows\\System32");
    dirs.push_back("C:\\Windows");
    dirs.push_back(m_vfs->cwd());
    if (m_path_lookup) {
        const std::string path = m_path_lookup();
        usize start = 0;
        while (start <= path.size()) {
            const usize end = path.find(';', start);
            const std::string dir = path.substr(start, end == std::string::npos
                                                          ? std::string::npos
                                                          : end - start);
            if (!dir.empty()) dirs.push_back(dir);
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    for (std::string dir : dirs) {
        if (dir.empty()) continue;
        if (dir.back() != '\\' && dir.back() != '/') dir.push_back('\\');
        const std::string candidate = dir + name;
        if (m_vfs->stat(candidate).is_ok()) return candidate;
    }
    return {};
}

std::vector<u8> VersionManager::read_disk_version_resource(
    const std::string& path, u32& error) const {
    std::vector<u8> none;
    error = error_codes::ERROR_FILE_NOT_FOUND;
    if (m_vfs == nullptr) return none;

    // Read the PE from the capsule.
    auto h = m_vfs->open(path, capsule::AccessMode::Read,
                         capsule::ShareMode::Read,
                         capsule::CreationDisposition::OpenExisting);
    if (h.is_err()) {
        error = m_vfs->stat(path).is_ok() ? error_codes::ERROR_ACCESS_DENIED
                                          : error_codes::ERROR_FILE_NOT_FOUND;
        return none;
    }
    // From here on the file exists; anything that is not a PE image with
    // resources is ERROR_RESOURCE_DATA_NOT_FOUND (version_probe size_not_pe).
    error = ERROR_RESOURCE_DATA_NOT_FOUND_CODE;
    std::vector<u8> d;
    if (auto st = m_vfs->stat(path); st.is_ok()) {
        d.reserve(static_cast<usize>(st.value().size));
    }
    std::vector<u8> buf(64 * 1024);
    for (;;) {
        auto chunk = m_vfs->read(h.value(), std::span<u8>(buf.data(), buf.size()));
        if (chunk.is_err() || chunk.value() == 0) break;
        d.insert(d.end(), buf.begin(),
                 buf.begin() + static_cast<std::ptrdiff_t>(chunk.value()));
    }
    (void)m_vfs->close(h.value());
    return version_resource_from_image(d, error);
}

std::vector<u8> version_resource_from_image(const std::vector<u8>& d, u32& error) {
    std::vector<u8> none;
    error = ERROR_RESOURCE_DATA_NOT_FOUND_CODE;
    if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z') return none;

    // DOS -> NT headers.
    u32 pe_off = 0;
    if (!le_read(d, 0x3C, pe_off) || pe_off + 24 > d.size()) return none;
    if (d[pe_off] == 'N' && d[pe_off + 1] == 'E') {
        std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED version!GetFileVersionInfo: 16-bit NE "
            "version resources\n");
        return none;
    }
    if (d[pe_off] != 'P' || d[pe_off + 1] != 'E') return none;
    u16 nsec = 0, opt_size = 0, magic = 0;
    if (!le_read(d, pe_off + 6, nsec)) return none;
    if (!le_read(d, pe_off + 20, opt_size)) return none;
    if (!le_read(d, pe_off + 24, magic)) return none;

    // Resource directory is data-directory entry 2; its RVA sits at a
    // different offset for PE32 (0x60) vs PE32+ (0x70).
    const usize dd = pe_off + 24 + ((magic == 0x20B) ? 0x70 : 0x60);
    u32 rsrc_rva = 0, rsrc_size = 0;
    if (!le_read(d, dd + 16, rsrc_rva)) return none;
    if (!le_read(d, dd + 20, rsrc_size)) return none;
    if (rsrc_rva == 0 || rsrc_size == 0) return none;

    // Map an RVA to a file offset through the section table.
    const usize sec = pe_off + 24 + opt_size;
    auto rva_to_off = [&](u32 rva) -> usize {
        for (u16 i = 0; i < nsec; ++i) {
            const usize e = sec + static_cast<usize>(i) * 40;
            u32 va = 0, vsz = 0, praw = 0, rawsz = 0;
            if (!le_read(d, e + 12, va) || !le_read(d, e + 8, vsz) ||
                !le_read(d, e + 20, praw) || !le_read(d, e + 16, rawsz)) {
                continue;
            }
            const u32 span = (vsz > rawsz) ? vsz : rawsz;
            if (rva >= va && rva < va + span) {
                return praw + (rva - va);
            }
        }
        return 0;
    };

    const usize root = rva_to_off(rsrc_rva);
    if (root == 0) return none;

    // Walk type -> name -> lang. Entries are 8 bytes; the high bit of the
    // second dword marks a subdirectory, otherwise it points at a data entry.
    auto find_entry = [&](usize dir, u32 want_id, bool match_id,
                          u32& out_offset, bool& is_dir) -> bool {
        u16 n_named = 0, n_id = 0;
        if (!le_read(d, dir + 12, n_named) || !le_read(d, dir + 14, n_id)) {
            return false;
        }
        const usize count = static_cast<usize>(n_named) + n_id;
        for (usize i = 0; i < count; ++i) {
            const usize e = dir + 16 + i * 8;
            u32 name = 0, off = 0;
            if (!le_read(d, e, name) || !le_read(d, e + 4, off)) return false;
            const bool named = (name & 0x80000000u) != 0;
            if (match_id && (named || name != want_id)) continue;
            out_offset = off & 0x7FFFFFFFu;
            is_dir = (off & 0x80000000u) != 0;
            return true;
        }
        return false;
    };

    constexpr u32 RT_VERSION = 16;
    u32 off = 0; bool is_dir = false;
    error = ERROR_RESOURCE_TYPE_NOT_FOUND;
    if (!find_entry(root, RT_VERSION, true, off, is_dir) || !is_dir) return none;
    if (!find_entry(root + off, 0, false, off, is_dir)) return none;   // first name
    if (is_dir && !find_entry(root + off, 0, false, off, is_dir)) return none; // first lang
    if (is_dir) return none;

    // Data entry: RVA, size.
    const usize de = root + off;
    u32 data_rva = 0, data_size = 0;
    if (!le_read(d, de, data_rva) || !le_read(d, de + 4, data_size)) return none;
    if (data_size == 0) return none;
    const usize data_off = rva_to_off(data_rva);
    if (data_off == 0 || data_off + data_size > d.size()) return none;

    return std::vector<u8>(d.begin() + static_cast<std::ptrdiff_t>(data_off),
                           d.begin() + static_cast<std::ptrdiff_t>(data_off + data_size));
}

std::vector<u8> VersionManager::read_version_resource(
    const std::string& path, u32& error) const {
    // Learn (GetFileVersionInfoSizeW/GetFileVersionInfoW): "If a full path is
    // not specified, the function uses the search sequence specified by the
    // LoadLibrary function."
    std::string file = path;
    if (!is_explicit_windows_path(path)) {
        file = search_file(path);
    }
    if (!file.empty()) {
        auto resource = read_disk_version_resource(file, error);
        if (!resource.empty() || m_module_manager == nullptr) {
            return resource;
        }
        // A real file with no version resource fails (1812). A built-in's
        // served file carries the synthetic RT_VERSION itself; when no file
        // is served for it (no generated-file source), fall through to the
        // same metadata.
        if (error != error_codes::ERROR_FILE_NOT_FOUND &&
            error != ERROR_RESOURCE_DATA_NOT_FOUND_CODE) {
            return resource;
        }
    } else {
        error = error_codes::ERROR_FILE_NOT_FOUND;
    }
    if (m_module_manager == nullptr) return {};

    // A loaded module named without a path is read from its own image.
    // get_module_info_by_handle(0) is the main executable. A failed lookup
    // must not report that image's version resource (version_probe
    // size_missing_path / size_missing_bare: size 0, ERROR_FILE_NOT_FOUND).
    const GuestAddr module_base = m_module_manager->get_module_handle(path);
    const ModuleInfo* module = module_base == 0
        ? nullptr
        : m_module_manager->get_module_info_by_handle(module_base);
    if (module != nullptr && !module->synthetic_image) {
        return read_disk_version_resource(module->path, error);
    }
    const bool explicit_path = is_explicit_windows_path(path);
    if (module != nullptr && module->synthetic_image) {
        // A fully qualified query must identify this exact synthetic module
        // path, not a same-named application DLL elsewhere.
        if (explicit_path &&
            normalize_windows_path(path) != normalize_windows_path(module->path)) {
            return {};
        }
        error = error_codes::ERROR_SUCCESS;
        return make_synthetic_system_version_resource(module->name);
    }
    // A Vitreo-provided system DLL that is not loaded yet: on Windows the
    // search order finds it in System32.
    std::string base = path;
    if (const usize slash = base.find_last_of("\\/"); slash != std::string::npos) {
        const std::string dir = normalize_windows_path(base.substr(0, slash));
        if (dir != "c:\\windows\\system32" && dir != "c:\\windows\\syswow64") {
            return {};
        }
        base = base.substr(slash + 1);
    }
    if (m_builtin_dll && m_builtin_dll(base)) {
        // The version strings are the file's (InternalName, OriginalFilename),
        // not the query's spelling: the file a built-in is served as carries
        // the name the thunk registry gives it, which is lowercase.
        std::transform(base.begin(), base.end(), base.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        error = error_codes::ERROR_SUCCESS;
        return make_synthetic_system_version_resource(base);
    }
    return {};
}

u32 VersionManager::get_file_version_info_size(const std::string& filename,
                                                u32& handle) const {
    handle = 0;
    if (filename.empty()) {
        t_version_last_error = error_codes::ERROR_INVALID_PARAMETER;
        return 0;
    }
    u32 error = error_codes::ERROR_SUCCESS;
    auto res = read_version_resource(filename, error);
    if (res.empty()) {
        VITREO_TRACE("[version] GetFileVersionInfoSize: %s -> 0 (error %u)\n",
                     filename.c_str(), error);
        t_version_last_error = error == error_codes::ERROR_SUCCESS
            ? ERROR_RESOURCE_TYPE_NOT_FOUND : error;
        return 0;
    }
    // A successful call stores ERROR_SUCCESS (version_probe size_self gle=0).
    // For a 32-bit (Unicode) resource Windows reports twice the resource
    // plus 4: room for the "FE2X" signature and the ANSI copies
    // VerQueryValueA makes behind the resource (s7_version_probe: wLength
    // 0x148 -> 0x294 from both the A and W forms).
    t_version_last_error = error_codes::ERROR_SUCCESS;
    return static_cast<u32>(res.size() * 2 + 4);
}

bool VersionManager::get_file_version_info(const std::string& filename,
                                            u32 /*handle*/, u32 len,
                                            void* data) const {
    if (filename.empty() || !data) {
        t_version_last_error = error_codes::ERROR_INVALID_PARAMETER;
        return false;
    }
    u32 error = error_codes::ERROR_SUCCESS;
    auto res = read_version_resource(filename, error);
    if (res.empty()) {
        t_version_last_error = error == error_codes::ERROR_SUCCESS
            ? ERROR_RESOURCE_TYPE_NOT_FOUND : error;
        return false;
    }
    // Windows 10 refuses a buffer shorter than the resource with
    // ERROR_INSUFFICIENT_BUFFER (version_probe info_short40), although
    // Learn describes truncation. (Windows 11 accepts wLength-1 and returns
    // a re-cut block -- s7_version_probe "infoW wLength-1"; not modelled.)
    if (len < res.size()) {
        t_version_last_error = error_codes::ERROR_INSUFFICIENT_BUFFER;
        return false;
    }
    std::memcpy(data, res.data(), res.size());
    // "FE2X" follows the resource when the caller's buffer has room for
    // it; the rest of the buffer is left as it was (s7_version_probe).
    if (len >= res.size() + 4) {
        static constexpr u8 kSignature[4] = {'F', 'E', '2', 'X'};
        std::memcpy(static_cast<u8*>(data) + res.size(), kSignature, 4);
    }
    t_version_last_error = error_codes::ERROR_SUCCESS;
    return true;
}

bool VersionManager::get_file_version_info_ex(u32 /*flags*/,
                                               const std::string& filename,
                                               u32 /*handle*/, u32 len,
                                               void* data) const {
    // Delegate to the normal path — the flags don't matter for our stub.
    return get_file_version_info(filename, 0, len, data);
}

std::optional<u64> fixed_file_version(const std::vector<u8>& image) {
    u32 error = 0;
    const std::vector<u8> block = version_resource_from_image(image, error);
    if (block.empty()) return std::nullopt;
    const auto root = parse_version_block(block, 0, block.size());
    constexpr u32 kFixedInfoSignature = 0xFEEF04BDu;
    if (!root || !ascii_iequals(root->key, "VS_VERSION_INFO") || root->value_length < 52 ||
        read_u32(block, root->value_offset) != kFixedInfoSignature) {
        return std::nullopt;
    }
    // VS_FIXEDFILEINFO: dwSignature, dwStrucVersion, dwFileVersionMS, dwFileVersionLS.
    const auto ms = read_u32(block, root->value_offset + 8);
    const auto ls = read_u32(block, root->value_offset + 12);
    if (!ms || !ls) return std::nullopt;
    return (static_cast<u64>(*ms) << 32) | *ls;
}

std::optional<VersionQueryResult> VersionManager::ver_query_value(
    std::span<const u8> block,
    const std::string& sub_block) const {
    auto current = parse_version_block(block, 0, block.size());
    if (!current || !ascii_iequals(current->key, "VS_VERSION_INFO")) {
        return std::nullopt;
    }
    constexpr u16 kFixedInfoSize = 52;
    constexpr u32 kFixedInfoSignature = 0xFEEF04BDu;
    // wValueLength 0 is legal: the resource simply has no VS_FIXEDFILEINFO,
    // and \StringFileInfo / \VarFileInfo queries still resolve.
    const bool has_fixed = current->value_length != 0;
    if (has_fixed &&
        (current->type != 0 ||
         current->value_length < kFixedInfoSize ||
         read_u32(block, current->value_offset) != kFixedInfoSignature)) {
        VITREO_TRACE(
            "[version] VerQueryValue('%s') -> malformed fixed info\n",
            sub_block.c_str());
        return std::nullopt;
    }

    const auto segments = split_query_path(sub_block);
    if (segments.empty() && !has_fixed) return std::nullopt;
    for (const std::string_view segment : segments) {
        current = find_child_block(block, *current, segment);
        if (!current) {
            VITREO_TRACE(
                "[version] VerQueryValue('%s') -> '%.*s' not found\n",
                sub_block.c_str(), static_cast<int>(segment.size()),
                segment.data());
            return std::nullopt;
        }
    }

    if (current->value_offset > std::numeric_limits<u32>::max()) {
        return std::nullopt;
    }
    const u32 length = current->value_length;
    VITREO_TRACE(
        "[version] VerQueryValue('%s') -> offset=%zu length=%u type=%u\n",
        sub_block.c_str(), current->value_offset, length, current->type);
    // A block without a value (a StringTable, say) answers a pointer to
    // its key's terminating NUL -- an empty string (s7_version_probe: the
    // 040904b0 table at 0x80 answers 0x96, not the aligned 0x98).
    const usize answer = length == 0 ? current->key_end - 2 : current->value_offset;
    return VersionQueryResult{
        static_cast<u32>(answer),
        length,
        current->type == 1,
    };
}

u32 VersionManager::install_file(
    u32 flags,
    const std::string& source_filename,
    const std::string& destination_filename,
    const std::string& source_directory,
    const std::string& destination_directory,
    const std::string& current_directory,
    std::string& temporary_file) const {
    using namespace version_install_flags;
    temporary_file.clear();
    if (m_vfs == nullptr || source_filename.empty() ||
        destination_filename.empty() || source_directory.empty() ||
        destination_directory.empty()) {
        VITREO_TRACE(
            "[version] VerInstallFileA invalid arguments"
            " src='%s' dst='%s' src_dir='%s' dst_dir='%s'\n",
            source_filename.c_str(), destination_filename.c_str(),
            source_directory.c_str(), destination_directory.c_str());
        return VIF_ACCESSVIOLATION;
    }

    const std::string source_path =
        join_windows_path(source_directory, source_filename);
    const std::string destination_path =
        join_windows_path(destination_directory, destination_filename);

    capsule::VfsError io_error = capsule::VfsError::None;
    std::vector<u8> source_bytes;
    if (!read_vfs_file(*m_vfs, source_path, source_bytes, io_error)) {
        VITREO_TRACE(
            "[version] VerInstallFileA source read failed"
            " path='%s' error=%s\n",
            source_path.c_str(), capsule::vfs_error_to_string(io_error));
        return VIF_CANNOTREADSRC;
    }

    std::vector<u8> installed_bytes;
    bool was_compressed = false;
    if (!expand_szdd(source_bytes, installed_bytes, was_compressed)) {
        VITREO_TRACE(
            "[version] VerInstallFileA invalid SZDD source path='%s'\n",
            source_path.c_str());
        return VIF_CANNOTLOADLZ32;
    }

    const bool destination_exists = m_vfs->stat(destination_path).is_ok();
    if (destination_exists && (flags & VIFF_FORCEINSTALL) == 0) {
        // Full Windows version/language comparison is not yet available.
        // Preserve the candidate in the destination directory and report a
        // recoverable mismatch rather than silently replacing an existing DLL.
        for (u32 suffix = 0; suffix < 1000; ++suffix) {
            temporary_file =
                destination_filename + ".v" + std::to_string(suffix) + ".tmp";
            const std::string temporary_path =
                join_windows_path(destination_directory, temporary_file);
            if (m_vfs->stat(temporary_path).is_ok()) continue;
            if (!write_vfs_file(
                    *m_vfs, temporary_path, installed_bytes, io_error)) {
                temporary_file.clear();
                return destination_failure_flags(io_error);
            }
            VITREO_TRACE(
                "[version] VerInstallFileA preserved candidate"
                " path='%s' current_dir='%s' -> VIF_TEMPFILE|VIF_MISMATCH\n",
                temporary_path.c_str(), current_directory.c_str());
            return VIF_TEMPFILE | VIF_MISMATCH;
        }
        return VIF_CANNOTCREATE;
    }

    if (!write_vfs_file(
            *m_vfs, destination_path, installed_bytes, io_error)) {
        VITREO_TRACE(
            "[version] VerInstallFileA destination write failed"
            " path='%s' error=%s\n",
            destination_path.c_str(), capsule::vfs_error_to_string(io_error));
        return destination_failure_flags(io_error);
    }

    VITREO_TRACE(
        "[version] VerInstallFileA src='%s' dst='%s' compressed=%u"
        " input=%zu output=%zu flags=0x%x -> 0\n",
        source_path.c_str(), destination_path.c_str(),
        was_compressed ? 1u : 0u, source_bytes.size(),
        installed_bytes.size(), flags);
    return 0;
}

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

static void impl_GetFileVersionInfoSizeW(Marshaler& m,
                                          VersionManager& mgr) {
    GuestAddr filename_addr = m.arg_ptr(0);
    GuestAddr handle_addr   = m.arg_ptr(1);
    std::string filename = m.read_wide_string(filename_addr).value_or("");
    u32 handle = 0;
    u32 size = mgr.get_file_version_info_size(filename, handle);
    if (handle_addr) (void)m.write_u32(handle_addr, handle);
    set_last_error(mgr.last_error()); // ERROR_SUCCESS on success (probe)
    m.set_return(static_cast<u64>(size));
}

static void impl_GetFileVersionInfoSizeA(Marshaler& m,
                                          VersionManager& mgr) {
    GuestAddr filename_addr = m.arg_ptr(0);
    GuestAddr handle_addr   = m.arg_ptr(1);
    std::string filename = m.read_ansi_string(filename_addr).value_or("");
    u32 handle = 0;
    u32 size = mgr.get_file_version_info_size(filename, handle);
    if (handle_addr) (void)m.write_u32(handle_addr, handle);
    set_last_error(mgr.last_error());
    m.set_return(static_cast<u64>(size));
}

static bool copy_file_version_info_to_guest(
    Marshaler& m,
    VersionManager& mgr,
    const std::string& filename,
    u32 handle,
    u32 len,
    GuestAddr data_addr,
    std::optional<u32> flags = std::nullopt) {
    u32 ignored_handle = 0;
    const u32 reported =
        mgr.get_file_version_info_size(filename, ignored_handle);
    if (reported == 0) {
        set_last_error(mgr.last_error());
        return false;
    }
    // The resource itself; the reported size adds the ANSI scratch area.
    const u32 required = (reported - 4) / 2;
    if (data_addr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    if (len < required) {
        VITREO_TRACE(
            "[version] GetFileVersionInfo: %s guest buffer too small"
            " (%u < %u)\n",
            filename.c_str(), len, required);
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        return false;
    }

    const u32 written = len >= required + 4 ? required + 4 : required;
    std::vector<Byte> block(written);
    const bool loaded =
        flags
            ? mgr.get_file_version_info_ex(
                  *flags, filename, handle, written, block.data())
            : mgr.get_file_version_info(
                  filename, handle, written, block.data());
    if (!loaded) {
        set_last_error(mgr.last_error());
        return false;
    }
    if (m.memory().write_bytes(
            data_addr, block.data(), block.size()).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return false;
    }
    // version_probe.expected info_full: success stores ERROR_SUCCESS.
    set_last_error(error_codes::ERROR_SUCCESS);
    return true;
}

static void impl_GetFileVersionInfoW(Marshaler& m,
                                      VersionManager& mgr) {
    GuestAddr filename_addr = m.arg_ptr(0);
    u32 handle              = static_cast<u32>(m.arg(1));
    u32 len                 = static_cast<u32>(m.arg(2));
    GuestAddr data_addr     = m.arg_ptr(3);
    std::string filename = m.read_wide_string(filename_addr).value_or("");
    const bool ok = copy_file_version_info_to_guest(
        m, mgr, filename, handle, len, data_addr);
    m.set_return(ok ? 1u : 0u);
}

static void impl_GetFileVersionInfoA(Marshaler& m,
                                      VersionManager& mgr) {
    GuestAddr filename_addr = m.arg_ptr(0);
    u32 handle              = static_cast<u32>(m.arg(1));
    u32 len                 = static_cast<u32>(m.arg(2));
    GuestAddr data_addr     = m.arg_ptr(3);
    std::string filename = m.read_ansi_string(filename_addr).value_or("");
    const bool ok = copy_file_version_info_to_guest(
        m, mgr, filename, handle, len, data_addr);
    m.set_return(ok ? 1u : 0u);
}

static void impl_GetFileVersionInfoExW(Marshaler& m,
                                        VersionManager& mgr) {
    u32 flags               = static_cast<u32>(m.arg(0));
    GuestAddr filename_addr = m.arg_ptr(1);
    u32 handle              = static_cast<u32>(m.arg(2));
    u32 len                 = static_cast<u32>(m.arg(3));
    GuestAddr data_addr     = m.arg_ptr(4);
    std::string filename = m.read_wide_string(filename_addr).value_or("");
    const bool ok = copy_file_version_info_to_guest(
        m, mgr, filename, handle, len, data_addr, flags);
    m.set_return(ok ? 1u : 0u);
}

static void impl_VerQueryValue(
    Marshaler& m, VersionManager& mgr, bool wide) {
    GuestAddr block_addr    = m.arg_ptr(0);
    GuestAddr subblock_addr = m.arg_ptr(1);
    GuestAddr ppbuf_addr    = m.arg_ptr(2); // LPVOID*
    GuestAddr pulen_addr    = m.arg_ptr(3); // PUINT

    if (block_addr == 0 || subblock_addr == 0 ||
        ppbuf_addr == 0 || pulen_addr == 0) {
        m.set_return(0u);
        return;
    }

    const auto sub_block =
        wide ? m.read_wide_string(subblock_addr)
             : m.read_ansi_string(subblock_addr);
    const auto total_length = m.read_u16(block_addr);
    if (sub_block.is_err() || total_length.is_err() ||
        total_length.value() < 6) {
        m.set_return(0u);
        return;
    }

    std::vector<u8> block(total_length.value());
    if (m.memory().read_bytes(
            block_addr, block.data(), block.size()).is_err()) {
        m.set_return(0u);
        return;
    }

    const auto result = mgr.ver_query_value(block, sub_block.value());
    if (!result) {
        // Measured (s7_version_probe, A and W): FALSE,
        // ERROR_RESOURCE_TYPE_NOT_FOUND and *puLen = 0.
        (void)m.write_u32(pulen_addr, 0);
        set_last_error(error_codes::ERROR_RESOURCE_TYPE_NOT_FOUND);
        m.set_return(0u);
        return;
    }
    GuestAddr value = block_addr + result->value_offset;
    u32 length = result->length;
    if (!wide && result->text && length != 0) {
        // VerQueryValueA converts a text value to the ANSI code page and
        // stores it at the same offset in the area behind the resource
        // (block + wLength + 4, which GetFileVersionInfoSize reserved);
        // *puLen is the converted byte count including the NUL
        // (s7_version_probe: CompanyName W off 0xB8 -> A off 0x204, len 13).
        std::vector<u16> units(length);
        for (u32 i = 0; i < length; ++i) {
            units[i] = static_cast<u16>(block[result->value_offset + i * 2] |
                                        (block[result->value_offset + i * 2 + 1] << 8));
        }
        std::vector<u8> ansi(units.size() * 4 + 4);
        const auto converted = codepage::wide_to_multi_byte(
            codepage::kCpAcp, 0, units.data(), units.size(),
            ansi.data(), ansi.size(), nullptr, false);
        if (!converted.ok()) {
            m.set_return(0u);
            return;
        }
        ansi.resize(converted.count);
        const GuestAddr target =
            block_addr + total_length.value() + 4 + result->value_offset;
        if (m.memory().write_bytes(
                target, ansi.data(), ansi.size()).is_err()) {
            m.set_return(0u);
            return;
        }
        value = target;
        length = static_cast<u32>(ansi.size());
    }
    if (m.write_ptr(ppbuf_addr, value).is_err() ||
        m.write_u32(pulen_addr, length).is_err()) {
        m.set_return(0u);
        return;
    }
    set_last_error(error_codes::ERROR_SUCCESS);  // measured: A and W
    m.set_return(1u);
}

static void impl_VerQueryValueW(Marshaler& m, VersionManager& mgr) {
    impl_VerQueryValue(m, mgr, true);
}

static void impl_VerQueryValueA(Marshaler& m, VersionManager& mgr) {
    impl_VerQueryValue(m, mgr, false);
}

static void impl_VerInstallFileA(Marshaler& m, VersionManager& mgr) {
    using namespace version_install_flags;
    const u32 flags = static_cast<u32>(m.arg(0));
    const auto source_filename = m.read_ansi_string(m.arg_ptr(1));
    const auto destination_filename = m.read_ansi_string(m.arg_ptr(2));
    const auto source_directory = m.read_ansi_string(m.arg_ptr(3));
    const auto destination_directory = m.read_ansi_string(m.arg_ptr(4));
    const auto current_directory = m.read_ansi_string(m.arg_ptr(5));
    const GuestAddr temporary_file_address = m.arg_ptr(6);
    const GuestAddr temporary_length_address = m.arg_ptr(7);

    if (source_filename.is_err() || destination_filename.is_err() ||
        source_directory.is_err() || destination_directory.is_err() ||
        current_directory.is_err() || temporary_file_address == 0 ||
        temporary_length_address == 0) {
        m.set_return(VIF_ACCESSVIOLATION);
        return;
    }
    const auto capacity = m.read_u32(temporary_length_address);
    if (capacity.is_err()) {
        m.set_return(VIF_ACCESSVIOLATION);
        return;
    }

    std::string temporary_file;
    u32 result = mgr.install_file(
        flags, source_filename.value(), destination_filename.value(),
        source_directory.value(), destination_directory.value(),
        current_directory.value(), temporary_file);
    const u32 required =
        static_cast<u32>(temporary_file.size() + 1);
    if (m.write_u32(temporary_length_address, required).is_err()) {
        result |= VIF_ACCESSVIOLATION;
    } else if (capacity.value() < required) {
        result |= VIF_BUFFTOOSMALL;
    } else if (m.write_ansi_string(
                   temporary_file_address, temporary_file).is_err()) {
        result |= VIF_ACCESSVIOLATION;
    }

    VITREO_TRACE(
        "[version] VerInstallFileA src='%s\\%s' dst='%s\\%s'"
        " flags=0x%x tmp='%s' tmp_capacity=%u required=%u -> 0x%x\n",
        source_directory.value().c_str(), source_filename.value().c_str(),
        destination_directory.value().c_str(),
        destination_filename.value().c_str(), flags,
        temporary_file.c_str(), capacity.value(), required, result);
    m.set_return(result);
}

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports create_version_thunks(VersionManager& mgr) {
    DllExports exports;
    exports.entries.reserve(9);

    // GetFileVersionInfoSizeW (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetFileVersionInfoSizeW", 8,
        [&mgr](Marshaler& m) { impl_GetFileVersionInfoSizeW(m, mgr); },
        8
    });
    // GetFileVersionInfoSizeA (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetFileVersionInfoSizeA", 5,
        [&mgr](Marshaler& m) { impl_GetFileVersionInfoSizeA(m, mgr); },
        8
    });
    // GetFileVersionInfoW (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "GetFileVersionInfoW", 9,
        [&mgr](Marshaler& m) { impl_GetFileVersionInfoW(m, mgr); },
        16
    });
    // GetFileVersionInfoA (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "GetFileVersionInfoA", 1,
        [&mgr](Marshaler& m) { impl_GetFileVersionInfoA(m, mgr); },
        16
    });
    // GetFileVersionInfoExW (5 args, stdcall_bytes=20)
    exports.entries.push_back(ExportEntry{
        "GetFileVersionInfoExW", 4,
        [&mgr](Marshaler& m) { impl_GetFileVersionInfoExW(m, mgr); },
        20
    });
    // VerQueryValueW (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "VerQueryValueW", 17,
        [&mgr](Marshaler& m) { impl_VerQueryValueW(m, mgr); },
        16
    });
    // VerQueryValueA (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "VerQueryValueA", 16,
        [&mgr](Marshaler& m) { impl_VerQueryValueA(m, mgr); },
        16
    });

    // GetFileVersionInfoSizeExW (3 args: flags, filename, handle)
    exports.entries.push_back(ExportEntry{
        "GetFileVersionInfoSizeExW", 7,
        [&mgr](Marshaler& m) {
            (void)m.arg(0); // flags (ignored)
            GuestAddr filename_addr = m.arg_ptr(1);
            GuestAddr handle_addr   = m.arg_ptr(2);
            std::string filename = m.read_wide_string(filename_addr).value_or("");
            u32 handle = 0;
            u32 size = mgr.get_file_version_info_size(filename, handle);
            if (handle_addr) (void)m.write_u32(handle_addr, handle);
            if (size == 0) set_last_error(mgr.last_error());
            m.set_return(static_cast<u64>(size));
        },
        12
    });

    // VerInstallFileA (8 args, stdcall_bytes=32)
    exports.entries.push_back(ExportEntry{
        "VerInstallFileA", 12,
        [&mgr](Marshaler& m) { impl_VerInstallFileA(m, mgr); },
        32
    });

    // DWORD VerLanguageNameW(DWORD wLang, LPWSTR szLang, DWORD cch)
    exports.entries.push_back(ExportEntry{
        "VerLanguageNameW", 15,
        [](Marshaler& m) {
            const u32 lang = static_cast<u32>(m.arg(0));
            const GuestAddr buf = m.arg_ptr(1);
            const u32 cch = static_cast<u32>(m.arg(2));
            // Names match GetLocaleInfoW LOCALE_SLANGUAGE for known LCIDs
            // (Windows VerLanguageNameW).
            const char16_t* name = u"Language Neutral";
            switch (lang) {
            case 0x0409: name = u"English (United States)"; break;
            case 0x0809: name = u"English (United Kingdom)"; break;
            case 0x0407: name = u"German (Germany)"; break;
            case 0x040C: name = u"French (France)"; break;
            case 0x0C0A: name = u"Spanish (Spain)"; break;
            case 0x0410: name = u"Italian (Italy)"; break;
            case 0x0416: name = u"Portuguese (Brazil)"; break;
            case 0x0413: name = u"Dutch (Netherlands)"; break;
            case 0x0411: name = u"Japanese (Japan)"; break;
            case 0x0404: name = u"Chinese (Traditional, Taiwan)"; break;
            case 0x0804: name = u"Chinese (Simplified, China)"; break;
            case 0x0412: name = u"Korean (Korea)"; break;
            case 0x0419: name = u"Russian (Russia)"; break;
            case 0x0415: name = u"Polish (Poland)"; break;
            case 0x041D: name = u"Swedish (Sweden)"; break;
            case 0x041F: name = u"Turkish (Turkey)"; break;
            case 0x0422: name = u"Ukrainian (Ukraine)"; break;
            case 0x0405: name = u"Czech (Czechia)"; break;
            case 0: break;
            default: name = u"Language Neutral"; break;
            }
            u32 needed = 0;
            while (name[needed] != 0) ++needed;
            ++needed; // NUL
            if (buf == 0 || cch == 0) {
                m.set_return(needed);
                return;
            }
            // Windows: small buffer truncates and returns chars written
            // (excluding NUL), e.g. cch=2 → "E\0", return 1.
            const u32 store = (cch < needed) ? (cch > 0 ? cch - 1 : 0)
                                             : (needed - 1);
            for (u32 i = 0; i < store; ++i) {
                (void)m.memory().write_u16(buf + static_cast<u64>(i) * 2,
                                           static_cast<u16>(name[i]));
            }
            if (cch > 0) {
                (void)m.memory().write_u16(
                    buf + static_cast<u64>(store) * 2, 0);
            }
            m.set_return(store);
        },
        12
    });
    exports.entries.push_back(ExportEntry{
        "VerLanguageNameA", 14,
        [](Marshaler& m) {
            // Narrow via the wide path on a stack temp is heavy; reuse names
            // as ASCII-only (all current table entries are ASCII).
            const u32 lang = static_cast<u32>(m.arg(0));
            const GuestAddr buf = m.arg_ptr(1);
            const u32 cch = static_cast<u32>(m.arg(2));
            const char* name = "Language Neutral";
            switch (lang) {
            case 0x0409: name = "English (United States)"; break;
            case 0x0809: name = "English (United Kingdom)"; break;
            case 0x0407: name = "German (Germany)"; break;
            case 0x040C: name = "French (France)"; break;
            case 0x0C0A: name = "Spanish (Spain)"; break;
            case 0x0410: name = "Italian (Italy)"; break;
            case 0x0416: name = "Portuguese (Brazil)"; break;
            case 0x0413: name = "Dutch (Netherlands)"; break;
            case 0x0411: name = "Japanese (Japan)"; break;
            case 0x0404: name = "Chinese (Traditional, Taiwan)"; break;
            case 0x0804: name = "Chinese (Simplified, China)"; break;
            case 0x0412: name = "Korean (Korea)"; break;
            case 0x0419: name = "Russian (Russia)"; break;
            case 0x0415: name = "Polish (Poland)"; break;
            case 0x041D: name = "Swedish (Sweden)"; break;
            case 0x041F: name = "Turkish (Turkey)"; break;
            case 0x0422: name = "Ukrainian (Ukraine)"; break;
            case 0x0405: name = "Czech (Czechia)"; break;
            case 0: break;
            default: break;
            }
            const u32 needed = static_cast<u32>(std::strlen(name)) + 1;
            if (buf == 0 || cch == 0) {
                m.set_return(needed);
                return;
            }
            const u32 store = (cch < needed) ? (cch > 0 ? cch - 1 : 0)
                                             : (needed - 1);
            for (u32 i = 0; i < store; ++i) {
                (void)m.memory().write_u8(buf + i, static_cast<u8>(name[i]));
            }
            if (cch > 0) (void)m.memory().write_u8(buf + store, 0);
            m.set_return(store);
        },
        12
    });

    return exports;
}

} // namespace vitreo::winapi
