// SPDX-License-Identifier: LGPL-2.1-only

#include "core/pe/parser/manifest_info.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace vitreo::pe {

namespace {

constexpr u32 kDirectoryBit = 0x80000000u;
constexpr u16 kManifestResourceType = 24;
constexpr usize kMaxManifestSize = 1024 * 1024;

[[nodiscard]] bool read_u16(
    std::span<const u8> data, usize offset, u16& value) noexcept {
    if (offset > data.size() || data.size() - offset < 2) return false;
    value = static_cast<u16>(
        static_cast<u16>(data[offset]) |
        static_cast<u16>(static_cast<u16>(data[offset + 1]) << 8));
    return true;
}

[[nodiscard]] bool read_u32(
    std::span<const u8> data, usize offset, u32& value) noexcept {
    if (offset > data.size() || data.size() - offset < 4) return false;
    value = static_cast<u32>(data[offset]) |
            (static_cast<u32>(data[offset + 1]) << 8) |
            (static_cast<u32>(data[offset + 2]) << 16) |
            (static_cast<u32>(data[offset + 3]) << 24);
    return true;
}

struct ResourceEntry {
    u32 name = 0;
    u32 value = 0;
};

[[nodiscard]] bool directory_entries(
    std::span<const u8> data,
    usize resource_base,
    usize resource_size,
    u32 directory_offset,
    usize& entries_offset,
    usize& entry_count) noexcept {
    const usize relative = directory_offset;
    if (relative > resource_size || resource_size - relative < 16) {
        return false;
    }
    const usize directory = resource_base + relative;
    if (directory < resource_base || directory > data.size() ||
        data.size() - directory < 16) {
        return false;
    }
    u16 named = 0;
    u16 ids = 0;
    if (!read_u16(data, directory + 12, named) ||
        !read_u16(data, directory + 14, ids)) {
        return false;
    }
    entry_count = static_cast<usize>(named) + static_cast<usize>(ids);
    entries_offset = directory + 16;
    const usize entries_size = entry_count * 8;
    return entry_count <= (resource_size - relative - 16) / 8 &&
           entries_size <= data.size() - entries_offset;
}

[[nodiscard]] bool read_entry(
    std::span<const u8> data,
    usize offset,
    ResourceEntry& entry) noexcept {
    return read_u32(data, offset, entry.name) &&
           read_u32(data, offset + 4, entry.value);
}

[[nodiscard]] bool find_id_entry(
    std::span<const u8> data,
    usize resource_base,
    usize resource_size,
    u32 directory_offset,
    u16 id,
    ResourceEntry& result) noexcept {
    usize entries = 0;
    usize count = 0;
    if (!directory_entries(
            data, resource_base, resource_size, directory_offset,
            entries, count)) {
        return false;
    }
    for (usize index = 0; index < count; ++index) {
        ResourceEntry entry;
        if (!read_entry(data, entries + index * 8, entry)) return false;
        if ((entry.name & kDirectoryBit) == 0 &&
            static_cast<u16>(entry.name) == id) {
            result = entry;
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool first_entry(
    std::span<const u8> data,
    usize resource_base,
    usize resource_size,
    u32 directory_offset,
    ResourceEntry& result) noexcept {
    usize entries = 0;
    usize count = 0;
    if (!directory_entries(
            data, resource_base, resource_size, directory_offset,
            entries, count) ||
        count == 0) {
        return false;
    }
    return read_entry(data, entries, result);
}

[[nodiscard]] std::span<const u8> first_manifest(
    std::span<const u8> data,
    const ParsedPe& pe) noexcept {
    if (pe.num_data_directories <= DIR_RESOURCE) return {};
    const auto& directory = pe.data_directories[DIR_RESOURCE];
    if (directory.VirtualAddress == 0 || directory.Size < 16) return {};

    const auto base_result =
        pe.rva_to_file_offset(directory.VirtualAddress);
    if (base_result.is_err()) return {};
    const usize resource_base = base_result.value();
    const usize resource_size = directory.Size;
    if (resource_base >= data.size() ||
        resource_size > data.size() - resource_base) {
        return {};
    }

    ResourceEntry type;
    if (!find_id_entry(
            data, resource_base, resource_size, 0,
            kManifestResourceType, type) ||
        (type.value & kDirectoryBit) == 0) {
        return {};
    }
    ResourceEntry name;
    if (!first_entry(
            data, resource_base, resource_size,
            type.value & ~kDirectoryBit, name) ||
        (name.value & kDirectoryBit) == 0) {
        return {};
    }
    ResourceEntry language;
    if (!first_entry(
            data, resource_base, resource_size,
            name.value & ~kDirectoryBit, language) ||
        (language.value & kDirectoryBit) != 0) {
        return {};
    }

    const usize data_entry_offset = language.value;
    if (data_entry_offset > resource_size ||
        resource_size - data_entry_offset < 16) {
        return {};
    }
    const usize data_entry = resource_base + data_entry_offset;
    u32 data_rva = 0;
    u32 data_size = 0;
    if (!read_u32(data, data_entry, data_rva) ||
        !read_u32(data, data_entry + 4, data_size) ||
        data_size == 0 || data_size > kMaxManifestSize) {
        return {};
    }
    const auto offset_result = pe.rva_to_file_offset(data_rva);
    if (offset_result.is_err()) return {};
    const usize offset = offset_result.value();
    if (offset > data.size() ||
        static_cast<usize>(data_size) > data.size() - offset) {
        return {};
    }
    return data.subspan(offset, data_size);
}

[[nodiscard]] std::string manifest_text(
    std::span<const u8> manifest) {
    std::string result;
    if (manifest.size() >= 2 &&
        ((manifest[0] == 0xFF && manifest[1] == 0xFE) ||
         (manifest[0] == 0xFE && manifest[1] == 0xFF))) {
        const bool little_endian = manifest[0] == 0xFF;
        result.reserve(manifest.size() / 2);
        for (usize offset = 2; offset + 1 < manifest.size(); offset += 2) {
            const u16 unit = little_endian
                ? static_cast<u16>(
                      static_cast<u16>(manifest[offset]) |
                      static_cast<u16>(
                          static_cast<u16>(manifest[offset + 1]) << 8))
                : static_cast<u16>(
                      static_cast<u16>(
                          static_cast<u16>(manifest[offset]) << 8) |
                      static_cast<u16>(manifest[offset + 1]));
            if (unit == 0) break;
            result.push_back(
                unit <= 0x7Fu ? static_cast<char>(unit) : ' ');
        }
    } else {
        result.assign(
            reinterpret_cast<const char*>(manifest.data()),
            manifest.size());
    }
    std::transform(
        result.begin(), result.end(), result.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return result;
}

[[nodiscard]] bool inside_comment(
    std::string_view text, usize offset) noexcept {
    const usize comment_start = text.rfind("<!--", offset);
    const usize comment_end = text.rfind("-->", offset);
    return comment_start != std::string_view::npos &&
           (comment_end == std::string_view::npos ||
            comment_end < comment_start);
}

[[nodiscard]] std::optional<std::string_view> attribute_value(
    std::string_view tag,
    std::string_view name) noexcept {
    usize search_from = 0;
    while (true) {
        const usize attribute = tag.find(name, search_from);
        if (attribute == std::string_view::npos) return std::nullopt;
        search_from = attribute + name.size();

        const bool left_boundary =
            attribute == 0 ||
            !(std::isalnum(static_cast<unsigned char>(tag[attribute - 1])) ||
              tag[attribute - 1] == '_' || tag[attribute - 1] == ':');
        const bool right_boundary =
            search_from >= tag.size() ||
            !(std::isalnum(static_cast<unsigned char>(tag[search_from])) ||
              tag[search_from] == '_' || tag[search_from] == ':');
        if (!left_boundary || !right_boundary) continue;

        usize cursor = search_from;
        while (cursor < tag.size() &&
               std::isspace(static_cast<unsigned char>(tag[cursor]))) {
            ++cursor;
        }
        if (cursor >= tag.size() || tag[cursor] != '=') continue;
        ++cursor;
        while (cursor < tag.size() &&
               std::isspace(static_cast<unsigned char>(tag[cursor]))) {
            ++cursor;
        }
        if (cursor >= tag.size() ||
            (tag[cursor] != '\'' && tag[cursor] != '"')) {
            return std::nullopt;
        }
        const char quote = tag[cursor++];
        const usize end = tag.find(quote, cursor);
        if (end == std::string_view::npos) return std::nullopt;
        return tag.substr(cursor, end - cursor);
    }
}

[[nodiscard]] bool valid_private_assembly_name(
    std::string_view name) noexcept {
    if (name.empty() || name.size() > 255 || name == "." || name == "..") {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-';
    });
}

} // namespace

RequestedExecutionLevel parse_manifest_execution_level(
    std::span<const u8> manifest) noexcept {
    if (manifest.empty() || manifest.size() > kMaxManifestSize) {
        return RequestedExecutionLevel::Unspecified;
    }
    const std::string text = manifest_text(manifest);
    usize search_from = 0;
    while (true) {
        const usize element =
            text.find("requestedexecutionlevel", search_from);
        if (element == std::string::npos) {
            return RequestedExecutionLevel::Unspecified;
        }
        search_from = element + 1;
        const usize tag_start = text.rfind('<', element);
        const usize tag_end = text.find('>', element);
        if (tag_start == std::string::npos ||
            tag_end == std::string::npos ||
            tag_start > element || inside_comment(text, element)) {
            continue;
        }

        const std::string_view tag(
            text.data() + tag_start, tag_end - tag_start + 1);
        usize level = tag.find("level");
        while (level != std::string_view::npos) {
            const bool left_boundary =
                level == 0 ||
                !(std::isalnum(
                      static_cast<unsigned char>(tag[level - 1])) ||
                  tag[level - 1] == '_' || tag[level - 1] == ':');
            usize cursor = level + 5;
            while (cursor < tag.size() &&
                   std::isspace(
                       static_cast<unsigned char>(tag[cursor]))) {
                ++cursor;
            }
            if (left_boundary && cursor < tag.size() &&
                tag[cursor] == '=') {
                ++cursor;
                while (cursor < tag.size() &&
                       std::isspace(
                           static_cast<unsigned char>(tag[cursor]))) {
                    ++cursor;
                }
                if (cursor >= tag.size() ||
                    (tag[cursor] != '\'' && tag[cursor] != '"')) {
                    return RequestedExecutionLevel::Unspecified;
                }
                const char quote = tag[cursor++];
                const usize end = tag.find(quote, cursor);
                if (end == std::string_view::npos) {
                    return RequestedExecutionLevel::Unspecified;
                }
                const std::string_view value =
                    tag.substr(cursor, end - cursor);
                if (value == "asinvoker") {
                    return RequestedExecutionLevel::AsInvoker;
                }
                if (value == "highestavailable") {
                    return RequestedExecutionLevel::HighestAvailable;
                }
                if (value == "requireadministrator") {
                    return RequestedExecutionLevel::RequireAdministrator;
                }
                return RequestedExecutionLevel::Unspecified;
            }
            level = tag.find("level", level + 5);
        }
    }
}

RequestedExecutionLevel pe_requested_execution_level(
    std::span<const u8> pe_data,
    const ParsedPe& pe) noexcept {
    return parse_manifest_execution_level(first_manifest(pe_data, pe));
}

bool parse_manifest_supports_windows_10(
    std::span<const u8> manifest) noexcept {
    if (manifest.empty() || manifest.size() > kMaxManifestSize) return false;

    constexpr std::string_view windows_10_id =
        "8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a";
    const std::string text = manifest_text(manifest);
    usize search_from = 0;
    while (true) {
        const usize guid = text.find(windows_10_id, search_from);
        if (guid == std::string::npos) return false;
        if (!inside_comment(text, guid)) return true;
        search_from = guid + windows_10_id.size();
    }
}

bool pe_supports_windows_10(
    std::span<const u8> pe_data,
    const ParsedPe& pe) noexcept {
    return parse_manifest_supports_windows_10(first_manifest(pe_data, pe));
}

bool parse_manifest_supports_windows_81(
    std::span<const u8> manifest) noexcept {
    if (manifest.empty() || manifest.size() > kMaxManifestSize) return false;

    constexpr std::string_view windows_81_id =
        "1f676c76-80e1-4239-95bb-83d0f6d0da78";
    const std::string text = manifest_text(manifest);
    usize search_from = 0;
    while (true) {
        const usize guid = text.find(windows_81_id, search_from);
        if (guid == std::string::npos) return false;
        if (!inside_comment(text, guid)) return true;
        search_from = guid + windows_81_id.size();
    }
}

bool pe_supports_windows_81(
    std::span<const u8> pe_data,
    const ParsedPe& pe) noexcept {
    return parse_manifest_supports_windows_81(first_manifest(pe_data, pe));
}

bool parse_manifest_supports_windows_8(
    std::span<const u8> manifest) noexcept {
    if (manifest.empty() || manifest.size() > kMaxManifestSize) return false;

    constexpr std::string_view windows_8_id =
        "4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38";
    const std::string text = manifest_text(manifest);
    usize search_from = 0;
    while (true) {
        const usize guid = text.find(windows_8_id, search_from);
        if (guid == std::string::npos) return false;
        if (!inside_comment(text, guid)) return true;
        search_from = guid + windows_8_id.size();
    }
}

bool pe_supports_windows_8(
    std::span<const u8> pe_data,
    const ParsedPe& pe) noexcept {
    return parse_manifest_supports_windows_8(first_manifest(pe_data, pe));
}

namespace {

/// Call @p fn with the <assemblyIdentity ...> tag of every
/// <dependentAssembly> outside a comment (lower-cased manifest text), up to
/// 64 of them.
template <typename Fn>
void for_each_dependent_identity(const std::string& text, Fn&& fn) {
    usize search_from = 0;
    for (int seen = 0; seen < 64; ++seen) {
        const usize dependent = text.find("dependentassembly", search_from);
        if (dependent == std::string::npos) break;
        search_from = dependent + 1;

        const usize dependent_start = text.rfind('<', dependent);
        const usize dependent_tag_end = text.find('>', dependent);
        if (dependent_start == std::string::npos ||
            dependent_tag_end == std::string::npos ||
            dependent_start > dependent || inside_comment(text, dependent)) {
            continue;
        }
        const usize dependent_end =
            text.find("</dependentassembly", dependent_tag_end + 1);
        if (dependent_end == std::string::npos) continue;
        search_from = dependent_end + 2;

        const usize identity =
            text.find("assemblyidentity", dependent_tag_end + 1);
        if (identity == std::string::npos || identity >= dependent_end ||
            inside_comment(text, identity)) {
            continue;
        }
        const usize identity_start = text.rfind('<', identity);
        const usize identity_end = text.find('>', identity);
        if (identity_start == std::string::npos ||
            identity_end == std::string::npos ||
            identity_start > identity || identity_end >= dependent_end) {
            continue;
        }
        fn(std::string_view(text.data() + identity_start,
                            identity_end - identity_start + 1));
    }
}

} // namespace

std::vector<std::string> parse_manifest_private_assemblies(
    std::span<const u8> manifest) {
    std::vector<std::string> assemblies;
    if (manifest.empty() || manifest.size() > kMaxManifestSize) {
        return assemblies;
    }
    const std::string text = manifest_text(manifest);
    for_each_dependent_identity(text, [&](std::string_view tag) {
        const auto name = attribute_value(tag, "name");
        if (!name || !valid_private_assembly_name(*name)) return;
        if (std::find(assemblies.begin(), assemblies.end(), *name) ==
            assemblies.end()) {
            assemblies.emplace_back(*name);
        }
    });
    return assemblies;
}

bool parse_manifest_common_controls_v6(std::span<const u8> manifest) {
    if (manifest.empty() || manifest.size() > kMaxManifestSize) return false;
    const std::string text = manifest_text(manifest);
    bool v6 = false;
    for_each_dependent_identity(text, [&](std::string_view tag) {
        const auto name = attribute_value(tag, "name");
        const auto version = attribute_value(tag, "version");
        if (!name || *name != "microsoft.windows.common-controls" || !version) return;
        // version="6.0.0.0": the major part selects comctl32 v6.
        const std::string_view v = *version;
        const usize dot = v.find('.');
        if (v.substr(0, dot) == "6") v6 = true;
    });
    return v6;
}

bool pe_common_controls_v6(std::span<const u8> pe_data, const ParsedPe& pe) {
    return parse_manifest_common_controls_v6(first_manifest(pe_data, pe));
}

std::vector<std::string> pe_private_assemblies(
    std::span<const u8> pe_data,
    const ParsedPe& pe) {
    return parse_manifest_private_assemblies(first_manifest(pe_data, pe));
}

bool parse_manifest_active_code_page_utf8(
    std::span<const u8> manifest) noexcept {
    if (manifest.empty() || manifest.size() > kMaxManifestSize) return false;
    const std::string text = manifest_text(manifest);
    usize search_from = 0;
    while (true) {
        const usize element = text.find("activecodepage", search_from);
        if (element == std::string::npos) return false;
        search_from = element + 1;
        if (inside_comment(text, element)) continue;
        const usize tag_start = text.rfind('<', element);
        const usize tag_end = text.find('>', element);
        if (tag_start == std::string::npos || tag_end == std::string::npos ||
            tag_start > element) {
            continue;
        }
        // Skip closing tags: </activeCodePage>
        usize name_pos = tag_start + 1;
        while (name_pos < tag_end &&
               std::isspace(static_cast<unsigned char>(text[name_pos]))) {
            ++name_pos;
        }
        if (name_pos < tag_end && text[name_pos] == '/') continue;

        // Content between this start tag and its matching close.
        const usize content_begin = tag_end + 1;
        const usize close = text.find("</", content_begin);
        if (close == std::string::npos) continue;
        std::string value = text.substr(content_begin, close - content_begin);
        // Trim whitespace.
        while (!value.empty() &&
               std::isspace(static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }
        while (!value.empty() &&
               std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        std::transform(
            value.begin(), value.end(), value.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (value == "utf-8" || value == "utf8") return true;
    }
}

namespace {

/// Text content of the first element named exactly @p element (any
/// namespace prefix), lower-cased and trimmed.
std::optional<std::string> manifest_element_value(const std::string& text,
                                                  std::string_view element) {
    usize search_from = 0;
    while (true) {
        const usize at = text.find(element, search_from);
        if (at == std::string::npos) return std::nullopt;
        search_from = at + 1;
        if (inside_comment(text, at)) continue;
        const usize after = at + element.size();
        if (after >= text.size()) return std::nullopt;
        const char next = text[after];
        if (!(next == '>' || next == '/' || std::isspace(static_cast<unsigned char>(next)))) {
            continue;
        }
        const usize tag_start = text.rfind('<', at);
        const usize tag_end = text.find('>', at);
        if (tag_start == std::string::npos || tag_end == std::string::npos) continue;
        // The name must follow '<' directly or through a "prefix:".
        bool name_ok = true;
        for (usize i = tag_start + 1; i < at; ++i) {
            const char c = text[i];
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == ':' || c == '_' || c == '-')) {
                name_ok = false;
                break;
            }
        }
        if (!name_ok || text[tag_end - 1] == '/') continue;
        const usize close = text.find("</", tag_end + 1);
        if (close == std::string::npos) continue;
        std::string value = text.substr(tag_end + 1, close - tag_end - 1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }
}

} // namespace

i64 parse_manifest_dpi_awareness_context(std::span<const u8> manifest) noexcept {
    if (manifest.empty() || manifest.size() > kMaxManifestSize) return 0;
    const std::string text = manifest_text(manifest);
    if (auto list = manifest_element_value(text, "dpiawareness")) {
        usize pos = 0;
        while (pos <= list->size()) {
            usize comma = list->find(',', pos);
            if (comma == std::string::npos) comma = list->size();
            std::string item = list->substr(pos, comma - pos);
            while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front()))) item.erase(item.begin());
            while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back()))) item.pop_back();
            if (item == "permonitorv2") return -4;
            if (item == "permonitor") return -3;
            if (item == "system") return -2;
            if (item == "unaware") return -1;
            pos = comma + 1;
        }
    }
    if (auto aware = manifest_element_value(text, "dpiaware")) {
        if (aware->rfind("true/pm", 0) == 0 || *aware == "per monitor") return -3;
        if (aware->rfind("true", 0) == 0) return -2;
        if (*aware == "false") return -1;
    }
    return 0;
}

i64 pe_dpi_awareness_context(std::span<const u8> pe_data, const ParsedPe& pe) noexcept {
    return parse_manifest_dpi_awareness_context(first_manifest(pe_data, pe));
}

bool pe_active_code_page_utf8(
    std::span<const u8> pe_data,
    const ParsedPe& pe) noexcept {
    return parse_manifest_active_code_page_utf8(first_manifest(pe_data, pe));
}

bool is_legacy_installer_candidate(
    RequestedExecutionLevel level,
    const ParsedPe& pe,
    std::string_view executable_path) noexcept {
    // Windows installer detection is a compatibility path for legacy 32-bit
    // interactive applications. An explicit requestedExecutionLevel, including
    // asInvoker, disables the heuristic.
    if (level != RequestedExecutionLevel::Unspecified || pe.is_64bit ||
        pe.subsystem != Subsystem::WindowsGui) {
        return false;
    }

    const usize separator = executable_path.find_last_of("/\\");
    std::string filename(executable_path.substr(
        separator == std::string_view::npos ? 0 : separator + 1));
    std::transform(
        filename.begin(), filename.end(), filename.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    // These are the documented filename keywords used by Windows' legacy UAC
    // installer detection. Match only the basename so a normal executable in
    // a directory named "Installer" is not elevated.
    constexpr std::string_view keywords[] = {
        "install",
        "setup",
        "update",
        "patch",
    };
    for (const std::string_view keyword : keywords) {
        if (filename.find(keyword) != std::string::npos) return true;
    }
    return false;
}

} // namespace vitreo::pe
