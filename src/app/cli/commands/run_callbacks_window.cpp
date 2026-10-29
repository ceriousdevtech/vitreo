// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CLI-002: guest-call boundaries for WinHTTP, window procedures, COM callers and dialogs

#include "run_internal.hpp"
#include "runtime/process/runtime_context.hpp"
#include "runtime/execution/guest_run_loop.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/user32/resources/icon_planes.hpp"
#include <vitreo/diagnostics.hpp>

#include <cstdio>

namespace vitreo::cli {

void install_window_message_callbacks(
    cpu::CpuState& cpu, GuestMemory& memory, RuntimeContext& ctx,
    bool is_64bit, u64 peb_address, u64 pe_tls_data_start,
    u32 pe_tls_data_size) {
    ctx.winhttp_mgr.set_status_callback_invoker(
        [&cpu, &memory, &ctx, is_64bit](GuestAddr procedure, u32 handle,
                                       u64 context, u32 status,
                                       GuestAddr status_information,
                                       u32 status_information_length) {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            const u64 args[5] = {handle, context, status,
                                 status_information,
                                 status_information_length};
            (void)run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                     ctx.process_mgr, procedure, args, 5,
                                     is_64bit);
        });
    // WinHTTP asynchronous completions (WinHttpGetProxyForUrlEx) arrive on
    // the WinHTTP service thread; like the winmm service threads it gets its
    // own guest thread context and runs WINHTTP_STATUS_CALLBACK there.
    ctx.winhttp_mgr.set_async_callback_invoker(
        [&memory, &ctx, is_64bit, peb_address, pe_tls_data_start,
         pe_tls_data_size](GuestAddr procedure, u32 handle, u64 context,
                           u32 status, GuestAddr status_information,
                           u32 status_information_length) {
            constexpr u32 WINHTTP_SERVICE_TID = 0xF0000004u;
            static thread_local bool initialised = false;
            static thread_local GuestThreadContext gtc{};
            if (!initialised) {
                initialised = true;
                winapi::ThreadManager::set_current_thread_id(WINHTTP_SERVICE_TID);
                ctx.thread_mgr.add_service_thread(WINHTTP_SERVICE_TID);
                init_thread_jit_state(memory);
                GuestThreadContextParams params{};
                params.is_64bit = is_64bit;
                params.peb_address = peb_address;
                params.pe_tls_data_start = pe_tls_data_start;
                params.pe_tls_data_size = pe_tls_data_size;
                gtc = create_guest_thread_context(memory, ctx, params,
                                                  WINHTTP_SERVICE_TID);
            }
            if (!gtc.valid()) return;
            cpu::CpuState callback_cpu = make_guest_cpu_state(memory, gtc, is_64bit);
            const u64 args[5] = {handle, context, status, status_information,
                                 status_information_length};
            (void)run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                     ctx.process_mgr, procedure, args, 5,
                                     is_64bit);
        });
    ctx.msgloop_mgr.set_wndproc_callback(
        [&ctx, is_64bit](
            windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam, u64 wndproc
        ) -> u64 {
            // Creation parameters are owned by the synchronous creator.
            // Forward even a null lParam unchanged; never invent guest state.
            return ctx.comctl32_mgr.dispatch_window_message(
                hwnd, msg, wParam, lParam, wndproc, is_64bit);
        }
    );

    ctx.msgsend_mgr.set_gdi_manager(&ctx.gdi_mgr);
    ctx.msgsend_mgr.set_image_list_size_query([&ctx](u64 himl, i32& cx, i32& cy) {
        return ctx.comctl32_mgr.image_list_size(himl, cx, cy);
    });
    ctx.msgsend_mgr.set_icon_size_query([&ctx](u64 icon, i32& cx, i32& cy) {
        const auto planes = ctx.resource_mgr.icon_planes(static_cast<u32>(icon));
        if (!planes) return false;
        cx = static_cast<i32>(planes->width);
        cy = static_cast<i32>(planes->height);
        return true;
    });
    ctx.msgsend_mgr.set_icon_rgba_query([&ctx](u64 icon, u32& w, u32& h, std::vector<u8>& rgba) {
        const auto planes = ctx.resource_mgr.icon_planes(static_cast<u32>(icon));
        if (!planes) return false;
        w = planes->width;
        h = planes->height;
        rgba = winapi::icon_display_rgba(*planes);
        return rgba.size() == static_cast<usize>(w) * h * 4u;
    });

    // Also wire up msgsend_mgr for SendMessageW
    ctx.msgsend_mgr.set_wndproc_callback(
        [&ctx, is_64bit](
            windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam, u64 wndproc
        ) -> u64 {
            return ctx.comctl32_mgr.dispatch_window_message(
                hwnd, msg, wParam, lParam, wndproc, is_64bit);
        }
    );

    // Wire up window_mgr for synchronous WM_CREATE/WM_NCCREATE during CreateWindowExW
    ctx.window_mgr.set_wndproc_callback(
        [&ctx, is_64bit](
            windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam, u64 wndproc
        ) -> u64 {
            // Creation parameters are owned by the synchronous creator.
            // Forward even a null lParam unchanged; never invent guest state.
            return ctx.comctl32_mgr.dispatch_window_message(
                hwnd, msg, wParam, lParam, wndproc, is_64bit);
        }
    );

    // IDXGIObject::SetPrivateDataInterface has to AddRef the interface the
    // guest hands it and Release it later, on an interface the guest itself
    // implements -- so the frontend has to call into guest code through that
    // object's own vtable. See winapi/core/guest_com.hpp, including why a
    // Release is allowed to re-enter the frontend.
    const auto guest_com_caller =
        [&cpu, &memory, &ctx, is_64bit](GuestAddr function, const u64* args,
                                        usize arg_count) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            return run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, function, args,
                                      arg_count, is_64bit);
        };
    ctx.dxgi_mgr.set_guest_function_caller(guest_com_caller);
    ctx.d3d11_mgr.set_guest_function_caller(guest_com_caller);
    ctx.d3d12_mgr.set_guest_function_caller(guest_com_caller);
    // RtlLookupFunctionEntry called by the guest runs the
    // GET_RUNTIME_FUNCTION_CALLBACK of a covering RtlInstallFunctionTableCallback.
    ctx.module_mgr.set_runtime_function_callback_runner(
        [guest_com_caller](GuestAddr callback, GuestAddr control_pc,
                           GuestAddr context) -> GuestAddr {
            const u64 args[2] = {static_cast<u64>(control_pc),
                                 static_cast<u64>(context)};
            return static_cast<GuestAddr>(guest_com_caller(callback, args, 2));
        });

    // Wire up dialog manager for DialogBoxParamW modal DLGPROC dispatch
    ctx.dialog_mgr.set_dlgproc_callback(
        [&cpu, &memory, &ctx, is_64bit](
            windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam, u64 dlgproc
        ) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            // DLGPROC signature: INT_PTR CALLBACK DlgProc(HWND, UINT, WPARAM, LPARAM)
            u64 args[4] = {
                static_cast<u64>(hwnd),
                static_cast<u64>(msg),
                wParam,
                lParam
            };
            return run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, dlgproc, args, 4, is_64bit);
        }
    );

    // Release any active modal dialog when the guest queues a chain-load via
    // ShellExecute/CreateProcess. Our chain-load model serializes processes
    // (the current guest must exit before the chained executable runs), so
    // any pumping modal loop has to terminate to let the runtime proceed.
    ctx.dialog_mgr.set_modal_exit_predicate(
        [&ctx]() -> bool {
            return ctx.process_mgr.has_pending_launch();
        }
    );
}

} // namespace vitreo::cli
