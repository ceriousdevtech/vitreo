// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// DLL registry: populates ThunkRegistry with exports from all managers

#pragma once

namespace vitreo {
class GuestMemory;
}

namespace vitreo::cli {

struct RuntimeContext;

/// Register all DLL exports from managers in @p ctx into @p ctx.thunk_registry.
///
/// Merges per-manager export tables and registers each DLL:
///   kernel32.dll, ntdll.dll, msvcrt.dll, advapi32.dll, gdi32.dll,
///   user32.dll, ole32.dll, shell32.dll, version.dll, ws2_32.dll, mswsock.dll,
///   authz.dll, sspicli.dll, wkscli.dll, netutils.dll, httpapi.dll
void register_all_dlls(RuntimeContext& ctx, GuestMemory& memory, bool is_64bit);

} // namespace vitreo::cli
