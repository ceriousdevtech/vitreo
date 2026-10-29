// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// DLL registry part: ws2_32, mswsock, oleaut32, crypt32 and the security / netapi DLLs

#include "dll_registry_internal.hpp"

#include "windowing/cocoa/cocoa_bridge.hpp"

#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::cli {

void register_winsock_oleaut_crypto_dlls(RuntimeContext& ctx) {
    auto& reg = ctx.thunk_registry;

    // -- ws2_32.dll --
    {
        ctx.ws2_mgr.set_sync_manager(&ctx.sync_mgr);
        ctx.ws2_mgr.set_heap_manager(&ctx.heap_mgr);
        ctx.ws2_mgr.set_host_name_provider(
            [&ctx]() { return ctx.sysinfo_mgr.get_computer_name(); });
        ctx.ws2_mgr.set_post_message(
            [&ctx](u64 hwnd, u32 msg, u64 wparam, u64 lparam) {
                return ctx.msgsend_mgr.post_message(
                    static_cast<windowing::HWND>(static_cast<u32>(hwnd)), msg,
                    wparam, lparam);
            });
        auto ws2_exports = winapi::create_ws2_thunks(ctx.ws2_mgr);
        (void)reg.register_dll("ws2_32.dll", std::move(ws2_exports));
        // Map the scratch buffer for DNS results (getaddrinfo, gethostbyname, etc.)
        ctx.vmm.virtual_alloc(winapi::WS2_SCRATCH_BASE,
                              winapi::WS2_SCRATCH_SIZE,
                              0x3000 /*MEM_COMMIT|MEM_RESERVE*/,
                              0x04   /*PAGE_READWRITE*/);
    }

    // -- mswsock.dll --
    {
        auto mswsock_exports = winapi::create_mswsock_thunks(ctx.mswsock_mgr, ctx.ws2_mgr);
        (void)reg.register_dll("mswsock.dll", std::move(mswsock_exports));
    }

    // -- oleaut32.dll --
    {
        // LoadTypeLib reads .tlb files through the capsule VFS and
        // RegisterTypeLib publishes HKCR\TypeLib entries in the capsule hive;
        // the thunk registry supplies the ITypeLib vtable slots.
        ctx.oleaut32_mgr.set_vfs(&ctx.vfs);
        ctx.oleaut32_mgr.set_registry(&ctx.registry);
        ctx.oleaut32_mgr.set_thunk_registry(&ctx.thunk_registry);
        ctx.com_mgr.set_automation_layout_resolver(
            [&oleaut = ctx.oleaut32_mgr](const winapi::Guid& iid)
                -> std::optional<winapi::ComCoreManager::AutomationInterfaceLayout> {
                winapi::TypeLibGuid typelib_iid{};
                std::memcpy(typelib_iid.data(), &iid, sizeof(iid));
                auto info = oleaut.resolve_registered_type_info(typelib_iid);
                if (!info) return std::nullopt;
                winapi::ComCoreManager::AutomationInterfaceLayout layout;
                layout.methods.reserve(info->funcs.size());
                for (const auto& function : info->funcs) {
                    std::vector<winapi::Guid> parameter_iids;
                    parameter_iids.reserve(function.param_iids.size());
                    for (const auto& parameter_iid : function.param_iids) {
                        winapi::Guid guid{};
                        std::memcpy(&guid, parameter_iid.data(), sizeof(guid));
                        parameter_iids.push_back(guid);
                    }
                    layout.methods.push_back({
                        function.o_vft,
                        function.c_params,
                        function.param_vts,
                        function.param_flags,
                        std::move(parameter_iids),
                        function.name});
                }
                return layout;
            });
        ctx.com_mgr.set_automation_bstr_allocator(
            [&oleaut = ctx.oleaut32_mgr](
                winapi::Marshaler& marshaler,
                std::span<const Byte> utf16_bytes) {
                return oleaut.allocate_bstr_bytes(marshaler, utf16_bytes);
            });
        // BSTRs and SAFEARRAYs come from the COM task allocator, as on
        // Windows, so SysFreeString / SafeArrayDestroy really free them.
        ctx.oleaut32_mgr.set_task_memory(&ctx.com_task_memory_mgr);
        ctx.oleaut32_mgr.set_virtual_memory_manager(&ctx.vmm);
        auto oleaut32_exports = winapi::create_oleaut32_thunks(ctx.oleaut32_mgr);
        (void)reg.register_dll("oleaut32.dll", std::move(oleaut32_exports));
        // RESERVE the scratch range for typelib objects; the
        // bump allocator commits it in 1 MB steps as it advances.
        //
        // Committing all 256 MB here made every guest process pay for it
        // whether or not it ever called oleaut32 -- it WAS the dirty-memory
        // floor, 256 MB of a hello-world guest's 300 MB, and four times over
        // for four guest processes.
        ctx.vmm.virtual_alloc(winapi::OLEAUT32_SCRATCH_BASE,
                              winapi::OLEAUT32_SCRATCH_SIZE,
                              0x2000 /*MEM_RESERVE*/,
                              0x04   /*PAGE_READWRITE*/);
    }

    // -- authz.dll --
    {
        auto authz_exports = winapi::create_authz_thunks(ctx.authz_mgr);
        (void)reg.register_dll("authz.dll", std::move(authz_exports));
    }

    // -- crypt32.dll --
    {
        ctx.crypt32_mgr.set_vfs(&ctx.vfs);
        ctx.crypt32_mgr.set_root_auto_update(true);
        ctx.crypt32_mgr.set_local_memory(&ctx.gl_mgr);
        // Capsule-local DPAPI master key avoids Keychain prompts after an
        // unsigned rebuild. VITREO_DPAPI_KEY_FILE still overrides when set.
        if (!ctx.capsule_root().empty()) {
            ctx.crypt32_mgr.set_dpapi_key_file(
                ctx.capsule_root() + "/dpapi-master.key");
        }
        auto crypt32_exports = winapi::create_crypt32_thunks(ctx.crypt32_mgr);
        (void)reg.register_dll("crypt32.dll", std::move(crypt32_exports));
        // Map scratch buffer for guest-side cert structures (CERT_CONTEXT, etc.)
        ctx.vmm.virtual_alloc(winapi::CRYPT32_SCRATCH_BASE,
                              winapi::CRYPT32_SCRATCH_SIZE,
                              0x3000 /*MEM_COMMIT|MEM_RESERVE*/,
                              0x04   /*PAGE_READWRITE*/);
    }

    // -- sspicli.dll --
    {
        auto sspicli_exports = winapi::create_sspicli_thunks(ctx.sspicli_mgr);
        (void)reg.register_dll("sspicli.dll", std::move(sspicli_exports));
    }

    // -- wkscli.dll / netutils.dll / netapi32.dll --
    // On Windows, NetGetJoinInformation / DsRole* / NetGetAadJoinInformation and
    // NetApiBufferFree are exported from netapi32.dll (wkscli/netutils are
    // implementation DLLs). The guest and the process that launches it import netapi32
    // by name; a missing netapi32 thunk installs a fail-thunk that returns
    // 0xFFFFFFFF.
    {
        ctx.wkscli_mgr.set_heap_manager(&ctx.heap_mgr);
        ctx.netutils_mgr.set_heap_manager(&ctx.heap_mgr);
        auto wkscli_exports = winapi::create_wkscli_thunks(ctx.wkscli_mgr);
        auto netutils_exports = winapi::create_netutils_thunks(ctx.netutils_mgr);

        winapi::DllExports netapi;
        merge_exports(netapi, winapi::create_wkscli_thunks(ctx.wkscli_mgr));
        merge_exports(netapi, winapi::create_netutils_thunks(ctx.netutils_mgr));
        // UCHAR Netbios(PNCB) — stdcall=4. Legacy NetBIOS is unsupported;
        // return NRC_ENVNOTDEF (0x32) and stamp ncb_retcode when possible.
        netapi.entries.push_back({"Netbios", 0,
            [](winapi::Marshaler& m) {
                constexpr u8 NRC_BADNCB = 0x07;
                constexpr u8 NRC_ENVNOTDEF = 0x32;
                const vitreo::GuestAddr ncb = m.arg_ptr(0);
                if (ncb == 0) {
                    m.set_return(NRC_BADNCB);
                    return;
                }
                (void)m.write_u8(ncb + 1, NRC_ENVNOTDEF); // ncb_retcode
                VITREO_TRACE("[netapi32] Netbios -> NRC_ENVNOTDEF\n");
                m.set_return(NRC_ENVNOTDEF);
            }, 4});
        (void)reg.register_dll("netapi32.dll", std::move(netapi));

        (void)reg.register_dll("wkscli.dll", std::move(wkscli_exports));
        (void)reg.register_dll("netutils.dll", std::move(netutils_exports));
    }

    // -- httpapi.dll (HTTP Server API) --
    (void)reg.register_dll("httpapi.dll", winapi::create_httpapi_thunks(ctx.httpapi_mgr));

    // -- psapi.dll --
    {
        // GetMappedFileName reports the NT device path QueryDosDevice maps
        // back (C: is \Device\HarddiskVolume3).
        ctx.psapi_mgr.set_mapped_path_resolver([&ctx](GuestAddr address) {
            auto path = ctx.fmm_mgr.mapped_file_path(address);
            if (path.empty()) {
                const auto module = ctx.module_mgr.module_for_address(address);
                if (module.info) path = module.info->path;
            }
            if (path.empty()) return path;
            return winapi::win32_to_nt_device_path(ctx.vfs, path).value_or(std::string{});
        });
        auto psapi_exports =
            winapi::create_psapi_thunks(ctx.psapi_mgr, ctx.k32_ext_mgr);
        (void)reg.register_dll("psapi.dll", std::move(psapi_exports));
    }
}

} // namespace vitreo::cli
