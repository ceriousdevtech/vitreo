// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/system/setupapi/setupapi.hpp"

#include "capsule/vfs/vfs.hpp"
#include "winapi/system/setupapi/setupapi_internal.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace vitreo::winapi {

namespace {

[[nodiscard]] bool is_regular_file(
    capsule::Vfs& vfs, const std::string& path) {
    const auto st = vfs.stat(path);
    if (st.is_err()) return false;
    const u32 attributes = static_cast<u32>(st.value().attributes);
    const u32 directory = static_cast<u32>(capsule::FileAttributes::Directory);
    return (attributes & directory) == 0;
}

[[nodiscard]] bool is_directory_path(
    capsule::Vfs& vfs, const std::string& path) {
    if (path.empty()) return false;
    if (path.back() == '\\' || path.back() == '/') return true;
    const auto st = vfs.stat(path);
    if (st.is_err()) return false;
    const u32 attributes = static_cast<u32>(st.value().attributes);
    const u32 directory = static_cast<u32>(capsule::FileAttributes::Directory);
    return (attributes & directory) != 0;
}

[[nodiscard]] std::string path_leaf(const std::string& path) {
    const auto pos = path.find_last_of("\\/");
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

[[nodiscard]] std::string path_dir(const std::string& path) {
    const auto pos = path.find_last_of("\\/");
    if (pos == std::string::npos) return {};
    return path.substr(0, pos);
}

[[nodiscard]] std::string trim_trailing_slashes(std::string path) {
    while (path.size() > 3 &&
           (path.back() == '\\' || path.back() == '/')) {
        path.pop_back();
    }
    return path;
}

} // namespace

namespace setupapi_internal {

[[nodiscard]] std::string join_win_path(
    const std::string& directory, const std::string& name) {
    if (directory.empty()) return name;
    if (name.empty()) return directory;
    if (directory.back() == '\\' || directory.back() == '/') {
        return directory + name;
    }
    return directory + '\\' + name;
}

} // namespace setupapi_internal

namespace {

[[nodiscard]] std::string uncompressed_source_leaf(
    const std::string& source_path, std::span<const u8> bytes) {
    std::string leaf = path_leaf(source_path);
    constexpr std::array<u8, 8> signature{
        'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33};
    if (leaf.empty() || bytes.size() < 14) return leaf;
    if (!std::equal(signature.begin(), signature.end(), bytes.begin()) ||
        bytes[8] != 'A') {
        return leaf;
    }
    leaf.back() = static_cast<char>(bytes[9]);
    return leaf;
}

[[nodiscard]] std::optional<std::string> locate_source_file(
    capsule::Vfs& vfs, const std::string& source, bool search_compressed) {
    if (is_regular_file(vfs, source)) return source;
    if (!search_compressed || source.empty()) return std::nullopt;
    std::string alt = source;
    alt.back() = '_';
    if (alt != source && is_regular_file(vfs, alt)) return alt;
    alt.back() = '$';
    if (alt != source && is_regular_file(vfs, alt)) return alt;
    return std::nullopt;
}

[[nodiscard]] bool expand_szdd(
    std::span<const u8> source, std::vector<u8>& output) {
    constexpr std::array<u8, 8> signature{
        'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33};
    if (source.size() < signature.size() ||
        !std::equal(signature.begin(), signature.end(), source.begin())) {
        output.assign(source.begin(), source.end());
        return true;
    }
    if (source.size() < 14 || source[8] != 'A') return false;
    const u32 expanded_size =
        static_cast<u32>(source[10]) |
        (static_cast<u32>(source[11]) << 8) |
        (static_cast<u32>(source[12]) << 16) |
        (static_cast<u32>(source[13]) << 24);
    constexpr u32 maximum_expanded_size = 512u * 1024u * 1024u;
    if (expanded_size == 0 || expanded_size > maximum_expanded_size) {
        return false;
    }
    output.clear();
    output.reserve(expanded_size);
    std::array<u8, 4096> window{};
    window.fill(0x20);
    usize window_position = 0xFF0;
    usize input_position = 14;
    while (output.size() < expanded_size) {
        if (input_position >= source.size()) return false;
        const u8 control = source[input_position++];
        for (u8 bit = 0; bit < 8 && output.size() < expanded_size; ++bit) {
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
                 index < match_length && output.size() < expanded_size;
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

[[nodiscard]] bool read_vfs_file(
    capsule::Vfs& vfs, const std::string& path, std::vector<u8>& bytes) {
    auto opened = vfs.open(
        path, capsule::AccessMode::Read, capsule::ShareMode::Read,
        capsule::CreationDisposition::OpenExisting);
    if (opened.is_err()) return false;
    bytes.clear();
    std::array<u8, 64 * 1024> chunk{};
    for (;;) {
        auto read = vfs.read(opened.value(), chunk);
        if (read.is_err()) {
            (void)vfs.close(opened.value());
            return false;
        }
        if (read.value() == 0) break;
        bytes.insert(
            bytes.end(), chunk.begin(),
            chunk.begin() + static_cast<std::ptrdiff_t>(read.value()));
    }
    (void)vfs.close(opened.value());
    return true;
}

[[nodiscard]] bool write_vfs_file(
    capsule::Vfs& vfs, const std::string& path, std::span<const u8> bytes) {
    auto opened = vfs.open(
        path, capsule::AccessMode::Write, capsule::ShareMode::None,
        capsule::CreationDisposition::CreateAlways);
    if (opened.is_err()) return false;
    usize offset = 0;
    while (offset < bytes.size()) {
        auto written = vfs.write(opened.value(), bytes.subspan(offset));
        if (written.is_err() || written.value() == 0) {
            (void)vfs.close(opened.value());
            return false;
        }
        offset += written.value();
    }
    (void)vfs.close(opened.value());
    return true;
}

} // namespace

namespace setupapi_internal {

bool vfs_read_all(capsule::Vfs& vfs, const std::string& path, std::vector<u8>& bytes) {
    return read_vfs_file(vfs, path, bytes);
}

bool vfs_write_all(capsule::Vfs& vfs, const std::string& path, std::span<const u8> bytes) {
    return write_vfs_file(vfs, path, bytes);
}

bool vfs_is_file(capsule::Vfs& vfs, const std::string& path) {
    return is_regular_file(vfs, path);
}

} // namespace setupapi_internal

using setupapi_internal::join_win_path;

u32 setup_decompress_or_copy_file(
    capsule::Vfs& vfs,
    const std::string& source_file_name,
    const std::string& target_file_name,
    const u32* compression_type) {
    // https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdecompressorcopyfilea
    if (source_file_name.empty() || target_file_name.empty()) {
        return error_codes::ERROR_INVALID_PARAMETER;
    }
    if (compression_type != nullptr &&
        (*compression_type == file_compression::MSZIP ||
         *compression_type == file_compression::NTCAB)) {
        return error_codes::ERROR_NOT_SUPPORTED;
    }

    const bool search_compressed = compression_type == nullptr;
    const auto located =
        locate_source_file(vfs, source_file_name, search_compressed);
    if (!located) return error_codes::ERROR_FILE_NOT_FOUND;

    std::vector<u8> source_bytes;
    if (!read_vfs_file(vfs, *located, source_bytes)) {
        return error_codes::ERROR_ACCESS_DENIED;
    }

    const bool copy_only =
        compression_type != nullptr &&
        *compression_type == file_compression::NONE;
    std::vector<u8> output;
    if (copy_only) {
        output = std::move(source_bytes);
    } else if (!expand_szdd(source_bytes, output)) {
        return error_codes::ERROR_INVALID_DATA;
    }
    if (!write_vfs_file(vfs, target_file_name, output)) {
        return error_codes::ERROR_ACCESS_DENIED;
    }
    VITREO_TRACE(
        "[setupapi] SetupDecompressOrCopyFile src='%s' located='%s' "
        "dst='%s' bytes=%zu\n",
        source_file_name.c_str(), located->c_str(),
        target_file_name.c_str(), output.size());
    return error_codes::ERROR_SUCCESS;
}

u32 setup_install_file(
    capsule::Vfs& vfs,
    const std::string& source_file,
    const std::string& source_path_root,
    const std::string& destination_name,
    u32 copy_style) {
    // https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupinstallfilea
    if (source_file.empty() || destination_name.empty()) {
        return error_codes::ERROR_INVALID_PARAMETER;
    }

    const bool nodecomp = (copy_style & sp_copy::NODECOMP) != 0;
    const bool source_absolute =
        (copy_style & sp_copy::SOURCE_ABSOLUTE) != 0;
    const std::string source = source_absolute
        ? source_file
        : join_win_path(source_path_root, source_file);

    const auto located = locate_source_file(vfs, source, !nodecomp);
    if (!located) return error_codes::ERROR_FILE_NOT_FOUND;

    const bool dest_is_dir = is_directory_path(vfs, destination_name);
    std::string dest_dir;
    std::string dest_leaf;
    if (nodecomp) {
        // Dest leaf is the source name; DestinationName's file name is stripped.
        dest_leaf = path_leaf(*located);
        dest_dir = dest_is_dir
            ? trim_trailing_slashes(destination_name)
            : path_dir(destination_name);
    } else if (dest_is_dir) {
        std::vector<u8> header;
        if (!read_vfs_file(vfs, *located, header)) {
            return error_codes::ERROR_ACCESS_DENIED;
        }
        dest_leaf = uncompressed_source_leaf(*located, header);
        dest_dir = trim_trailing_slashes(destination_name);
    } else {
        dest_dir = path_dir(destination_name);
        dest_leaf = path_leaf(destination_name);
    }
    if (dest_leaf.empty()) return error_codes::ERROR_INVALID_PARAMETER;
    const std::string dest = join_win_path(dest_dir, dest_leaf);

    if ((copy_style & (sp_copy::NOOVERWRITE | sp_copy::FORCE_NOOVERWRITE)) !=
            0 &&
        is_regular_file(vfs, dest)) {
        return error_codes::ERROR_FILE_EXISTS;
    }

    const u32 none = file_compression::NONE;
    const u32 result = setup_decompress_or_copy_file(
        vfs, *located, dest, nodecomp ? &none : nullptr);
    if (result != error_codes::ERROR_SUCCESS) return result;

    if ((copy_style & sp_copy::DELETESOURCE) != 0) {
        (void)vfs.remove(*located);
    }
    return error_codes::ERROR_SUCCESS;
}

} // namespace vitreo::winapi
