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

/// @file regenumvaluew.cpp
/// @brief RegEnumValueW
///
/// Closed cut from registry.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// RegEnumValueW
// =============================================================================

u32 RegistryThunks::reg_enum_value_w(u64 hkey, u32 index,
                                      std::wstring& name_out,
                                      u32* type_out, u8* data_out,
                                      u32* data_size) {
    capsule::RegHandle rh = resolve_hkey(hkey);
    auto res = m_registry.enum_values(rh);
    if (res.is_err()) {
        return ERROR_FILE_NOT_FOUND;
    }

    const auto& names = res.value();
    if (index >= static_cast<u32>(names.size())) {
        return ERROR_NO_MORE_ITEMS;
    }

    name_out = names[index];

    // Optionally fill data if the caller wants it.
    if (type_out || data_out || data_size) {
        auto val_res = m_registry.query_value(rh, name_out);
        if (val_res.is_ok()) {
            const auto& val = val_res.value();
            if (type_out) {
                *type_out = reg_type_to_win32(val);
            }
            u32 required = serialise_value(val, nullptr, 0);
            if (data_out && data_size && *data_size >= required) {
                serialise_value(val, data_out, *data_size);
            }
            if (data_size) {
                *data_size = required;
            }
        }
    }

    return ERROR_SUCCESS;
}

u32 RegistryThunks::reg_enum_value_entry(u64 hkey, u32 index,
                                         std::wstring& name_out,
                                         capsule::RegValue& value_out) {
    const capsule::RegHandle rh = resolve_hkey(hkey);
    if (rh == 0) return ERROR_INVALID_HANDLE;
    auto res = m_registry.enum_values(rh);
    if (res.is_err()) return ERROR_INVALID_HANDLE;
    const auto& names = res.value();
    if (index >= static_cast<u32>(names.size())) return ERROR_NO_MORE_ITEMS;
    name_out = names[index];
    auto val = m_registry.query_value(rh, name_out);
    if (val.is_err()) return ERROR_NO_MORE_ITEMS;
    value_out = val.value();
    return ERROR_SUCCESS;
}

} // namespace vitreo::winapi
