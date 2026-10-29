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

/// @file regqueryinfokeya_return_key_metadata.cpp
/// @brief RegQueryInfoKeyA — return key metadata
///
/// Closed cut from registry.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// RegQueryInfoKeyA — return key metadata
// =============================================================================

u32 RegistryThunks::reg_query_info_key_a(u64 hkey, u32* num_sub_keys,
                                          u32* max_subkey_len,
                                          u32* num_values,
                                          u32* max_value_len,
                                          u32* max_data_len) {
    capsule::RegHandle rh = resolve_hkey(hkey);
    // An unknown key is ERROR_INVALID_HANDLE (s4_registry_probe qikw_bad_key).
    if (rh == 0) return ERROR_INVALID_HANDLE;

    u32 sub_key_count = 0;
    u32 max_sk_len = 0;
    auto keys_res = m_registry.enum_keys(rh);
    if (keys_res.is_ok()) {
        const auto& keys = keys_res.value();
        sub_key_count = static_cast<u32>(keys.size());
        for (const auto& k : keys) {
            u32 klen = static_cast<u32>(k.size());
            if (klen > max_sk_len) max_sk_len = klen;
        }
    }

    u32 value_count = 0;
    u32 max_vn_len = 0;
    u32 max_vd_len = 0;
    auto vals_res = m_registry.enum_values(rh);
    if (vals_res.is_ok()) {
        const auto& vals = vals_res.value();
        value_count = static_cast<u32>(vals.size());
        for (const auto& v : vals) {
            u32 vlen = static_cast<u32>(v.size());
            if (vlen > max_vn_len) max_vn_len = vlen;
            // lpcbMaxValueLen: the largest stored data, in bytes (qikw mvd=e
            // for the 14-byte REG_MULTI_SZ; the ANSI entry reports the same).
            if (auto val = m_registry.query_value(rh, v); val.is_ok()) {
                max_vd_len = std::max(max_vd_len, serialise_value(val.value(), nullptr, 0));
            }
        }
    } else if (keys_res.is_err()) {
        return ERROR_INVALID_HANDLE;
    }

    if (num_sub_keys)   *num_sub_keys   = sub_key_count;
    if (max_subkey_len) *max_subkey_len = max_sk_len;
    if (num_values)     *num_values     = value_count;
    if (max_value_len)  *max_value_len  = max_vn_len;
    if (max_data_len)   *max_data_len   = max_vd_len;

    return ERROR_SUCCESS;
}

} // namespace vitreo::winapi
