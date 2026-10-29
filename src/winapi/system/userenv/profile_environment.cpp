// SPDX-License-Identifier: LGPL-2.1-only
/// @file profile_environment.cpp
/// @brief userenv profile environment blocks and %VAR% expansion against them.

#include "winapi/system/userenv/profile_environment.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <algorithm>
#include <cctype>

namespace vitreo::winapi {

namespace {

bool equals_ignore_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// The variables a logon block carries (machine environment plus the user's
// profile variables). Anything else in the capsule environment is a
// per-process addition and stays out of a bInherit=FALSE block.
constexpr std::string_view kSystemAndProfile[] = {
    "ALLUSERSPROFILE", "APPDATA", "CommonProgramFiles",
    "CommonProgramFiles(x86)", "CommonProgramW6432",
    "COMPUTERNAME", "ComSpec", "DriverData", "HOMEDRIVE",
    "HOMEPATH", "LOCALAPPDATA", "LOGONSERVER",
    "NUMBER_OF_PROCESSORS", "OS", "Path", "PATHEXT",
    "PROCESSOR_ARCHITECTURE", "PROCESSOR_IDENTIFIER",
    "PROCESSOR_LEVEL", "PROCESSOR_REVISION", "ProgramData",
    "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432",
    "PSModulePath", "PUBLIC", "SystemDrive", "SystemRoot",
    "TEMP", "TMP", "USERDOMAIN", "USERDOMAIN_ROAMINGPROFILE",
    "USERNAME", "USERPROFILE", "windir"};

// Present in a user's block, absent from the NULL-token block (probe
// "block null" vs "block proc").
constexpr std::string_view kUserOnly[] = {
    "APPDATA", "HOMEDRIVE", "HOMEPATH", "LOCALAPPDATA", "LOGONSERVER",
    "OneDrive", "USERDOMAIN", "USERDOMAIN_ROAMINGPROFILE"};

template <usize N>
bool in_list(const std::string_view (&list)[N], std::string_view name) {
    return std::any_of(std::begin(list), std::end(list),
                       [&](std::string_view entry) {
                           return equals_ignore_case(entry, name);
                       });
}

} // namespace

std::optional<std::string> find_environment_variable(
    const EnvironmentVariables& block, std::string_view name) {
    for (const auto& [key, value] : block) {
        if (equals_ignore_case(key, name)) return value;
    }
    return std::nullopt;
}

EnvironmentVariables profile_environment_block(const EnvironmentManager& env,
                                               ProfileEnvironment who) {
    EnvironmentVariables block;
    for (auto& entry : env.get_all_variables_as_named()) {
        if (!in_list(kSystemAndProfile, entry.first)) continue;
        if (who == ProfileEnvironment::System && in_list(kUserOnly, entry.first))
            continue;
        block.push_back(std::move(entry));
    }
    if (who == ProfileEnvironment::User) return block;

    // The system block is the LocalSystem logon over the Default profile:
    // USERNAME=SYSTEM, USERPROFILE=<SystemDrive>\Users\Default and the
    // machine TEMP/TMP of %SystemRoot%\TEMP.
    const std::string drive =
        find_environment_variable(block, "SystemDrive").value_or("C:");
    const std::string root =
        find_environment_variable(block, "SystemRoot").value_or(drive + "\\Windows");
    for (auto& [key, value] : block) {
        if (equals_ignore_case(key, "USERNAME")) value = "SYSTEM";
        else if (equals_ignore_case(key, "USERPROFILE")) value = drive + "\\Users\\Default";
        else if (equals_ignore_case(key, "TEMP") || equals_ignore_case(key, "TMP"))
            value = root + "\\TEMP";
    }
    return block;
}

std::string expand_environment_percent_tokens(
    std::string_view input,
    const std::function<std::optional<std::string>(std::string_view)>& lookup) {
    std::string expanded;
    expanded.reserve(input.size());
    usize i = 0;
    while (i < input.size()) {
        if (input[i] != '%') {
            expanded += input[i++];
            continue;
        }
        const usize end = input.find('%', i + 1);
        if (end == std::string_view::npos || end == i + 1) {
            expanded += '%';
            ++i;
            continue;
        }
        const auto value = lookup(input.substr(i + 1, end - i - 1));
        if (value && !value->empty()) {
            expanded += *value;
        } else {
            // Variable not found -- keep the original %VAR% text.
            expanded.append(input.substr(i, end - i + 1));
        }
        i = end + 1;
    }
    return expanded;
}

void impl_ExpandEnvironmentStringsForUser(Marshaler& m,
                                          const EnvironmentManager& env,
                                          const SecurityStubs& security,
                                          const HandleTable& handles,
                                          bool wide) {
    constexpr u32 kTokenImpersonate = 0x0004;
    const u64 token = m.arg(0);
    const GuestAddr src = m.arg_ptr(1);
    const GuestAddr dest = m.arg_ptr(2);
    const u32 size = m.arg32(3);
    if (token != 0 && !security.is_token_handle(static_cast<Handle>(token))) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    auto input = wide ? m.read_wide_string(src) : m.read_ansi_string(src);
    if (input.is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    const auto block = profile_environment_block(
        env, token == 0 ? ProfileEnvironment::System : ProfileEnvironment::User);
    const std::string expanded = expand_environment_percent_tokens(
        input.value(), [&block](std::string_view name) {
            return find_environment_variable(block, name);
        });
    const bool impersonable =
        token == 0 ||
        (handles.get_type(static_cast<Handle>(token)) == HandleType::Token &&
         (handles.get_access(static_cast<Handle>(token)) & kTokenImpersonate) != 0);
    if (wide) {
        const std::vector<u16> units = utf8_to_utf16(expanded);
        const u32 needed = static_cast<u32>(units.size()) + 1;
        if (size < needed) {
            for (u32 i = 0; dest != 0 && i + 1 < size; ++i)
                (void)m.write_u16(dest + i * 2, units[i]);
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return;
        }
        for (u32 i = 0; i < units.size(); ++i)
            (void)m.write_u16(dest + i * 2, units[i]);
        (void)m.write_u16(dest + (needed - 1) * 2, 0);
        m.set_return(1);
    } else {
        const std::string ansi = utf8_to_acp(expanded);
        const u32 needed = static_cast<u32>(ansi.size()) + 1;
        if (size < needed) {
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return;
        }
        for (u32 i = 0; i < ansi.size(); ++i)
            (void)m.write_u8(dest + i, static_cast<u8>(ansi[i]));
        (void)m.write_u8(dest + needed - 1, 0);
        m.set_return(needed);
    }
    set_last_error(impersonable ? error_codes::ERROR_ENVVAR_NOT_FOUND
                                : error_codes::ERROR_ACCESS_DENIED);
}

} // namespace vitreo::winapi
