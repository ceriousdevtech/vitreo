#pragma once
/// @file api_set_resolver.hpp
/// @brief Maps Windows API Set schema DLL names to real DLL names

#include <string>
#include <string_view>

namespace vitreo::pe {

/// Resolve an API Set schema name to a real DLL name.
///
/// Handles mappings like:
///   api-ms-win-core-processthreads-l1-1-0.dll → kernel32.dll
///   api-ms-win-core-rtlsupport-l1-1-0.dll    → ntdll.dll
///   api-ms-win-security-systemfunctions-*    → advapi32.dll
///   api-ms-win-security-base-*               → advapi32.dll
///   ext-ms-win-*                              → kernel32.dll
///
/// @param dll_name The DLL name from the PE import table
/// @return The resolved real DLL name, or the original name if no mapping exists
[[nodiscard]] std::string_view resolve_api_set(std::string_view dll_name);

} // namespace vitreo::pe
