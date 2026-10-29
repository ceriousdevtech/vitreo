// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file synthetic_version_resource.cpp
/// @brief See synthetic_version_resource.hpp.

#include "winapi/common/synthetic_version_resource.hpp"

#include <limits>
#include <string>

namespace vitreo::winapi {

namespace {

using VersionBytes = std::vector<u8>;

void append_le16(VersionBytes& bytes, u16 value) {
    bytes.push_back(static_cast<u8>(value));
    bytes.push_back(static_cast<u8>(value >> 8));
}

void put_le16(VersionBytes& bytes, usize offset, u16 value) {
    bytes[offset] = static_cast<u8>(value);
    bytes[offset + 1] = static_cast<u8>(value >> 8);
}

void put_le32(VersionBytes& bytes, usize offset, u32 value) {
    for (usize index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] =
            static_cast<u8>(value >> (index * 8));
    }
}

void append_utf16_z(VersionBytes& bytes, std::u16string_view value) {
    for (const char16_t unit : value) {
        append_le16(bytes, static_cast<u16>(unit));
    }
    append_le16(bytes, 0);
}

void align_version_block(VersionBytes& bytes) {
    while ((bytes.size() & 3u) != 0) bytes.push_back(0);
}

[[nodiscard]] VersionBytes make_version_block(
    std::u16string_view key,
    u16 value_length,
    u16 type,
    const VersionBytes& value,
    const std::vector<VersionBytes>& children = {}) {
    VersionBytes block(6, 0);
    put_le16(block, 2, value_length);
    put_le16(block, 4, type);
    append_utf16_z(block, key);
    align_version_block(block);
    block.insert(block.end(), value.begin(), value.end());
    align_version_block(block);
    for (const auto& child : children) {
        block.insert(block.end(), child.begin(), child.end());
        align_version_block(block);
    }
    if (block.size() > std::numeric_limits<u16>::max()) return {};
    put_le16(block, 0, static_cast<u16>(block.size()));
    return block;
}

[[nodiscard]] std::u16string ascii_to_utf16(std::string_view value) {
    std::u16string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        result.push_back(static_cast<char16_t>(character));
    }
    return result;
}

[[nodiscard]] VersionBytes make_string_version_block(
    std::u16string_view key, std::u16string_view value) {
    VersionBytes encoded;
    append_utf16_z(encoded, value);
    return make_version_block(
        key, static_cast<u16>(value.size() + 1), 1, encoded);
}

}  // namespace

std::vector<u8> make_synthetic_system_version_resource(
    std::string_view module_name) {
    // Keep this consistent with RtlGetVersion/GetVersionEx. This describes
    // Vitreo's Windows-compatible synthetic module, not a copied host binary.
    constexpr u16 major = 10;
    constexpr u16 minor = 0;
    constexpr u16 build = 19041;
    constexpr u16 revision = 1;

    VersionBytes fixed_info(52, 0);
    put_le32(fixed_info, 0, 0xFEEF04BDu);
    put_le32(fixed_info, 4, 0x00010000u);
    put_le32(
        fixed_info, 8,
        (static_cast<u32>(major) << 16) | minor);
    put_le32(
        fixed_info, 12,
        (static_cast<u32>(build) << 16) | revision);
    put_le32(
        fixed_info, 16,
        (static_cast<u32>(major) << 16) | minor);
    put_le32(
        fixed_info, 20,
        (static_cast<u32>(build) << 16) | revision);
    put_le32(fixed_info, 24, 0x0000003Fu);
    put_le32(fixed_info, 32, 0x00040004u); // VOS_NT_WINDOWS32
    put_le32(fixed_info, 36, 0x00000002u); // VFT_DLL

    const std::u16string module = ascii_to_utf16(module_name);
    const std::vector<VersionBytes> strings{
        make_string_version_block(u"CompanyName", u"Vitreo Project"),
        make_string_version_block(
            u"FileDescription", u"Vitreo Windows API Runtime"),
        make_string_version_block(u"FileVersion", u"10.0.19041.1"),
        make_string_version_block(u"InternalName", module),
        make_string_version_block(u"OriginalFilename", module),
        make_string_version_block(u"ProductName", u"Vitreo Windows Runtime"),
        make_string_version_block(u"ProductVersion", u"10.0.19041.1"),
    };
    const VersionBytes string_table =
        make_version_block(u"040904B0", 0, 1, {}, strings);
    const VersionBytes string_file_info =
        make_version_block(
            u"StringFileInfo", 0, 1, {}, {string_table});

    const VersionBytes translation{0x09, 0x04, 0xB0, 0x04};
    const VersionBytes var_file_info =
        make_version_block(
            u"VarFileInfo", 0, 1, {},
            {make_version_block(
                u"Translation", 4, 0, translation)});

    return make_version_block(
        u"VS_VERSION_INFO", 52, 0, fixed_info,
        {string_file_info, var_file_info});
}

}  // namespace vitreo::winapi
