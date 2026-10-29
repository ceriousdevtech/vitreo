// SPDX-License-Identifier: LGPL-2.1-only

/// @file security.cpp
/// @brief advapi32 security stubs implementation (WIN32-062)

#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_internal.hpp"
#include "winapi/common/elevation.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "capsule/vfs/vfs.hpp"
#include "ipc/ipc_gate.hpp"

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cwctype>
#include <cstring>
#include <stdlib.h> // arc4random_buf
#include <unistd.h>
#include <pwd.h>
#include <cerrno>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <set>
#include <variant>
#include <CommonCrypto/CommonDigest.h>

#ifdef __APPLE__
#include <os/log.h>
#include <sys/xattr.h>
#endif

/// @file adjusttokenprivileges.cpp
/// @brief AdjustTokenPrivileges
///
/// Closed cut from security.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// AdjustTokenPrivileges
// =============================================================================

bool SecurityStubs::adjust_token_privileges(
    Handle token, bool disable_all,
    const std::vector<TokenPrivilege>& requested,
    bool& all_assigned) {
    all_assigned = true;
    if (!is_token_handle(token)) return false;

    std::lock_guard lock(m_token_mutex);
    const auto found = m_token_states.find(token);
    if (found == m_token_states.end()) return false;
    auto& privileges = found->second->privileges;
    if (disable_all) {
        for (auto& privilege : privileges) {
            privilege.attributes &= ~0x02u; // SE_PRIVILEGE_ENABLED
        }
        return true;
    }

    for (const auto& requested_privilege : requested) {
        const auto privilege = std::find_if(
            privileges.begin(), privileges.end(),
            [&requested_privilege](const TokenPrivilege& candidate) {
                return candidate.luid == requested_privilege.luid;
            });
        if (privilege == privileges.end()) {
            all_assigned = false;
            continue;
        }
        if ((requested_privilege.attributes & 0x04u) != 0) {
            privileges.erase(privilege); // SE_PRIVILEGE_REMOVED
            continue;
        }
        privilege->attributes =
            (privilege->attributes & ~0x02u) |
            (requested_privilege.attributes & 0x02u);
    }
    return true;
}

std::vector<SecurityStubs::TokenPrivilege>
SecurityStubs::default_process_token_privileges() {
    constexpr u32 kEnabled = 0x3u; // ENABLED | ENABLED_BY_DEFAULT
    return {
        {5u, 0u},       // SeIncreaseQuotaPrivilege
        {8u, 0u},       // SeSecurityPrivilege
        {9u, 0u},       // SeTakeOwnershipPrivilege
        {17u, 0u},      // SeBackupPrivilege
        {18u, 0u},      // SeRestorePrivilege
        {19u, 0u},      // SeShutdownPrivilege
        {20u, 0u},      // SeDebugPrivilege
        {23u, kEnabled}, // SeChangeNotifyPrivilege
        {25u, 0u},      // SeUndockPrivilege
        {29u, kEnabled}, // SeImpersonatePrivilege
        {30u, kEnabled}, // SeCreateGlobalPrivilege
    };
}

bool SecurityStubs::describe_privilege_adjustment(
    Handle token, bool disable_all,
    const std::vector<TokenPrivilege>& requested,
    std::vector<TokenPrivilege>& previous_out,
    bool& all_assigned) const {
    previous_out.clear();
    all_assigned = true;
    if (!is_token_handle(token)) return false;

    std::lock_guard lock(m_token_mutex);
    const auto found = m_token_states.find(token);
    if (found == m_token_states.end()) return false;
    const auto& privileges = found->second->privileges;
    if (disable_all) {
        for (const auto& privilege : privileges) {
            if ((privilege.attributes & 0x02u) != 0) {
                previous_out.push_back(privilege);
            }
        }
        return true;
    }
    for (const auto& requested_privilege : requested) {
        const auto privilege = std::find_if(
            privileges.begin(), privileges.end(),
            [&requested_privilege](const TokenPrivilege& candidate) {
                return candidate.luid == requested_privilege.luid;
            });
        if (privilege == privileges.end()) {
            all_assigned = false;
            continue;
        }
        // Learn: removed privileges are not listed in PreviousState.
        if ((requested_privilege.attributes & 0x04u) != 0) continue;
        const u32 next_attributes =
            (privilege->attributes & ~0x02u) |
            (requested_privilege.attributes & 0x02u);
        if (next_attributes != privilege->attributes) {
            previous_out.push_back(*privilege);
        }
    }
    return true;
}

bool SecurityStubs::on_handle_close(Handle token) {
    std::lock_guard lock(m_token_mutex);
    m_token_states.erase(token);
    return true;
}

void SecurityStubs::on_thread_handle_close(Handle thread) {
    std::lock_guard lock(m_token_mutex);
    m_thread_token_states.erase(thread_token_key(thread));
}

void SecurityStubs::duplicate_thread_token_association(
    Handle source, Handle duplicate) {
    std::lock_guard lock(m_token_mutex);
    const auto found = m_thread_token_states.find(thread_token_key(source));
    if (found != m_thread_token_states.end()) {
        m_thread_token_states[thread_token_key(duplicate)] = found->second;
    }
}

} // namespace vitreo::winapi
