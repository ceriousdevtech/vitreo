// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CLI-002: guest-call boundaries for guest thread creation and execution

#include "run_internal.hpp"
#include "runtime/process/runtime_context.hpp"
#include "runtime/execution/guest_run_loop.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/thread/thread_context.hpp"
#include <vitreo/diagnostics.hpp>

#include <memory>
#include <thread>

namespace vitreo::cli {

void install_thread_callbacks(
    GuestMemory& memory, RuntimeContext& ctx, bool is_64bit,
    u64 peb_address, u64 pe_tls_data_start, u32 pe_tls_data_size) {
    // CreateThread builds the new thread's stack, TEB and TLS before it
    // returns (so an allocation failure fails the call, and a CREATE_SUSPENDED
    // thread already has the start context GetThreadContext reports).
    ctx.thread_mgr.set_thread_context_callbacks(
        [&memory, &ctx, is_64bit, peb_address, pe_tls_data_start,
         pe_tls_data_size](winapi::GuestThread& gt) -> bool {
            GuestThreadContextParams params{};
            params.is_64bit = is_64bit;
            params.peb_address = peb_address;
            params.pe_tls_data_start = pe_tls_data_start;
            params.pe_tls_data_size = pe_tls_data_size;
            params.stack_reserve = gt.stack_size;
            auto gtc = std::make_shared<GuestThreadContext>(
                create_guest_thread_context(memory, ctx, params, gt.thread_id));
            if (!gtc->valid()) return false;
            gt.stack_base = gtc->stack_base;
            gt.cpu = make_guest_cpu_state(memory, *gtc, is_64bit);
            // Seed RIP with the same VA GetProcAddress(ntdll, RtlUserThreadStart)
            // returns (the synthetic PE export stub). The thunk-registry address
            // can live in a different arena; guests compare the two
            // (thread_lifecycle_probe SUSPENDED_START ip=RtlUserThreadStart).
            {
                const GuestAddr ntdll =
                    ctx.module_mgr.get_module_handle("ntdll.dll");
                const GuestAddr start = ntdll != 0
                    ? ctx.module_mgr.get_proc_address(ntdll, "RtlUserThreadStart")
                    : 0;
                const GuestAddr exit = ntdll != 0
                    ? ctx.module_mgr.get_proc_address(ntdll, "RtlExitUserThread")
                    : 0;
                if (start != 0 && exit != 0) {
                    ctx.thread_mgr.set_start_thunks(start, exit);
                }
            }
            ctx.thread_mgr.seed_start_context(gt.cpu, gt.start_address,
                                              gt.parameter);
            gt.launch_context = gtc;
            return true;
        },
        [&memory, &ctx](winapi::GuestThread& gt) {
            if (auto gtc = std::static_pointer_cast<GuestThreadContext>(
                    gt.launch_context)) {
                release_guest_thread_context(memory, ctx, *gtc);
            }
        });
    {
        const GuestAddr ntdll = ctx.module_mgr.get_module_handle("ntdll.dll");
        const GuestAddr start = ntdll != 0
            ? ctx.module_mgr.get_proc_address(ntdll, "RtlUserThreadStart")
            : 0;
        const GuestAddr exit = ntdll != 0
            ? ctx.module_mgr.get_proc_address(ntdll, "RtlExitUserThread")
            : 0;
        if (start != 0 && exit != 0) {
            ctx.thread_mgr.set_start_thunks(start, exit);
        } else {
            auto tstart = ctx.thunk_registry.get_thunk_address(
                "ntdll.dll", "RtlUserThreadStart");
            auto texit = ctx.thunk_registry.get_thunk_address(
                "ntdll.dll", "RtlExitUserThread");
            if (tstart.is_ok() && texit.is_ok()) {
                ctx.thread_mgr.set_start_thunks(tstart.value(), texit.value());
            }
        }
    }

    set_thread_start_hook([&ctx] { (void)ctx.k32_ext_mgr.try_deliver_user_apcs(); });

    // Wire up thread execution: launch guest threads on real host threads.
    ctx.thread_mgr.set_run_callback(
        [&memory, &ctx, is_64bit, peb_address, pe_tls_data_start, pe_tls_data_size](u32 tid, GuestAddr func_addr, GuestAddr param, u64 second_param, u64 third_param, usize arg_count) {
            GuestThreadContextParams params{};
            params.is_64bit = is_64bit;
            params.peb_address = peb_address;
            params.pe_tls_data_start = pe_tls_data_start;
            params.pe_tls_data_size = pe_tls_data_size;
            auto* gt = ctx.thread_mgr.get_thread(tid);
            if (gt) params.stack_reserve = gt->stack_size;

            // Normally built by CreateThread (set_thread_context_callbacks);
            // built here only for a thread object created some other way.
            std::shared_ptr<GuestThreadContext> prepared =
                gt ? std::static_pointer_cast<GuestThreadContext>(
                         gt->launch_context)
                   : nullptr;
            const GuestThreadContext gtc =
                prepared ? *prepared
                         : create_guest_thread_context(memory, ctx, params, tid);
            if (!gtc.valid()) return;
            if (gt) gt->launch_context.reset();

            if (gt) gt->stack_base = gtc.stack_base;
            // A CREATE_SUSPENDED thread whose context SetThreadContext changed
            // starts at exactly that context (thread_lifecycle_probe
            // SUSPENDED_IP_REDIRECT); otherwise at its start routine.
            const bool at_context = gt != nullptr && gt->context_set;
            const cpu::CpuState set_context =
                at_context ? gt->cpu : cpu::CpuState{};
            // Thread-pool wait callbacks take a fourth argument (TP_WAIT_RESULT).
            const u64 fourth_param = gt != nullptr ? gt->fourth_parameter : 0;

            // Count before launch so a short-lived worker cannot decrement the
            // active count before its creator increments it.
            ctx.thread_mgr.thread_started();

            // Launch a real host thread. ThreadManager retains the joinable
            // owner so no worker can outlive RuntimeContext.
            std::thread host_thread([&memory, &ctx, is_64bit, tid, func_addr, param, second_param, third_param, fourth_param, arg_count, gtc, at_context, set_context]() {
                // Set per-thread guest TID
                winapi::ThreadManager::set_current_thread_id(tid);
                ctx.thread_mgr.bind_current_host_thread(tid);
                // Windows scheduling, including a level CreateThread's caller
                // set while the thread was still suspended.
                ctx.thread_mgr.apply_current_thread_scheduling();
                windowing::CocoaBridge::NativeWaitScope native_wait([&ctx, tid] {
                    ctx.msg_queue.dispatch_sent_messages(tid);
                });

                // Initialize per-thread JIT state for this worker thread
                init_thread_jit_state(memory);

                cpu::CpuState thread_cpu =
                    make_guest_cpu_state(memory, gtc, is_64bit);

                // SKIP_THREAD_ATTACH shows in the TEB: SameTebFlags (x64
                // +0x17EE, x86 +0xFCA) bit 3 SkipThreadAttach
                // (skip_thread_attach_probe).
                if (winapi::ThreadManager::current_thread_skips_thread_attach()) {
                    const GuestAddr flags = gtc.teb_addr + (is_64bit ? 0x17EEu : 0xFCAu);
                    const u16 value = memory.read_u16(flags).value_or(0);
                    (void)memory.write_u16(flags, static_cast<u16>(value | 0x0008u));
                }

                VITREO_TRACE(
                    "[thread] Host thread started: tid=%u func=0x%llx param=0x%llx stack=0x%llx\n",
                    tid,
                    static_cast<unsigned long long>(func_addr),
                    static_cast<unsigned long long>(param),
                    static_cast<unsigned long long>(thread_cpu.sp()));

                u64 args[4] = { param, second_param, third_param, fourth_param };
                u64 result = 0;
                if (at_context) {
                    thread_cpu = set_context;
                    result = run_guest_callback(
                        thread_cpu, memory, ctx.thunk_registry,
                        ctx.process_mgr, thread_cpu.rip, nullptr, 0, is_64bit,
                        nullptr, GuestCallbackKind::ThreadEntryAtContext);
                } else {
                    result = run_guest_callback(
                        thread_cpu, memory, ctx.thunk_registry,
                        ctx.process_mgr, func_addr, args, arg_count, is_64bit,
                        nullptr, GuestCallbackKind::ThreadEntry);
                }

                VITREO_TRACE(
                    "[thread] Host thread finished: tid=%u exit_code=%u\n",
                    tid, static_cast<u32>(result));

                ctx.thread_mgr.exit_thread(tid, static_cast<u32>(result));
                // The thread's guest code has returned and exit_thread runs no
                // guest callback, so nothing can touch this thread's stack,
                // TEB or TLS again. Windows reclaims both at thread exit; not
                // reclaiming them let a thread-churning guest (a launcher content
                // downloader) grow the region map without bound, which slows
                // every later allocation search down.
                release_guest_thread_context(memory, ctx, gtc);
                ctx.thread_mgr.thread_finished();
            });
            ctx.thread_mgr.track_host_thread(std::move(host_thread));
        }
    );
}

} // namespace vitreo::cli
