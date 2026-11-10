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
#include <mutex>

/// @file regqueryvalueexw.cpp
/// @brief RegQueryValueExW
///
/// Closed cut from registry.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {
#include "reg_query_contract.inc"

// =============================================================================
// RegQueryValueExW
// =============================================================================

u32 RegistryThunks::reg_query_value_ex_w(u64 hkey, const std::wstring& value_name,
                                          u32* type_out, u8* data_out,
                                          u32* data_size) {
    return query_value_ex(hkey, value_name, false, type_out, data_out, data_size);
}

u32 RegistryThunks::query_value_ex(u64 hkey, const std::wstring& value_name,
                                    bool ansi, u32* type_out, u8* data_out,
                                    u32* data_size) {
    // Learn: lpcbData can be NULL only when lpData is NULL. Windows checks
    // this relationship before changing type, size, or data (oracle probe).
    if (data_out != nullptr && data_size == nullptr) return ERROR_INVALID_PARAMETER;
    const u32 low = static_cast<u32>(hkey);
    const bool canonical = hkey == low ||
        hkey == static_cast<u64>(static_cast<i64>(static_cast<i32>(low)));
    if (canonical && (low == reg_query_contract::HKEY_CURRENT_CONFIG ||
        low == reg_query_contract::HKEY_PERFORMANCE_DATA ||
        low == reg_query_contract::HKEY_PERFORMANCE_TEXT ||
        low == reg_query_contract::HKEY_PERFORMANCE_NLSTEXT)) {
        static std::once_flag warning;
        std::call_once(warning, [] { std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED advapi32!RegQueryValueExA/W: current-config/performance registry provider\n"); });
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
    const bool predefined = low == capsule::HKEY_CLASSES_ROOT ||
        low == capsule::HKEY_CURRENT_USER || low == capsule::HKEY_LOCAL_MACHINE ||
        low == capsule::HKEY_USERS;
    if ((!canonical && predefined) || (!predefined && hkey != low)) return ERROR_INVALID_HANDLE;
    // Copy the numeric capsule handle under HandleTable's lock. Do not retain
    // an unlocked HandleEntry pointer while another thread can close it.
    const capsule::RegHandle rh = predefined ? resolve_hkey(hkey) :
        static_cast<capsule::RegHandle>(reinterpret_cast<uintptr_t>(
            m_handles.get(static_cast<Handle>(hkey), HandleType::Registry)));
    if (rh == 0) return ERROR_INVALID_HANDLE;
    const u32 access = predefined ? reg_query_contract::KEY_QUERY_VALUE :
                                   m_handles.get_access(static_cast<Handle>(hkey));
    if ((access & reg_query_contract::KEY_QUERY_VALUE) == 0 &&
        (access & reg_query_contract::MAXIMUM_ALLOWED) == 0) {
        if (type_out != nullptr) *type_out = REG_NONE;
        return ERROR_ACCESS_DENIED;
    }
    auto res = m_registry.query_value(rh, value_name);

    if (res.is_err()) {
        if (res.error() == Error::InvalidHandle) return ERROR_INVALID_HANDLE;
        if (type_out != nullptr) *type_out = REG_NONE;
        return ERROR_FILE_NOT_FOUND;
    }

    const capsule::RegValue& val = res.value();
    const u32 kind = reg_type_to_win32(val);
    if (data_out == nullptr && data_size == nullptr) {
        if (type_out != nullptr) *type_out = kind;
        return ERROR_SUCCESS;
    }
    if ((kind == REG_SZ || kind == REG_EXPAND_SZ || kind == REG_MULTI_SZ) &&
        serialise_value(val, nullptr, 0) % sizeof(u16) != 0) {
        static std::once_flag warning;
        std::call_once(warning, [] { std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED advapi32!RegQueryValueExA/W: odd-byte UTF-16 string retrieval\n"); });
        return ERROR_CALL_NOT_IMPLEMENTED;
    }

    if (type_out) {
        *type_out = kind;
    }

    u32 conversion_error = ERROR_SUCCESS;
    u32 required = ansi ? serialise_value_ansi(val, nullptr, 0, &conversion_error)
                        : serialise_value(val, nullptr, 0);
    if (conversion_error != ERROR_SUCCESS) return conversion_error;

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
    if (ansi) serialise_value_ansi(val, data_out, avail, &conversion_error);
    else serialise_value(val, data_out, avail);
    if (conversion_error != ERROR_SUCCESS) return conversion_error;
    if (data_size) {
        *data_size = required;
    }

    return ERROR_SUCCESS;
}

} // namespace vitreo::winapi
