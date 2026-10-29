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
    capsule::RegHandle rh = resolve_hkey(hkey);
    auto res = m_registry.query_value(rh, ansi_to_wide(value_name));
    if (res.is_err()) {
        return ERROR_FILE_NOT_FOUND;
    }

    const capsule::RegValue& val = res.value();
    if (type_out) {
        *type_out = reg_type_to_win32(val);
    }

    if (val.type != capsule::RegType::Sz &&
        val.type != capsule::RegType::ExpandSz) {
        const u32 required = serialise_value(val, nullptr, 0);
        if (data_out == nullptr) {
            if (data_size) *data_size = required;
            return ERROR_SUCCESS;
        }
        const u32 available = data_size ? *data_size : 0;
        if (available < required) {
            if (data_size) *data_size = required;
            return ERROR_MORE_DATA;
        }
        serialise_value(val, data_out, available);
        if (data_size) *data_size = required;
        return ERROR_SUCCESS;
    }

    const auto& text = std::get<std::wstring>(val.data);
    const u32 required = static_cast<u32>(text.size() + 1);
    if (data_out == nullptr) {
        if (data_size) *data_size = required;
        return ERROR_SUCCESS;
    }
    const u32 available = data_size ? *data_size : 0;
    if (available < required) {
        if (data_size) *data_size = required;
        return ERROR_MORE_DATA;
    }
    for (usize i = 0; i < text.size(); ++i) {
        data_out[i] = text[i] <= 0xFF ? static_cast<u8>(text[i]) : static_cast<u8>('?');
    }
    data_out[text.size()] = 0;
    if (data_size) *data_size = required;
    return ERROR_SUCCESS;
}

} // namespace vitreo::winapi
