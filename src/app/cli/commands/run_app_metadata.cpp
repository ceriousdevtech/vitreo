// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CLI-002: prelaunch application identity - display name, icon, version strings

#include "run.hpp"
#include "run_internal.hpp"
#include "capsule/vfs/image_mount_store.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/pe/mapper/section_mapper.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "core/pe/parser/pe_structures.hpp"
#include "core/pe/parser/version_info.hpp"
#include "windowing/resources/resource_walker.hpp"

#include <algorithm>
#include <cctype>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::cli {

std::string application_name_from_windows_path(std::string_view windows_path) {
    if (windows_path.size() >= 2 && windows_path.front() == '"' &&
        windows_path.back() == '"') {
        windows_path.remove_prefix(1);
        windows_path.remove_suffix(1);
    }

    const usize separator = windows_path.find_last_of("\\/");
    std::string name(windows_path.substr(
        separator == std::string_view::npos ? 0 : separator + 1));
    if (name.size() >= 4) {
        const usize suffix = name.size() - 4;
        if (name[suffix] == '.' &&
            std::tolower(static_cast<unsigned char>(name[suffix + 1])) == 'e' &&
            std::tolower(static_cast<unsigned char>(name[suffix + 2])) == 'x' &&
            std::tolower(static_cast<unsigned char>(name[suffix + 3])) == 'e') {
            name.resize(suffix);
        }
    }
    return name;
}

namespace {

[[nodiscard]] std::string normalized_display_name(std::string_view value) {
    std::string result;
    result.reserve(std::min<usize>(value.size(), 256));
    bool pending_space = false;
    for (const char raw_ch : value) {
        const auto ch = static_cast<unsigned char>(raw_ch);
        if (ch <= 0x20u || ch == 0x7Fu) {
            pending_space = !result.empty();
            continue;
        }
        if (pending_space) result.push_back(' ');
        pending_space = false;
        result.push_back(raw_ch);
    }
    return result;
}

} // namespace

std::string select_application_display_name(
    std::string_view explicit_name,
    const ApplicationLaunchMetadata& metadata,
    std::string_view windows_path) {
    for (const std::string_view candidate : {
             explicit_name,
             std::string_view(metadata.file_description),
             std::string_view(metadata.product_name)}) {
        std::string name = normalized_display_name(candidate);
        if (!name.empty() && name != "." && name != "..") return name;
    }
    std::string fallback = normalized_display_name(
        application_name_from_windows_path(windows_path));
    if (fallback == "." || fallback == "..") return {};
    return fallback;
}

ApplicationLaunchMetadata extract_application_metadata_for_launch(
    const std::string& pe_path,
    const std::string& capsule_name) {
    ApplicationLaunchMetadata metadata;
    if (pe_path.empty()) return metadata;

    const std::string selected_capsule =
        capsule_name.empty() ? std::string("default") : capsule_name;
    const std::string capsule_root = capsule_root_for(selected_capsule);

    std::string resolved_path = pe_path;
    bool pe_from_image = false;
    if (resolved_path.size() >= 3 &&
        std::isalpha(static_cast<unsigned char>(resolved_path[0])) &&
        resolved_path[1] == ':' &&
        (resolved_path[2] == '\\' || resolved_path[2] == '/')) {
        const char drive = static_cast<char>(std::toupper(
            static_cast<unsigned char>(resolved_path[0])));
        capsule::ImageMountStore mounts(capsule_root);
        for (const auto& record : mounts.list()) {
            if (record.drive == drive) {
                pe_from_image = true;
                break;
            }
        }
        if (!pe_from_image) {
            const char drive_lower = static_cast<char>(std::tolower(
                static_cast<unsigned char>(resolved_path[0])));
            std::string relative = resolved_path.substr(2);
            for (char& ch : relative) {
                if (ch == '\\') ch = '/';
            }
            resolved_path =
                capsule_root + "/drives/" + drive_lower + relative;
        }
    }

    std::vector<u8> pe_data;
    const auto read_result = pe_from_image
        ? read_image_file_bytes(capsule_root, pe_path, pe_data)
        : read_file_bytes(resolved_path, pe_data);
    if (!read_result.success) return metadata;

    auto parsed = pe::parse_pe(std::span<const u8>(pe_data));
    if (parsed.is_err() ||
        parsed.value().subsystem != pe::Subsystem::WindowsGui ||
        parsed.value().num_data_directories <= pe::DIR_RESOURCE ||
        parsed.value().data_directories[pe::DIR_RESOURCE].VirtualAddress == 0) {
        return metadata;
    }

    GuestMemory memory;
    auto mapped = pe::map_pe(memory, parsed.value(),
                             std::span<const u8>(pe_data));
    if (mapped.is_err()) return metadata;

    const auto& resource =
        parsed.value().data_directories[pe::DIR_RESOURCE];
    metadata.icon_data = windowing::extract_app_icon(
        memory, mapped.value().base_address,
        resource.VirtualAddress, resource.Size);

    const auto version = windowing::find_first_resource(
        memory, mapped.value().base_address,
        resource.VirtualAddress, resource.Size, windowing::RT::VERSION);
    if (version.data != nullptr && version.size != 0) {
        const auto strings = pe::parse_version_info_strings(
            std::span<const u8>(version.data, version.size));
        metadata.product_name = strings.product_name;
        metadata.file_description = strings.file_description;
    }
    return metadata;
}

std::vector<u8> extract_application_icon_for_launch(
    const std::string& pe_path,
    const std::string& capsule_name) {
    return extract_application_metadata_for_launch(pe_path, capsule_name)
        .icon_data;
}

} // namespace vitreo::cli
