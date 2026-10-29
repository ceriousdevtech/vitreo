// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// DLL registry part: wintrust, dbghelp, dbgcore, userenv, MDMRegistration and tbs

#include "dll_registry_internal.hpp"

#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/system/dbghelp/dbghelp.hpp"
#include "winapi/system/userenv/profile_environment.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace vitreo::cli {

void register_trust_profile_tpm_dlls(RuntimeContext& ctx) {
    auto& reg = ctx.thunk_registry;

    // -- wintrust.dll --
    {
        winapi::DllExports wt;
        wt.entries.push_back({"WinVerifyTrust", 0,
            [&mgr = ctx.crypt32_mgr](winapi::Marshaler& m){
                m.set_return(mgr.win_verify_trust(m)); }, 12});
        wt.entries.push_back({"WTHelperGetProvCertFromChain", 0,
            [&mgr = ctx.crypt32_mgr](winapi::Marshaler& m){
                m.set_return(mgr.wt_helper_get_prov_cert_from_chain(m)); }, 8});
        wt.entries.push_back({"WTHelperGetProvSignerFromChain", 0,
            [&mgr = ctx.crypt32_mgr](winapi::Marshaler& m){
                m.set_return(mgr.wt_helper_get_prov_signer_from_chain(m)); }, 16});
        wt.entries.push_back({"WTHelperProvDataFromStateData", 0,
            [&mgr = ctx.crypt32_mgr](winapi::Marshaler& m){
                m.set_return(mgr.wt_helper_prov_data_from_state_data(m)); }, 4});
        wt.entries.push_back({"CryptCATAdminAcquireContext", 0,
            [](winapi::Marshaler& m){
                vitreo::GuestAddr cat = m.arg_ptr(0);
                (void)m.arg(1); (void)m.arg(2);
                if (cat) (void)m.write_u32(cat, 0x434C0001u);
                m.set_return(1);
            }, 12});
        wt.entries.push_back({"CryptCATAdminCalcHashFromFileHandle", 0,
            [](winapi::Marshaler& m){
                (void)m.arg(0);
                vitreo::GuestAddr sz = m.arg_ptr(1);
                vitreo::GuestAddr h = m.arg_ptr(2);
                (void)m.arg(3);
                if (sz) {
                    uint32_t cap = m.read_u32(sz).value_or(0);
                    (void)m.write_u32(sz, 20);
                    if (cap >= 20 && h) {
                        for (uint32_t i = 0; i < 20; ++i) (void)m.write_u8(h + i, 0);
                        m.set_return(1);
                        return;
                    }
                }
                m.set_return(0);
            }, 16});
        wt.entries.push_back({"CryptCATAdminEnumCatalogFromHash", 0,
            [](winapi::Marshaler& m){
                for (uint32_t i = 0; i < 5; ++i) (void)m.arg(i);
                m.set_return(0);
            }, 20});
        wt.entries.push_back({"CryptCATAdminReleaseCatalogContext", 0,
            [](winapi::Marshaler& m){ (void)m.arg(0); (void)m.arg(1); (void)m.arg(2); m.set_return(1); }, 12});
        wt.entries.push_back({"CryptCATAdminReleaseContext", 0,
            [](winapi::Marshaler& m){ (void)m.arg(0); (void)m.arg(1); m.set_return(1); }, 8});
        wt.entries.push_back({"CryptCATCatalogInfoFromContext", 0,
            [](winapi::Marshaler& m){ (void)m.arg(0); (void)m.arg(1); (void)m.arg(2); m.set_return(0); }, 12});
        (void)reg.register_dll("wintrust.dll", std::move(wt));
    }

    // -- dbghelp.dll (no symbol info available) --
    (void)reg.register_dll("dbghelp.dll", winapi::create_dbghelp_thunks());

    // -- dbgcore.dll (minidump entry points; dbghelp forwards to these) --
    // Present since Windows 8.1, so a crash reporter that finds dbghelp will
    // go looking for this too. Without it LoadLibrary fails outright, which a
    // reporter treats as "no debug support" and abandons.
    (void)reg.register_dll("dbgcore.dll", winapi::create_dbgcore_thunks());

    // -- userenv.dll (no per-user profile services on macOS) --
    {
        winapi::DllExports ue;
        // BOOL CreateEnvironmentBlock(LPVOID* lpEnvironment, HANDLE hToken,
        //                            BOOL bInherit)
        // https://learn.microsoft.com/en-us/windows/win32/api/userenv/nf-userenv-createenvironmentblock
        // A Unicode block (NAME=value\0...\0\0) on the process heap,
        // released with DestroyEnvironmentBlock. bInherit TRUE: the current
        // process environment. FALSE: only the system/user/profile variables
        // (no per-process additions) -- Vitreo's system and profile
        // variables are the capsule defaults the environment manager holds.
        // Measured (tools/oracle_probe/s7_misc_probe.expected): success
        // leaves ERROR_ENVVAR_NOT_FOUND as the last error;
        // DestroyEnvironmentBlock(NULL) is FALSE / ERROR_INVALID_PARAMETER.
        ue.entries.push_back({"CreateEnvironmentBlock", 0,
            [&ctx](winapi::Marshaler& m){
                const GuestAddr out = m.arg_ptr(0);
                const bool inherit = m.arg32(2) != 0;
                if (out == 0) {
                    winapi::set_last_error(winapi::error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0);
                    return;
                }
                // bInherit FALSE: the logon block for hToken (NULL = system).
                std::vector<std::pair<std::string, std::string>> vars =
                    inherit ? ctx.env_mgr.get_all_variables_as_named()
                            : winapi::profile_environment_block(
                                  ctx.env_mgr, m.arg(1) == 0
                                      ? winapi::ProfileEnvironment::System
                                      : winapi::ProfileEnvironment::User);
                std::vector<u16> block;
                for (const auto& [name, value] : vars) {
                    for (u16 unit : winapi::utf8_to_utf16(name + "=" + value)) block.push_back(unit);
                    block.push_back(0);
                }
                block.push_back(0);
                if (block.size() == 1) block.push_back(0);
                const u64 heap = ctx.heap_mgr.get_process_heap();
                const u64 memory = ctx.heap_mgr.heap_alloc(
                    static_cast<winapi::Handle>(heap), 0, block.size() * 2);
                if (memory == 0 ||
                    m.memory().write_bytes(memory, reinterpret_cast<const Byte*>(block.data()),
                                           block.size() * 2).is_err()) {
                    winapi::set_last_error(winapi::error_codes::ERROR_NOT_ENOUGH_MEMORY);
                    m.set_return(0);
                    return;
                }
                (void)m.write_ptr(out, memory);
                winapi::set_last_error(winapi::error_codes::ERROR_ENVVAR_NOT_FOUND);
                m.set_return(1);
            }, 12});
        ue.entries.push_back({"DestroyEnvironmentBlock", 0,
            [&ctx](winapi::Marshaler& m){
                const GuestAddr block = m.arg_ptr(0);
                if (block == 0) {
                    winapi::set_last_error(winapi::error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0);
                    return;
                }
                const u64 heap = ctx.heap_mgr.get_process_heap();
                m.set_return(ctx.heap_mgr.heap_free(static_cast<winapi::Handle>(heap), 0, block) ? 1 : 0);
            }, 4});
        // ExpandEnvironmentStringsForUser{W,A}: see profile_environment.hpp.
        auto expand_for_user = [&ctx](winapi::Marshaler& m, bool wide) {
            winapi::impl_ExpandEnvironmentStringsForUser(
                m, ctx.env_mgr, ctx.sec_stubs, ctx.handle_table, wide);
        };
        ue.entries.push_back({"ExpandEnvironmentStringsForUserW", 0,
            [expand_for_user](winapi::Marshaler& m){ expand_for_user(m, true); }, 16});
        ue.entries.push_back({"ExpandEnvironmentStringsForUserA", 0,
            [expand_for_user](winapi::Marshaler& m){ expand_for_user(m, false); }, 16});
        // AppContainer profiles need the Windows profile/firewall service;
        // they fail with the SID out-parameter cleared at pointer width.
        ue.entries.push_back({"CreateAppContainerProfile", 0,
            [](winapi::Marshaler& m){
                if (const GuestAddr out = m.arg_ptr(5); out != 0) (void)m.write_ptr(out, 0);
                static std::once_flag once;
                std::call_once(once, [] {
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED userenv!CreateAppContainerProfile\n");
                });
                m.set_return(0x80004001u); // E_NOTIMPL
            }, 24});
        ue.entries.push_back({"DeleteAppContainerProfile", 0,
            [](winapi::Marshaler& m){ (void)m.arg(0); m.set_return(0x80004001u); }, 4});
        ue.entries.push_back({"DeriveAppContainerSidFromAppContainerName", 0,
            [](winapi::Marshaler& m){
                if (const GuestAddr out = m.arg_ptr(1); out != 0) (void)m.write_ptr(out, 0);
                static std::once_flag once;
                std::call_once(once, [] {
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED userenv!"
                                         "DeriveAppContainerSidFromAppContainerName\n");
                });
                m.set_return(0x80004001u);
            }, 8});
        ue.entries.push_back({"GetUserProfileDirectoryW", 0,
            [](winapi::Marshaler& m){
                (void)m.arg(0); (void)m.arg(1);
                GuestAddr psz = m.arg_ptr(2);
                if (psz) (void)m.write_u32(psz, 1);
                m.set_return(0);
            }, 12});
        ue.entries.push_back({"LoadUserProfileW", 0,
            [](winapi::Marshaler& m){ (void)m.arg(0); (void)m.arg(1); m.set_return(0); }, 8});
        ue.entries.push_back({"UnloadUserProfile", 0,
            [](winapi::Marshaler& m){ (void)m.arg(0); (void)m.arg(1); m.set_return(1); }, 8});
        // Group Policy primitives used when the guest refreshes its
        // proxy. The capsule has one
        // policy store that never changes, so a registered event is never
        // signalled -- which is exactly Windows' behaviour between policy
        // refreshes. Measured in an interactive session
        // (s7_misc_probe desktop run): Enter returns a handle and sets the
        // last error to 0; Leave(NULL) is FALSE / ERROR_INVALID_PARAMETER;
        // Register(NULL) is FALSE / ERROR_INVALID_PARAMETER; Unregister of
        // an event that is not registered is FALSE / ERROR_FILE_NOT_FOUND.
        struct PolicyState {
            std::mutex mutex;
            std::unordered_set<u64> sections;
            std::unordered_set<u64> events;
            u64 next_section{0x67504C50u};
        };
        static PolicyState policy;
        ue.entries.push_back({"EnterCriticalPolicySection", 0,
            [](winapi::Marshaler& m){
                (void)m.arg32(0);
                std::lock_guard lock(policy.mutex);
                const u64 handle = policy.next_section;
                policy.next_section += 4;
                policy.sections.insert(handle);
                winapi::set_last_error(winapi::error_codes::ERROR_SUCCESS);
                m.set_return(handle);
            }, 4});
        ue.entries.push_back({"LeaveCriticalPolicySection", 0,
            [](winapi::Marshaler& m){
                std::lock_guard lock(policy.mutex);
                if (policy.sections.erase(m.arg(0)) == 0) {
                    winapi::set_last_error(winapi::error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0);
                    return;
                }
                m.set_return(1);
            }, 4});
        ue.entries.push_back({"RegisterGPNotification", 0,
            [](winapi::Marshaler& m){
                const u64 event = m.arg(0);
                if (event == 0) {
                    winapi::set_last_error(winapi::error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0);
                    return;
                }
                std::lock_guard lock(policy.mutex);
                policy.events.insert(event);
                m.set_return(1);
            }, 8});
        ue.entries.push_back({"UnregisterGPNotification", 0,
            [](winapi::Marshaler& m){
                std::lock_guard lock(policy.mutex);
                if (policy.events.erase(m.arg(0)) == 0) {
                    winapi::set_last_error(winapi::error_codes::ERROR_FILE_NOT_FOUND);
                    m.set_return(0);
                    return;
                }
                m.set_return(1);
            }, 4});
        ue.entries.push_back({"RefreshPolicy", 0,
            [](winapi::Marshaler& m){ (void)m.arg(0); m.set_return(1); }, 4});
        ue.entries.push_back({"RefreshPolicyEx", 0,
            [](winapi::Marshaler& m){ (void)m.arg(0); (void)m.arg(1); m.set_return(1); }, 8});
        // BOOL GetProfileType(DWORD* pdwFlags) — local non-roaming profile.
        ue.entries.push_back({"GetProfileType", 0,
            [](winapi::Marshaler& m){
                GuestAddr flags = m.arg_ptr(0);
                if (!flags) {
                    winapi::set_last_error(winapi::error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0);
                    return;
                }
                (void)m.write_u32(flags, 0); // not temporary, roaming, or mandatory
                m.set_return(1);
            }, 4});
        (void)reg.register_dll("userenv.dll", std::move(ue));
    }

    // -- MDMRegistration.dll --
    // Capsule has no MDM enrollment. Probe APIs succeed with "not registered"
    // / "registration not allowed" rather than LoadLibrary failure.
    {
        winapi::DllExports mdm;
        // HRESULT IsDeviceRegisteredWithManagement(BOOL*, DWORD, LPWSTR)
        mdm.entries.push_back({"IsDeviceRegisteredWithManagement", 0,
            [](winapi::Marshaler& m){
                const GuestAddr out = m.arg_ptr(0);
                (void)m.arg(1);
                (void)m.arg_ptr(2);
                if (out == 0) {
                    m.set_return(0x80070057u); // E_INVALIDARG
                    return;
                }
                (void)m.write_u32(out, 0); // FALSE — not enrolled
                m.set_return(0); // ERROR_SUCCESS / S_OK
            }, 12});
        // HRESULT IsManagementRegistrationAllowed(BOOL*)
        mdm.entries.push_back({"IsManagementRegistrationAllowed", 0,
            [](winapi::Marshaler& m){
                const GuestAddr out = m.arg_ptr(0);
                if (out == 0) {
                    m.set_return(0x80070057u);
                    return;
                }
                (void)m.write_u32(out, 0); // FALSE — no enrollment path
                m.set_return(0);
            }, 4});
        // HRESULT RegisterDeviceWithManagement(...) — not configured.
        mdm.entries.push_back({"RegisterDeviceWithManagement", 0,
            [](winapi::Marshaler& m){
                for (uint32_t i = 0; i < 3; ++i) (void)m.arg(i);
                m.set_return(0x80180031u); // MENROLL_E_MDM_NOT_CONFIGURED
            }, 12});
        // HRESULT UnregisterDeviceWithManagement(LPCWSTR) — nothing enrolled.
        mdm.entries.push_back({"UnregisterDeviceWithManagement", 0,
            [](winapi::Marshaler& m){
                (void)m.arg_ptr(0);
                m.set_return(0x80180031u);
            }, 4});
        (void)reg.register_dll("MDMRegistration.dll", std::move(mdm));
    }

    // -- tbs.dll (TPM Base Services) --
    // No guest TPM. Return TBS_E_TPM_NOT_FOUND for presence probes.
    {
        winapi::DllExports tbs;
        constexpr u32 TBS_E_TPM_NOT_FOUND = 0x8028400Fu;
        constexpr u32 TBS_E_BAD_PARAMETER = 0x80284002u;
        // TBS_RESULT Tbsi_Context_Create(PCTBS_CONTEXT_PARAMS, PTBS_HCONTEXT)
        tbs.entries.push_back({"Tbsi_Context_Create", 0,
            [](winapi::Marshaler& m){
                (void)m.arg_ptr(0);
                const GuestAddr out = m.arg_ptr(1);
                if (out) (void)m.write_ptr(out, 0);
                m.set_return(TBS_E_TPM_NOT_FOUND);
            }, 8});
        // TBS_RESULT Tbsip_Context_Close(TBS_HCONTEXT)
        tbs.entries.push_back({"Tbsip_Context_Close", 0,
            [](winapi::Marshaler& m){
                (void)m.arg(0);
                m.set_return(TBS_E_BAD_PARAMETER);
            }, 4});
        // TBS_RESULT Tbsi_GetDeviceInfo(UINT32 Size, PVOID Info)
        tbs.entries.push_back({"Tbsi_GetDeviceInfo", 0,
            [](winapi::Marshaler& m){
                (void)m.arg(0);
                (void)m.arg_ptr(1);
                m.set_return(TBS_E_TPM_NOT_FOUND);
            }, 8});
        // The remaining documented tbs.h entry points must exist as exports:
        // callers reach them through the delay-load helper, and a missing
        // procedure raises VcppException 0xC06D007F (ERROR_PROC_NOT_FOUND)
        // rather than letting the caller observe an honest TBS_RESULT.
        // Because Tbsi_Context_Create never hands out a context on a machine
        // with no TPM, every context-taking entry point reports
        // TBS_E_INVALID_CONTEXT, and the context-free ones report
        // TBS_E_TPM_NOT_FOUND. Parameter validation still comes first.
        constexpr u32 TBS_E_INVALID_CONTEXT = 0x80284004u;
        // TBS_RESULT Tbsip_Submit_Command(TBS_HCONTEXT, TBS_COMMAND_LOCALITY,
        //     TBS_COMMAND_PRIORITY, PCBYTE pabCommand, UINT32 cbCommand,
        //     PBYTE pabResult, PUINT32 pcbResult)
        tbs.entries.push_back({"Tbsip_Submit_Command", 0,
            [](winapi::Marshaler& m){
                (void)m.arg(0); // hContext
                const u64 locality = m.arg(1);
                (void)m.arg(2); // Priority
                const GuestAddr command = m.arg_ptr(3);
                const u32 command_size = static_cast<u32>(m.arg(4));
                (void)m.arg_ptr(5); // pabResult
                const GuestAddr result_size = m.arg_ptr(6);
                // Locality four is the highest documented value.
                if (locality > 4 || command == 0 || command_size == 0 ||
                    result_size == 0) {
                    m.set_return(TBS_E_BAD_PARAMETER);
                    return;
                }
                m.set_return(TBS_E_INVALID_CONTEXT);
            }, 28});
        // TBS_RESULT Tbsip_Cancel_Commands(TBS_HCONTEXT)
        tbs.entries.push_back({"Tbsip_Cancel_Commands", 0,
            [](winapi::Marshaler& m){
                (void)m.arg(0);
                m.set_return(TBS_E_INVALID_CONTEXT);
            }, 4});
        // TBS_RESULT Tbsi_Get_TCG_Log(TBS_HCONTEXT, PBYTE pOutputBuf,
        //     PUINT32 pOutputBufLen)
        tbs.entries.push_back({"Tbsi_Get_TCG_Log", 0,
            [](winapi::Marshaler& m){
                (void)m.arg(0);
                (void)m.arg_ptr(1);
                const GuestAddr out_len = m.arg_ptr(2);
                if (out_len == 0) {
                    m.set_return(TBS_E_BAD_PARAMETER);
                    return;
                }
                m.set_return(TBS_E_INVALID_CONTEXT);
            }, 12});
        // TBS_RESULT Tbsi_Get_TCG_Log_Ex(UINT32 logType, PBYTE pbOutput,
        //     PUINT32 pcbOutput) — no context handle, so the honest answer is
        // that the machine has no TPM to have produced a boot log.
        tbs.entries.push_back({"Tbsi_Get_TCG_Log_Ex", 0,
            [](winapi::Marshaler& m){
                (void)m.arg(0);
                (void)m.arg_ptr(1);
                const GuestAddr out_len = m.arg_ptr(2);
                if (out_len == 0) {
                    m.set_return(TBS_E_BAD_PARAMETER);
                    return;
                }
                m.set_return(TBS_E_TPM_NOT_FOUND);
            }, 12});
        // TBS_RESULT Tbsi_Get_OwnerAuth(TBS_HCONTEXT, UINT32 ownerauthType,
        //     PBYTE pOutputBuf, PUINT32 pOutputBufLen)
        tbs.entries.push_back({"Tbsi_Get_OwnerAuth", 0,
            [](winapi::Marshaler& m){
                (void)m.arg(0);
                (void)m.arg(1);
                (void)m.arg_ptr(2);
                const GuestAddr out_len = m.arg_ptr(3);
                if (out_len == 0) {
                    m.set_return(TBS_E_BAD_PARAMETER);
                    return;
                }
                m.set_return(TBS_E_INVALID_CONTEXT);
            }, 16});
        // TBS_RESULT Tbsi_Physical_Presence_Command(TBS_HCONTEXT,
        //     PCBYTE pabInput, UINT32 cbInput, PBYTE pabOutput,
        //     PUINT32 pcbOutput)
        tbs.entries.push_back({"Tbsi_Physical_Presence_Command", 0,
            [](winapi::Marshaler& m){
                (void)m.arg(0);
                const GuestAddr input = m.arg_ptr(1);
                const u32 input_size = static_cast<u32>(m.arg(2));
                (void)m.arg_ptr(3);
                const GuestAddr out_len = m.arg_ptr(4);
                if (input == 0 || input_size == 0 || out_len == 0) {
                    m.set_return(TBS_E_BAD_PARAMETER);
                    return;
                }
                m.set_return(TBS_E_INVALID_CONTEXT);
            }, 20});
        // TBS_RESULT Tbsi_Revoke_Attestation(void)
        tbs.entries.push_back({"Tbsi_Revoke_Attestation", 0,
            [](winapi::Marshaler& m){
                m.set_return(TBS_E_TPM_NOT_FOUND);
            }, 0});
        (void)reg.register_dll("tbs.dll", std::move(tbs));
    }

    // d3d12.dll is registered further down, from create_d3d12_thunks().
    // A two-export stub used to live here. register_dll() keeps the FIRST
    // registration and returns AlreadyExists for any later one, which every
    // caller discards -- so this stub silently shadowed the real table: the
    // D3D12 frontend's exports and vtable thunks never resolved, and neither
    // stub export carried its Windows ordinal, which is how the SDK import
    // library binds D3D12CreateDevice (#101) and D3D12GetDebugInterface
    // (#102). A guest's delay-load of those ordinals then raised
    // 0xC06D007F (delay-load procedure not found) and killed the process.
}

} // namespace vitreo::cli
