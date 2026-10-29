#pragma once

/// @file profile_environment.hpp
/// @brief userenv profile environment blocks and %VAR% expansion against them.
///
/// CreateEnvironmentBlock(bInherit=FALSE) and ExpandEnvironmentStringsForUser
/// do not read the calling process's environment: they build the block a
/// user (or, for a NULL token, the system) gets at logon. Measured on Windows
/// 11 by tools/oracle_probe/userenv_computer_object_probe.c ("block" mode).

#include <vitreo/common.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vitreo::winapi {

class EnvironmentManager;
class HandleTable;
class Marshaler;
class SecurityStubs;

using EnvironmentVariables = std::vector<std::pair<std::string, std::string>>;

/// Whose logon environment a block describes.
enum class ProfileEnvironment {
    System,  ///< hToken == NULL: system variables only, the Default profile
    User,    ///< a user token: system plus the user's profile variables
};

/// The logon block for @p who, derived from the capsule's system and profile
/// variables held by @p env (per-process additions are not included).
[[nodiscard]] EnvironmentVariables profile_environment_block(
    const EnvironmentManager& env, ProfileEnvironment who);

/// Case-insensitive lookup in a block built by profile_environment_block.
[[nodiscard]] std::optional<std::string> find_environment_variable(
    const EnvironmentVariables& block, std::string_view name);

/// ExpandEnvironmentStrings' %NAME% rule: a defined, non-empty NAME is
/// replaced; an undefined one, and a lone '%', are kept literally.
[[nodiscard]] std::string expand_environment_percent_tokens(
    std::string_view input,
    const std::function<std::optional<std::string>(std::string_view)>& lookup);

/// BOOL ExpandEnvironmentStringsForUser{W,A}(HANDLE hToken, LPCTSTR lpSrc,
///     LPTSTR lpDest, DWORD dwSize)
/// https://learn.microsoft.com/en-us/windows/win32/api/userenv/nf-userenv-expandenvironmentstringsforuserw
/// Expands against the logon block for hToken (NULL: system variables only),
/// never the caller's environment. Measured on Windows 11, x64 and x86 alike
/// (tools/oracle_probe/userenv_computer_object_probe.c):
///  - a non-NULL handle that is not a token: FALSE, ERROR_INVALID_HANDLE;
///  - dwSize < length+1: FALSE, ERROR_INSUFFICIENT_BUFFER; W copies the first
///    dwSize-1 characters with no terminator, A writes nothing;
///  - success: W returns TRUE, A returns length+1, and the last error is left
///    at ERROR_ENVVAR_NOT_FOUND -- or ERROR_ACCESS_DENIED when the token lacks
///    TOKEN_IMPERSONATE (the expansion still succeeds).
void impl_ExpandEnvironmentStringsForUser(Marshaler& m,
                                          const EnvironmentManager& env,
                                          const SecurityStubs& security,
                                          const HandleTable& handles,
                                          bool wide);

} // namespace vitreo::winapi
