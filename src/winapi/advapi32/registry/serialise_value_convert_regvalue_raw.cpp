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
    const capsule::RegValue& val, u8* out, u32 buf_size) {
    if (val.type == capsule::RegType::Sz ||
        val.type == capsule::RegType::ExpandSz) {
        const auto& text = std::get<std::wstring>(val.data);
        const u32 required = static_cast<u32>(text.size() + 1);
        if (out && buf_size >= required) {
            for (usize i = 0; i < text.size(); ++i) {
                out[i] = text[i] <= 0xff
                    ? static_cast<u8>(text[i])
                    : static_cast<u8>('?');
            }
            out[text.size()] = 0;
        }
        return required;
    }
    if (val.type == capsule::RegType::MultiSz) {
        const auto& strings =
            std::get<std::vector<std::wstring>>(val.data);
        usize byte_count = 1;
        for (const auto& string : strings) {
            byte_count += string.size() + 1;
        }
        if (strings.empty()) ++byte_count;
        const u32 required = static_cast<u32>(byte_count);
        if (out && buf_size >= required) {
            u32 offset = 0;
            for (const auto& string : strings) {
                for (const wchar_t ch : string) {
                    out[offset++] = ch <= 0xff
                        ? static_cast<u8>(ch)
                        : static_cast<u8>('?');
                }
                out[offset++] = 0;
            }
            out[offset++] = 0;
            if (strings.empty()) out[offset] = 0;
        }
        return required;
    }
    if (val.type == capsule::RegType::Raw &&
        (val.raw_type == 1 || val.raw_type == 2 || val.raw_type == 7)) {
        // REG_SZ / REG_EXPAND_SZ / REG_MULTI_SZ kept byte-exact: the ANSI
        // view converts each stored UTF-16 unit, NULs included, and keeps
        // the stored length (a string stored without its NUL stays so).
        const auto& bytes = std::get<std::vector<u8>>(val.data);
        const u32 required = static_cast<u32>(bytes.size() / 2);
        if (out && buf_size >= required) {
            for (u32 i = 0; i < required; ++i) {
                const u16 unit = static_cast<u16>(bytes[i * 2] | (bytes[i * 2 + 1] << 8));
                out[i] = unit <= 0xff ? static_cast<u8>(unit) : static_cast<u8>('?');
            }
        }
        return required;
    }
    return serialise_value(val, out, buf_size);
}

} // namespace vitreo::winapi
