// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file secur32.hpp
/// @brief Secur32.dll / Security.dll — SSPI Schannel security package.
///
/// The Schannel package (AcquireCredentialsHandle / InitializeSecurityContext /
/// EncryptMessage / DecryptMessage / …) is implemented for real against macOS
/// Secure Transport — see schannel_tls.hpp. libcurl's schannel backend
/// reaches this package by LoadLibrary-ing
/// security.dll and calling through the table returned by InitSecurityInterface,
/// so both DLL names share one export set. Non-Schannel packages (Kerberos,
/// NTLM, LSA) remain honest failures.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo {
namespace winapi {

class ModuleManager;
class HeapManager;
class Crypt32Manager;

/// Owns the wiring Schannel SSPI needs that a plain thunk lambda cannot reach:
/// the ModuleManager (to resolve this DLL's own export addresses when building
/// the guest SecurityFunctionTable) and the HeapManager (to allocate output
/// tokens the caller frees via FreeContextBuffer).
class Secur32Manager {
public:
    void set_module_manager(ModuleManager* mm) noexcept { m_module_mgr = mm; }
    void set_heap_manager(HeapManager* h) noexcept { m_heap_mgr = h; }
    void set_crypt32_manager(Crypt32Manager* c) noexcept { m_crypt32 = c; }

    [[nodiscard]] ModuleManager* module_manager() const noexcept { return m_module_mgr; }
    [[nodiscard]] HeapManager* heap_manager() const noexcept { return m_heap_mgr; }
    [[nodiscard]] Crypt32Manager* crypt32_manager() const noexcept { return m_crypt32; }

    /// Cached guest addresses of the ANSI and Unicode SecurityFunctionTables.
    /// Their layouts match, but their text-taking entries must not be shared.
    GuestAddr func_table_a{0};
    GuestAddr func_table_w{0};

private:
    ModuleManager* m_module_mgr{nullptr};
    HeapManager* m_heap_mgr{nullptr};
    Crypt32Manager* m_crypt32{nullptr};
};

/// Build the export table shared by secur32.dll and security.dll.
DllExports create_secur32_thunks(Secur32Manager& mgr);

} // namespace winapi
} // namespace vitreo
