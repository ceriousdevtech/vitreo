// SPDX-License-Identifier: LGPL-2.1-only

/// @file lookupaccountsidw.cpp
/// @brief LookupAccountSidW / LookupAccountNameW
///
/// Closed cut from security.cpp — verified with plan_extraction.py.

#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_internal.hpp"

#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi {

namespace {

bool equals_ignore_case(std::wstring_view lhs, std::wstring_view rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (usize i = 0; i < lhs.size(); ++i) {
        if (std::towlower(lhs[i]) != std::towlower(rhs[i])) return false;
    }
    return true;
}

bool local_lookup_system(std::wstring_view system) {
    while (!system.empty() && (system.front() == L'\\' ||
                               system.front() == L'/')) {
        system.remove_prefix(1);
    }
    return system.empty() || equals_ignore_case(system, L".") ||
           equals_ignore_case(system, L"VITREO") ||
           equals_ignore_case(system, L"BUILTIN") ||
           equals_ignore_case(system, L"NT AUTHORITY");
}

} // namespace

bool SecurityStubs::is_local_system_name(std::wstring_view system) {
    return local_lookup_system(system);
}

bool SecurityStubs::lookup_account_sid_w(std::span<const u8> sid,
                                         std::wstring& name_out,
                                         std::wstring& domain_out,
                                         u32& use_out) {
    return account_from_sid(sid, name_out, domain_out, use_out);
}

bool SecurityStubs::lookup_account_name_w(
    const std::wstring& system_name, const std::wstring& account_name,
    std::vector<u8>& sid_out, std::wstring& domain_out, u32& use_out) {
    if (!local_lookup_system(system_name)) return false;

    // The qualifier must be the account's own domain (sid_from_account_name
    // checks it), so OTHERDOM\Users does not resolve to BUILTIN\Users.
    std::wstring_view account = account_name;
    if (account.empty()) return false;

    std::string ansi;
    ansi.reserve(account.size());
    for (wchar_t ch : account) {
        if (ch > 0x7f) return false;
        ansi.push_back(static_cast<char>(ch));
    }
    if (!sid_from_account_name(ansi, sid_out)) return false;
    std::wstring name;
    return account_from_sid(sid_out, name, domain_out, use_out);
}

} // namespace vitreo::winapi
