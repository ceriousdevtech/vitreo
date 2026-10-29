// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file propsys.hpp
/// @brief Windows Property System: PROPSYS.dll exports and the property
///        schema lookups other host code (Windows Installer) shares.

#include "winapi/core/thunk_registry.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace vitreo::winapi {

class ComCoreManager;
class ComTaskMemoryManager;

/// PROPERTYKEY (wtypes.h): fmtid GUID then pid.
struct PropertyKey {
    u32 data1{0};
    u16 data2{0};
    u16 data3{0};
    std::array<u8, 8> data4{};
    u32 pid{0};
    friend bool operator==(const PropertyKey&, const PropertyKey&) = default;
};

/// What PSGetPropertyKeyFromName resolves a name to, apart from its COM
/// apartment check (tools/oracle_probe/propsys_name_probe.c):
///   * a name starting with '{' is a "{fmtid} pid" key string: longer than
///     50 characters -> STRSAFE_E_INSUFFICIENT_BUFFER, unparsable ->
///     E_INVALIDARG, otherwise S_OK whether or not the schema knows it;
///   * otherwise a canonical name (case-sensitive) or a Learn legacy name
///     (ASCII case-insensitive) -> S_OK, else TYPE_E_ELEMENTNOTFOUND.
/// The key is zero on failure.
struct PropertyKeyLookup {
    u32 hr{0};
    PropertyKey key{};
};
[[nodiscard]] PropertyKeyLookup property_key_from_name(std::u16string_view name);

/// Vitreo's schema entry for a key (Windows SDK propkey.h), with the facts
/// winoracle's registered schema adds. nullopt when propkey.h does not
/// define the key.
struct PropertySchema {
    std::string_view canonical_name;
    u16 vt{0};               ///< registered VARTYPE (0 when not registered)
    bool registered{false};  ///< Windows registers a description for it
    bool tree{false};        ///< PDTF_ISTREEPROPERTY
    bool enumerated{false};  ///< has a property enumeration (discrete/ranged)
};
[[nodiscard]] std::optional<PropertySchema> property_schema(const PropertyKey& key);

/// Create the PROPSYS.dll exports implemented by Vitreo. The name lookups
/// need the calling thread's COM apartment (CO_E_NOTINITIALIZED outside one)
/// and return CoTaskMemAlloc'd strings.
[[nodiscard]] DllExports make_propsys_exports(ComCoreManager& com,
                                              ComTaskMemoryManager& task_memory);

} // namespace vitreo::winapi
