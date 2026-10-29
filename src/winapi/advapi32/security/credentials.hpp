// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file credentials.hpp
/// @brief Credential Manager (advapi32 CredReadW / CredWriteW / CredDeleteW /
/// CredFree) backed by a per-capsule store.
///
/// Persistence follows CRED_PERSIST_* (Learn CREDENTIALW): SESSION
/// credentials last for the logon session — modelled as the host boot, so
/// every process of the capsule sees them until the machine restarts —
/// while LOCAL_MACHINE and ENTERPRISE credentials are persisted with the
/// capsule. Validation rules and error codes were measured on Windows 10
/// (tools/oracle_probe/cred_privilege_probe.system.expected).

#include <vitreo/common.hpp>

#include "winapi/core/thunk_registry.hpp"

#include <string>

namespace vitreo::winapi {

class SecurityStubs;

/// Directory holding the capsule's credential files. Empty (the default)
/// keeps everything in memory, which is what bare unit tests use.
void set_credential_store_root(std::string root);

/// Register CredReadW / CredWriteW / CredDeleteW / CredFree. Returned
/// CREDENTIALW blocks come from `stubs`' LocalAlloc heap.
void add_credential_exports(DllExports& exports, SecurityStubs& stubs);

} // namespace vitreo::winapi
