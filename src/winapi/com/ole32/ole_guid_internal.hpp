// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file ole_guid_internal.hpp
/// @brief Shared GUID compare/parse/format helpers for OLE32 TUs.

#include "com_core.hpp"

#include <string>
#include <string_view>

namespace vitreo::winapi {

inline constexpr Guid IID_IUNKNOWN{
    0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

/// IMalloc — {00000002-0000-0000-C000-000000000046}
inline constexpr Guid IID_IMALLOC{
    0x00000002u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

inline constexpr u32 MEMCTX_TASK = 1;

[[nodiscard]] bool guid_equal(const Guid& a, const Guid& b);
[[nodiscard]] bool parse_guid_string(std::string_view s, Guid& out);
[[nodiscard]] std::string format_guid(const Guid& g);

} // namespace vitreo::winapi
