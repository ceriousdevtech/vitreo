// SPDX-License-Identifier: LGPL-2.1-only

#include <gtest/gtest.h>

#include "core/pe/parser/version_info.hpp"

#include <initializer_list>
#include <string_view>
#include <vector>

using namespace vitreo;
using namespace vitreo::pe;

namespace {

using Bytes = std::vector<u8>;

void append_u16(Bytes& bytes, u16 value) {
    bytes.push_back(static_cast<u8>(value & 0xFFu));
    bytes.push_back(static_cast<u8>(value >> 8));
}

void write_u16(Bytes& bytes, usize offset, u16 value) {
    bytes[offset] = static_cast<u8>(value & 0xFFu);
    bytes[offset + 1] = static_cast<u8>(value >> 8);
}

void align4(Bytes& bytes) {
    while ((bytes.size() & 3u) != 0) bytes.push_back(0);
}

Bytes utf16_value(std::u16string_view value) {
    Bytes bytes;
    for (const char16_t unit : value) {
        append_u16(bytes, static_cast<u16>(unit));
    }
    append_u16(bytes, 0);
    return bytes;
}

Bytes make_block(
    std::u16string_view key,
    u16 value_length,
    u16 type,
    const Bytes& value,
    std::initializer_list<Bytes> children = {}) {
    Bytes block(6, 0);
    write_u16(block, 2, value_length);
    write_u16(block, 4, type);
    for (const char16_t unit : key) {
        append_u16(block, static_cast<u16>(unit));
    }
    append_u16(block, 0);
    align4(block);
    block.insert(block.end(), value.begin(), value.end());
    align4(block);
    for (const Bytes& child : children) {
        block.insert(block.end(), child.begin(), child.end());
        align4(block);
    }
    write_u16(block, 0, static_cast<u16>(block.size()));
    return block;
}

Bytes make_string(std::u16string_view key, std::u16string_view value) {
    const Bytes encoded = utf16_value(value);
    return make_block(
        key, static_cast<u16>(value.size() + 1), 1, encoded);
}

Bytes make_table(
    std::u16string_view language,
    std::initializer_list<Bytes> strings) {
    return make_block(language, 0, 1, {}, strings);
}

Bytes make_version(std::initializer_list<Bytes> tables) {
    const Bytes string_file_info =
        make_block(u"StringFileInfo", 0, 1, {}, tables);
    return make_block(
        u"VS_VERSION_INFO", 52, 0, Bytes(52, 0),
        {string_file_info});
}

} // namespace

TEST(VersionInfoParserTest, ReadsProductNameAndFileDescription) {
    const Bytes resource = make_version({
        make_table(u"040904B0", {
            make_string(u"ProductName", u"Battle.net"),
            make_string(u"FileDescription", u"Battle.net Setup"),
        }),
    });

    const auto result = parse_version_info_strings(resource);
    EXPECT_EQ(result.product_name, "Battle.net");
    EXPECT_EQ(result.file_description, "Battle.net Setup");
}

TEST(VersionInfoParserTest, PrefersEnglishUsRegardlessOfResourceOrder) {
    const Bytes resource = make_version({
        make_table(u"040C04B0", {
            make_string(u"ProductName", u"Nom fran\u00E7ais"),
        }),
        make_table(u"040904B0", {
            make_string(u"ProductName", u"English Name"),
        }),
    });

    EXPECT_EQ(
        parse_version_info_strings(resource).product_name,
        "English Name");
}

TEST(VersionInfoParserTest, NormalizesDisplayWhitespaceAndUnicode) {
    const Bytes resource = make_version({
        make_table(u"040904B0", {
            make_string(
                u"FileDescription",
                u"  Pok\u00E9mon\tLauncher\r\n"),
        }),
    });

    EXPECT_EQ(
        parse_version_info_strings(resource).file_description,
        "Pok\xC3\xA9mon Launcher");
}

TEST(VersionInfoParserTest, MalformedLengthFailsClosed) {
    Bytes resource = make_version({
        make_table(u"040904B0", {
            make_string(u"ProductName", u"Unsafe"),
        }),
    });
    resource[0] = 0xFF;
    resource[1] = 0xFF;

    const auto result = parse_version_info_strings(resource);
    EXPECT_TRUE(result.product_name.empty());
    EXPECT_TRUE(result.file_description.empty());
}
