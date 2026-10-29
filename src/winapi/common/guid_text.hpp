// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file guid_text.hpp
/// @brief Canonical text form of a Windows GUID.
///
/// Registry keys under HKCR\CLSID, HKCR\Interface and HKCR\TypeLib are named
/// by the braced, upper-case, hyphenated form of a GUID. More than one DLL
/// implementation needs to produce those key names, and they have to agree
/// exactly — a single stray case or brace makes a key the guest writes and a
/// key the guest reads two different keys.

#include <vitreo/common.hpp>

#include <array>
#include <string>

namespace vitreo::winapi {

/// Format the 16 bytes of a GUID as `{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}`.
///
/// @param guid GUID bytes in memory order: Data1, Data2 and Data3 are stored
///             little-endian, Data4 is a plain byte string.
[[nodiscard]] std::string format_guid_braced(const std::array<u8, 16>& guid);

} // namespace vitreo::winapi
