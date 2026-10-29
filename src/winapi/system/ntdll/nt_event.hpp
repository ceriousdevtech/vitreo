#pragma once
/// @file nt_event.hpp
/// @brief ntdll's event services (NtCreateEvent, NtOpenEvent, NtSetEvent,
///        NtResetEvent, NtClearEvent, NtPulseEvent, NtQueryEvent) over the
///        SyncManager objects CreateEvent uses, and the NT object names of the
///        Win32 BaseNamedObjects namespace.

#include <string>
#include <string_view>

#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/handle.hpp"

namespace vitreo::winapi {

class SyncManager;

namespace ntdll {

/// The Terminal Services session every Vitreo process runs in
/// (ProcessIdToSessionId, WTSGetActiveConsoleSessionId report 1).
inline constexpr u32 kVitreoSessionId = 1;

/// An NT object path (OBJECT_ATTRIBUTES.ObjectName with no RootDirectory) as
/// the Win32 name SyncManager keys it by: "\Sessions\1\BaseNamedObjects\X" is
/// X (Local\X), "\BaseNamedObjects\X" is Global\X. Returns the NTSTATUS for a
/// path that names no BaseNamedObjects entry (0 on success).
[[nodiscard]] u32 nt_object_path_to_win32(std::string_view nt_path, std::string& win32_name);

/// The NT path ObjectNameInformation reports for a Win32 object name
/// (the inverse of nt_object_path_to_win32).
[[nodiscard]] std::string win32_object_name_to_nt(std::string_view win32_name);

DllExports make_nt_event_exports(SyncManager& sync, HandleTable& handles);

}  // namespace ntdll
}  // namespace vitreo::winapi
