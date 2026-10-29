// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include <vitreo/common.hpp>

#include <span>
#include <string>

namespace vitreo::pe {

struct VersionInfoStrings {
    std::string product_name;
    std::string file_description;
};

/// Parse display-name fields from a raw VS_VERSION_INFO resource.
///
/// Malformed blocks and unsupported layouts fail closed and return empty
/// fields. English-US tables are preferred, followed by other English,
/// language-neutral, and finally the first valid table.
[[nodiscard]] VersionInfoStrings parse_version_info_strings(
    std::span<const u8> resource) noexcept;

} // namespace vitreo::pe
