// SPDX-License-Identifier: LGPL-2.1-only

/// @file registry.cpp
/// @brief advapi32 registry API thunks implementation (WIN32-060)

#include "winapi/advapi32/registry/registry.hpp"
#include "winapi/advapi32/registry/registry_internal.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>
#include <variant>
#include "winapi/kernel32/locale/codepage_convert.hpp"

/// @file regqueryvalueexa.cpp
/// @brief RegQueryValueExA
///
/// Closed cut from registry.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// RegQueryValueExA
// =============================================================================

u32 RegistryThunks::reg_query_value_ex_a(u64 hkey, const std::string& value_name,
                                          u32* type_out, u8* data_out,
                                          u32* data_size) {
    std::wstring wide_name;
    if (!value_name.empty()) {
        const auto* bytes = reinterpret_cast<const u8*>(value_name.data());
        const auto count = codepage::multi_byte_to_wide(
            codepage::kCpAcp, 0, bytes, value_name.size(), nullptr, 0);
        if (!count.ok()) return count.error;
        std::vector<u16> units(count.count);
        const auto converted = codepage::multi_byte_to_wide(
            codepage::kCpAcp, 0, bytes, value_name.size(), units.data(), units.size());
        if (!converted.ok()) return converted.error;
        wide_name.assign(units.begin(), units.end());
    }
    return query_value_ex(hkey, wide_name, true, type_out, data_out, data_size);
}

} // namespace vitreo::winapi
