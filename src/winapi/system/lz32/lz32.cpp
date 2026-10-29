/// @file lz32.cpp
/// @brief Implementation of the legacy LZ32 file APIs

#include "lz32.hpp"

#include "capsule/vfs/vfs.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u16 OF_WRITE = 0x0001;
constexpr u16 OF_READWRITE = 0x0002;
constexpr u16 OF_SHARE_EXCLUSIVE = 0x0010;
constexpr u16 OF_SHARE_DENY_WRITE = 0x0020;
constexpr u16 OF_SHARE_DENY_READ = 0x0030;
constexpr u16 OF_SHARE_DENY_NONE = 0x0040;
constexpr u16 OF_CREATE = 0x1000;

#pragma pack(push, 1)
struct OfStruct {
    u8 c_bytes{sizeof(OfStruct)};
    u8 fixed_disk{1};
    u16 error{0};
    u16 reserved1{0};
    u16 reserved2{0};
    char path[128]{};
};
#pragma pack(pop)

static_assert(sizeof(OfStruct) == 136);

[[nodiscard]] u16 dos_error(capsule::VfsError error) {
    switch (error) {
        case capsule::VfsError::FileNotFound: return 2;
        case capsule::VfsError::PathNotFound: return 3;
        case capsule::VfsError::TooManyOpenFiles: return 4;
        case capsule::VfsError::AccessDenied: return 5;
        case capsule::VfsError::InvalidHandle:
        case capsule::VfsError::HandleClosed: return 6;
        case capsule::VfsError::OutOfMemory: return 8;
        case capsule::VfsError::InvalidDriveLetter:
        case capsule::VfsError::DriveNotMounted: return 15;
        case capsule::VfsError::SharingViolation: return 32;
        case capsule::VfsError::PathAlreadyExists: return 80;
        default: return 1;
    }
}

[[nodiscard]] i32 open_error(capsule::VfsError error) {
    if (error == capsule::VfsError::OutOfMemory) return lz_errors::GLOBALLOC;
    return lz_errors::BADVALUE;
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

[[nodiscard]] capsule::ShareMode share_mode(u16 style) {
    switch (style & 0x0070u) {
        case OF_SHARE_EXCLUSIVE: return capsule::ShareMode::None;
        case OF_SHARE_DENY_WRITE: return capsule::ShareMode::Read;
        case OF_SHARE_DENY_READ: return capsule::ShareMode::Write;
        case OF_SHARE_DENY_NONE: return capsule::ShareMode::ReadWrite;
        default: return capsule::ShareMode::Read;
    }
}

} // namespace

Lz32Manager::~Lz32Manager() {
    for (const auto& [handle, file] : files_) {
        (void)handle;
        (void)vfs_.close(file.vfs_handle);
    }
}

LzOpenResult Lz32Manager::open_file(const std::string& path, u16 style) {
    const u16 access_bits = style & 0x0003u;
    const bool create = (style & OF_CREATE) != 0;
    capsule::AccessMode access = capsule::AccessMode::Read;
    bool readable = true;
    bool writable = false;
    if (create || access_bits == OF_WRITE) {
        access = capsule::AccessMode::Write;
        readable = false;
        writable = true;
    } else if (access_bits == OF_READWRITE) {
        access = capsule::AccessMode::ReadWrite;
        writable = true;
    }

    auto opened = vfs_.open(
        path, access, share_mode(style),
        create ? capsule::CreationDisposition::CreateAlways
               : capsule::CreationDisposition::OpenExisting);
    std::string opened_path = path;
    if (opened.is_err() && !create && !path.empty()) {
        std::string compressed_path = path;
        compressed_path.back() = '_';
        if (compressed_path != path) {
            opened = vfs_.open(
                compressed_path, access, share_mode(style),
                capsule::CreationDisposition::OpenExisting);
            if (opened.is_ok()) opened_path = std::move(compressed_path);
        }
    }
    if (opened.is_err()) {
        VITREO_TRACE(
            "[lz32] LZOpenFileA path='%s' style=0x%04x error=%s -> %d\n",
            path.c_str(), style, capsule::vfs_error_to_string(opened.error()),
            open_error(opened.error()));
        return {open_error(opened.error()), dos_error(opened.error()), path};
    }

    const i32 handle = next_handle_++;
    files_.emplace(handle, OpenFile{opened.value(), readable, writable,
                                    opened_path});
    VITREO_TRACE(
        "[lz32] LZOpenFile path='%s' opened='%s' style=0x%04x -> %d\n",
        path.c_str(), opened_path.c_str(), style, handle);
    return {handle, 0, std::move(opened_path)};
}

ExpandedNameResult Lz32Manager::expanded_name(const std::string& source) {
    // https://learn.microsoft.com/en-us/windows/win32/api/lzexpand/nf-lzexpand-getexpandednamea
    // Success is 1; failure is LZERROR_BADVALUE. Does not SetLastError.
    // COMPRESS.EXE /r stores the replaced extension character at SZDD header[9].
    // Without /r, or when the file is not SZDD, the source name is duplicated.
    // OpenFile (used by GetExpandedName) falls back to a trailing '_' the same
    // way LZOpenFile does, so callers that pass the original name still recover
    // the header from the compressed media name.
    auto opened = vfs_.open(
        source, capsule::AccessMode::Read, capsule::ShareMode::Read,
        capsule::CreationDisposition::OpenExisting);
    if (opened.is_err() && !source.empty()) {
        std::string compressed_path = source;
        compressed_path.back() = '_';
        if (compressed_path != source) {
            opened = vfs_.open(
                compressed_path, capsule::AccessMode::Read,
                capsule::ShareMode::Read,
                capsule::CreationDisposition::OpenExisting);
        }
    }
    if (opened.is_err()) {
        VITREO_TRACE(
            "[lz32] GetExpandedName path='%s' error=%s -> %d\n",
            source.c_str(), capsule::vfs_error_to_string(opened.error()),
            lz_errors::BADVALUE);
        return {};
    }

    std::array<u8, 14> header{};
    const auto read = vfs_.read(opened.value(), header);
    (void)vfs_.close(opened.value());

    std::string name = source;
    constexpr std::array<u8, 8> signature{
        'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33};
    if (read.is_ok() && read.value() >= header.size() &&
        std::equal(signature.begin(), signature.end(), header.begin()) &&
        header[8] == 'A' && !name.empty()) {
        name.back() = static_cast<char>(header[9]);
    }
    VITREO_TRACE(
        "[lz32] GetExpandedName path='%s' -> '%s'\n",
        source.c_str(), name.c_str());
    return {1, std::move(name)};
}

i32 Lz32Manager::copy(i32 source, i32 destination) {
    const auto source_file = files_.find(source);
    if (source_file == files_.end() || !source_file->second.readable) {
        return lz_errors::BADINHANDLE;
    }
    const auto destination_file = files_.find(destination);
    if (destination_file == files_.end() || !destination_file->second.writable) {
        return lz_errors::BADOUTHANDLE;
    }

    std::vector<u8> source_bytes;
    std::array<u8, 64 * 1024> chunk{};
    for (;;) {
        auto read = vfs_.read(source_file->second.vfs_handle, chunk);
        if (read.is_err()) return lz_errors::READ;
        if (read.value() == 0) break;
        source_bytes.insert(
            source_bytes.end(), chunk.begin(),
            chunk.begin() + static_cast<std::ptrdiff_t>(read.value()));
    }

    std::vector<u8> output;
    if (!expand_szdd(source_bytes, output)) {
        VITREO_TRACE(
            "[lz32] LZCopy source=%d destination=%d input=%zu -> %d\n",
            source, destination, source_bytes.size(), lz_errors::UNKNOWNALG);
        return lz_errors::UNKNOWNALG;
    }
    if (output.size() > static_cast<usize>(std::numeric_limits<i32>::max())) {
        return lz_errors::GLOBALLOC;
    }
    usize offset = 0;
    while (offset < output.size()) {
        auto written = vfs_.write(
            destination_file->second.vfs_handle,
            std::span<const u8>(output).subspan(offset));
        if (written.is_err() || written.value() == 0) return lz_errors::WRITE;
        offset += written.value();
    }
    const i32 result = static_cast<i32>(output.size());
    VITREO_TRACE(
        "[lz32] LZCopy source=%d destination=%d input=%zu output=%zu -> %d\n",
        source, destination, source_bytes.size(), output.size(), result);
    return result;
}

void Lz32Manager::close(i32 handle) {
    const auto file = files_.find(handle);
    if (file == files_.end()) return;
    (void)vfs_.close(file->second.vfs_handle);
    files_.erase(file);
}

DllExports create_lz32_thunks(Lz32Manager& manager) {
    const auto open_file = [&manager](bool wide) {
        return [&manager, wide](Marshaler& m) {
            const auto path = wide ? m.read_wide_string(m.arg_ptr(0))
                                   : m.read_ansi_string(m.arg_ptr(0));
            const GuestAddr ofstruct_address = m.arg_ptr(1);
            const u16 style = static_cast<u16>(m.arg(2));
            LzOpenResult result;
            if (path.is_ok() && ofstruct_address != 0) {
                result = manager.open_file(path.value(), style);
                OfStruct ofstruct;
                ofstruct.error = result.error;
                const usize length =
                    std::min(result.path.size(), sizeof(ofstruct.path) - 1);
                std::memcpy(ofstruct.path, result.path.data(), length);
                if (m.write_struct(ofstruct_address, ofstruct).is_err() &&
                    result.handle >= 0) {
                    manager.close(result.handle);
                    result.handle = lz_errors::BADVALUE;
                }
            }
            m.set_return(static_cast<u32>(result.handle));
        };
    };
    const auto expanded_name = [&manager](bool wide) {
        return [&manager, wide](Marshaler& m) {
            const auto path = wide ? m.read_wide_string(m.arg_ptr(0))
                                   : m.read_ansi_string(m.arg_ptr(0));
            const GuestAddr buffer = m.arg_ptr(1);
            if (path.is_err() || buffer == 0) {
                m.set_return(static_cast<u32>(lz_errors::BADVALUE));
                return;
            }
            auto result = manager.expanded_name(path.value());
            if (result.code == 1) {
                const auto written = wide
                    ? m.write_wide_string(buffer, result.name)
                    : m.write_ansi_string(buffer, result.name);
                if (written.is_err()) result.code = lz_errors::BADVALUE;
            }
            m.set_return(static_cast<u32>(result.code));
        };
    };

    DllExports exports;
    exports.entries.reserve(6);
    exports.entries.push_back({"LZOpenFileA", 9, open_file(false), 12});
    exports.entries.push_back({"LZOpenFileW", 10, open_file(true), 12});
    exports.entries.push_back({"LZCopy", 5,
        [&manager](Marshaler& m) {
            const i32 result = manager.copy(
                static_cast<i32>(m.arg(0)), static_cast<i32>(m.arg(1)));
            m.set_return(static_cast<u32>(result));
        }, 8});
    exports.entries.push_back({"LZClose", 3,
        [&manager](Marshaler& m) {
            manager.close(static_cast<i32>(m.arg(0)));
            m.set_return(0);
        }, 4});
    // GetExpandedNameA/W: two LPSTR/LPWSTR arguments, stdcall 8.
    // https://learn.microsoft.com/en-us/windows/win32/api/lzexpand/nf-lzexpand-getexpandednamea
    exports.entries.push_back({"GetExpandedNameA", 7, expanded_name(false), 8});
    exports.entries.push_back({"GetExpandedNameW", 8, expanded_name(true), 8});
    return exports;
}

} // namespace vitreo::winapi