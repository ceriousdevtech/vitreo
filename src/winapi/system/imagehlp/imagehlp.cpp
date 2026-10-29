/// @file imagehlp.cpp
/// @brief ImageHlp PE certificate-table APIs

#include "imagehlp.hpp"

#include "capsule/vfs/vfs.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/system/dbghelp/dbghelp.hpp"

#include <algorithm>
#include <limits>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u16 CERT_SECTION_TYPE_ANY = 0x00FF;
constexpr usize WIN_CERTIFICATE_HEADER_SIZE = 8;
constexpr usize MAX_CERTIFICATE_SIZE = 64u * 1024u * 1024u;

struct CertificateEntry {
    usize offset{0};
    usize padded_size{0};
    u16 type{0};
};

struct PeCertificateTable {
    usize directory_entry_offset{0};
    u32 file_offset{0};
    u32 size{0};
    std::vector<CertificateEntry> entries;
};

[[nodiscard]] u16 read_u16(std::span<const u8> bytes, usize offset) {
    return static_cast<u16>(bytes[offset]) |
           static_cast<u16>(bytes[offset + 1] << 8);
}

[[nodiscard]] u32 read_u32(std::span<const u8> bytes, usize offset) {
    return static_cast<u32>(bytes[offset]) |
           (static_cast<u32>(bytes[offset + 1]) << 8) |
           (static_cast<u32>(bytes[offset + 2]) << 16) |
           (static_cast<u32>(bytes[offset + 3]) << 24);
}

void write_u32(std::span<u8> bytes, usize offset, u32 value) {
    for (usize index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<u8>(value >> (index * 8));
    }
}

[[nodiscard]] bool add_overflows(usize left, usize right) {
    return right > std::numeric_limits<usize>::max() - left;
}

[[nodiscard]] bool align_up_8(usize value, usize& aligned) {
    if (value > std::numeric_limits<usize>::max() - 7) return false;
    aligned = (value + 7) & ~usize{7};
    return true;
}

[[nodiscard]] bool parse_certificate_table(
    std::span<const u8> image, PeCertificateTable& table) {
    // IMAGE_DOS_HEADER.e_lfanew is at 0x3c.
    if (image.size() < 0x40 || image[0] != 'M' || image[1] != 'Z') {
        return false;
    }
    const usize pe_offset = read_u32(image, 0x3c);
    if (add_overflows(pe_offset, 24) || pe_offset + 24 > image.size() ||
        image[pe_offset] != 'P' || image[pe_offset + 1] != 'E' ||
        image[pe_offset + 2] != 0 || image[pe_offset + 3] != 0) {
        return false;
    }

    const usize optional_offset = pe_offset + 24;
    const usize optional_size = read_u16(image, pe_offset + 20);
    if (add_overflows(optional_offset, optional_size) ||
        optional_offset + optional_size > image.size() || optional_size < 2) {
        return false;
    }

    const u16 magic = read_u16(image, optional_offset);
    usize number_of_directories_offset = 0;
    usize data_directories_offset = 0;
    if (magic == 0x10B) { // PE32
        number_of_directories_offset = optional_offset + 92;
        data_directories_offset = optional_offset + 96;
    } else if (magic == 0x20B) { // PE32+
        number_of_directories_offset = optional_offset + 108;
        data_directories_offset = optional_offset + 112;
    } else {
        return false;
    }

    constexpr usize SECURITY_DIRECTORY_INDEX = 4;
    const usize directory_entry =
        data_directories_offset + SECURITY_DIRECTORY_INDEX * 8;
    if (number_of_directories_offset + 4 > optional_offset + optional_size ||
        directory_entry + 8 > optional_offset + optional_size) {
        return false;
    }
    if (read_u32(image, number_of_directories_offset) <=
        SECURITY_DIRECTORY_INDEX) {
        return false;
    }

    table.directory_entry_offset = directory_entry;
    table.file_offset = read_u32(image, directory_entry);
    table.size = read_u32(image, directory_entry + 4);
    table.entries.clear();
    if (table.file_offset == 0 && table.size == 0) return true;
    if (table.file_offset == 0 || table.size == 0 ||
        table.file_offset < directory_entry + 8 ||
        static_cast<u64>(table.file_offset) + table.size > image.size()) {
        return false;
    }

    const usize end = static_cast<usize>(table.file_offset) + table.size;
    usize offset = table.file_offset;
    while (offset < end) {
        if (end - offset < WIN_CERTIFICATE_HEADER_SIZE) return false;
        const u32 length = read_u32(image, offset);
        usize padded_size = 0;
        if (length < WIN_CERTIFICATE_HEADER_SIZE ||
            !align_up_8(length, padded_size) || padded_size > end - offset) {
            return false;
        }
        table.entries.push_back(
            {offset, padded_size, read_u16(image, offset + 6)});
        offset += padded_size;
    }
    return offset == end;
}

[[nodiscard]] bool has_access(FileManager& files, Handle handle, u32 mask) {
    if (files.handle_table().get_type(handle) != HandleType::File ||
        files.vfs_handle_for(handle) == capsule::INVALID_FILE_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    const u32 granted = files.handle_table().get_access(handle);
    if ((granted & mask) == 0 && granted != HandleAccess::All) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return false;
    }
    return true;
}

[[nodiscard]] bool read_image(
    FileManager& files, Handle handle, std::vector<u8>& image,
    u64& original_position) {
    auto& vfs = files.vfs();
    const auto vfs_handle = files.vfs_handle_for(handle);
    auto current = vfs.seek(vfs_handle, 0, capsule::SeekOrigin::Current);
    if (current.is_err()) {
        set_last_error(vfs_error_to_win32(current.error()));
        return false;
    }
    original_position = current.value();
    auto end = vfs.seek(vfs_handle, 0, capsule::SeekOrigin::End);
    if (end.is_err() || end.value() > std::numeric_limits<usize>::max()) {
        (void)vfs.seek(vfs_handle, static_cast<i64>(original_position),
                       capsule::SeekOrigin::Begin);
        set_last_error(end.is_err() ? vfs_error_to_win32(end.error())
                                    : error_codes::ERROR_FILE_TOO_LARGE);
        return false;
    }
    image.resize(static_cast<usize>(end.value()));
    auto begin = vfs.seek(vfs_handle, 0, capsule::SeekOrigin::Begin);
    if (begin.is_err()) {
        set_last_error(vfs_error_to_win32(begin.error()));
        return false;
    }
    usize offset = 0;
    while (offset < image.size()) {
        auto result = vfs.read(
            vfs_handle, std::span<u8>(image).subspan(offset));
        if (result.is_err() || result.value() == 0) {
            (void)vfs.seek(vfs_handle, static_cast<i64>(original_position),
                           capsule::SeekOrigin::Begin);
            set_last_error(result.is_err()
                               ? vfs_error_to_win32(result.error())
                               : error_codes::ERROR_HANDLE_EOF);
            return false;
        }
        offset += result.value();
    }
    (void)vfs.seek(vfs_handle, static_cast<i64>(original_position),
                   capsule::SeekOrigin::Begin);
    return true;
}

[[nodiscard]] bool write_image(
    FileManager& files, Handle handle, std::span<const u8> image,
    u64 original_position) {
    auto& vfs = files.vfs();
    const auto vfs_handle = files.vfs_handle_for(handle);
    auto begin = vfs.seek(vfs_handle, 0, capsule::SeekOrigin::Begin);
    if (begin.is_err()) {
        set_last_error(vfs_error_to_win32(begin.error()));
        return false;
    }
    usize offset = 0;
    while (offset < image.size()) {
        auto result = vfs.write(vfs_handle, image.subspan(offset));
        if (result.is_err() || result.value() == 0) {
            (void)vfs.seek(vfs_handle, static_cast<i64>(original_position),
                           capsule::SeekOrigin::Begin);
            set_last_error(result.is_err()
                               ? vfs_error_to_win32(result.error())
                               : error_codes::ERROR_WRITE_FAULT);
            return false;
        }
        offset += result.value();
    }
    auto truncated = vfs.truncate(vfs_handle, image.size());
    if (truncated.is_err()) {
        set_last_error(vfs_error_to_win32(truncated.error()));
        return false;
    }
    const u64 restored = std::min<u64>(original_position, image.size());
    (void)vfs.seek(vfs_handle, static_cast<i64>(restored),
                   capsule::SeekOrigin::Begin);
    return true;
}

[[nodiscard]] bool fail_bad_image() {
    set_last_error(error_codes::ERROR_BAD_EXE_FORMAT);
    return false;
}

} // namespace

bool ImageHlpManager::enumerate_certificates(
    Marshaler& marshaler, Handle file_handle, u16 type_filter,
    GuestAddr certificate_count, GuestAddr indices, u32 index_count) {
    std::lock_guard lock(mutex_);
    constexpr u32 FILE_READ_DATA = 0x0001;
    if (!has_access(files_, file_handle, GENERIC_READ | FILE_READ_DATA)) {
        return false;
    }
    if (certificate_count == 0 || (indices == 0 && index_count != 0)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }

    std::vector<u8> image;
    u64 original_position = 0;
    if (!read_image(files_, file_handle, image, original_position)) return false;
    PeCertificateTable table;
    if (!parse_certificate_table(image, table)) return fail_bad_image();

    std::vector<u32> matching_indices;
    for (usize index = 0; index < table.entries.size(); ++index) {
        if (type_filter == CERT_SECTION_TYPE_ANY ||
            table.entries[index].type == type_filter) {
            matching_indices.push_back(static_cast<u32>(index));
        }
    }
    if (marshaler.write_u32(certificate_count,
                            static_cast<u32>(matching_indices.size())).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return false;
    }
    const usize output_count =
        indices == 0 ? 0 : std::min<usize>(index_count, matching_indices.size());
    for (usize index = 0; index < output_count; ++index) {
        if (marshaler.write_u32(indices + index * sizeof(u32),
                                matching_indices[index]).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            return false;
        }
    }
    set_last_error(error_codes::ERROR_SUCCESS);
    return true;
}

bool ImageHlpManager::add_certificate(
    Marshaler& marshaler, Handle file_handle, GuestAddr certificate,
    GuestAddr index) {
    std::lock_guard lock(mutex_);
    constexpr u32 FILE_READ_DATA = 0x0001;
    constexpr u32 FILE_WRITE_DATA = 0x0002;
    if (!has_access(files_, file_handle,
                    GENERIC_READ | GENERIC_WRITE |
                        FILE_READ_DATA | FILE_WRITE_DATA)) {
        return false;
    }
    const u32 granted = files_.handle_table().get_access(file_handle);
    if (granted != HandleAccess::All &&
        ((granted & (GENERIC_READ | FILE_READ_DATA)) == 0 ||
         (granted & (GENERIC_WRITE | FILE_WRITE_DATA)) == 0)) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return false;
    }
    if (certificate == 0 || index == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    auto length_result = marshaler.read_u32(certificate);
    if (length_result.is_err() ||
        length_result.value() < WIN_CERTIFICATE_HEADER_SIZE ||
        length_result.value() > MAX_CERTIFICATE_SIZE) {
        set_last_error(length_result.is_err() ? error_codes::ERROR_NOACCESS
                                               : error_codes::ERROR_INVALID_DATA);
        return false;
    }
    usize padded_size = 0;
    if (!align_up_8(length_result.value(), padded_size)) {
        set_last_error(error_codes::ERROR_INVALID_DATA);
        return false;
    }
    std::vector<u8> certificate_bytes(padded_size, 0);
    if (marshaler.memory().read_bytes(
            certificate, reinterpret_cast<Byte*>(certificate_bytes.data()),
            length_result.value()).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return false;
    }

    std::vector<u8> image;
    u64 original_position = 0;
    if (!read_image(files_, file_handle, image, original_position)) return false;
    PeCertificateTable table;
    if (!parse_certificate_table(image, table)) return fail_bad_image();
    if (table.size > std::numeric_limits<u32>::max() - padded_size) {
        set_last_error(error_codes::ERROR_FILE_TOO_LARGE);
        return false;
    }

    usize insertion_offset = 0;
    if (table.size == 0) {
        if (!align_up_8(image.size(), insertion_offset) ||
            insertion_offset > std::numeric_limits<u32>::max()) {
            set_last_error(error_codes::ERROR_FILE_TOO_LARGE);
            return false;
        }
        image.resize(insertion_offset, 0);
        table.file_offset = static_cast<u32>(insertion_offset);
    } else {
        insertion_offset = static_cast<usize>(table.file_offset) + table.size;
    }
    image.insert(image.begin() + static_cast<std::ptrdiff_t>(insertion_offset),
                 certificate_bytes.begin(), certificate_bytes.end());
    table.size += static_cast<u32>(padded_size);
    write_u32(image, table.directory_entry_offset, table.file_offset);
    write_u32(image, table.directory_entry_offset + 4, table.size);

    if (!write_image(files_, file_handle, image, original_position)) return false;
    if (marshaler.write_u32(index,
                            static_cast<u32>(table.entries.size())).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return false;
    }
    set_last_error(error_codes::ERROR_SUCCESS);
    return true;
}

bool ImageHlpManager::remove_certificate(Handle file_handle, u32 index) {
    std::lock_guard lock(mutex_);
    constexpr u32 FILE_READ_DATA = 0x0001;
    constexpr u32 FILE_WRITE_DATA = 0x0002;
    if (!has_access(files_, file_handle,
                    GENERIC_READ | GENERIC_WRITE |
                        FILE_READ_DATA | FILE_WRITE_DATA)) {
        return false;
    }
    const u32 granted = files_.handle_table().get_access(file_handle);
    if (granted != HandleAccess::All &&
        ((granted & (GENERIC_READ | FILE_READ_DATA)) == 0 ||
         (granted & (GENERIC_WRITE | FILE_WRITE_DATA)) == 0)) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return false;
    }

    std::vector<u8> image;
    u64 original_position = 0;
    if (!read_image(files_, file_handle, image, original_position)) return false;
    PeCertificateTable table;
    if (!parse_certificate_table(image, table)) return fail_bad_image();
    if (index >= table.entries.size()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }

    const CertificateEntry entry = table.entries[index];
    image.erase(image.begin() + static_cast<std::ptrdiff_t>(entry.offset),
                image.begin() + static_cast<std::ptrdiff_t>(
                                    entry.offset + entry.padded_size));
    table.size -= static_cast<u32>(entry.padded_size);
    if (table.size == 0) table.file_offset = 0;
    write_u32(image, table.directory_entry_offset, table.file_offset);
    write_u32(image, table.directory_entry_offset + 4, table.size);
    if (!write_image(files_, file_handle, image, original_position)) return false;
    set_last_error(error_codes::ERROR_SUCCESS);
    return true;
}

DllExports create_imagehlp_thunks(ImageHlpManager& manager) {
    DllExports exports;
    exports.entries.push_back({"ImageAddCertificate", 0,
        [&manager](Marshaler& marshaler) {
            marshaler.set_return(manager.add_certificate(
                marshaler, static_cast<Handle>(marshaler.arg(0)),
                marshaler.arg_ptr(1), marshaler.arg_ptr(2)) ? 1 : 0);
        }, 12});
    exports.entries.push_back({"ImageRemoveCertificate", 0,
        [&manager](Marshaler& marshaler) {
            marshaler.set_return(manager.remove_certificate(
                static_cast<Handle>(marshaler.arg(0)),
                static_cast<u32>(marshaler.arg(1))) ? 1 : 0);
        }, 8});
    exports.entries.push_back({"ImageEnumerateCertificates", 0,
        [&manager](Marshaler& marshaler) {
            marshaler.set_return(manager.enumerate_certificates(
                marshaler, static_cast<Handle>(marshaler.arg(0)),
                static_cast<u16>(marshaler.arg(1)), marshaler.arg_ptr(2),
                marshaler.arg_ptr(3), static_cast<u32>(marshaler.arg(4)))
                                     ? 1
                                     : 0);
        }, 20});
    // imagehlp's Sym*/StackWalk* exports are forwarders to dbghelp on
    // Windows: publish dbghelp's own entries under every name the real
    // imagehlp.dll exports, so the two cannot drift (a private SymFromAddr
    // copy here had the wrong x86 stdcall size).
    static const std::unordered_set<std::string_view> kImagehlpNames = {
#include "winapi/system/imagehlp/imagehlp_export_names.inc"
    };
    for (auto& entry : create_dbghelp_thunks().entries) {
        if (kImagehlpNames.contains(entry.name)) {
            exports.entries.push_back(std::move(entry));
        }
    }
    return exports;
}

} // namespace vitreo::winapi
