// SPDX-License-Identifier: LGPL-2.1-only

/// @file ntdll_system_services.hpp
/// @brief Which ntdll exports are system services, and their Nt/Zw names.
///
/// Windows ntdll exports every system service twice, as NtXxx and ZwXxx, and
/// both names identify one stub (tools/oracle_probe/ntdll_service_layout_probe:
/// every one of the 490 x64 and 510 WoW64 Zw* exports has an Nt* twin at the
/// same RVA). Every Nt* export is a service except the few that have no Zw*
/// twin and no service stub: NtGetTickCount, NtCurrentTeb (x86), and the
/// NtdllDefWindowProc_* / NtdllDialogWndProc_* window procedures.
///
/// Guests that resolve system calls themselves depend on the pairing: they
/// collect the Zw* exports, sort them by address and take the rank as the
/// service number (SysWhispers2, FreshyCalls), so a service published under
/// only its Nt* name is invisible to them, and every service after it is
/// numbered wrongly.

#pragma once

#include "winapi/core/thunk_registry.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

/// True when @p name is an ntdll system service (an NtXxx or ZwXxx export
/// with a service stub), false for every other ntdll export.
[[nodiscard]] inline bool is_ntdll_system_service(std::string_view name) {
    if (name.size() <= 2) return false;
    if (name.starts_with("Zw")) return true;
    if (!name.starts_with("Nt")) return false;
    return name != "NtGetTickCount" && name != "NtCurrentTeb" &&
           !name.starts_with("Ntdll");
}

/// Publish the missing half of every system service's Nt/Zw pair: the twin
/// is the same export (handler, stdcall bytes, in-chain class) under the
/// other name, name-only like the rest of Vitreo's ntdll. A name registered
/// twice resolves to its last entry (ThunkRegistry::register_dll), so that
/// is the entry its twin copies.
inline void add_ntdll_service_twins(DllExports& exports) {
    std::unordered_map<std::string, usize> kept;
    for (usize i = 0; i < exports.entries.size(); ++i) {
        kept[exports.entries[i].name] = i;
    }
    std::vector<ExportEntry> twins;
    for (usize i = 0; i < exports.entries.size(); ++i) {
        const ExportEntry& entry = exports.entries[i];
        if (kept[entry.name] != i || entry.internal ||
            !is_ntdll_system_service(entry.name)) {
            continue;
        }
        std::string twin =
            (entry.name.starts_with("Nt") ? "Zw" : "Nt") + entry.name.substr(2);
        if (kept.contains(twin)) continue;
        ExportEntry copy = entry;
        copy.name = std::move(twin);
        copy.ordinal = 0;
        twins.push_back(std::move(copy));
    }
    for (auto& twin : twins) exports.entries.push_back(std::move(twin));
}

}  // namespace vitreo::winapi
