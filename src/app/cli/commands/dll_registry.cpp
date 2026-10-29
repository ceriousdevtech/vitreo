// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// DLL registry: populates ThunkRegistry with exports from all managers

#include "dll_registry.hpp"
#include "dll_registry_internal.hpp"

#include "core/memory/guest_memory.hpp"
#include "runtime/process/runtime_context.hpp"
#include "winapi/kernel32/error.hpp"

#include <string>
#include <string_view>

namespace vitreo::cli {

void register_all_dlls(RuntimeContext& ctx, GuestMemory& memory, bool guest_is_64bit) {
    auto& reg = ctx.thunk_registry;

    // A 32-bit guest is a WOW64 process: its registry calls go through the
    // registry redirector's 32-bit view (Learn, "Registry Redirector").
    ctx.registry.set_wow64_process(!guest_is_64bit);

    // A missing-export stub returns a failure marker; give it the thread's
    // last-error slot so the guest reads ERROR_CALL_NOT_IMPLEMENTED instead of
    // "failed with GetLastError() == 0", which no Win32 API ever reports.
    winapi::ThunkRegistry::set_missing_export_error_hook(
        [](vitreo::u32 code) { winapi::set_last_error(code); });

    // Wire cross-manager dependencies used by kernel32 extended APIs.
    ctx.k32_ext_mgr.set_module_manager(&ctx.module_mgr);
    // The DLL search order's last step reads the guest's current PATH.
    ctx.module_mgr.set_path_variable_provider([&ctx]() -> std::string {
        auto value = ctx.env_mgr.get_variable("PATH");
        return value.is_ok() ? value.value() : std::string{};
    });
    // Let GetProcAddress reach the CRT's data exports, which have no thunk.
    ctx.module_mgr.set_crt_data_resolver(
        [&ctx](std::string_view name) -> vitreo::u64 {
            if (const vitreo::u64 direct = ctx.crt_data.resolve_data_import(name);
                direct != 0) {
                return direct;
            }
            // _environ and friends are published through the thunk registry's
            // data-export table under each CRT alias, and the HMODULE the
            // guest passes does not necessarily resolve back to the same key,
            // so try the aliases by name.
            for (const char* crt_dll : {"msvcrt.dll", "crtdll.dll"}) {
                if (const auto addr =
                        ctx.thunk_registry.lookup_data_export(crt_dll, name);
                    addr != 0) {
                    return static_cast<vitreo::u64>(addr);
                }
            }
            return 0;
        });
    ctx.k32_ext_mgr.set_process_manager(&ctx.process_mgr);
    ctx.k32_ext_mgr.set_sync_manager(&ctx.sync_mgr);
    ctx.k32_ext_mgr.set_critical_section_manager(&ctx.cs_mgr);
    ctx.k32_ext_mgr.set_thread_manager(&ctx.thread_mgr);
    ctx.sync_mgr.set_thread_manager(&ctx.thread_mgr);
    ctx.sync_mgr.set_object_token_provider([&ctx]{return ctx.sec_stubs.effective_object_token();});
    ctx.k32_ext_mgr.set_environment_manager(&ctx.env_mgr);
    ctx.k32_ext_mgr.set_file_search_manager(&ctx.fsearch_mgr);
    ctx.k32_ext_mgr.set_ntdll_ext_manager(&ctx.ntdll_ext_mgr);
    ctx.k32_ext_mgr.set_sysinfo_manager(&ctx.sysinfo_mgr);
    ctx.k32_ext_mgr.set_file_manager(&ctx.file_mgr);
    ctx.k32_ext_mgr.set_socket_handle_probe(
        [&ctx](u32 handle) { return ctx.ws2_mgr.get_native_fd(handle) >= 0; });
    ctx.k32_ext_mgr.set_socket_io_cancel(
        [&ctx](u32 guest_socket, GuestAddr overlapped, bool only_calling_thread,
               u32 calling_tid) {
            return ctx.ws2_mgr.cancel_overlapped_socket_io(
                guest_socket, overlapped, only_calling_thread, calling_tid);
        });
    ctx.file_mgr.set_socket_handle_probe(
        [&ctx](u32 handle) { return ctx.ws2_mgr.get_native_fd(handle) >= 0; });
    ctx.file_mgr.set_file_mapping_manager(&ctx.fmm_mgr);
    ctx.ws2_mgr.set_memory(&memory);
    ctx.ws2_mgr.set_file_manager(&ctx.file_mgr);
    ctx.ws2_mgr.set_overlapped_completion_callback(
        [&ctx](winapi::Handle file_handle, u32 bytes_transferred,
               GuestAddr overlapped, bool is_64bit, u32 nt_status,
               u32 win_error, bool completed_synchronously) {
            ctx.k32_ext_mgr.complete_overlapped_file_io(
                file_handle, bytes_transferred, overlapped, is_64bit,
                nt_status, win_error, completed_synchronously);
        });
    ctx.ws2_mgr.set_nt_completion_callback(
        [&ctx](winapi::Handle socket, GuestAddr apc_context, winapi::Handle event,
               GuestAddr apc_routine, GuestAddr io_status_block, u32 issuing_tid,
               u32 bytes, u32 nt_status, u32 win_error, bool synchronous) {
            ctx.k32_ext_mgr.complete_nt_io(socket, apc_context, event, apc_routine,
                                           io_status_block, issuing_tid, bytes,
                                           nt_status, win_error, synchronous);
        });
    ctx.ntdll_ext_mgr.set_socket_device_control(
        [&ctx](winapi::Marshaler& m, winapi::Handle handle, u32 code, GuestAddr input,
               u32 input_length, GuestAddr output, u32 output_length,
               GuestAddr io_status_block, GuestAddr apc_context, winapi::Handle event,
               GuestAddr apc_routine) -> std::optional<u32> {
            bool handled = false;
            const u32 status = ctx.ws2_mgr.afd_device_control(
                m, static_cast<u32>(handle), code, input, input_length, output,
                output_length, io_status_block, apc_context, event, apc_routine, handled);
            if (!handled) return std::nullopt;
            return status;
        });
    ctx.httpapi_mgr.set_handle_table(&ctx.handle_table);
    ctx.httpapi_mgr.set_memory(&memory);
    ctx.httpapi_mgr.set_sync_manager(&ctx.sync_mgr);
    ctx.httpapi_mgr.set_overlapped_completion(
        [&ctx](winapi::Handle queue, u32 bytes, GuestAddr overlapped, bool is_64bit,
               u32 nt_status, u32 win_error, bool completed_synchronously) {
            ctx.k32_ext_mgr.complete_overlapped_file_io(
                queue, bytes, overlapped, is_64bit, nt_status, win_error,
                completed_synchronously);
        });
    ctx.httpapi_mgr.set_port_association_probe(
        [&ctx](winapi::Handle queue) { return ctx.k32_ext_mgr.has_iocp_association(queue); });
    ctx.k32_ext_mgr.set_device_io_cancel(
        [&ctx](winapi::Handle handle, GuestAddr overlapped, bool only_calling_thread,
               u32 calling_tid) {
            return ctx.httpapi_mgr.cancel_io(handle, overlapped, only_calling_thread,
                                             calling_tid);
        });
    ctx.ws2_mgr.set_wsa_completion_routine_queue(
        [&ctx](u32 thread_id, GuestAddr pfn, u32 error_code, u32 bytes,
               GuestAddr overlapped, u32 flags) {
            (void)ctx.thread_mgr.queue_wsa_completion_apc(
                thread_id, pfn, error_code, bytes, overlapped, flags);
        });
    ctx.ntdll_ext_mgr.set_sync_manager(&ctx.sync_mgr);
    ctx.ntdll_ext_mgr.set_thread_manager(&ctx.thread_mgr);
    ctx.ntdll_ext_mgr.set_virtual_memory_manager(&ctx.vmm);

    winapi::DllExports kernelbase;
    std::unordered_set<std::string> kernelbase_seen;

    register_core_dlls(ctx, kernelbase, kernelbase_seen, guest_is_64bit);
    register_crt_dlls(ctx, guest_is_64bit);
    register_advapi_rpc_dlls(ctx, kernelbase, kernelbase_seen);
    register_ui_core_dlls(ctx, guest_is_64bit, kernelbase, kernelbase_seen);
    register_shell_ole_dlls(ctx, kernelbase, kernelbase_seen);
    register_ui_controls_dlls(ctx, guest_is_64bit);
    register_media_foundation_dcomp_dlls(ctx);
    register_shell_support_dlls(ctx);
    register_winsock_oleaut_crypto_dlls(ctx);
    register_input_and_legacy_graphics_dlls(ctx, memory);
    register_system_services_dlls(ctx);
    register_printing_network_power_dlls(ctx);
    register_dwrite_dlls(ctx);
    register_trust_profile_tpm_dlls(ctx);
    register_mmcss_qos_probe_dlls(ctx);
    register_dxgi_d3d_dlls(ctx);
    register_perf_event_dlls(ctx);
    register_combase_dlls(ctx);
    register_system_program_dll(ctx);
    register_device_diagnostic_dlls(ctx, memory);

    // Set thunk registry on managers that need extension function lookups
    ctx.ws2_mgr.set_thunk_registry(&reg);
}

} // namespace vitreo::cli
