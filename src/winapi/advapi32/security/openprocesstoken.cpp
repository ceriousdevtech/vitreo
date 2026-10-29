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

/// @file openprocesstoken.cpp
/// @brief OpenProcessToken
///
/// Closed cut from security.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// OpenProcessToken
// =============================================================================

std::shared_ptr<SecurityStubs::TokenState>
SecurityStubs::primary_token_object_locked() {
    if (!m_primary_token_object) {
        m_primary_token_object =
            std::make_shared<TokenState>(m_primary_token_state);
    }
    return m_primary_token_object;
}

u32 SecurityStubs::map_token_access(u32 access) {
    // The token object type's GENERIC_MAPPING, measured rather than taken from
    // winnt.h's TOKEN_READ/TOKEN_WRITE/TOKEN_EXECUTE (which are narrower):
    // tools/oracle_probe/advapi_token2_probe.expected opt_access /
    // dte_generic_*.
    constexpr u32 GENERIC_READ = 0x80000000u, GENERIC_WRITE = 0x40000000u,
                  GENERIC_EXECUTE = 0x20000000u, GENERIC_ALL = 0x10000000u,
                  MAXIMUM_ALLOWED = 0x02000000u;
    constexpr u32 kGenericRead = 0x0002001Au;    // READ_CONTROL|QUERY_SOURCE|QUERY|DUPLICATE
    constexpr u32 kGenericWrite = 0x000201E0u;   // READ_CONTROL|ADJUST_*
    constexpr u32 kGenericExecute = 0x00020005u; // READ_CONTROL|IMPERSONATE|ASSIGN_PRIMARY
    constexpr u32 kGenericAll = 0x000F01FFu;     // TOKEN_ALL_ACCESS
    u32 mapped = access & 0x00FFFFFFu & ~MAXIMUM_ALLOWED;
    if (access & GENERIC_READ) mapped |= kGenericRead;
    if (access & GENERIC_WRITE) mapped |= kGenericWrite;
    if (access & GENERIC_EXECUTE) mapped |= kGenericExecute;
    if ((access & (GENERIC_ALL | MAXIMUM_ALLOWED)) != 0) mapped |= kGenericAll;
    return mapped;
}

bool SecurityStubs::open_process_token(Handle process, u32 access,
                                       Handle& token_out, u32& error) {
    constexpr Handle CURRENT_PROCESS_PSEUDO = 0xFFFFFFFFu;
    constexpr u32 PROCESS_QUERY_INFORMATION = 0x0400u;
    constexpr u32 PROCESS_QUERY_LIMITED_INFORMATION = 0x1000u;
    token_out = NULL_HANDLE;
    if (process != CURRENT_PROCESS_PSEUDO) {
        // advapi_token_probe opt_null / opt_event / opt_garbage: anything
        // that is not a process handle is ERROR_INVALID_HANDLE.
        if (m_handles.get_type(process) != HandleType::Process) {
            error = error_codes::ERROR_INVALID_HANDLE;
            return false;
        }
        const u32 granted = m_handles.get_access(process);
        if (granted != HandleAccess::All &&
            (granted & (PROCESS_QUERY_INFORMATION |
                        PROCESS_QUERY_LIMITED_INFORMATION)) == 0) {
            error = error_codes::ERROR_ACCESS_DENIED;
            return false;
        }
    }
    // An access check cannot grant nothing, nor a right a token does not
    // have (SYNCHRONIZE, 0x200): ERROR_ACCESS_DENIED (probe opt_access
    // req=0 / 0x200 / 0x100008).
    // HandleAccess::All is Vitreo's in-process "every right" request (host
    // callers of this API); the OpenProcessToken thunk never forwards it.
    const u32 mapped = access == HandleAccess::All ? 0x000F01FFu : map_token_access(access);
    if (mapped == 0 || (mapped & ~kTokenValidAccess) != 0) {
        error = error_codes::ERROR_ACCESS_DENIED;
        return false;
    }
    token_out = m_handles.allocate(HandleType::Token, nullptr, 0, mapped);
    if (token_out == 0) {
        error = error_codes::ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    std::lock_guard lock(m_token_mutex);
    m_token_states[token_out] = primary_token_object_locked();
    error = error_codes::ERROR_SUCCESS;
    return true;
}

u64 SecurityStubs::thread_token_key(Handle thread) const {
    // A thread's token belongs to the THREAD, not to the handle used to
    // reach it: every handle to one thread keys the same entry.
    constexpr Handle CURRENT_THREAD_PSEUDO = 0xFFFFFFFEu;
    if (thread == CURRENT_THREAD_PSEUDO) {
        return (u64{1} << 63) |
               static_cast<u64>(ThreadManager::get_current_thread_id_static());
    }
    if (m_threads != nullptr) {
        if (const GuestThread* t = m_threads->get_thread_by_handle(thread);
            t != nullptr) {
            return (u64{1} << 63) | static_cast<u64>(t->thread_id);
        }
    }
    return static_cast<u64>(thread);
}

bool SecurityStubs::set_thread_token(Handle thread, Handle token, u32& error) {
    constexpr Handle CURRENT_THREAD_PSEUDO = 0xFFFFFFFEu;
    constexpr u32 TOKEN_IMPERSONATE = 0x0004;

    if (thread != CURRENT_THREAD_PSEUDO &&
        m_handles.get_type(thread) != HandleType::Thread) {
        error = error_codes::ERROR_INVALID_HANDLE;
        return false;
    }

    const u64 key = thread_token_key(thread);
    if (token == NULL_HANDLE) {
        std::lock_guard lock(m_token_mutex);
        m_thread_token_states.erase(key);
        error = error_codes::ERROR_SUCCESS;
        return true;
    }
    if (!is_token_handle(token)) {
        error = error_codes::ERROR_INVALID_HANDLE;
        return false;
    }

    const TokenState state = token_state(token);
    if (state.type != 2) {
        error = error_codes::ERROR_BAD_TOKEN_TYPE;
        return false;
    }
    if (m_handles.get_type(token) == HandleType::Token &&
        !token_has_access(token, TOKEN_IMPERSONATE)) {
        error = error_codes::ERROR_ACCESS_DENIED;
        return false;
    }

    std::shared_ptr<TokenState> token_object;
    {
        std::lock_guard lock(m_token_mutex);
        const auto found = m_token_states.find(token);
        if (found != m_token_states.end()) token_object = found->second;
        if (!token_object) {
            token_object = std::make_shared<TokenState>(state);
        }
        m_thread_token_states[key] = std::move(token_object);
    }
    error = error_codes::ERROR_SUCCESS;
    return true;
}

bool SecurityStubs::open_thread_token(Handle thread, u32 access,
                                      Handle& token_out, u32& error) {
    constexpr Handle CURRENT_THREAD_PSEUDO = 0xFFFFFFFEu;
    token_out = NULL_HANDLE;
    if (thread != CURRENT_THREAD_PSEUDO &&
        m_handles.get_type(thread) != HandleType::Thread) {
        error = error_codes::ERROR_INVALID_HANDLE;
        return false;
    }

    std::shared_ptr<TokenState> token_object;
    {
        std::lock_guard lock(m_token_mutex);
        const auto found = m_thread_token_states.find(thread_token_key(thread));
        if (found == m_thread_token_states.end()) {
            error = error_codes::ERROR_NO_TOKEN;
            return false;
        }
        token_object = found->second;
    }
    // Learn: an anonymous-level token is not opened
    // (advapi_token_probe ott_anon: ERROR_CANT_OPEN_ANONYMOUS).
    if (token_object->type == 2 && token_object->impersonation_level == 0) {
        error = 1347u; // ERROR_CANT_OPEN_ANONYMOUS
        return false;
    }

    token_out = m_handles.allocate(
        HandleType::Token, nullptr, 0, map_token_access(access));
    if (token_out == NULL_HANDLE) {
        error = error_codes::ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    {
        std::lock_guard lock(m_token_mutex);
        m_token_states[token_out] = std::move(token_object);
    }
    error = error_codes::ERROR_SUCCESS;
    return true;
}

bool SecurityStubs::impersonate_self(u32 impersonation_level, u32& error) {
    if (impersonation_level > 3) {
        error = error_codes::ERROR_INVALID_PARAMETER;
        return false;
    }
    std::lock_guard lock(m_token_mutex);
    TokenState state = *primary_token_object_locked();
    state.type = 2;
    state.impersonation_level = impersonation_level;
    state.token_id = m_next_token_luid++;
    state.modified_id = m_next_token_luid++;
    m_thread_token_states[thread_token_key(0xFFFFFFFEu)] =
        std::make_shared<TokenState>(std::move(state));
    error = error_codes::ERROR_SUCCESS;
    return true;
}

bool SecurityStubs::impersonate_logged_on_user(Handle token, u32& error) {
    if (!is_token_handle(token)) {
        error = error_codes::ERROR_INVALID_HANDLE;
        return false;
    }
    TokenState state = token_state(token);
    // Learn: the handle needs TOKEN_QUERY plus TOKEN_DUPLICATE (primary
    // token) or TOKEN_IMPERSONATE (impersonation token).
    constexpr u32 TOKEN_DUPLICATE = 0x0002, TOKEN_IMPERSONATE = 0x0004,
                  TOKEN_QUERY = 0x0008;
    if (m_handles.get_type(token) == HandleType::Token &&
        !token_has_access(token, TOKEN_QUERY |
                                     (state.type == 1 ? TOKEN_DUPLICATE
                                                      : TOKEN_IMPERSONATE))) {
        error = error_codes::ERROR_ACCESS_DENIED;
        return false;
    }
    // Learn: tokens below SecurityImpersonation cannot be used to impersonate.
    if (state.type == 2 && state.impersonation_level < 2) {
        error = 1346u; // ERROR_BAD_IMPERSONATION_LEVEL
        return false;
    }
    state.type = 2;
    if (state.impersonation_level < 2) {
        state.impersonation_level = 2;
    }
    std::lock_guard lock(m_token_mutex);
    state.token_id = m_next_token_luid++;
    state.modified_id = m_next_token_luid++;
    m_thread_token_states[thread_token_key(0xFFFFFFFEu)] =
        std::make_shared<TokenState>(std::move(state));
    error = error_codes::ERROR_SUCCESS;
    return true;
}

u32 SecurityStubs::impersonate_anonymous(Handle thread) {
    constexpr Handle CURRENT_THREAD_PSEUDO = 0xFFFFFFFEu;
    if (thread != CURRENT_THREAD_PSEUDO &&
        m_handles.get_type(thread) != HandleType::Thread) {
        return error_codes::ERROR_INVALID_HANDLE;   // probe anon_badhandle / anon_event
    }
    std::lock_guard lock(m_token_mutex);
    TokenState state;
    state.type = 2;
    state.impersonation_level = 2;
    state.anonymous = true;
    state.elevated = false;
    state.privileges.clear();
    state.integrity_rid = 0;  // SECURITY_MANDATORY_UNTRUSTED_RID
    state.authentication_id = 0x3E6;  // ANONYMOUS_LOGON_LUID
    state.token_id = m_next_token_luid++;
    state.modified_id = m_next_token_luid++;
    m_thread_token_states[thread_token_key(thread)] =
        std::make_shared<TokenState>(std::move(state));
    return error_codes::ERROR_SUCCESS;
}

bool SecurityStubs::impersonate_com_caller(bool elevated) {
    std::lock_guard lock(m_token_mutex);
    TokenState state = *primary_token_object_locked();
    state.type = 2;
    state.impersonation_level = 2;
    state.elevated = elevated;
    state.token_id = m_next_token_luid++;
    state.modified_id = m_next_token_luid++;
    m_thread_token_states[thread_token_key(0xFFFFFFFEu)] =
        std::make_shared<TokenState>(std::move(state));
    return true;
}

bool SecurityStubs::token_is_elevated(Handle token) const {
    if (token == 0 || token == 0xFFFFFFFCu) {
        return is_elevated();
    }
    if (token == 0xFFFFFFFBu || token == 0xFFFFFFFAu) {
        std::lock_guard lock(m_token_mutex);
        const auto found = m_thread_token_states.find(
            thread_token_key(0xFFFFFFFEu));
        return found != m_thread_token_states.end()
            ? found->second->elevated
            : is_elevated();
    }
    std::lock_guard lock(m_token_mutex);
    const auto found = m_token_states.find(token);
    return found != m_token_states.end()
        ? found->second->elevated
        : is_elevated();
}

bool SecurityStubs::revert_to_self() {
    std::lock_guard lock(m_token_mutex);
    m_thread_token_states.erase(thread_token_key(0xFFFFFFFEu));
    return true;
}

} // namespace vitreo::winapi
