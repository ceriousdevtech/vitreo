// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file file_mapping_internal.hpp
/// @brief Shared helpers for FileMappingManager translation units.

#include "file_mapping.hpp"

#include <cstdlib>

#include <optional>
#include <string>
#include <string_view>

namespace vitreo::winapi {

[[nodiscard]] inline bool file_mapping_diag_enabled() noexcept {
    static const bool enabled =
        std::getenv("VITREO_ENABLE_DIAG_LOGS") != nullptr ||
        std::getenv("VITREO_FILE_MAPPING_DIAG") != nullptr;
    return enabled;
}

// File-mapping access rights accepted by OpenFileMapping, DuplicateHandle,
// and MapViewOfFile.
#include "mapview_contract.inc"
inline constexpr u32 FILE_MAPPING_ACCESS_MASK =
    FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE | FILE_MAP_LARGE_PAGES |
    FILE_MAP_TARGETS_INVALID;

inline constexpr GuestAddr kWindowsAllocationGranularity = 0x10000;

[[nodiscard]] usize host_mapping_page_size();
[[nodiscard]] GuestAddr view_base_alignment();
[[nodiscard]] bool is_writable_protect(u32 protect);
[[nodiscard]] bool is_copy_on_write_access(u32 desired_access);
[[nodiscard]] bool valid_file_mapping_access(u32 access);
[[nodiscard]] std::string normalize_mapping_name(std::string name);
[[nodiscard]] std::string hex_encode(std::string_view value);
[[nodiscard]] std::optional<std::string> hex_decode(std::string_view value);
[[nodiscard]] std::optional<u32> parse_u32(std::string_view value);

} // namespace vitreo::winapi
