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

/// @file regsetvalueexw.cpp
/// @brief RegSetValueExW
///
/// Closed cut from registry.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// RegSetValueExW
// =============================================================================

u32 RegistryThunks::reg_set_value_ex_w(u64 hkey, const std::wstring& value_name,
                                        u32 type, const u8* data,
                                        u32 data_size) {
    capsule::RegHandle rh = resolve_hkey(hkey);
    const auto key_path = m_registry.canonical_key_path(rh);

    capsule::RegValue val;

    switch (type) {
        case REG_SZ:
        case REG_EXPAND_SZ: {
            val.type = (type == REG_SZ) ? capsule::RegType::Sz
                                        : capsule::RegType::ExpandSz;
            // Data is UTF-16LE (2 bytes per code unit) with null terminator.
            // On macOS wchar_t is 4 bytes, so we must decode char16_t units.
            usize char_count = data_size / 2;  // UTF-16LE: 2 bytes per unit
            if (char_count > 0) {
                const auto* u16p = reinterpret_cast<const char16_t*>(data);
                // Strip trailing null if present.
                if (u16p[char_count - 1] == u'\0') {
                    --char_count;
                }
                std::wstring ws;
                ws.reserve(char_count);
                for (usize i = 0; i < char_count; ++i) {
                    ws.push_back(static_cast<wchar_t>(u16p[i]));
                }
                val.data = std::move(ws);
            } else {
                val.data = std::wstring{};
            }
            break;
        }

        case REG_BINARY: {
            val.type = capsule::RegType::Binary;
            val.data = std::vector<u8>(data, data + data_size);
            break;
        }

        case REG_DWORD: {
            val.type = capsule::RegType::Dword;
            u32 v = 0;
            if (data_size >= sizeof(u32)) {
                std::memcpy(&v, data, sizeof(u32));
            }
            val.data = v;
            break;
        }

        case REG_QWORD: {
            val.type = capsule::RegType::Qword;
            u64 v = 0;
            if (data_size >= sizeof(u64)) {
                std::memcpy(&v, data, sizeof(u64));
            }
            val.data = v;
            break;
        }

        case REG_MULTI_SZ: {
            // A list of NUL-terminated UTF-16 strings closed by an empty one.
            val.type = capsule::RegType::MultiSz;
            std::vector<std::wstring> strings;
            const auto* u16p = reinterpret_cast<const char16_t*>(data);
            const usize units = data_size / 2;
            std::wstring current;
            for (usize i = 0; i < units; ++i) {
                if (u16p[i] == u'\0') {
                    if (current.empty()) break;
                    strings.push_back(std::move(current));
                    current.clear();
                } else {
                    current.push_back(static_cast<wchar_t>(u16p[i]));
                }
            }
            val.data = std::move(strings);
            break;
        }

        default:
            val.type = capsule::RegType::Raw;
            break;
    }

    // Windows stores exactly the caller's bytes and returns them unchanged
    // (Learn: RegSetValueExW, "Registry Value Types"). A typed form is kept
    // only when it reproduces those bytes; REG_SZ/REG_EXPAND_SZ may also drop
    // a missing terminator, which the typed store re-adds. Everything else —
    // REG_NONE with data, BIG_ENDIAN/LINK/resource lists, other codes, a
    // DWORD/QWORD of the wrong size, a malformed MULTI_SZ — was previously
    // stored as an empty REG_NONE; it is now kept byte-exact.
    if (val.type != capsule::RegType::Raw && data_size != 0) {
        const u32 need = serialise_value(val, nullptr, 0);
        std::vector<u8> round(need);
        (void)serialise_value(val, round.data(), need);
        const bool exact = need == data_size &&
            std::memcmp(round.data(), data, data_size) == 0;
        const bool string_without_nul =
            (type == REG_SZ || type == REG_EXPAND_SZ) && need == data_size + 2 &&
            std::memcmp(round.data(), data, data_size) == 0;
        if (!exact && !string_without_nul) val.type = capsule::RegType::Raw;
    } else if (val.type != capsule::RegType::Raw && data_size == 0 &&
               (type == REG_DWORD || type == REG_QWORD || type == REG_MULTI_SZ)) {
        val.type = capsule::RegType::Raw;
    }
    if (val.type == capsule::RegType::Raw) {
        if (type == REG_NONE && data_size == 0) {
            val.type = capsule::RegType::None;
            val.data = std::monostate{};
        } else {
            val.raw_type = type;
            val.data = std::vector<u8>(data, data + data_size);
        }
    }

    auto res = m_registry.set_value(rh, value_name, val);
    if (res.is_ok() && key_path.has_value()) {
        notify_registry_change(*key_path, 0x00000004, false);
    }
    return res.is_ok() ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

} // namespace vitreo::winapi
