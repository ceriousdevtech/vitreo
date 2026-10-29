// SPDX-License-Identifier: LGPL-2.1-only

/// @file dbghelp.hpp
/// @brief dbghelp.dll export table.
///
/// Vitreo ships no symbol database and writes no minidumps, so most of these
/// fail — but they must fail the way Windows documents (correct return value
/// *and* a retrievable last-error), and the exports must exist: crash handlers
/// resolve them one by one and abandon setup the moment one comes back NULL.

#pragma once

#include "winapi/core/thunk_registry.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace vitreo::winapi {

/// Guest environment lookup for the default symbol search path
/// (_NT_SYMBOL_PATH). Wired once by the runtime.
void set_dbghelp_environment_lookup(
    std::function<std::optional<std::string>(std::string_view)> lookup);

/// Build the dbghelp.dll export table.
/// Module facts the symbol handler reads from the process (dbghelp inspects
/// its own process's loaded modules): the base of the module containing an
/// address (0 = none) and, for x64, the address of the RUNTIME_FUNCTION
/// covering a PC in that module's exception directory (0 = none).
struct DbghelpModuleLookup {
    std::function<u64(u64 address)> module_base;
    std::function<u64(u64 pc)> runtime_function;
    /// Whether @p handle names an open kernel object (MiniDumpWriteDump
    /// validates hFile before anything else). Unset = not checked.
    std::function<bool(u64 handle)> is_open_handle;
};
void set_dbghelp_module_lookup(DbghelpModuleLookup lookup);

[[nodiscard]] DllExports create_dbghelp_thunks();

/// Build the dbgcore.dll export table.
///
/// Since Windows 8.1 the minidump entry points live here and dbghelp.dll
/// forwards to them, so the two tables share one definition. A real
/// dbgcore.dll has exactly two named exports; this matches it.
[[nodiscard]] DllExports create_dbgcore_thunks();

}  // namespace vitreo::winapi
