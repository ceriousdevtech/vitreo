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

/// @file regqueryvalueexw.cpp
/// @brief RegQueryValueExW
///
/// Closed cut from registry.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// RegQueryValueExW
// =============================================================================

u32 RegistryThunks::reg_query_value_ex_w(u64 hkey, const std::wstring& value_name,
                                          u32* type_out, u8* data_out,
                                          u32* data_size) {
    capsule::RegHandle rh = resolve_hkey(hkey);
    auto res = m_registry.query_value(rh, value_name);

    if (res.is_err()) {
        return ERROR_FILE_NOT_FOUND;
    }

    const capsule::RegValue& val = res.value();

    if (type_out) {
        *type_out = reg_type_to_win32(val);
    }

    u32 required = serialise_value(val, nullptr, 0);

    // Size-only query.
    if (data_out == nullptr) {
        if (data_size) {
            *data_size = required;
        }
        return ERROR_SUCCESS;
    }

    // Buffer provided but too small.
    u32 avail = data_size ? *data_size : 0;
    if (avail < required) {
        if (data_size) {
            *data_size = required;
        }
        return ERROR_MORE_DATA;
    }

    // Copy data.
    serialise_value(val, data_out, avail);
    if (data_size) {
        *data_size = required;
    }

    return ERROR_SUCCESS;
}

} // namespace vitreo::winapi
