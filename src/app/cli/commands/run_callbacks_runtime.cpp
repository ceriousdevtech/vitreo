// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CLI-002: guest-call boundaries for CRT initializers, exception filters, COM and modules

#include "run_internal.hpp"
#include "dll_registry_internal.hpp"
#include "runtime/process/runtime_context.hpp"
#include "runtime/execution/guest_run_loop.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "hardware/contracts/bluetooth_host.hpp"
#include "hardware/contracts/now_playing.hpp"
#include "winapi/system/msi/msi_ui.hpp"
#include "winapi/system/shlwapi/shlwapi.hpp"
#include "winapi/common/c_specific_handler.hpp"
#include <vitreo/diagnostics.hpp>

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace vitreo::cli {

void install_crt_and_exception_callbacks(
    cpu::CpuState& cpu, GuestMemory& memory, RuntimeContext& ctx,
    bool is_64bit) {
    // Wire up _initterm: run each guest void(*)(void) initializer function.
    ctx.crt_init_mgr.set_initterm_callback(
        [&cpu, &memory, &ctx, is_64bit](GuestAddr func_addr) {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            // void (*init_func)(void) — no args, no return value used
            u64 result = run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                            ctx.process_mgr, func_addr, nullptr, 0, is_64bit);
            (void)result;
        }
    );

    // Wire up _initterm_e: run each guest int(*)(void) initializer, return result.
    ctx.crt_init_mgr.set_initterm_e_callback(
        [&cpu, &memory, &ctx, is_64bit](GuestAddr func_addr) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            // int (*init_func)(void) — no args, return value is error code (0 = success)
            return run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, func_addr, nullptr, 0, is_64bit);
        }
    );

    // Wire up qsort: invoke the guest cdecl comparator with two element pointers.
    ctx.crt_mem_mgr.set_compare_callback(
        [&cpu, &memory, &ctx, is_64bit](GuestAddr compare, GuestAddr lhs,
                                       GuestAddr rhs) -> i32 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            const u64 args[2] = {lhs, rhs};
            const u64 result = run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                                  ctx.process_mgr, compare, args, 2,
                                                  is_64bit);
            return static_cast<i32>(static_cast<u32>(result));
        }
    );

    // Generic guest __cdecl call for CRT contracts that run user callbacks
    // (invalid-parameter handler, signal handlers, atexit tables).
    ctx.crt_mem_mgr.set_guest_caller(
        [&cpu, &memory, &ctx, is_64bit](GuestAddr function, const u64* args,
                                        usize count) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            return run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, function, args, count,
                                      is_64bit);
        }
    );

    // x64 __C_specific_handler (ntdll / CRT) runs __except filters and
    // __finally blocks on the thread that is dispatching the exception.
    winapi::set_seh_guest_call(
        [&memory, &ctx](cpu::CpuState& handler_cpu, GuestAddr function, const u64* args,
                        usize count, bool* completed) -> u64 {
            return run_guest_callback(handler_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, function, args, count,
                                      true, completed);
        }
    );

    ctx.crt_mem_mgr.set_void_callback(
        [&cpu, &memory, &ctx, is_64bit](GuestAddr func_addr) {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            (void)run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                     ctx.process_mgr, func_addr, nullptr, 0,
                                     is_64bit);
        }
    );

    // EH3 __except filters are nested functions: they run with the parent
    // function's EBP so GetExceptionInformation/GetExceptionCode resolve.
    ctx.crt_mem_mgr.set_seh_filter_callback(
        [&cpu, &memory, &ctx](GuestAddr filter, u32 parent_ebp) -> i32 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            callback_cpu.gpr.set32(static_cast<u8>(cpu::RegIndex::RBP),
                                   parent_ebp);
            bool completed_normally = false;
            const u64 result = run_guest_callback(
                callback_cpu, memory, ctx.thunk_registry, ctx.process_mgr,
                filter, nullptr, 0, false, &completed_normally);
            // Microsoft Learn (try-except): 0 = EXCEPTION_CONTINUE_SEARCH.
            // run_guest_callback returns 1 on abnormal abort, which is
            // EXCEPTION_EXECUTE_HANDLER and would transfer into __except
            // with a half-built frame.
            if (!completed_normally) return 0;
            return static_cast<i32>(static_cast<u32>(result));
        }
    );

    // UnhandledExceptionFilter invokes the guest top-level filter installed by
    // SetUnhandledExceptionFilter (errhandlingapi). Same run_guest_callback
    // path the SEH dispatchers use after frame ContinueSearch.
    winapi::set_unhandled_exception_filter_invoker(
        [&cpu, &memory, &ctx](GuestAddr filter, GuestAddr exception_pointers,
                              bool is_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            const u64 args[1] = {static_cast<u64>(exception_pointers)};
            bool completed_normally = false;
            const u64 result = run_guest_callback(
                callback_cpu, memory, ctx.thunk_registry, ctx.process_mgr,
                filter, args, 1, is_64bit, &completed_normally);
            if (!completed_normally) {
                return 1; // EXCEPTION_EXECUTE_HANDLER
            }
            return result;
        });

    // Windows Installer: this process's custom-action host and its way to the
    // installer service (msi_service.hpp).
    install_msi_process_host(ctx, memory, is_64bit);
    // shlwapi's StrCmpN / StrCmpI family compares with this process's
    // CompareString.
    winapi::set_shlwapi_locale_strings(&ctx.locale_str_mgr);
}

void install_module_and_com_callbacks(
    cpu::CpuState& cpu, GuestMemory& memory, RuntimeContext& ctx) {
    // Wire up dynamic module entrypoints (DllMain) for LoadLibrary/FreeLibrary.
    // This is used by ModuleManager when it maps real PE-backed DLLs from VFS.
    // WinRT completion delegates (IAsyncOperation::put_Completed) are guest
    // code, and Windows.Devices.Bluetooth reports the real host radio.
    ctx.combase_mgr.set_guest_call(
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args,
                              usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            bool completed_normally = false;
            const u64 result = run_guest_callback(
                callback_cpu, memory, ctx.thunk_registry, ctx.process_mgr,
                func_addr, args, arg_count, is_mod_64bit, &completed_normally);
            if (!completed_normally) {
                std::fprintf(stderr,
                    "[combase] WinRT delegate at 0x%llx terminated abnormally\n",
                    static_cast<unsigned long long>(func_addr));
                return 0;
            }
            return result;
        });
    static std::unique_ptr<vitreo::hardware::BluetoothHost> bluetooth_host =
        vitreo::hardware::make_platform_bluetooth_host();
    ctx.combase_mgr.set_bluetooth_host(bluetooth_host.get());
    static std::unique_ptr<vitreo::hardware::NowPlayingHost> now_playing_host =
        vitreo::hardware::make_platform_now_playing_host();
    ctx.combase_mgr.set_now_playing_host(now_playing_host.get());

    // oleaut32 calls guest COM objects: IUnknown::AddRef/Release for
    // SAFEARRAY / VARIANT interface elements and IDispatch coercions.
    ctx.oleaut32_mgr.set_guest_call(
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args,
                              usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            bool completed_normally = false;
            const u64 result = run_guest_callback(
                callback_cpu, memory, ctx.thunk_registry, ctx.process_mgr,
                func_addr, args, arg_count, is_mod_64bit, &completed_normally);
            if (!completed_normally) {
                std::fprintf(stderr,
                    "[oleaut32] COM callback at 0x%llx terminated abnormally\n",
                    static_cast<unsigned long long>(func_addr));
                return 0;
            }
            return result;
        });

    // IP Helper: NotifyIpInterfaceChange's initial notification runs the
    // guest callback on the registering thread; later changes run it on
    // guest worker threads the thread manager creates.
    ctx.iphlpapi_mgr.set_guest_callbacks(
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args,
                              usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            bool completed_normally = false;
            const u64 result = run_guest_callback(
                callback_cpu, memory, ctx.thunk_registry, ctx.process_mgr,
                func_addr, args, arg_count, is_mod_64bit, &completed_normally);
            if (!completed_normally) {
                std::fprintf(stderr,
                    "[iphlpapi] notification callback at 0x%llx terminated abnormally\n",
                    static_cast<unsigned long long>(func_addr));
                return 0;
            }
            return result;
        },
        &ctx.thread_mgr);

    // FDI drives the caller: FDICopy invokes its open/read/write/close/seek
    // and notification callbacks, all of which are DIAMONDAPI (__cdecl).
    // run_guest_callback saves and restores the full CPU state, so it is safe
    // for either convention.
    ctx.cabinet_mgr.set_guest_call(
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args,
                              usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            bool completed_normally = false;
            const u64 result = run_guest_callback(
                callback_cpu, memory, ctx.thunk_registry, ctx.process_mgr,
                func_addr, args, arg_count, is_mod_64bit, &completed_normally);
            if (!completed_normally) {
                std::fprintf(stderr,
                    "[cabinet] FDI callback at 0x%llx terminated abnormally\n",
                    static_cast<unsigned long long>(func_addr));
                return 0;
            }
            return result;
        });

    // SetupAPI's PSP_FILE_CALLBACKs (queue commits, SetupIterateCabinet)
    // are WINAPI; run_guest_callback serves either convention. Their
    // notification records go in a guest ring reserved on first use.
    ctx.setupapi_services.call =
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args,
                              usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            bool completed_normally = false;
            const u64 result = run_guest_callback(
                callback_cpu, memory, ctx.thunk_registry, ctx.process_mgr,
                func_addr, args, arg_count, is_mod_64bit, &completed_normally);
            if (!completed_normally) {
                std::fprintf(stderr,
                    "[setupapi] file callback at 0x%llx terminated abnormally\n",
                    static_cast<unsigned long long>(func_addr));
                return 0;
            }
            return result;
        };
    ctx.system_program_call = ctx.setupapi_services.call;
    ctx.setupapi_services.reserve = [&ctx](u32 size) -> GuestAddr {
        constexpr u32 kCommitReserve = 0x3000;  // MEM_COMMIT | MEM_RESERVE
        constexpr u32 kReadWrite = 0x04;        // PAGE_READWRITE
        return static_cast<GuestAddr>(
            ctx.vmm.virtual_alloc(0, size, kCommitReserve, kReadWrite));
    };

    ctx.module_mgr.set_guest_call(
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args, usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            // Windows guarantees that the current thread's static-TLS vector
            // is populated before a TLS callback or DllMain runs.  Synchronize
            // the actual callback CPU's TEB here as the final loader boundary;
            // helper and newly-created guest threads can become active while a
            // dynamic DLL load is concurrently publishing a new TLS index.
            const GuestAddr callback_teb = is_mod_64bit
                ? static_cast<GuestAddr>(callback_cpu.seg.gs_desc.base)
                : static_cast<GuestAddr>(callback_cpu.seg.fs_desc.base);
            ctx.module_mgr.register_thread_teb(
                callback_teb, is_mod_64bit ? 64u : 32u);
            if (std::getenv("VITREO_TLS_DIAG") != nullptr) {
                GuestAddr tls_array = 0;
                if (is_mod_64bit) {
                    tls_array = static_cast<GuestAddr>(
                        memory.read_u64(callback_teb + 0x58).value_or(0));
                } else {
                    tls_array = static_cast<GuestAddr>(
                        memory.read_u32(callback_teb + 0x2C).value_or(0));
                }
                std::fprintf(stderr,
                    "[loader-tls-diag] callback=0x%llx teb=0x%llx array=0x%llx",
                    static_cast<unsigned long long>(func_addr),
                    static_cast<unsigned long long>(callback_teb),
                    static_cast<unsigned long long>(tls_array));
                for (const auto& slot : ctx.module_mgr.dll_tls_slots()) {
                    if (slot.bitness != (is_mod_64bit ? 64u : 32u) ||
                        tls_array == 0) {
                        continue;
                    }
                    const GuestAddr slot_addr = tls_array +
                        static_cast<GuestAddr>(slot.slot_index) *
                            (is_mod_64bit ? 8u : 4u);
                    const u64 value = is_mod_64bit
                        ? memory.read_u64(slot_addr).value_or(0)
                        : static_cast<u64>(
                              memory.read_u32(slot_addr).value_or(0));
                    std::fprintf(stderr, " slot%u=0x%llx", slot.slot_index,
                        static_cast<unsigned long long>(value));
                }
                std::fprintf(stderr, "\n");
            }
            bool completed_normally = false;
            u64 result = run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                            ctx.process_mgr, func_addr, args, arg_count,
                                            is_mod_64bit, &completed_normally);
            if (!completed_normally) {
                std::fprintf(stderr,
                    "[loader] guest module callback at 0x%llx terminated abnormally; treating it as failure\n",
                    static_cast<unsigned long long>(func_addr));
                return 0;
            }
            return result;
        }
    );

    // DirectDraw enumeration callbacks (EnumDisplayModes) run guest code.
    ctx.ddraw_mgr.set_guest_call(
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args,
                              usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            return run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, func_addr, args,
                                      arg_count, is_mod_64bit, nullptr);
        }
    );

    // DirectWrite font fallback synchronously calls the guest-owned
    // IDWriteTextAnalysisSource to obtain text and locale ranges.
    ctx.dwrite_mgr.set_guest_call(
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args,
                              usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            return run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, func_addr, args,
                                      arg_count, is_mod_64bit, nullptr);
        }
    );

    // A GLU quadric's GLU_ERROR callback, void CALLBACK fn(GLenum).
    ctx.glu32_mgr.set_guest_call(
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args,
                              usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            return run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, func_addr, args,
                                      arg_count, is_mod_64bit, nullptr);
        }
    );

    // EnumFontFamiliesEx calls the guest's FONTENUMPROC per face.
    ctx.gdi_mgr.set_guest_call(
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args,
                              usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            return run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, func_addr, args,
                                      arg_count, is_mod_64bit, nullptr);
        }
    );

    // DirectInput EnumDevices invokes LPDIENUMDEVICESCALLBACKA/W.
    ctx.com_mgr.set_direct_input_manager(&ctx.dinput_mgr);
    ctx.dinput_mgr.set_guest_call(
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args,
                              usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            return run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, func_addr, args,
                                      arg_count, is_mod_64bit, nullptr);
        }
    );

    ctx.dsound_mgr.set_guest_call(
        [&cpu, &memory, &ctx](GuestAddr func, const u64* args,
                              usize count, bool x64) -> u64 {
            return run_guest_callback(active_guest_cpu_or(cpu), memory,
                ctx.thunk_registry, ctx.process_mgr, func, args, count, x64, nullptr);
        });

    ctx.msvfw32_mgr.set_guest_call(
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args,
                              usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            return run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, func_addr, args,
                                      arg_count, is_mod_64bit, nullptr);
        }
    );

    // oledlg calls user32/kernel32/ole32 through their guest exports and
    // runs the caller's LPFNOLEUIHOOK.
    ctx.oledlg_mgr.set_guest_call(
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args,
                              usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            return run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, func_addr, args,
                                      arg_count, is_mod_64bit, nullptr);
        }
    );

    ctx.dnsapi_mgr.set_guest_call(
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args, usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            return run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, func_addr, args, arg_count, is_mod_64bit);
        }
    );

    // MsiSetExternalUI(Record) handlers are guest callbacks (msi_ui.hpp).
    winapi::msi::set_ui_guest(ctx.msi_mgr, winapi::msi::make_guest_call_ui(memory,
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args, usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            return run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, func_addr, args, arg_count, is_mod_64bit);
        }));

    ctx.k32_ext_mgr.set_guest_call(
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args, usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            return run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, func_addr, args, arg_count, is_mod_64bit);
        }
    );

    // EnumSystemLocalesA/W/Ex invoke the guest LOCALE_ENUMPROC(Ex) callback.
    ctx.locale_str_mgr.set_guest_call(
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args, usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            return run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, func_addr, args, arg_count, is_mod_64bit);
        }
    );

    // ITypeInfo::Invoke calls through guest COM vtables (e.g. _NewEnum).
    ctx.oleaut32_mgr.set_guest_call(
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args, usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            return run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, func_addr, args, arg_count, is_mod_64bit);
        }
    );

    // CoMarshalInterThreadInterfaceInStream / CoGetInterfaceAndReleaseStream
    // AddRef/QI/Release the guest interface being handed across threads.
    ctx.com_mgr.set_guest_call(
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args, usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            return run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, func_addr, args, arg_count, is_mod_64bit);
        }
    );

    // StartServiceCtrlDispatcher keeps the calling guest thread as the control
    // dispatcher. Windows creates a separate thread for ServiceMain, so use
    // the normal guest-thread boundary and wait for that thread to finish.
    ctx.scm_mgr.set_service_dispatch_wait([&ctx] {
        if (ctx.sync_mgr.shutdown_flag()->load(std::memory_order_acquire)) return false;
        ctx.sync_mgr.sleep(10);
        return !ctx.sync_mgr.shutdown_flag()->load(std::memory_order_acquire);
    });

    ctx.scm_mgr.set_service_thread_call(
        [&ctx, &memory](
            GuestAddr func_addr,
            const std::vector<std::wstring>& service_arguments,
            bool wide, bool is_mod_64bit) -> bool {
            if (service_arguments.empty() ||
                service_arguments.size() >
                    static_cast<usize>(std::numeric_limits<u32>::max())) {
                return false;
            }

            const u64 pointer_size = is_mod_64bit ? 8u : 4u;
            const u64 character_size = wide ? 2u : 1u;
            u64 allocation_size =
                static_cast<u64>(service_arguments.size()) * pointer_size;
            for (const auto& argument : service_arguments) {
                const u64 units = static_cast<u64>(argument.size()) + 1u;
                if (units >
                    (std::numeric_limits<u64>::max() - allocation_size) /
                        character_size) {
                    return false;
                }
                allocation_size += units * character_size;
            }

            const GuestAddr argument_block = ctx.vmm.virtual_alloc(
                0, allocation_size, 0x3000 /*MEM_COMMIT|MEM_RESERVE*/,
                0x04 /*PAGE_READWRITE*/);
            if (argument_block == 0) return false;

            GuestAddr string_at = argument_block +
                static_cast<GuestAddr>(
                    service_arguments.size() * pointer_size);
            bool wrote_arguments = true;
            for (usize index = 0; index < service_arguments.size(); ++index) {
                const GuestAddr pointer_at = argument_block +
                    static_cast<GuestAddr>(index * pointer_size);
                wrote_arguments =
                    (is_mod_64bit
                         ? memory.write_u64(pointer_at, string_at).is_ok()
                         : memory.write_u32(
                               pointer_at, static_cast<u32>(string_at)).is_ok());
                if (!wrote_arguments) break;

                for (const wchar_t ch : service_arguments[index]) {
                    if (wide) {
                        wrote_arguments = memory.write_u16(
                            string_at, static_cast<u16>(ch)).is_ok();
                        string_at += 2;
                    } else {
                        const u8 byte = ch <= 0xFF
                            ? static_cast<u8>(ch)
                            : static_cast<u8>('?');
                        wrote_arguments =
                            memory.write_u8(string_at++, byte).is_ok();
                    }
                    if (!wrote_arguments) break;
                }
                if (!wrote_arguments) break;
                wrote_arguments = wide
                    ? memory.write_u16(string_at, 0).is_ok()
                    : memory.write_u8(string_at, 0).is_ok();
                string_at += character_size;
                if (!wrote_arguments) break;
            }
            if (!wrote_arguments) {
                (void)ctx.vmm.virtual_free(
                    argument_block, 0, 0x8000 /*MEM_RELEASE*/);
                return false;
            }

            u32 tid = 0;
            const winapi::Handle thread = ctx.thread_mgr.create_thread(
                0, func_addr,
                static_cast<u64>(service_arguments.size()), 0, &tid,
                argument_block);
            if (thread == winapi::INVALID_HANDLE || thread == 0) {
                (void)ctx.vmm.virtual_free(
                    argument_block, 0, 0x8000 /*MEM_RELEASE*/);
                return false;
            }

            if (auto* guest_thread = ctx.thread_mgr.get_thread(tid)) {
                guest_thread->launched = true;
            } else {
                (void)ctx.handle_table.close(thread);
                (void)ctx.vmm.virtual_free(
                    argument_block, 0, 0x8000 /*MEM_RELEASE*/);
                return false;
            }

            ctx.thread_mgr.run_async(
                tid, func_addr, static_cast<u64>(service_arguments.size()),
                argument_block, 0, 2);
            const u32 wait = ctx.sync_mgr.wait_for_single_object(
                thread, winapi::INFINITE_WAIT);
            (void)ctx.handle_table.close(thread);
            (void)ctx.vmm.virtual_free(
                argument_block, 0, 0x8000 /*MEM_RELEASE*/);
            return wait == winapi::WAIT_OBJECT_0;
        }
    );

    // comctl32 subclass callbacks use the same guest callback boundary as
    // normal window procedures, with the SUBCLASSPROC six-argument signature.
    ctx.comctl32_mgr.set_guest_call(
        [&cpu, &memory, &ctx](
            GuestAddr func_addr, const u64* args, usize arg_count,
            bool is_mod_64bit
        ) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            return run_guest_callback(
                callback_cpu, memory, ctx.thunk_registry, ctx.process_mgr,
                func_addr, args, arg_count, is_mod_64bit);
        }
    );

    // Wire up user32_ext guest callbacks (EnumWindows, etc.)
    ctx.u32_ext_mgr.set_guest_call(
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args, usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            return run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, func_addr, args, arg_count, is_mod_64bit);
        }
    );
    ctx.msgsend_mgr.set_guest_call(
        [&cpu, &memory, &ctx](GuestAddr func_addr, const u64* args, usize arg_count, bool is_mod_64bit) -> u64 {
            cpu::CpuState& callback_cpu = active_guest_cpu_or(cpu);
            return run_guest_callback(callback_cpu, memory, ctx.thunk_registry,
                                      ctx.process_mgr, func_addr, args, arg_count, is_mod_64bit);
        }
    );
}

} // namespace vitreo::cli
