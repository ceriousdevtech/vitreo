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

/// @file serialise_value_convert_regvalue_raw.cpp
/// @brief serialise_value — convert a RegValue to raw bytes
///
/// Closed cut from registry.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// serialise_value — convert a RegValue to raw bytes
// =============================================================================

u32 RegistryThunks::serialise_value(const capsule::RegValue& val,
                                    u8* out, u32 buf_size) {
    switch (val.type) {
        case capsule::RegType::Sz:
        case capsule::RegType::ExpandSz: {
            const auto& ws = std::get<std::wstring>(val.data);
            // UTF-16LE including null terminator: 2 bytes per code unit
            const u32 byte_len = static_cast<u32>((ws.size() + 1) * 2);
            if (out && buf_size >= byte_len) {
                // Convert wchar_t (4 bytes on macOS) to UTF-16LE (2 bytes)
                for (u32 i = 0; i < ws.size(); ++i) {
                    u16 ch = static_cast<u16>(ws[i]);
                    out[i * 2]     = static_cast<u8>(ch & 0xFF);
                    out[i * 2 + 1] = static_cast<u8>((ch >> 8) & 0xFF);
                }
                // Write null terminator (2 bytes)
                out[ws.size() * 2]     = 0;
                out[ws.size() * 2 + 1] = 0;
            }
            return byte_len;
        }

        case capsule::RegType::Binary:
        case capsule::RegType::Raw: {
            const auto& vec = std::get<std::vector<u8>>(val.data);
            const u32 byte_len = static_cast<u32>(vec.size());
            if (out && buf_size >= byte_len) {
                std::memcpy(out, vec.data(), byte_len);
            }
            return byte_len;
        }

        case capsule::RegType::Dword: {
            constexpr u32 byte_len = sizeof(u32);
            if (out && buf_size >= byte_len) {
                u32 v = std::get<u32>(val.data);
                std::memcpy(out, &v, byte_len);
            }
            return byte_len;
        }

        case capsule::RegType::Qword: {
            constexpr u32 byte_len = sizeof(u64);
            if (out && buf_size >= byte_len) {
                u64 v = std::get<u64>(val.data);
                std::memcpy(out, &v, byte_len);
            }
            return byte_len;
        }

        case capsule::RegType::MultiSz: {
            const auto& strings =
                std::get<std::vector<std::wstring>>(val.data);
            usize code_units = 1;
            for (const auto& string : strings) {
                code_units += string.size() + 1;
            }
            if (strings.empty()) ++code_units;
            const u32 byte_len = static_cast<u32>(code_units * 2);
            if (out && buf_size >= byte_len) {
                u32 offset = 0;
                for (const auto& string : strings) {
                    for (const wchar_t ch : string) {
                        const u16 unit = static_cast<u16>(ch);
                        out[offset++] = static_cast<u8>(unit & 0xff);
                        out[offset++] = static_cast<u8>(unit >> 8);
                    }
                    out[offset++] = 0;
                    out[offset++] = 0;
                }
                out[offset++] = 0;
                out[offset++] = 0;
                if (strings.empty()) {
                    out[offset++] = 0;
                    out[offset] = 0;
                }
            }
            return byte_len;
        }

        default:
            return 0;
    }
}

u32 RegistryThunks::serialise_value_ansi(
    const capsule::RegValue& val, u8* out, u32 buf_size,
    u32* conversion_error) {
    if (conversion_error != nullptr) *conversion_error = ERROR_SUCCESS;
    const u32 type = reg_type_to_win32(val);
    if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ)
        return serialise_value(val, out, buf_size);

    // A registry string is stored as UTF-16, including every embedded NUL.
    // Convert the entire counted value through the measured process ACP;
    // a MULTI_SZ is not binary data and CP1252 is not Latin-1.
    const u32 wide_size = serialise_value(val, nullptr, 0);
    if (wide_size == 0) return 0;
    std::vector<u8> wide_bytes(wide_size);
    (void)serialise_value(val, wide_bytes.data(), wide_size);
    std::vector<u16> units(wide_size / sizeof(u16));
    for (usize i = 0; i < units.size(); ++i)
        units[i] = static_cast<u16>(wide_bytes[i * sizeof(u16)] |
                    (static_cast<u16>(wide_bytes[i * sizeof(u16) + 1]) << 8));
    const auto needed = codepage::wide_to_multi_byte(codepage::kCpAcp, 0,
        units.data(), units.size(), nullptr, 0, nullptr, false);
    if (!needed.ok()) {
        if (conversion_error != nullptr) *conversion_error = needed.error;
        return 0;
    }
    if (out != nullptr && buf_size >= needed.count) {
        const auto converted = codepage::wide_to_multi_byte(codepage::kCpAcp, 0,
            units.data(), units.size(), out, buf_size, nullptr, false);
        if (!converted.ok()) {
            if (conversion_error != nullptr) *conversion_error = converted.error;
            return 0;
        }
    }
    return needed.count;
}

} // namespace vitreo::winapi
