// SPDX-License-Identifier: LGPL-2.1-only

#include "core/pe/parser/version_info.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace vitreo::pe {

namespace {

struct VersionBlock {
    usize start = 0;
    usize end = 0;
    usize value_offset = 0;
    usize children_offset = 0;
    u16 value_length = 0;
    u16 type = 0;
    std::u16string key;
};

[[nodiscard]] bool read_u16(
    std::span<const u8> data, usize offset, u16& out) noexcept {
    if (offset > data.size() || data.size() - offset < 2) return false;
    out = static_cast<u16>(
        static_cast<u16>(data[offset]) |
        static_cast<u16>(static_cast<u16>(data[offset + 1]) << 8));
    return true;
}

[[nodiscard]] usize align4(usize value) noexcept {
    return (value + 3u) & ~usize{3u};
}

[[nodiscard]] std::optional<VersionBlock> parse_block(
    std::span<const u8> data, usize start, usize limit) noexcept {
    if (limit > data.size() || start > limit || limit - start < 6) {
        return std::nullopt;
    }

    u16 length = 0;
    VersionBlock block;
    if (!read_u16(data, start, length) ||
        !read_u16(data, start + 2, block.value_length) ||
        !read_u16(data, start + 4, block.type) ||
        length < 6 || static_cast<usize>(length) > limit - start) {
        return std::nullopt;
    }

    block.start = start;
    block.end = start + length;

    usize cursor = start + 6;
    bool terminated = false;
    while (cursor <= block.end && block.end - cursor >= 2) {
        u16 unit = 0;
        if (!read_u16(data, cursor, unit)) return std::nullopt;
        cursor += 2;
        if (unit == 0) {
            terminated = true;
            break;
        }
        if (block.key.size() >= 128) return std::nullopt;
        block.key.push_back(static_cast<char16_t>(unit));
    }
    if (!terminated) return std::nullopt;

    block.value_offset = align4(cursor);
    if (block.value_offset > block.end) return std::nullopt;

    const usize value_bytes =
        block.type == 1
            ? static_cast<usize>(block.value_length) * 2u
            : static_cast<usize>(block.value_length);
    if (value_bytes > block.end - block.value_offset) {
        return std::nullopt;
    }

    block.children_offset = align4(block.value_offset + value_bytes);
    if (block.children_offset > block.end) {
        block.children_offset = block.end;
    }
    return block;
}

[[nodiscard]] std::vector<VersionBlock> child_blocks(
    std::span<const u8> data, const VersionBlock& parent) noexcept {
    std::vector<VersionBlock> children;
    usize cursor = parent.children_offset;
    while (cursor < parent.end) {
        cursor = align4(cursor);
        if (cursor >= parent.end || parent.end - cursor < 6) break;

        u16 length = 0;
        if (!read_u16(data, cursor, length) || length == 0) break;
        auto child = parse_block(data, cursor, parent.end);
        if (!child.has_value() || child->end <= cursor) break;
        children.push_back(std::move(*child));
        cursor = children.back().end;
    }
    return children;
}

[[nodiscard]] bool ascii_key_equals(
    std::u16string_view actual, std::string_view expected) noexcept {
    if (actual.size() != expected.size()) return false;
    for (usize i = 0; i < actual.size(); ++i) {
        char16_t lhs = actual[i];
        unsigned char rhs = static_cast<unsigned char>(expected[i]);
        if (lhs >= u'a' && lhs <= u'z') lhs -= u'a' - u'A';
        if (rhs >= static_cast<unsigned char>('a') &&
            rhs <= static_cast<unsigned char>('z')) {
            rhs = static_cast<unsigned char>(
                rhs - static_cast<unsigned char>('a') +
                static_cast<unsigned char>('A'));
        }
        if (lhs != static_cast<char16_t>(rhs)) return false;
    }
    return true;
}

void append_utf8(std::string& output, u32 code_point) {
    if (code_point <= 0x7Fu) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FFu) {
        output.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
        output.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else if (code_point <= 0xFFFFu) {
        output.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
        output.push_back(
            static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else {
        output.push_back(static_cast<char>(0xF0u | (code_point >> 18)));
        output.push_back(
            static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
        output.push_back(
            static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    }
}

[[nodiscard]] std::string normalized_utf8(
    std::span<const u16> units) {
    std::u16string normalized;
    normalized.reserve(std::min<usize>(units.size(), 256));
    bool pending_space = false;
    for (const u16 raw_unit : units) {
        if (raw_unit == 0) break;
        if (raw_unit <= 0x20u || raw_unit == 0x7Fu) {
            pending_space = !normalized.empty();
            continue;
        }
        if (pending_space && normalized.size() < 256) {
            normalized.push_back(u' ');
        }
        pending_space = false;
        if (normalized.size() >= 256) break;
        normalized.push_back(static_cast<char16_t>(raw_unit));
    }

    std::string output;
    output.reserve(normalized.size() * 3);
    for (usize i = 0; i < normalized.size(); ++i) {
        u32 code_point = static_cast<u16>(normalized[i]);
        if (code_point >= 0xD800u && code_point <= 0xDBFFu) {
            if (i + 1 < normalized.size()) {
                const u32 low = static_cast<u16>(normalized[i + 1]);
                if (low >= 0xDC00u && low <= 0xDFFFu) {
                    code_point =
                        0x10000u + ((code_point - 0xD800u) << 10) +
                        (low - 0xDC00u);
                    ++i;
                } else {
                    code_point = 0xFFFDu;
                }
            } else {
                code_point = 0xFFFDu;
            }
        } else if (code_point >= 0xDC00u && code_point <= 0xDFFFu) {
            code_point = 0xFFFDu;
        }
        append_utf8(output, code_point);
    }
    return output;
}

[[nodiscard]] std::string string_value(
    std::span<const u8> data, const VersionBlock& block) {
    if (block.type != 1 || block.value_length == 0) return {};

    const usize unit_count = block.value_length;
    std::vector<u16> units;
    units.reserve(unit_count);
    for (usize i = 0; i < unit_count; ++i) {
        u16 unit = 0;
        if (!read_u16(data, block.value_offset + i * 2, unit)) return {};
        units.push_back(unit);
    }
    return normalized_utf8(units);
}

[[nodiscard]] int hex_digit(char16_t ch) noexcept {
    if (ch >= u'0' && ch <= u'9') return static_cast<int>(ch - u'0');
    if (ch >= u'a' && ch <= u'f') return 10 + static_cast<int>(ch - u'a');
    if (ch >= u'A' && ch <= u'F') return 10 + static_cast<int>(ch - u'A');
    return -1;
}

[[nodiscard]] int table_rank(std::u16string_view key) noexcept {
    if (key.size() < 4) return 4;
    u16 language = 0;
    for (usize i = 0; i < 4; ++i) {
        const int digit = hex_digit(key[i]);
        if (digit < 0) return 4;
        language = static_cast<u16>(
            static_cast<u16>(language << 4) |
            static_cast<u16>(digit));
    }
    if (language == 0x0409u) return 0;
    if ((language & 0x03FFu) == 0x0009u) return 1;
    if (language == 0) return 2;
    return 3;
}

struct RankedString {
    std::string value;
    int rank = std::numeric_limits<int>::max();
};

void consider_string(
    RankedString& destination, std::string value, int rank) {
    if (!value.empty() && rank < destination.rank) {
        destination.value = std::move(value);
        destination.rank = rank;
    }
}

} // namespace

VersionInfoStrings parse_version_info_strings(
    std::span<const u8> resource) noexcept {
    VersionInfoStrings result;
    auto root = parse_block(resource, 0, resource.size());
    if (!root.has_value() ||
        !ascii_key_equals(root->key, "VS_VERSION_INFO")) {
        return result;
    }

    RankedString product_name;
    RankedString file_description;
    for (const VersionBlock& child : child_blocks(resource, *root)) {
        if (!ascii_key_equals(child.key, "StringFileInfo")) continue;

        for (const VersionBlock& table : child_blocks(resource, child)) {
            const int rank = table_rank(table.key);
            for (const VersionBlock& string :
                 child_blocks(resource, table)) {
                if (ascii_key_equals(string.key, "ProductName")) {
                    consider_string(
                        product_name, string_value(resource, string), rank);
                } else if (ascii_key_equals(
                               string.key, "FileDescription")) {
                    consider_string(
                        file_description,
                        string_value(resource, string),
                        rank);
                }
            }
        }
    }

    result.product_name = std::move(product_name.value);
    result.file_description = std::move(file_description.value);
    return result;
}

} // namespace vitreo::pe
