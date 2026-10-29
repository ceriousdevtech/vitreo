// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CLI-002: running the primary guest thread and tearing the process down

#include "run_internal.hpp"
#include "runtime/process/runtime_context.hpp"
#include "runtime/checkpoint/checkpoint_format.hpp"
#include "runtime/checkpoint/runtime_checkpoint.hpp"
#include "runtime/execution/guest_run_loop.hpp"
#include "runtime/execution/thunk_fast_path.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/cpu/jit/translation_store_stats.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/pe/mapper/section_mapper.hpp"
#include "ipc/hwnd_registry.hpp"
#include "ipc/process_session.hpp"
#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <pthread.h>
#include <thread>
#include <unistd.h>

namespace vitreo::cli {

RunResult run_guest_primary_thread(
    const RunConfig& config, RuntimeContext& ctx, GuestMemory& memory,
    cpu::CpuState& cpu, const pe::MappedImage& image, bool is_64bit,
    u64 pe_tls_callbacks_addr, const std::string& win_exe_path) {
    // Provide an event pump callback to keep the UI responsive during execution.
    // This translates native events into queued Win32 messages and keeps
    // AppKit responsive. It must not retrieve or dispatch those messages:
    // Windows leaves posted input queued until the guest calls GetMessage or
    // PeekMessage, and calls the wndproc only through DispatchMessage.
    auto event_pump = [&ctx]() {
        ctx.cocoa_bridge.begin_host_input_pump();
        struct HostPumpGuard {
            windowing::CocoaBridge& bridge;
            ~HostPumpGuard() { bridge.end_host_input_pump(); }
        } guard{ctx.cocoa_bridge};
        ctx.cocoa_bridge.pump_events(ctx.msg_queue);
    };

    // Wire up module manager for x64 SEH dispatch (.pdata lookup)
    set_module_manager(&ctx.module_mgr);
    // Enter/Leave/TryEnterCriticalSection fast thunks need the same
    // CriticalSectionManager that owns the registered kernel32 exports.
    set_fast_thunk_cs_manager(&ctx.cs_mgr);
    set_fast_thunk_sync_manager(&ctx.sync_mgr);

    // Windows primary-thread guest code (DllMain, TLS callbacks, entry) must
    // not occupy the AppKit thread. The guest CPU runs on a worker;
    // GetMessage/PeekMessage already hop to the main thread
    // via vtr_run_on_main_sync. Keep that split so parse is not serialized
    // with nextDrawable / NSApp on com.apple.main-thread.
    auto run_primary = [&]() -> RunResult {
        const u32 primary_tid = ctx.thread_mgr.main_thread_id();
        ctx.thread_mgr.bind_current_host_thread(primary_tid);
        ctx.thread_mgr.apply_current_thread_scheduling();
        struct PrimaryCpuAccountingGuard {
            winapi::ThreadManager& threads;
            u32 tid;
            ~PrimaryCpuAccountingGuard() { threads.finish_thread_cpu_accounting(tid); }
        } primary_cpu_accounting_guard{ctx.thread_mgr, primary_tid};
        windowing::CocoaBridge::NativeWaitScope native_wait([&ctx] {
            ctx.msg_queue.dispatch_sent_messages(
                winapi::ThreadManager::get_current_thread_id_static());
        });
        // Guest CPU often runs on this worker, not the thread that wired
        // set_module_manager above. Re-bind so x64 .pdata dispatch can see
        // loaded modules (thread_local + process-wide fallback).
        set_module_manager(&ctx.module_mgr);
        set_fast_thunk_cs_manager(&ctx.cs_mgr);
        set_fast_thunk_sync_manager(&ctx.sync_mgr);

        checkpoint::Checkpoint restored_checkpoint;
        bool restored = false;
        if (!config.checkpoint_restore_path.empty()) {
            std::string checkpoint_error;
            if (!checkpoint::load_file(config.checkpoint_restore_path,
                                       restored_checkpoint,
                                       checkpoint_error)) {
                return RunResult{1, 0, checkpoint_error, false};
            }
            const auto architecture = is_64bit
                ? checkpoint::GuestArchitecture::X64
                : checkpoint::GuestArchitecture::X86;
            const auto main_module = std::find_if(
                restored_checkpoint.modules.begin(),
                restored_checkpoint.modules.end(),
                [&](const checkpoint::ModuleMetadata& module) {
                    return module.base == image.base_address;
                });
            if (main_module == restored_checkpoint.modules.end() ||
                main_module->image_size != image.image_size) {
                return RunResult{
                    1, 0,
                    "Checkpoint main module does not match loaded executable",
                    false};
            }
            if (!checkpoint::restore(restored_checkpoint, architecture,
                                     memory, cpu, checkpoint_error)) {
                return RunResult{1, 0, checkpoint_error, false};
            }
            restored = true;
        }

        // Call DllMain for any companion DLLs that were loaded during static
        // import resolution (before m_guest_call was wired up).
        if (!restored) {
            ctx.module_mgr.call_deferred_dll_mains();
        }

        if (!restored && pe_tls_callbacks_addr != 0 &&
            !ctx.process_mgr.has_exited()) {
            constexpr u64 DLL_PROCESS_ATTACH = 1;
            constexpr usize MAX_TLS_CALLBACKS = 64;
            for (usize index = 0; index < MAX_TLS_CALLBACKS; ++index) {
                u64 callback = 0;
                if (is_64bit) {
                    auto callback_result =
                        memory.read_u64(pe_tls_callbacks_addr + index * 8);
                    if (callback_result.is_err()) {
                        break;
                    }
                    callback = callback_result.value();
                } else {
                    auto callback_result =
                        memory.read_u32(pe_tls_callbacks_addr + index * 4);
                    if (callback_result.is_err()) {
                        break;
                    }
                    callback = callback_result.value();
                }

                if (callback == 0) {
                    break;
                }

                u64 args[3] = {
                    static_cast<u64>(image.base_address),
                    DLL_PROCESS_ATTACH,
                    0,
                };
                (void)run_guest_callback(cpu, memory, ctx.thunk_registry,
                                         ctx.process_mgr, callback, args, 3,
                                         is_64bit);
                if (ctx.process_mgr.has_exited()) {
                    break;
                }
            }
        }

        diagnostics::phase_mark(diagnostics::LoadPhase::PeEntry);
        auto run_result = run_execution_loop(
            cpu, memory, ctx.thunk_registry, ctx.process_mgr, config, event_pump);

        // ExitThread on the primary thread while other threads live: the
        // thread ends, the process does not (Learn ExitThread). Deliver its
        // DLL_THREAD_DETACH, signal it, then wait for the last thread; that
        // thread's exit code is the process's (thread_lifecycle_probe:
        // AFTER_MAIN_EXIT ... exit=42). ExitProcess from any thread still
        // ends everything at once.
        if (ctx.thread_mgr.primary_exit_pending() &&
            !ctx.process_mgr.has_exited()) {
            const u32 main_tid = ctx.thread_mgr.main_thread_id();
            // TerminateThread sends no DLL_THREAD_DETACH.
            if (!ctx.thread_mgr.is_thread_terminated(main_tid)) {
                ctx.module_mgr.notify_thread_detach();
            }
            ctx.thread_mgr.exit_thread(main_tid,
                                       ctx.thread_mgr.primary_exit_code());
            while (!ctx.process_mgr.has_exited() &&
                   ctx.thread_mgr.any_live_thread_except(main_tid)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!ctx.process_mgr.has_exited()) {
                ctx.process_mgr.exit_process(ctx.thread_mgr.last_exit_code());
            }
            run_result.exit_code =
                static_cast<int>(ctx.process_mgr.exit_code());
            run_result.success = true;
        } else if (ctx.thread_mgr.is_thread_terminated(
                       ctx.thread_mgr.main_thread_id()) &&
                   !ctx.process_mgr.has_exited()) {
            // The primary thread terminated itself as the last thread: the
            // process ends with that thread's TerminateThread code.
            run_result.exit_code =
                static_cast<int>(ctx.thread_mgr.last_exit_code());
            run_result.success = true;
        }

        if (!config.checkpoint_save_path.empty()) {
            checkpoint::CaptureRequest request;
            request.guest_thread_id = ctx.thread_mgr.main_thread_id();
            request.architecture = is_64bit
                ? checkpoint::GuestArchitecture::X64
                : checkpoint::GuestArchitecture::X86;
            request.determinism.virtual_time_ns =
                restored_checkpoint.determinism.virtual_time_ns;
            request.determinism.rng_seed =
                config.deterministic_rng_seed != 0
                    ? config.deterministic_rng_seed
                    : restored_checkpoint.determinism.rng_seed;
            request.determinism.retired_instructions =
                restored_checkpoint.determinism.retired_instructions +
                run_result.instructions_executed;
            request.modules.push_back(checkpoint::ModuleMetadata{
                image.base_address,
                image.image_size,
                win_exe_path,
                config.pe_path,
            });

            checkpoint::Checkpoint saved;
            std::string checkpoint_error;
            if (!checkpoint::capture(memory, cpu, request, saved,
                                     checkpoint_error) ||
                !checkpoint::save_file(config.checkpoint_save_path, saved,
                                       checkpoint_error)) {
                return RunResult{1, run_result.instructions_executed,
                                 checkpoint_error, false};
            }
        }
        return run_result;
    };

    RunResult result{};
    if (pthread_main_np() != 0) {
        // AppKit is not guest thread 1. Native callbacks (for example
        // WM_NCHITTEST) must use SendMessage's cross-thread queue rather than
        // entering the primary CPU concurrently with the worker below.
        const u32 host_previous_tid =
            winapi::ThreadManager::get_current_thread_id_static();
        winapi::ThreadManager::set_current_thread_id(0);
        struct HostThreadIdGuard {
            u32 previous;
            ~HostThreadIdGuard() {
                winapi::ThreadManager::set_current_thread_id(previous);
            }
        } host_thread_id_guard{host_previous_tid};
        std::atomic<bool> finished{false};
        std::thread primary([&] {
            winapi::ThreadManager::set_current_thread_id(ctx.thread_mgr.main_thread_id());
            // DllMain and PE TLS callbacks execute before run_execution_loop
            // installs its JIT state. Give this fresh guest thread the same
            // callback engine as CreateThread workers during loader startup.
            init_thread_jit_state(memory);
            result = run_primary();
            // Release native synchronous sends before waiting for AppKit to
            // return: the primary guest can no longer answer its messages.
            ctx.msg_queue.begin_shutdown();
            finished.store(true, std::memory_order_release);
            ctx.cocoa_bridge.wake_main_runloop();
        });
        while (!finished.load(std::memory_order_acquire)) {
            ctx.cocoa_bridge.service_main_runloop(16);
        }
        primary.join();
    } else {
        result = run_primary();
    }

    return result;
}

void shut_down_guest_process(RuntimeContext& ctx, const RunResult& result,
                             const RunConfig& config,
                             ExternalAppMapping& app_mapping) {
    // Signal worker threads that shutdown is in progress before destroying ctx.
    ctx.thread_mgr.begin_shutdown();

    // Mark process as exited so worker threads' callback loops break out.
    // (Some exit paths like HLT don't go through ExitProcess.)
    if (!ctx.process_mgr.has_exited()) {
        ctx.process_mgr.exit_process(static_cast<u32>(result.exit_code));
    }

    // Windows process termination ends every guest thread. Interrupt internal
    // host waits without signaling or consuming any guest synchronization
    // object, so callback runners can observe process exit and unwind.
    ctx.k32_ext_mgr.begin_shutdown();
    ctx.threadpool_mgr.begin_shutdown();
    ctx.sync_mgr.begin_shutdown();
    // Learn EnterCriticalSection: contested enters must not stay parked across
    // ExitProcess — wake enter_slow waiters so join_host_threads can finish.
    ctx.cs_mgr.begin_shutdown();

    // Shutdown all sockets so threads blocked in recv/select wake up with errors.
    ctx.ws2_mgr.shutdown_all_sockets();

    // Signal the message queue so any thread blocked in GetMessageW wakes up
    ctx.msg_queue.post_quit(static_cast<int>(ctx.process_mgr.exit_code()));
    ctx.msg_queue.begin_shutdown();

    // Stop cross-thread AppKit pumps before joining workers. Otherwise a guest
    // thread blocked in GetMessage → pump_events → vtr_run_on_main_sync waits
    // forever for the main CFRunLoop while this thread waits for that worker.
    ctx.cocoa_bridge.begin_shutdown();

    // Drop native UI immediately on guest exit/crash. The host OS process often
    // stays alive in ProcessSession::wait_for_workers(); without this, installer
    // dialogs linger on screen after 0xC0000005. Do this before joining workers so
    // the user sees teardown even if a thread join stalls.
    ctx.hwnd_mgr.for_each_hwnd([](windowing::HWND hwnd) {
        ipc::hwnd_registry_unpublish(static_cast<u32>(hwnd));
    });
    ctx.cocoa_bridge.destroy_all_windows();

    // Wait for worker threads, pumping the main run loop so any in-flight
    // cross-thread AppKit blocks can complete and observe begin_shutdown().
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
        while (std::chrono::steady_clock::now() < deadline) {
            if (ctx.thread_mgr.wait_for_threads(std::chrono::milliseconds(16))) {
                break;
            }
            ctx.cocoa_bridge.service_main_runloop(16);
        }
        if (!ctx.thread_mgr.wait_for_threads(std::chrono::milliseconds(0))) {
            // Learn (ExitProcess / TerminateProcess): terminating a process
            // ends every thread in it. Windows does not wait for a thread that
            // is parked in a blocking call, and neither can we — joining one
            // wedges the host process forever with no guest left to wake it.
            // Seen with a guest whose fatal error handler tore the main thread
            // down while a worker sat in an infinite WaitOnAddress: the guest
            // was gone, the process lived on at 0% CPU indefinitely.
            //
            // Persist what a normal shutdown persists, then end the process
            // without unwinding. Destroying RuntimeContext is exactly what
            // must not happen here: the parked worker still holds references
            // into its managers and guest memory.
            std::fprintf(stderr,
                "[thread] %u guest worker(s) still parked after the shutdown "
                "wake; terminating without joining (exit_code=0x%x)\n",
                ctx.thread_mgr.active_thread_count(),
                static_cast<unsigned>(result.exit_code));
            ctx.k32_ext_mgr.set_file_manager(nullptr);
            ctx.save_registry();
            cpu::jit::tstats::shutdown();
            std::fflush(nullptr);
            ::_exit(result.exit_code & 0xFF);
        }
    }
    ctx.thread_mgr.join_host_threads();

    // Cancel pending OVERLAPPED pipe reads while GuestMemory and the IOCP
    // manager are both still alive. RuntimeContext destroys Kernel32ExtManager
    // before FileManager, so leaving callbacks armed until member destruction
    // would permit a late pipe completion to call a dead manager.
    ctx.k32_ext_mgr.set_file_manager(nullptr);

    // Persist registry state to capsule directory on shutdown
    ctx.save_registry();
    cpu::jit::tstats::shutdown();

    // Release the external app mapping now that the process has exited. A child
    // launched from this mapping still needs it for sibling DLL access and
    // Authenticode verification, so hand it to the owning process session
    // instead when a worker is still running; otherwise ~ExternalAppMapping
    // removes it, including on every early-return path above.
    if (app_mapping.is_owned() && config.process_session != nullptr &&
        config.process_session->owned_worker_count() != 0) {
        auto detached = app_mapping.detach();
        const std::string link_path = detached.link_path;
        config.process_session->defer_symlink_cleanup(
            std::move(detached.link_path), std::move(detached.target_path),
            std::move(detached.lease_path), detached.lease_fd);
        VITREO_TRACE("[vitreo] Deferred app mapping cleanup: %s\n",
                     link_path.c_str());
    }
}

} // namespace vitreo::cli
