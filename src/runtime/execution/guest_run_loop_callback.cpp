// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Execution loop: guest callbacks (run_guest_callback). Split out of
// guest_run_loop.cpp as verbatim code motion.

#include "runtime/execution/guest_run_loop.hpp"
#include "runtime/execution/guest_run_loop_internal.hpp"
#if defined(VITREO_ARCH_X86_64)
#include "runtime/execution/native32_loop.hpp"
#include "runtime/execution/native64_loop.hpp"
#endif
#include "core/cpu/state/cpu_state.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/jit_helpers.hpp"
#include "core/cpu/jit/hot_block_dump.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/system/time.hpp"
#include "winapi/kernel32/system/kuser_shared_data.hpp"
#include "winapi/media/winmm/winmm.hpp"
#include "graphics/common/autorelease_scope.hpp"
#include "graphics/common/perf_diag.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/memory/mem_watch.hpp"
#include "core/cpu/seh/seh32.hpp"
#include "core/cpu/lock_prefix.hpp"
#include "winapi/msvcrt/runtime/cxx_eh.hpp"
#include <vitreo/diagnostics.hpp>
#include "core/base/perf_window.hpp"
#include "runtime/diagnostics/pc_trace.hpp"
#include "runtime/diagnostics/hostcall_ring.hpp"
#include "runtime/diagnostics/callstream.hpp"
#include "runtime/execution/exception_dispatch.hpp"
#include "runtime/execution/block_decoder.hpp"
#include "runtime/execution/exception_dispatch32.hpp"
#include "runtime/diagnostics/fault_report.hpp"
#include "runtime/execution/thunk_fast_path.hpp"
#include "runtime/execution/thunk_direct.hpp"
#include "runtime/execution/thunk_inchain.hpp"
#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <execinfo.h>
#include <unistd.h>
#include <vector>
#include <mach/mach_time.h>
#include <csignal>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

namespace vitreo::cli {

static thread_local std::vector<Seh64HostCallbackFrame> t_callback_frames;

bool seh64_interrupted_guest_frame(Seh64HostCallbackFrame* out) {
    if (out == nullptr || t_callback_frames.empty()) {
        return false;
    }
    *out = t_callback_frames.back();
    return true;
}

// =============================================================================
// run_guest_callback — execute a guest callback function (WndProc, etc.)
// =============================================================================

namespace {
thread_local bool t_seh32_handler_left_dispatcher = false;
} // namespace

void mark_seh32_handler_left_dispatcher() noexcept {
    t_seh32_handler_left_dispatcher = true;
}

bool consume_seh32_handler_left_dispatcher() noexcept {
    const bool left = t_seh32_handler_left_dispatcher;
    t_seh32_handler_left_dispatcher = false;
    return left;
}

namespace {
std::function<void()> g_thread_start_hook;
} // namespace

void set_thread_start_hook(std::function<void()> hook) {
    g_thread_start_hook = std::move(hook);
}

u64 run_guest_callback(cpu::CpuState& cpu,
                       GuestMemory& memory,
                       winapi::ThunkRegistry& thunk_registry,
                       winapi::ProcessManager& process_mgr,
                       u64 func_addr,
                       const u64* args,
                       usize arg_count,
                       bool is_64bit,
                       bool* completed_normally,
                       GuestCallbackKind kind,
                       u32 seh_abandon_ctx_addr,
                       u32 seh_abandon_esp_min) {
    ActiveGuestCpuScope active_cpu_scope(cpu);
    // Guest code is about to run under a host call: an in-chain API crossing
    // that made this call must resume through the dispatcher (thunk_inchain).
    ++cpu.guest_callback_seq;

    // Windows initializes every loaded DLL on a newly-created thread after
    // provisioning that thread's static TLS and before calling its entry
    // point. This is where a ThreadEntry first has an active per-thread CPU,
    // so ModuleManager's guest-call bridge will execute the notifications on
    // the correct TEB/stack rather than the primary thread.
    winapi::ModuleManager* thread_modules = nullptr;
    const bool enter_at_context =
        kind == GuestCallbackKind::ThreadEntryAtContext;
    // A thread created with SKIP_THREAD_ATTACH runs neither the attach nor
    // the detach notifications, TLS callbacks included
    // (skip_thread_attach_probe: tls_attach=0 tls_detach=0).
    if (kind == GuestCallbackKind::ThreadEntry || enter_at_context) {
        if (!winapi::ThreadManager::current_thread_skips_thread_attach()) {
            thread_modules = module_manager();
            if (thread_modules != nullptr) {
                thread_modules->notify_thread_attach();
            }
        }
        if (g_thread_start_hook) g_thread_start_hook();
    }
    struct ThreadDetachGuard {
        winapi::ModuleManager* modules;
        winapi::ProcessManager* process;
        ~ThreadDetachGuard() {
            // Windows sends DLL_PROCESS_DETACH, not one DLL_THREAD_DETACH per
            // worker, when process termination is already in progress.
            // Learn TerminateThread: DLLs are not notified.
            if (modules != nullptr && process != nullptr &&
                !process->has_exited() &&
                !winapi::ThreadSuspendRegistry::instance().is_terminating(
                    winapi::ThreadManager::get_current_thread_id_static())) {
                modules->notify_thread_detach();
            }
        }
    } thread_detach_guard{thread_modules, &process_mgr};

    // Learn "Exception Dispatching" and SetUnhandledExceptionFilter, measured
    // by tools/oracle_probe/unhandled_thread_probe on Windows, x64 and x86:
    // an exception no handler takes on a thread's START ROUTINE -- once the
    // top-level filter has run and not returned EXCEPTION_CONTINUE_EXECUTION
    // -- ends the whole process, exit code the exception code, and delivers
    // neither DLL_THREAD_DETACH nor DLL_PROCESS_DETACH (so exit_process, not
    // the ExitProcess API). A Windows thread has no frame above its start
    // routine to return to; returning here instead ran the thread's detach
    // callbacks, freed its stack and let the process run on with whatever the
    // dead thread had linked into shared state (a browser renderer then
    // walked a list node on that freed stack, 2026-10-22). Declared after
    // thread_detach_guard so it runs first, and that guard sees the exit.
    // Other callback kinds (window procedures, DllMain, APCs) keep returning
    // to their host call: their Windows behaviour is not this one.
    struct UnhandledThreadEntryExit {
        const bool thread_entry;
        winapi::ProcessManager& process;
        u32 code{0};
        bool armed{false};
        void arm(u32 exception_code) noexcept {
            code = exception_code;
            armed = true;
        }
        ~UnhandledThreadEntryExit() {
            if (!thread_entry || !armed || process.has_exited()) return;
            std::fprintf(stderr,
                "[callback] exception 0x%08x left a thread's start routine "
                "unhandled: terminating the process with it\n", code);
            process.exit_process(code);
        }
    } unhandled_exit{kind == GuestCallbackKind::ThreadEntry || enter_at_context,
                     process_mgr};
    // The top-level filter for a 32-bit start routine's exception when the
    // thread registered no SEH frame. The fs:[0] walks below build the
    // EXCEPTION_RECORD and CONTEXT -- and call the filter -- only for a
    // non-empty chain, but Windows' own thread-start frame reaches the filter
    // either way (the probe's x86 workers register none). True when the
    // filter resumed execution from its (possibly edited) CONTEXT.
    auto thread_entry_filter_32 = [&](u32 code, u32 flags, u32 address,
                                      u32 nargs, const u32* args) -> bool {
        const u32 scratch_esp =
            (static_cast<u32>(cpu.gpr.rsp) -
             (cpu::CTX32_SIZE + cpu::EREC32_SIZE + 16)) & ~0xFu;
        const u32 ctx_addr = scratch_esp;
        const u32 erec_addr = scratch_esp + cpu::CTX32_SIZE;
        cpu::write_exception_record_32(memory, erec_addr, code, flags, address,
                                       nargs, args);
        cpu::write_context_32(memory, ctx_addr, cpu);
        return invoke_unhandled_exception_filter_32(
            cpu, memory, thunk_registry, process_mgr, erec_addr, ctx_addr,
            scratch_esp);
    };

    if (completed_normally != nullptr) {
        *completed_normally = false;
    }

    // Guard against infinite recursion (e.g. SEH handler triggers another
    // exception whose handler triggers another exception, etc).
    static thread_local int s_callback_depth = 0;
    // Track handler addresses currently being dispatched so we never
    // re-enter the same handler (nested exception → skip frame).
    static thread_local std::unordered_set<u64> s_active_seh_handlers;
    if (s_callback_depth >= 16) {
        std::fprintf(stderr,
            "[callback] recursion depth %d exceeded for func=0x%llx — returning ContinueSearch\n",
            s_callback_depth, static_cast<unsigned long long>(func_addr));
        return cpu::ExceptionContinueSearch;
    }
    ++s_callback_depth;
    struct DepthGuard { ~DepthGuard() { --s_callback_depth; } } depth_guard;
    // Save the complete CPU state so we can restore after callback.
    // We must save ALL GPRs because nested callbacks (e.g. DllMain called from
    // inside a DlgProc callback) will freely modify callee-saved registers
    // (EBP, EBX, ESI, EDI / RBX, RBP, RSI, RDI, R12-R15).  Without restoring
    // them, the outer guest code resumes with a corrupted frame pointer and
    // reads wrong values from the stack (observed: g_hwnd = 0x5a instead of
    // 0x10000 because EBP was trashed by a nested DllMain).
    u64 saved_rip = cpu.rip;
    u64 saved_rsp = cpu.sp();
    u64 saved_rax = cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::RAX));
    u64 saved_rcx = cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::RCX));
    u64 saved_rdx = cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::RDX));
    u64 saved_rbx = cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::RBX));
    u64 saved_rbp = cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::RBP));
    u64 saved_rsi = cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::RSI));
    u64 saved_rdi = cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::RDI));
    u64 saved_r8  = cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::R8));
    u64 saved_r9  = cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::R9));
    u64 saved_r10 = cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::R10));
    u64 saved_r11 = cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::R11));
    u64 saved_r12 = cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::R12));
    u64 saved_r13 = cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::R13));
    u64 saved_r14 = cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::R14));
    u64 saved_r15 = cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::R15));
    const auto saved_flags = cpu.flags;
    const auto saved_flag_op = cpu.flag_op;
    const u64 saved_flag_src = cpu.flag_src;
    const u64 saved_flag_dst = cpu.flag_dst;
    const auto saved_sse = cpu.sse;
    const auto saved_avx = cpu.avx;

    Seh64HostCallbackFrame interrupted{};
    interrupted.rip = saved_rip;
    interrupted.rsp = saved_rsp;
    for (u8 i = 0; i < 16; ++i) {
        interrupted.gpr[i] = cpu.gpr.get64(i);
    }
    t_callback_frames.push_back(interrupted);
    struct CallbackFrameGuard {
        ~CallbackFrameGuard() {
            if (!t_callback_frames.empty()) {
                t_callback_frames.pop_back();
            }
        }
    } callback_frame_guard;

    VITREO_TRACE( "[callback] run_guest_callback: func=0x%llx args=%zu is_64bit=%d",
                 static_cast<unsigned long long>(func_addr),
                 arg_count, is_64bit);
    if (arg_count >= 4) {
        // WndProc(HWND, UINT msg, WPARAM, LPARAM)
        VITREO_TRACE( " [hwnd=0x%llx msg=0x%llx wp=0x%llx lp=0x%llx]",
                     static_cast<unsigned long long>(args[0]),
                     static_cast<unsigned long long>(args[1]),
                     static_cast<unsigned long long>(args[2]),
                     static_cast<unsigned long long>(args[3]));
    }
    VITREO_TRACE( "\n");
    // Validate the guest stack before touching CpuState. Wrapping subtraction
    // here previously produced addresses near UINT64_MAX which the memory TLB
    // then accepted (its bound formed addr + size, which overflows), yielding a
    // wild host pointer and a host SIGBUS instead of a guest fault.
    // (Not for an exact-context entry: that builds no frame.)
    if (!enter_at_context) {
        const u64 arch_max = is_64bit ? std::numeric_limits<u64>::max()
                                      : static_cast<u64>(std::numeric_limits<u32>::max());
        const u64 x64_stack_arg_bytes = arg_count > 4
            ? static_cast<u64>(arg_count - 4) * sizeof(u64)
            : 0;
        // x64: 32B shadow + stack arguments + 16B alignment slack + 8B
        // sentinel. x86: one 32-bit word per physical ABI slot + sentinel.
        const u64 required = is_64bit
            ? (0x28ull + x64_stack_arg_bytes + 0x10ull + 8ull)
            : (static_cast<u64>(arg_count) * sizeof(u32) + sizeof(u32));

        const bool bad_sp =
            saved_rsp == 0 ||
            saved_rsp == std::numeric_limits<u64>::max() ||
            saved_rsp > arch_max ||
            saved_rsp < required;

        if (bad_sp) {
            std::fprintf(stderr,
                "[callback] invalid %s stack: func=0x%llx rsp=0x%llx required=%llu "
                "args=%zu depth=%d\n",
                is_64bit ? "x64" : "x86",
                static_cast<unsigned long long>(func_addr),
                static_cast<unsigned long long>(saved_rsp),
                static_cast<unsigned long long>(required),
                arg_count, s_callback_depth);
            return cpu::ExceptionContinueSearch;
        }

        const u64 stack_bottom = saved_rsp - required;
        if (!memory.is_range_writable(stack_bottom,
                                      static_cast<usize>(required), is_64bit)) {
            GuestAddr mapped_base = 0;
            usize mapped_size = 0;
            MemoryProtection mapped_protection = MemoryProtection::None;
            const bool mapped = memory.query_region(
                stack_bottom, mapped_base, mapped_size, mapped_protection);
            const GuestAddr teb = static_cast<GuestAddr>(
                is_64bit ? cpu.seg.gs_desc.base : cpu.seg.fs_desc.base);
            const u64 teb_stack_base =
                is_64bit ? memory.read_u64(teb + 0x08).value_or(0)
                         : memory.read_u32(teb + 0x04).value_or(0);
            const u64 teb_stack_limit =
                is_64bit ? memory.read_u64(teb + 0x10).value_or(0)
                         : memory.read_u32(teb + 0x08).value_or(0);
            std::fprintf(stderr,
                "[callback] callback stack not writable: func=0x%llx "
                "range=[0x%llx,0x%llx) depth=%d tid=%u teb=0x%llx "
                "teb_stack=[0x%llx,0x%llx) mapped=%d "
                "region=[0x%llx,+0x%zx) protection=0x%x "
                "argc=%zu args=[0x%llx,0x%llx,0x%llx,0x%llx]\n",
                static_cast<unsigned long long>(func_addr),
                static_cast<unsigned long long>(stack_bottom),
                static_cast<unsigned long long>(saved_rsp), s_callback_depth,
                winapi::ThreadManager::get_current_thread_id_static(),
                static_cast<unsigned long long>(teb),
                static_cast<unsigned long long>(teb_stack_limit),
                static_cast<unsigned long long>(teb_stack_base),
                mapped ? 1 : 0,
                static_cast<unsigned long long>(mapped_base), mapped_size,
                static_cast<unsigned>(mapped_protection), arg_count,
                static_cast<unsigned long long>(
                    arg_count > 0 ? args[0] : 0),
                static_cast<unsigned long long>(
                    arg_count > 1 ? args[1] : 0),
                static_cast<unsigned long long>(
                    arg_count > 2 ? args[2] : 0),
                static_cast<unsigned long long>(
                    arg_count > 3 ? args[3] : 0));
            return cpu::ExceptionContinueSearch;
        }
    }

    // Restore everything saved above and bail out without entering guest code.
    // Used when a stack write fails partway through argument construction.
    auto restore_and_fail = [&](const char* what, u64 target) -> u64 {
        std::fprintf(stderr,
            "[callback] %s failed: func=0x%llx rsp=0x%llx target=0x%llx depth=%d\n",
            what,
            static_cast<unsigned long long>(func_addr),
            static_cast<unsigned long long>(saved_rsp),
            static_cast<unsigned long long>(target), s_callback_depth);
        cpu.rip = saved_rip;
        cpu.set_sp(saved_rsp);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RAX), saved_rax);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RCX), saved_rcx);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RDX), saved_rdx);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RBX), saved_rbx);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RBP), saved_rbp);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RSI), saved_rsi);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RDI), saved_rdi);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R8),  saved_r8);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R9),  saved_r9);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R10), saved_r10);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R11), saved_r11);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R12), saved_r12);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R13), saved_r13);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R14), saved_r14);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R15), saved_r15);
        cpu.flags = saved_flags;
        cpu.flag_op = saved_flag_op;
        cpu.flag_src = saved_flag_src;
        cpu.flag_dst = saved_flag_dst;
        cpu.sse = saved_sse;
        cpu.avx = saved_avx;
        if (completed_normally != nullptr) *completed_normally = false;
        return cpu::ExceptionContinueSearch;
    };

    // x86: the highest stack address the callee's own frame reaches (its
    // arguments). A callee that has moved ESP above it can no longer return
    // here -- it has transferred control to a frame of its caller's.
    u64 x86_callback_frame_top = 0;

    if (enter_at_context) {
        // Exact-context entry: nothing is pushed and no register changes.
    } else if (is_64bit) {
        // Microsoft x64 calling convention: RCX, RDX, R8, R9, then
        // eight-byte stack slots after the 32-byte shadow store.
        const u64 stack_arg_count = arg_count > 4
            ? static_cast<u64>(arg_count - 4)
            : 0;
        const u64 stack_arg_bytes = stack_arg_count * sizeof(u64);
        u64 new_sp = saved_rsp - 0x28 - stack_arg_bytes;
        new_sp &= ~0xFULL;  // 16-byte align

        // Push return sentinel
        new_sp -= 8;
        if (memory.write_u64(new_sp, CALLBACK_RETURN_SENTINEL).is_err()) {
            return restore_and_fail("x64 sentinel push", new_sp);
        }

        // Set up register arguments (WndProc: HWND, UINT, WPARAM, LPARAM)
        if (arg_count > 0) cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RCX), args[0]);
        if (arg_count > 1) cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RDX), args[1]);
        if (arg_count > 2) cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R8),  args[2]);
        if (arg_count > 3) cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R9),  args[3]);
        // Microsoft x64 ABI: a FLOAT/DOUBLE among the first four arguments
        // travels in XMM0-3 instead (IDWriteTextRenderer::DrawGlyphRun's
        // baseline origin). Load both, as an MSVC caller of an unprototyped
        // function does; XMM0-5 are volatile, so the callee owns them.
        for (usize index = 0; index < arg_count && index < 4; ++index) {
            cpu.sse.xmm[index].u64_[0] = args[index];
            cpu.sse.xmm[index].u64_[1] = 0;
        }

        for (usize index = 4; index < arg_count; ++index) {
            const u64 address = new_sp + 0x28 +
                static_cast<u64>(index - 4) * sizeof(u64);
            if (memory.write_u64(address, args[index]).is_err()) {
                return restore_and_fail("x64 stack argument write", address);
            }
        }

        // Microsoft x64 ABI: a callee entered by CALL sees RSP ≡ 8 (mod 16).
        // Entering guest code on any other alignment makes every aligned XMM
        // spill the guest's compiler placed at a fixed frame offset #GP, so
        // this is a hard invariant of the frame built above, not a preference.
        if ((new_sp & 0xFu) != 8u) {
            std::fprintf(stderr,
                "[abi-align] VIOLATION x64 guest entry kind=%d func=0x%llx "
                "entry_rsp=0x%llx mod16=%llu (expected 8) caller_rsp=0x%llx\n",
                static_cast<int>(kind),
                static_cast<unsigned long long>(func_addr),
                static_cast<unsigned long long>(new_sp),
                static_cast<unsigned long long>(new_sp & 0xFu),
                static_cast<unsigned long long>(saved_rsp));
        }
        cpu.set_sp(new_sp);
    } else {
        // x86 cdecl/stdcall: push args right-to-left, then return address
        u64 new_sp = saved_rsp;

        // Push arguments in reverse order. The range was validated above, so a
        // failure here means the mapping changed underneath us — bail out with
        // the CPU state restored rather than entering the guest on a half-built
        // frame.
        for (usize i = arg_count; i > 0; --i) {
            new_sp -= 4;
            if (memory.write_u32(new_sp, static_cast<u32>(args[i - 1])).is_err()) {
                return restore_and_fail("x86 argument push", new_sp);
            }
        }

        // Push return sentinel (truncated to 32-bit)
        new_sp -= 4;
        if (memory.write_u32(new_sp,
                static_cast<u32>(CALLBACK_RETURN_SENTINEL & 0xFFFFFFFF)).is_err()) {
            return restore_and_fail("x86 sentinel push", new_sp);
        }

        cpu.set_sp(new_sp);
        x86_callback_frame_top = new_sp + 4 + 4 * static_cast<u64>(arg_count);
    }

    // Set RIP to callback function
    cpu.rip = func_addr;

    // Create interpreter and marshaler for the callback execution
    cpu::Interpreter interp(cpu, memory);
    winapi::Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    // Hot-thunk fast-path address cache (Stage B2).
    FastThunkAddrs cb_fast_thunks;
    resolve_fast_thunks(cb_fast_thunks, thunk_registry);

    // Run until we hit the sentinel return address or an error
    u64 result = 0;
    constexpr usize CB_TRACE_SIZE = 32;
    u64 cb_trace_rips[CB_TRACE_SIZE] = {};
    usize cb_trace_idx = 0;

    // Activate per-thread trace ring for worker threads
    t_thread_trace.active = true;
    t_thread_trace.idx = 0;

    // Grab per-thread JIT state (set by run_execution_loop or worker thread init)
    auto* cb_block_cache  = t_block_cache;
    auto* cb_jit_compiler = t_jit_compiler;
    // Keep decoded-block reuse independent from native callback codegen so
    // the interpreter A/B path still avoids decoding hot blocks repeatedly.
    const bool cb_block_cache_enabled =
        t_jit_enabled && cb_block_cache != nullptr;
    // Native callback JIT is the production default for both x86 and x64.
    // VITREO_CALLBACK_JIT=0 remains available for correctness bisection.
    const char* cb_jit_env = std::getenv("VITREO_CALLBACK_JIT");
    bool cb_jit_requested = native_callback_jit_default(is_64bit, kind);
    if (cb_jit_env != nullptr) {
        cb_jit_requested = !(
            std::strcmp(cb_jit_env, "0") == 0 ||
            std::strcmp(cb_jit_env, "false") == 0 ||
            std::strcmp(cb_jit_env, "FALSE") == 0 ||
            std::strcmp(cb_jit_env, "off") == 0 ||
            std::strcmp(cb_jit_env, "OFF") == 0 ||
            std::strcmp(cb_jit_env, "no") == 0 ||
            std::strcmp(cb_jit_env, "NO") == 0
        );
    }
    bool  cb_jit_enabled  = cb_jit_requested && t_jit_enabled && cb_block_cache && cb_jit_compiler
                            && !(is_64bit
                                 && std::getenv("VITREO_JIT_64BIT_DISABLE") != nullptr);
                            // 64-bit blocks compile via universal interpreter-delegate
                            // stencil (see JitCompiler::compile_instruction). Set
                            // VITREO_JIT_64BIT_DISABLE=1 to force interpreter-only for
                            // 64-bit guests as a diagnostic escape hatch.

    u64 sentinel_check = is_64bit ? CALLBACK_RETURN_SENTINEL
                                   : (CALLBACK_RETURN_SENTINEL & 0xFFFFFFFF);

    // Worker threads (and nested guest callbacks) run here; publish the live
    // register file so a peer's SuspendThread + GetThreadContext can capture it.
    auto suspend_guard = winapi::ThreadSuspendRegistry::instance().publish(
        winapi::ThreadManager::get_current_thread_id_static(), &cpu);
    // L3: this callback loop's own lend to an in-chain API crossing (the
    // enclosing loop's comes back when this one returns).
    InChainThunkEnv inchain_env{&thunk_registry, &memory, &marshaler,
                                &suspend_guard, nullptr, false};
    inchain_env.fast_thunks = &cb_fast_thunks;
    inchain_env.process = &process_mgr;
    const cpu::jit::JitCompiler::InChainScope inchain_scope{
        cb_jit_compiler, &serve_in_chain_thunk, &inchain_env};
    const bool callback_progress_diag =
        std::getenv("VITREO_CALLBACK_PROGRESS") != nullptr;
    auto callback_progress_last = std::chrono::steady_clock::now();

#if defined(VITREO_ARCH_X86_64)
    // Win64: every guest thread must run natively once native64_prepare has
    // filled the thunk region, because that region holds one encoding and the
    // interpreter cannot decode the native stubs. A thread that cannot be set
    // up natively is a hard error rather than a slow fallback.
    if (is_64bit && native64_is_prepared()) {
        u64 native_rax = 0;
        if (native64_run_until_sentinel(
                cpu, memory, thunk_registry, process_mgr, sentinel_check,
                &native_rax, completed_normally)) {
            result = native_rax;
            goto callback_restore;
        }
    }
    if (!is_64bit && native32_is_prepared()) {
        u64 native_eax = 0;
        if (native32_run_until_sentinel(
                cpu, memory, thunk_registry, process_mgr,
                static_cast<u32>(sentinel_check),
                seh_abandon_ctx_addr, seh_abandon_esp_min, func_addr,
                &native_eax, completed_normally)) {
            result = native_eax;
            goto callback_restore;
        }
    }
#endif

    for (;;) {
        suspend_guard.checkpoint();
        if (suspend_guard.terminate_requested()) cpu.rip = sentinel_check;
        if (callback_progress_diag) {
            const auto now = std::chrono::steady_clock::now();
            if (now - callback_progress_last >= std::chrono::seconds(5)) {
                callback_progress_last = now;
                std::fprintf(
                    stderr,
                    "[callback-progress] tid=%u func=0x%08llx rip=0x%08llx "
                    "eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x "
                    "ebp=%08x esp=%08x\n",
                    winapi::ThreadManager::get_current_thread_id_static(),
                    static_cast<unsigned long long>(func_addr),
                    static_cast<unsigned long long>(cpu.rip),
                    static_cast<u32>(cpu.gpr.rax),
                    static_cast<u32>(cpu.gpr.rcx),
                    static_cast<u32>(cpu.gpr.rdx),
                    static_cast<u32>(cpu.gpr.rbx),
                    static_cast<u32>(cpu.gpr.rsi),
                    static_cast<u32>(cpu.gpr.rdi),
                    static_cast<u32>(cpu.gpr.rbp),
                    static_cast<u32>(cpu.gpr.rsp));
                std::fflush(stderr);
            }
        }
        // Check if we've returned to the sentinel
        if (cpu.rip == sentinel_check) {
            // Callback has returned — result is in RAX/EAX
            result = is_64bit ? cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::RAX))
                              : cpu.gpr.get32(static_cast<u8>(cpu::RegIndex::RAX));
            if (completed_normally != nullptr) {
                *completed_normally = true;
            }
            VITREO_TRACE( "[callback] Callback returned: 0x%llx\n",
                         static_cast<unsigned long long>(result));
            break;
        }

        // SEH frame handlers may abandon RtlpExecuteHandlerForException's
        // call frame (Petite wipe stub: popal/popfw/add esp,0xC / jmp OEP).
        // Detect: ESP has been restored to/above the establisher frame AND
        // RIP has left the handler's page (not the mid-handler stack pivot).
        // Strict > establisher: equal means still inside wipe-stub cleanup
        // (popfw lands on the registration record; add esp,0xC finishes).
        //
        // The general form: ESP above the handler's own argument area. MSVC's
        // x86 catch continuation (_JumpToContinuation) and __except entry
        // both reload the target frame's ESP -- which sits BELOW that frame's
        // registration record, so the establisher test above never sees them
        // -- and then jump; neither returns to RtlpExecuteHandlerForException.
        const bool left_handler_frame =
            x86_callback_frame_top != 0 && cpu.sp() > x86_callback_frame_top;
        if (seh_abandon_ctx_addr != 0 && seh_abandon_esp_min != 0 &&
            !is_64bit &&
            cpu.rip != sentinel_check && cpu.rip != 0 &&
            (left_handler_frame ||
             (cpu.sp() > seh_abandon_esp_min &&
              (cpu.rip & ~0xFFFull) != (func_addr & ~0xFFFull)))) {
            std::fprintf(stderr,
                "[seh32] handler abandoned dispatcher stack: "
                "func=0x%llx rip=0x%llx esp=0x%llx establisher=0x%08x "
                "-> ExceptionContinueExecution\n",
                static_cast<unsigned long long>(func_addr),
                static_cast<unsigned long long>(cpu.rip),
                static_cast<unsigned long long>(cpu.sp()),
                seh_abandon_esp_min);
            mark_seh32_handler_left_dispatcher();
            result = cpu::ExceptionContinueExecution;
            if (completed_normally != nullptr) {
                *completed_normally = true;
            }
            break;
        }

        // Early exit if the process has exited (prevents accessing freed memory)
        if (process_mgr.has_exited()) {
            break;
        }

        // Same VITREO_PC_WATCH_ADDR probe as run_execution_loop. IOCP/Channel
        // completion runs here; without this, Ping-path traps looked silent.
        {
            static int s_cb_custom_watch_n = 0;
            static const bool s_after_ping =
                (std::getenv("VITREO_PC_WATCH_AFTER_PING") != nullptr);
            static const bool s_pc_watch_enabled = !pc_watch_addrs().empty();
            static const int s_watch_limit = s_after_ping ? 5000 : 2000;
            if (s_pc_watch_enabled && pc_watch_hit(cpu.rip) &&
                s_cb_custom_watch_n < s_watch_limit &&
                (!s_after_ping || vitreo::diagnostics::trace_milestone_reached())) {
                const GuestAddr sp = static_cast<GuestAddr>(cpu.sp());
                u32 stack[4]{};
                for (int i = 0; i < 4; ++i) {
                    stack[i] = memory.read_u32(sp + static_cast<GuestAddr>(i * 4))
                                   .value_or(0);
                }
                const bool long_mode = cpu.mode == cpu::CpuMode::Long;
                const u64 ret = long_mode ? memory.read_u64(sp).value_or(0)
                                          : stack[0];
                const u64 arg0 = long_mode ? cpu.gpr.rcx : stack[1];
                const u64 arg1 = long_mode ? cpu.gpr.rdx : stack[2];
                const u64 arg2 = long_mode ? cpu.gpr.r8 : stack[3];
                std::fprintf(
                    stderr,
                    "[pcwatch-custom] pid=%d rip=0x%llx tid=%u ret=0x%llx "
                    "eax=0x%x ecx=0x%x edx=0x%x ebx=0x%x esi=0x%x edi=0x%x "
                    "esp=0x%llx arg0=0x%llx arg1=0x%llx arg2=0x%llx",
                    static_cast<int>(::getpid()),
                    static_cast<unsigned long long>(cpu.rip),
                    winapi::ThreadManager::get_current_thread_id_static(),
                    static_cast<unsigned long long>(ret),
                    cpu.gpr.get32(0), cpu.gpr.get32(1), cpu.gpr.get32(2),
                    cpu.gpr.get32(3), cpu.gpr.get32(6), cpu.gpr.get32(7),
                    static_cast<unsigned long long>(sp),
                    static_cast<unsigned long long>(arg0),
                    static_cast<unsigned long long>(arg1),
                    static_cast<unsigned long long>(arg2));
                append_pc_watch_memory(cpu, memory);
                std::fprintf(stderr, " (#cb%d)\n", s_cb_custom_watch_n++);
                std::fflush(stderr);
            }
        }

        // Same as the main loop: the guest IAT points at the 8-byte NOP entry
        // stub, not the internal dispatch gateway. Worker threads run their
        // entire start routine inside this callback loop, so executing the
        // sled means single-stepping every NOP through interp.step() on each
        // import call. Skipping unmodified NOPs is architecturally identical
        // to running them (no flags or stack effect). A patched sled is left
        // alone so hook engines still see their bytes.
        maybe_skip_thunk_nop_sled(cpu, memory, thunk_registry);

        // Handle exact thunk gateways within the callback. Entry stubs execute
        // as guest code so installed hooks and original trampolines both work.
        if (thunk_registry.is_dispatch_address(cpu.rip)) {            // Stage B2: fast-path for hot kernel32 thunks (x86 and x64).
            if (maybe_fast_thunk_call(cpu, memory, cb_fast_thunks)) {
                const auto thunk_addr = cpu.rip;
                if (!finish_thunk_ret(cpu, memory, thunk_registry, thunk_addr)) {
                    break;
                }
                continue;
            }

            const bool skip_slow_body = t_chain_slow_thunk_pending_raise &&
                                        cpu.pending_raise.active;
            t_chain_slow_thunk_pending_raise = false;
            // The chain already ran this gateway's handler and it requested
            // an unwind: leave as its ControlTransferred exit would.
            const bool chain_unwinding = t_chain_slow_thunk_unwinding &&
                                         cpu.pending_unwind.active;
            t_chain_slow_thunk_unwinding = false;
            // x64: an unwind still in flight either leaves this level (its
            // frames are walked) or goes on here from the thunk's caller;
            // a raise it ends in takes the pending-raise path below.
            bool unwind_raised = false;
            if (chain_unwinding) {
                if (!is_64bit) break;
                const auto action = apply_pending_unwind_64(
                    cpu, memory, module_manager(), process_mgr,
                    /*in_callback=*/true);
                if (action == PendingUnwind64Action::Resume) continue;
                if (action != PendingUnwind64Action::Raise) break;
                unwind_raised = true;
            }

            if (!skip_slow_body && !unwind_raised) {
                const auto slow = dispatch_slow_api_thunk(
                    cpu, memory, thunk_registry, marshaler, suspend_guard,
                    cb_fast_thunks, module_manager(), /*chain_lease=*/nullptr,
                    /*slow_hostcall_diag=*/false,
                    /*slow_hostcall_threshold_ms=*/0);
                if (slow == SlowThunkDispatch::Unresolved) {
                    VITREO_TRACE("[callback] Unresolved thunk at 0x%llx\n",
                                 static_cast<unsigned long long>(cpu.rip));
                    break;
                }
                if (slow == SlowThunkDispatch::Fatal) {
                    break;
                }
                if (slow == SlowThunkDispatch::ControlTransferred) {
                    // pending_unwind / longjmp — leave without RET.
                    if (process_mgr.has_exited()) {
                        break;
                    }
                    if (cpu.pending_unwind.active) {
                        if (!is_64bit) break;
                        const auto action = apply_pending_unwind_64(
                            cpu, memory, module_manager(), process_mgr,
                            /*in_callback=*/true);
                        if (action == PendingUnwind64Action::Resume) continue;
                        if (action != PendingUnwind64Action::Raise) break;
                        unwind_raised = true;
                    } else {
                        continue;
                    }
                }
            }

            // Check ExitProcess
            if (process_mgr.has_exited()) {
                break;
            }

            // Check for pending RaiseException inside callback
            if (!is_64bit && cpu.pending_raise.active) {
                const bool raise_from_helper = cpu.pending_raise.from_helper;
                std::fprintf(stderr,
                    "[seh32/cb] pending exception code=0x%08x flags=0x%08x "
                    "rip=0x%08x address=0x%08x helper=%s callback=0x%08x\n",
                    cpu.pending_raise.code,
                    cpu.pending_raise.flags,
                    static_cast<u32>(cpu.rip),
                    cpu.pending_raise.num_args >= 2
                        ? static_cast<u32>(cpu.pending_raise.args[1])
                        : 0u,
                    raise_from_helper ? "yes" : "no",
                    static_cast<u32>(func_addr));
                static std::atomic_bool first_cxx_callback_trace_dumped{false};
                if (cpu.pending_raise.code == 0xE06D7363u &&
                    std::getenv("VITREO_CXX_EH_DIAG") != nullptr &&
                    !first_cxx_callback_trace_dumped.exchange(true)) {
                    vitreo_pctrace_dump_ring("first-cxx-callback-exception");
                }
                // If the raise originated from a Win32 RaiseException thunk,
                // simulate RET from the thunk so cpu.rip/sp reflect the caller.
                // For raises from JIT slow-path helpers (e.g. STATUS_ACCESS_VIOLATION
                // from an unmapped guest read), cpu.rip/sp already point at the
                // faulting block so we MUST NOT pop the return address.
                if (!raise_from_helper) {
                    auto thunk_addr_re = cpu.rip;
                    auto ret_re = memory.read_u32(cpu.sp());
                    if (ret_re.is_ok()) {
                        cpu.rip = static_cast<u64>(ret_re.value());
                        u16 cleanup_re = thunk_registry.resolve_stdcall_bytes(thunk_addr_re);
                        cpu.set_sp(cpu.sp() + 4 + cleanup_re);
                    }
                }

                u32 raise_code = cpu.pending_raise.code;
                u32 raise_flags = cpu.pending_raise.flags;
                u32 raise_nargs = cpu.pending_raise.num_args;
                u32 raise_exception_addr = static_cast<u32>(cpu.rip);
                if (raise_from_helper &&
                    raise_code == cpu::STATUS_BREAKPOINT &&
                    raise_exception_addr > 0) {
                    --raise_exception_addr;
                }
                u32 raise_args[15] = {};
                for (u32 ri = 0; ri < raise_nargs; ++ri)
                    raise_args[ri] = cpu.pending_raise.args[ri];
                cpu.pending_raise.clear();

                // Learn: /cpp/intrinsics/fastfail — user-mode INT 0x29 is
                // STATUS_STACK_BUFFER_OVERRUN and "no exception handlers
                // are invoked". Do not walk VEH/SEH.
                if (raise_code == cpu::STATUS_STACK_BUFFER_OVERRUN) {
                    dump_fastfail_context(cpu, memory, raise_args[0]);
                    process_mgr.exit_process(cpu::STATUS_STACK_BUFFER_OVERRUN);
                    result = 0;
                    break;
                }

                // Learn: VEH is first-chance -- it runs before ANY frame
                // handler, and it runs even when the thread has no
                // registration record at all. Both facts matter: gating this
                // on a non-empty fs:[0] chain would reproduce the bug it
                // fixes.
                if (cli::invoke_vectored_handlers_32(
                        cpu, memory, thunk_registry, process_mgr,
                        raise_code, raise_flags, raise_nargs, raise_args,
                        raise_exception_addr)) {
                    continue;
                }

                auto chain_head = cpu::read_seh_chain_head(memory, cpu);
                if (chain_head.is_ok() && chain_head.value() != 0xFFFFFFFF && chain_head.value() != 0) {
                    u32 orig_esp = static_cast<u32>(cpu.gpr.rsp);
                    u32 scratch_esp = orig_esp - (cpu::CTX32_SIZE + cpu::EREC32_SIZE + 16);
                    scratch_esp &= ~0xF;

                    u32 ctx_addr = scratch_esp;
                    u32 erec_addr = scratch_esp + cpu::CTX32_SIZE;
                    u32 disp_ctx_addr = erec_addr + cpu::EREC32_SIZE;

                    // A bare `throw;` arrives as a C++ exception with a NULL
                    // ThrowInfo; give it back the exception the enclosing
                    // catch is handling so it can match an outer handler.
                    (void)winapi::resolve_cxx_rethrow_32(
                        memory, raise_code, raise_nargs, raise_args);
                    cpu::write_exception_record_32(memory, erec_addr,
                        raise_code, raise_flags, raise_exception_addr,
                        raise_nargs, raise_args);
                    cpu::write_context_32(memory, ctx_addr, cpu);
                    (void)memory.write_u32(disp_ctx_addr, 0);

                    u32 frame_addr = chain_head.value();
                    bool handled = false;
                    u32 chain_depth = 0;

                    static const bool s_cxx_eh_diag =
                        std::getenv("VITREO_CXX_EH_DIAG") != nullptr;
                    static const char* s_cxx_eh_dump_type =
                        std::getenv("VITREO_CXX_EH_DUMP_TYPE");
                    if (s_cxx_eh_diag && raise_code == 0xE06D7363u) {
                        const std::string desc =
                            winapi::describe_cxx_throw_32(memory, erec_addr);
                        std::fprintf(stderr, "[cxx-diag] throw %s\n",
                                     desc.c_str());
                        std::fflush(stderr);
                        if (s_cxx_eh_dump_type != nullptr &&
                            desc.find(s_cxx_eh_dump_type) != std::string::npos) {
                            hostcall_dump("cxx throw of watched type");
                        }
                    }

                    while (frame_addr != 0xFFFFFFFF && frame_addr != 0 && chain_depth < 256) {
                        if (!cpu::seh32_registration_dispatchable(
                                memory, cpu, frame_addr)) {
                            std::fprintf(stderr,
                                "[seh32] registration 0x%08x is outside the "
                                "thread stack (depth=%u) — chain corrupt, "
                                "stopping dispatch\n",
                                frame_addr, chain_depth);
                            break;
                        }
                        auto handler_info = cpu::read_seh_handler(memory, frame_addr);
                        if (handler_info.is_err()) break;
                        auto info = handler_info.value();

                        if (!seh_handler_looks_valid(memory, info.handler_addr)) {
                            // Dump 16 bytes at the frame so we can tell whether this
                            // is a real-but-malformed EXCEPTION_REGISTRATION_RECORD
                            // (e.g. EH4 with scopetable/trylevel), uninit zero page,
                            // or stale data left behind by a guest bug.
                            u32 w0 = info.next_frame;
                            u32 w1 = static_cast<u32>(info.handler_addr);
                            auto w2r = memory.read_u32(info.frame_addr + 8);
                            auto w3r = memory.read_u32(info.frame_addr + 12);
                            u32 w2 = w2r.is_ok() ? w2r.value() : 0xDEADBEEFu;
                            u32 w3 = w3r.is_ok() ? w3r.value() : 0xDEADBEEFu;
                            u32 head_now = 0xFFFFFFFFu;
                            {
                                auto h = cpu::read_seh_chain_head(memory, cpu);
                                if (h.is_ok()) head_now = h.value();
                            }
                            std::fprintf(stderr,
                                "[seh32/cb] RaiseException: skipping bogus handler 0x%llx (frame=0x%08x depth=%u head=0x%08x) — chain may be corrupt; "
                                "frame[0..15]: next=0x%08x handler=0x%08x [+8]=0x%08x [+12]=0x%08x; raise_code=0x%08x rip=0x%llx\n",
                                static_cast<unsigned long long>(info.handler_addr),
                                info.frame_addr, chain_depth, head_now,
                                w0, w1, w2, w3,
                                raise_code, static_cast<unsigned long long>(cpu.rip));
                            frame_addr = info.next_frame;
                            ++chain_depth;
                            continue;
                        }

                        VITREO_TRACE("[seh32/cb] RaiseException: calling handler 0x%08x (frame=0x%08x) for code 0x%08x\n",
                                     info.handler_addr, info.frame_addr, raise_code);

                        // Skip handlers already being dispatched (nested exception)
                        if (s_active_seh_handlers.count(info.handler_addr)) {
                            frame_addr = info.next_frame;
                            ++chain_depth;
                            continue;
                        }

                        // Intercept MSVC __ehhandler thunks (see seh32 path above).
                        if (raise_code == 0xE06D7363u) {
                            u32 fi = cxx_ehhandler_uses_host_personality(
                                memory, thunk_registry, info.handler_addr)
                                ? detect_cxx_ehhandler_funcinfo(memory, info.handler_addr)
                                : 0;
                            if (s_cxx_eh_diag && fi == 0) {
                                std::fprintf(stderr,
                                    "[cxx-diag]   depth=%u frame=0x%08x "
                                    "handler=0x%08x not-a-cxx-ehhandler\n",
                                    chain_depth, info.frame_addr,
                                    static_cast<u32>(info.handler_addr));
                                std::fflush(stderr);
                            }
                            if (fi != 0) {
                                auto r = winapi::dispatch_cxx_exception_32(
                                    memory, fi, info.frame_addr,
                                    erec_addr, ctx_addr);
                                if (s_cxx_eh_diag) {
                                    std::fprintf(stderr,
                                        "[cxx-diag]   depth=%u frame=0x%08x "
                                        "fi=0x%08x match=%d %s\n",
                                        chain_depth, info.frame_addr, fi,
                                        r.match ? 1 : 0,
                                        winapi::describe_cxx_frame_catches_32(
                                            memory, fi,
                                            info.frame_addr).c_str());
                                    std::fflush(stderr);
                                }
                                if (r.match) {
                                    VITREO_TRACE("[cxx_eh/cb] caught in frame=0x%08x handler=0x%08x dispCatchObj=%d\n",
                                                 info.frame_addr, r.handler_addr, r.disp_catch_obj);
                                    if (enter_cxx_catch_32(
                                            cpu, memory, thunk_registry,
                                            process_mgr, r, info.frame_addr,
                                            ctx_addr, erec_addr, "cxx_eh/cb")) {
                                        handled = true;
                                        break;
                                    }
                                    // See the seh32 path: the first matching
                                    // catch owns the exception, so a failed
                                    // entry must not hand it to an outer frame.
                                    std::fprintf(stderr,
                                        "[cxx_eh/cb] catch entry failed in "
                                        "frame=0x%08x handler=0x%08x — cannot "
                                        "deliver exception\n",
                                        info.frame_addr, r.handler_addr);
                                    std::fflush(stderr);
                                    break;
                                }
                                frame_addr = info.next_frame;
                                ++chain_depth;
                                continue;
                            }
                        }

                        s_active_seh_handlers.insert(info.handler_addr);
                        std::fprintf(stderr,
                            "[seh32/cb] RaiseException invoking handler=0x%08x "
                            "frame=0x%08x depth=%u\n",
                            info.handler_addr, info.frame_addr, chain_depth);
                        std::fflush(stderr);
                        auto disposition = invoke_seh32_frame_handler(
                            cpu, memory, thunk_registry, process_mgr,
                            info.handler_addr, info.frame_addr,
                            erec_addr, ctx_addr, disp_ctx_addr, scratch_esp);
                        s_active_seh_handlers.erase(info.handler_addr);
                        std::fprintf(stderr,
                            "[seh32/cb] RaiseException handler=0x%08x "
                            "returned=%llu exited=%d pending_unwind=%d\n",
                            info.handler_addr,
                            static_cast<unsigned long long>(disposition.disposition),
                            process_mgr.has_exited() ? 1 : 0,
                            cpu.pending_unwind.active ? 1 : 0);
                        std::fflush(stderr);

                        if (apply_pending_unwind_32(cpu, memory, ctx_addr, "seh32/cb")) {
                            handled = true;
                            break;
                        }

                        VITREO_TRACE("[seh32/cb] RaiseException: handler returned disposition=%llu\n",
                                     static_cast<unsigned long long>(disposition.disposition));

                        if (disposition.disposition == cpu::ExceptionContinueExecution) {
                            if (!disposition.transferred) {
                                (void)cli::resume_from_context_32(cpu, memory, thunk_registry, process_mgr,
                                                             erec_addr, ctx_addr, scratch_esp);
                            }
                            handled = true;
                            break;
                        }
                        frame_addr = info.next_frame;
                        ++chain_depth;
                    }

                    if (handled) {
                        // Outer C++ catch was entered from inside this catch
                        // funclet (rethrow). CPU already holds that catch's
                        // continuation — exit without restoring the saved
                        // callback registers so the transfer sticks.
                        if (cxx_catch_transfer_active()) {
                            VITREO_TRACE(
                                "[seh32/cb] abandoning callback 0x%08x — "
                                "outer C++ catch owns CPU (rip=0x%08x)\n",
                                static_cast<u32>(func_addr),
                                static_cast<u32>(cpu.rip));
                            if (completed_normally != nullptr) {
                                *completed_normally = true;
                            }
                            return cpu.gpr.get64(
                                static_cast<u8>(cpu::RegIndex::RAX));
                        }
                        continue;
                    }

                    VITREO_TRACE("[seh32/cb] RaiseException: no handler caught 0x%08x\n", raise_code);

                    if (s_cxx_eh_diag && raise_code == 0xE06D7363u) {
                        std::fprintf(stderr,
                            "[cxx-diag] UNCAUGHT 0x%08x depth=%u eip=0x%08x "
                            "(callback)\n",
                            raise_code, chain_depth,
                            static_cast<u32>(cpu.rip));
                        std::fflush(stderr);
                        hostcall_dump("uncaught C++ exception (callback)");
                    }

                    // A start routine's exception goes to the top-level filter
                    // and, unless it resumes, ends the process (see
                    // unhandled_exit).
                    if (unhandled_exit.thread_entry) {
                        if (invoke_unhandled_exception_filter_32(
                                cpu, memory, thunk_registry, process_mgr,
                                erec_addr, ctx_addr, scratch_esp)) {
                            continue;
                        }
                        unhandled_exit.arm(raise_code);
                        return 0;
                    }

                    // For raises that originated in a JIT slow-path helper (e.g.
                    // STATUS_ACCESS_VIOLATION on an unmapped read) the faulting
                    // instruction is still at cpu.rip. Restoring the pre-dispatch
                    // context and continuing would simply re-fault forever, so
                    // bail out of the callback dispatch loop instead.
                    if (raise_code == 0xC0000005u || (raise_flags & 1)) {
                        VITREO_TRACE("[seh32/cb] Unhandled fatal exception 0x%08x \u2014 aborting callback\n",
                                     raise_code);
                        if (raise_code == 0xC0000005u) {
                            hostcall_dump("STATUS_ACCESS_VIOLATION (callback)");
                        }
                        cpu.pending_raise.clear();
                        return 0;
                    }

                    // Same as main-loop RaiseException handling: if we continue for
                    // compatibility, restore the pre-dispatch CPU context first.
                    (void)cpu::read_context_32(memory, ctx_addr, cpu);
                } else if (unhandled_exit.thread_entry) {
                    // No registration at all: only Windows' own thread-start
                    // frame is left, and it calls the top-level filter.
                    if (thread_entry_filter_32(raise_code, raise_flags,
                                               raise_exception_addr,
                                               raise_nargs, raise_args)) {
                        continue;
                    }
                    unhandled_exit.arm(raise_code);
                    return 0;
                }

                if (raise_flags & 1) {
                    VITREO_TRACE("[seh32/cb] Noncontinuable exception 0x%08x unhandled in callback — skipping (compatibility)\n", raise_code);
                }
                continue;
            }

            // Same contract on x64. Without this the raise stayed pending, the
            // callback loop fell through to "simulate RET from thunk", and the
            // JIT re-entered the throwing block forever (observed: a C++ throw
            // (0xE06D7363) from a guest callback nested inside a LoadLibraryExW
            // hostcall spun at 100% CPU with no dispatch attempted at all).
            if (is_64bit && cpu.pending_raise.active) {
                const bool raise_from_helper = cpu.pending_raise.from_helper;
                VITREO_TRACE(
                    "[seh64/cb] pending exception code=0x%08x flags=0x%08x "
                    "rip=0x%llx helper=%s callback=0x%llx\n",
                    cpu.pending_raise.code, cpu.pending_raise.flags,
                    static_cast<unsigned long long>(cpu.rip),
                    raise_from_helper ? "yes" : "no",
                    static_cast<unsigned long long>(func_addr));

                // A RaiseException thunk still has its return address on the
                // stack; a JIT slow-path helper raise already points at the
                // faulting instruction and must not be popped.
                if (!raise_from_helper) {
                    auto ret_re = memory.read_u64(cpu.sp());
                    if (ret_re.is_ok()) {
                        cpu.rip = ret_re.value();
                        cpu.set_sp(cpu.sp() + 8);
                    }
                }

                const u32 raise_code = cpu.pending_raise.code;
                const u32 raise_flags = cpu.pending_raise.flags;
                const u32 raise_nargs = cpu.pending_raise.num_args;
                u64 raise_exception_addr = cpu.rip;
                if (raise_from_helper &&
                    raise_code == cpu::STATUS_BREAKPOINT &&
                    raise_exception_addr > 0) {
                    --raise_exception_addr;
                }
                u64 raise_args[15] = {};
                for (u32 ri = 0; ri < raise_nargs; ++ri)
                    raise_args[ri] = cpu.pending_raise.args[ri];
                cpu.pending_raise.clear();

                if (raise_code == cpu::STATUS_STACK_BUFFER_OVERRUN) {
                    dump_fastfail_context(cpu, memory, raise_args[0]);
                    process_mgr.exit_process(cpu::STATUS_STACK_BUFFER_OVERRUN);
                    result = 0;
                    break;
                }

                const bool stack_overflow =
                    raise_code == cpu::STATUS_STACK_OVERFLOW;
                if (stack_overflow) ++cpu.stack_overflow_dispatch_depth;
                const bool handled = dispatch_x64_exception(
                    cpu, memory, thunk_registry, process_mgr, raise_code,
                    raise_flags, raise_nargs, raise_args,
                    raise_exception_addr, module_manager());
                if (stack_overflow) --cpu.stack_overflow_dispatch_depth;
                if (handled) {
                    // cpu now resumes inside the handler.
                    continue;
                }

                VITREO_TRACE(
                    "[seh64/cb] no handler caught 0x%08x in callback 0x%llx\n",
                    raise_code, static_cast<unsigned long long>(func_addr));
                if (process_mgr.has_exited()) break;
                // Nothing claimed it, and dispatch_x64_exception has run the
                // top-level filter. A start routine ends the process;
                // otherwise returning to the hostcall that invoked this
                // callback is the only non-spinning option: re-executing
                // would loop on the same throw forever.
                unhandled_exit.arm(raise_code);
                break;
            }

            // Normal / guard-dispatch: helper already applied RET or rewrote rip.
            continue;
        }



        // Normal instruction execution — use JIT when available
        cb_trace_rips[cb_trace_idx % CB_TRACE_SIZE] = cpu.rip;
        cb_trace_idx++;

        // Record to per-thread trace ring (accessible from thunk handlers)
        t_thread_trace.rips[t_thread_trace.idx % THREAD_TRACE_SIZE] = cpu.rip;
        t_thread_trace.idx++;
        maybe_sample_worker_pc(cpu, func_addr, thunk_registry);

        // ---- JIT dispatch in callback ----
        bool cb_jit_handled = false;
        // L3-K: an in-chain crossing saw the process exit (below).
        bool cb_inchain_exited = false;
        cpu::jit::BasicBlock* cb_block = nullptr;
        cpu::ExecResult exec_result{};
        bool have_pctrace_result = false;
        // VITREO_PCTRACE: force interpreter for PCs in the trace range
        // inside callback/worker-thread dispatch too.
        if (pctrace_active(cpu.rip)) {
            pctrace_capture(cpu, memory);
            exec_result = interp.step();
            if (exec_result == cpu::ExecResult::Continue ||
                exec_result == cpu::ExecResult::Branch) {
                continue;
            }
            have_pctrace_result = true;
            if (exec_result == cpu::ExecResult::Exception) {
                std::FILE* out = pctrace_output();
                std::fprintf(out,
                    "[pctrace-cb] interp.step() exception at rip=%016llX\n",
                    static_cast<unsigned long long>(cpu.rip));
                pctrace_dump_caller(out, cpu, memory);
                std::fflush(out);
            }
        }
        // run_guest_callback has its OWN chain loop and block cache; like the
        // main loop it never dispatches a thunk-region address as a block.
        if (!have_pctrace_result && cb_block_cache_enabled &&
            !thunk_registry.is_in_thunk_region(cpu.rip)) {
            cb_block = prepare_dispatch_block(
                cpu, memory, *cb_block_cache,
                cb_jit_enabled ? cb_jit_compiler : nullptr,
                thunk_registry);

            if (cb_block) {
                (void)compile_dispatch_block(
                    *cb_block, cb_jit_enabled ? cb_jit_compiler : nullptr);

                if (cb_jit_enabled && cb_block->compiled_code) {
                    // JIT block chain in callback — same as main loop
                    cpu::jit::JitCodeArena::ArenaReadLease chain_lease{
                        cpu::jit::JitCodeArena::ArenaReadLease::StableExecution{}};
                    if (!chain_lease) {
                        (void)cb_jit_compiler->synchronize_arena_generation();
                    } else {
                    u32 chain_len = 0;
                    bool cb_jit_deopt = false;
                    const bool pctrace_on = pctrace_enabled();
                    // L3: the lease an in-chain crossing parks (as in the
                    // main loop).
                    inchain_env.chain_lease = &chain_lease;
                    struct InChainLeaseLend {
                        InChainThunkEnv& env;
                        ~InChainLeaseLend() { env.chain_lease = nullptr; }
                    } inchain_lease_lend{inchain_env};
                    do {
                        const GuestAddr executed_block_start =
                            cb_block->start_addr;
                        cpu::jit::CompiledBlock ccb =
                            cpu::jit::compiled_from_cache(*cb_block);

                        // Pre-execute logging for chain-spin diagnosis.
                        // VITREO_BLOCK_TRACE_LO/HI=hex range. Logs every Nth
                        // entry of a block in the range.
                        {
                            static const u32 trace_lo = []() -> u32 {
                                const char* e = std::getenv("VITREO_BLOCK_TRACE_LO");
                                return e ? (u32)std::strtoul(e, nullptr, 0) : 0;
                            }();
                            static const u32 trace_hi = []() -> u32 {
                                const char* e = std::getenv("VITREO_BLOCK_TRACE_HI");
                                return e ? (u32)std::strtoul(e, nullptr, 0) : 0;
                            }();
                            static const u64 trace_every = []() -> u64 {
                                const char* e = std::getenv("VITREO_BLOCK_TRACE_EVERY");
                                return e ? std::strtoull(e, nullptr, 0) : 0;
                            }();
                            if (trace_lo && trace_every) {
                                u32 sa = static_cast<u32>(cb_block->start_addr);
                                if (sa >= trace_lo && sa < trace_hi) {
                                    thread_local u64 bt_count = 0;
                                    if ((++bt_count % trace_every) == 0) {
                                        u32 tid = winapi::ThreadManager::get_current_thread_id_static();
                                        fprintf(stderr,
                                            "[block-trace] T%u entering block=0x%x rip_pre=0x%llx\n",
                                            tid, sa, (unsigned long long)cpu.rip);
                                        fflush(stderr);
                                    }
                                }
                            }
                        }

                        cpu.jit_iters = 1;
                        auto jresult = cb_jit_compiler->execute(ccb, cpu);
                        const auto cb_inchain_break =
                            cb_jit_compiler->take_inchain_break();
                        if (jresult == cpu::jit::JitExecResult::StaleCode) {
                            (void)cb_jit_compiler->synchronize_arena_generation();
                            cb_block = nullptr;
                            break;
                        }
                        if (jresult == cpu::jit::JitExecResult::Unsupported) {
                            // Match the main dispatcher: resume with one
                            // interpreter step at the target's published RIP.
                            cb_block = nullptr;
                            cb_jit_deopt = true;
                            break;
                        }
                        // L3: an in-chain crossing that must leave the chain
                        // (this loop's try_chain_thunk_inline breaks on both
                        // BreakOuter and Fatal). Ahead of any use of
                        // cb_block, which a nested dispatch may have freed.
                        // A crossing that saw the process exit leaves here
                        // too (serve_in_chain_thunk answers BreakOuter): the
                        // callback loop then stops before any more guest
                        // code -- no SEH dispatch of a raise the crossing
                        // left pending -- the stop the main loop makes in
                        // its own branch and this loop's "Early exit if the
                        // process has exited" makes at its top.
                        if (cb_inchain_break != cpu::jit::InChainBreak::None) {
                            cb_block = nullptr;
                            cb_inchain_exited = process_mgr.has_exited();
                            break;
                        }

                        // Helper-delegated faults (e.g. STATUS_ACCESS_VIOLATION
                        // on RO PE headers) must leave the JIT chain so the
                        // callback loop can run SEH — same as the main loop.
                        // Returning here abandoned the guest at the callback
                        // sentinel (0xDEADC0DE) and marked DllMain as failed
                        // (Petite unpack intentionally AVs and continues).
                        if (cpu.pending_raise.active &&
                            cpu.pending_raise.from_helper) {
                            cpu::jit::jit_flush_pending_av_dump();
                            std::fprintf(stderr,
                                "[jit/cb] pending exception code=0x%08x "
                                "block=0x%llx rip=0x%llx address=0x%llx "
                                "callback=0x%llx esp=0x%llx — dispatching SEH\n",
                                cpu.pending_raise.code,
                                static_cast<unsigned long long>(
                                    cb_block->start_addr),
                                static_cast<unsigned long long>(cpu.rip),
                                cpu.pending_raise.num_args >= 2
                                    ? static_cast<unsigned long long>(
                                          cpu.pending_raise.args[1])
                                    : 0ull,
                                static_cast<unsigned long long>(func_addr),
                                static_cast<unsigned long long>(cpu.gpr.rsp));
                            // Integer divide faults name their operands only
                            // through the registers, so they get the same dump.
                            const u32 diag_code = cpu.pending_raise.code;
                            if ((diag_code == 0xc0000005u || diag_code == 0xc0000094u ||
                                 diag_code == 0xc0000095u) &&
                                std::getenv("VITREO_EXCEPTION_DIAG") != nullptr) {
                                if (diag_code != 0xc0000005u) {
                                    std::fprintf(stderr, "[av-cb-ctx] code=0x%08x bytes@rip:",
                                                 diag_code);
                                    for (u32 i = 0; i < 16; ++i) {
                                        std::fprintf(stderr, " %02x",
                                                     memory.read_u8(cpu.rip + i).value_or(0));
                                    }
                                    std::fprintf(stderr, "\n");
                                    // The operands come from earlier in the
                                    // block: dump it from its entry.
                                    const u64 block_start = cb_block->start_addr;
                                    if (block_start != 0 && block_start < cpu.rip &&
                                        cpu.rip - block_start <= 0x400) {
                                        std::fprintf(stderr, "[av-cb-ctx] block 0x%llx bytes:",
                                                     static_cast<unsigned long long>(block_start));
                                        for (u64 a = block_start; a < cpu.rip + 3; ++a) {
                                            std::fprintf(stderr, "%02x",
                                                         memory.read_u8(a).value_or(0));
                                        }
                                        std::fprintf(stderr, "\n");
                                    }
                                    // A PCTRACE ring (VITREO_PCTRACE_RING) holds the
                                    // instructions that produced the operands.
                                    vitreo_pctrace_dump_ring("divide-fault");
                                    // VITREO_STORE_RING_DUMP=<lo>-<hi>: the recorded
                                    // stores (VITREO_STORE_RING) into that range,
                                    // to see what wrote, or never wrote, a value.
                                    if (const char* ring = std::getenv("VITREO_STORE_RING_DUMP")) {
                                        char* sep = nullptr;
                                        const u64 lo = std::strtoull(ring, &sep, 16);
                                        const u64 hi = (sep && *sep == '-')
                                            ? std::strtoull(sep + 1, nullptr, 16) : lo + 0x100;
                                        store_ring_dump(lo, hi, 256);
                                    }
                                    // VITREO_EXCEPTION_DIAG_DUMP=<hex addr>,<hex len>:
                                    // any other guest range (a caller's code).
                                    if (const char* spec =
                                            std::getenv("VITREO_EXCEPTION_DIAG_DUMP")) {
                                        char* end = nullptr;
                                        const u64 at = std::strtoull(spec, &end, 16);
                                        const u64 len = (end && *end == ',')
                                            ? std::strtoull(end + 1, nullptr, 16) : 0x80;
                                        std::fprintf(stderr, "[av-cb-ctx] dump 0x%llx:",
                                                     static_cast<unsigned long long>(at));
                                        for (u64 a = at; a < at + std::min<u64>(len, 0x1000); ++a) {
                                            // "--": not readable (unmapped or no access).
                                            const auto byte = memory.read_u8(a);
                                            if (byte) std::fprintf(stderr, "%02x", byte.value());
                                            else std::fprintf(stderr, "--");
                                        }
                                        std::fprintf(stderr, "\n");
                                    }
                                }
                                // Bitness matters here. This dump used to be
                                // unconditionally 32-bit, so on an x64 guest
                                // every register printed truncated and the
                                // stack scan read from a wrapped address --
                                // which is worse than no dump, because the
                                // truncated values look like plausible small
                                // pointers. An AV that cannot name its caller
                                // is the hardest kind of bring-up failure, so
                                // the x64 path reports full registers and
                                // walks qwords.
                                if (cpu.is_64bit()) {
                                    std::fprintf(
                                        stderr,
                                        "[av-cb-ctx] rax=%016llx rbx=%016llx "
                                        "rcx=%016llx rdx=%016llx rsi=%016llx "
                                        "rdi=%016llx rbp=%016llx rsp=%016llx\n"
                                        "[av-cb-ctx] r8=%016llx r9=%016llx "
                                        "r10=%016llx r11=%016llx r12=%016llx "
                                        "r13=%016llx r14=%016llx r15=%016llx\n",
                                        (unsigned long long)cpu.gpr.rax,
                                        (unsigned long long)cpu.gpr.rbx,
                                        (unsigned long long)cpu.gpr.rcx,
                                        (unsigned long long)cpu.gpr.rdx,
                                        (unsigned long long)cpu.gpr.rsi,
                                        (unsigned long long)cpu.gpr.rdi,
                                        (unsigned long long)cpu.gpr.rbp,
                                        (unsigned long long)cpu.gpr.rsp,
                                        (unsigned long long)cpu.gpr.r8,
                                        (unsigned long long)cpu.gpr.r9,
                                        (unsigned long long)cpu.gpr.r10,
                                        (unsigned long long)cpu.gpr.r11,
                                        (unsigned long long)cpu.gpr.r12,
                                        (unsigned long long)cpu.gpr.r13,
                                        (unsigned long long)cpu.gpr.r14,
                                        (unsigned long long)cpu.gpr.r15);
                                    std::fprintf(stderr, "[av-cb-stack]");
                                    for (u64 offset = 0; offset < 0x100;
                                         offset += 8) {
                                        auto value = memory.read_u64(
                                            static_cast<GuestAddr>(
                                                cpu.gpr.rsp + offset));
                                        if (!value.is_ok()) break;
                                        std::fprintf(stderr, " +%02llx=%llx",
                                                     (unsigned long long)offset,
                                                     (unsigned long long)
                                                         value.value());
                                    }
                                    std::fprintf(stderr, "\n");
                                } else {
                                    std::fprintf(
                                        stderr,
                                        "[av-cb-ctx] eax=%08x ebx=%08x ecx=%08x "
                                        "edx=%08x esi=%08x edi=%08x ebp=%08x "
                                        "esp=%08x\n",
                                        static_cast<u32>(cpu.gpr.rax),
                                        static_cast<u32>(cpu.gpr.rbx),
                                        static_cast<u32>(cpu.gpr.rcx),
                                        static_cast<u32>(cpu.gpr.rdx),
                                        static_cast<u32>(cpu.gpr.rsi),
                                        static_cast<u32>(cpu.gpr.rdi),
                                        static_cast<u32>(cpu.gpr.rbp),
                                        static_cast<u32>(cpu.gpr.rsp));
                                    std::fprintf(stderr, "[av-cb-stack]");
                                    for (u32 offset = 0; offset < 32;
                                         offset += 4) {
                                        auto value = memory.read_u32(
                                            static_cast<u32>(cpu.gpr.rsp) +
                                            offset);
                                        if (!value.is_ok()) break;
                                        std::fprintf(stderr, " +%02x=%08x",
                                                     offset, value.value());
                                    }
                                    std::fprintf(stderr, "\n");
                                }
                            }
                            // STATUS_BREAKPOINT (guest abort): always
                            // dump a short guest stack so the abort site is
                            // attributable without VITREO_EXCEPTION_DIAG.
                            if (cpu.pending_raise.code == 0x80000003u) {
                                std::fprintf(stderr,
                                    "[int3-ctx] rip=0x%llx rax=0x%llx rcx=0x%llx"
                                    " rsi=0x%llx rdi=0x%llx rsp=0x%llx\n",
                                    static_cast<unsigned long long>(cpu.rip),
                                    static_cast<unsigned long long>(cpu.gpr.rax),
                                    static_cast<unsigned long long>(cpu.gpr.rcx),
                                    static_cast<unsigned long long>(cpu.gpr.rsi),
                                    static_cast<unsigned long long>(cpu.gpr.rdi),
                                    static_cast<unsigned long long>(cpu.gpr.rsp));
                                std::fprintf(stderr,
                                    "[int3-regs] rdx=0x%llx rbx=0x%llx rbp=0x%llx"
                                    " r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx"
                                    " r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx\n",
                                    static_cast<unsigned long long>(cpu.gpr.rdx),
                                    static_cast<unsigned long long>(cpu.gpr.rbx),
                                    static_cast<unsigned long long>(cpu.gpr.rbp),
                                    static_cast<unsigned long long>(cpu.gpr.r8),
                                    static_cast<unsigned long long>(cpu.gpr.r9),
                                    static_cast<unsigned long long>(cpu.gpr.r10),
                                    static_cast<unsigned long long>(cpu.gpr.r11),
                                    static_cast<unsigned long long>(cpu.gpr.r12),
                                    static_cast<unsigned long long>(cpu.gpr.r13),
                                    static_cast<unsigned long long>(cpu.gpr.r14),
                                    static_cast<unsigned long long>(cpu.gpr.r15));
                                for (auto [name, rv] :
                                     {std::pair<const char*, u64>{"rcx",
                                                                  cpu.gpr.rcx},
                                      {"rsi", cpu.gpr.rsi}}) {
                                    auto slot = memory.read_u32(rv + 0x18);
                                    auto pages = memory.read_u32(rv + 0x1c);
                                    auto recip = memory.read_u64(rv + 0x20);
                                    if (slot.is_ok() && pages.is_ok()) {
                                        std::fprintf(
                                            stderr,
                                            "[int3-obj] %s+0x18 slot=%u "
                                            "pages=%u recip=0x%llx\n",
                                            name, slot.value(), pages.value(),
                                            static_cast<unsigned long long>(
                                                recip.is_ok() ? recip.value()
                                                              : 0));
                                    }
                                }
                                const u32 slot_stride =
                                    cpu.is_64bit() ? 8u : 4u;
                                std::fprintf(stderr,
                                    "[jit/cb] breakpoint stack (esp slots):\n");
                                std::string ascii;
                                for (u32 slot = 0; slot < 96; ++slot) {
                                    const GuestAddr slot_addr =
                                        static_cast<GuestAddr>(
                                            cpu.gpr.rsp +
                                            slot * slot_stride);
                                    if (cpu.is_64bit()) {
                                        const auto word =
                                            memory.read_u64(slot_addr);
                                        if (word.is_err()) {
                                            std::fprintf(stderr,
                                                "  [%u] <unreadable>\n", slot);
                                            break;
                                        }
                                        std::fprintf(
                                            stderr, "  [%u] 0x%llx\n", slot,
                                            static_cast<unsigned long long>(
                                                word.value()));
                                        continue;
                                    }
                                    const auto word =
                                        memory.read_u32(slot_addr);
                                    if (word.is_err()) {
                                        std::fprintf(stderr,
                                            "  [%u] <unreadable>\n", slot);
                                        break;
                                    }
                                    const GuestAddr ret =
                                        static_cast<GuestAddr>(word.value());
                                    const std::string thunk_name =
                                        thunk_registry.is_in_thunk_region(ret)
                                            ? thunk_registry.resolve_name(ret)
                                            : std::string{};
                                    std::fprintf(
                                        stderr, "  [%u] 0x%08x%s%s\n", slot,
                                        word.value(),
                                        thunk_name.empty() ? "" : " ",
                                        thunk_name.c_str());
                                    const u32 v = word.value();
                                    for (u32 b = 0; b < 4; ++b) {
                                        const char c = static_cast<char>(
                                            (v >> (8 * b)) & 0xff);
                                        ascii.push_back(
                                            (c >= 32 && c < 127) ? c : '.');
                                    }
                                }
                                if (!ascii.empty()) {
                                    std::fprintf(stderr,
                                        "[jit/cb] breakpoint stack ascii: %s\n",
                                        ascii.c_str());
                                }
                            }
                            if (std::getenv("VITREO_EXCEPTION_DIAG") != nullptr) {
                                vitreo_hostcall_dump(
                                    "jit-callback-exception");
                                const usize trace_count =
                                    std::min(cb_trace_idx, CB_TRACE_SIZE);
                                const usize trace_start =
                                    cb_trace_idx - trace_count;
                                std::fprintf(
                                    stderr,
                                    "[jit/cb] last %zu callback blocks:\n",
                                    trace_count);
                                for (usize trace_i = trace_start;
                                     trace_i < cb_trace_idx; ++trace_i) {
                                    const GuestAddr trace_rip =
                                        cb_trace_rips[
                                            trace_i % CB_TRACE_SIZE];
                                    const std::string thunk_name =
                                        thunk_registry.is_in_thunk_region(
                                            trace_rip)
                                            ? thunk_registry.resolve_name(
                                                  trace_rip)
                                            : std::string{};
                                    std::fprintf(
                                        stderr, "  [%zu] 0x%llx%s%s\n", trace_i,
                                        static_cast<unsigned long long>(
                                            trace_rip),
                                        thunk_name.empty() ? "" : " ",
                                        thunk_name.c_str());
                                }
                            }
                            break;
                        }

                        if (jresult == cpu::jit::JitExecResult::Halt) {
                            // Callback execution halted; unwind to caller.
                            break;
                        }

                        // Heartbeat (callback dispatch path): VITREO_HEARTBEAT
                        // Maintains a ring buffer of recent block starts +
                        // post-execute rips so we can see the actual chain trace
                        // during spin loops.
                        {
                            static const u64 cb_hb_interval = []() -> u64 {
                                const char* e = std::getenv("VITREO_HEARTBEAT");
                                if (!e || !*e) return 0;
                                u64 v = std::strtoull(e, nullptr, 0);
                                return v ? v : 100000ULL;
                            }();
                            if (cb_hb_interval) {
                                thread_local u64 cb_hb_count = 0;
                                constexpr size_t RING_N = 32;
                                thread_local u32 ring_block[RING_N] = {};
                                thread_local u64 ring_rip[RING_N] = {};
                                thread_local size_t ring_idx = 0;
                                ring_block[ring_idx % RING_N] = static_cast<u32>(cb_block->start_addr);
                                ring_rip[ring_idx % RING_N] = cpu.rip;
                                ++ring_idx;
                                if ((++cb_hb_count % cb_hb_interval) == 0) {
                                    u32 tid = winapi::ThreadManager::get_current_thread_id_static();
                                    fprintf(stderr,
                                        "[heartbeat/cb] T%u rip=0x%llx blocks=%llu (last=0x%x len=%u) — recent chain:\n",
                                        tid,
                                        (unsigned long long)cpu.rip,
                                        (unsigned long long)cb_hb_count,
                                        (unsigned)cb_block->start_addr,
                                        (unsigned)cb_block->byte_length);
                                    size_t start = (ring_idx >= RING_N) ? ring_idx - RING_N : 0;
                                    for (size_t i = start; i < ring_idx; ++i) {
                                        size_t s = i % RING_N;
                                        fprintf(stderr, "  [hb-ring] #%zu block=0x%x exit_rip=0x%llx\n",
                                                i, ring_block[s],
                                                (unsigned long long)ring_rip[s]);
                                    }
                                    fflush(stderr);
                                }
                            }
                        }

                        // Check sentinel after each block
                        if (cpu.rip == sentinel_check) break;
                        // Check process exit (prevents accessing freed memory)
                        if (process_mgr.has_exited()) break;

                        ++chain_len;
                        // Same as the main loop: a pending SuspendThread must
                        // be able to park this thread. The callback loop runs
                        // every thread start routine, so without this a
                        // spinning worker could not be suspended at all.
                        if (suspend_guard.suspend_pending()) break;
                        if ((chain_len & 0xFu) == 0) {
                            // Same safepoint contract as the main loop's
                            // pump: a retired slot means `cb_block` may name
                            // recycled code, so leave and re-look-up at RIP.
                            cpu::jit::JitCodeArena::HostCallSafepoint
                                pump_safepoint{&chain_lease};
                            pump_safepoint.park();
                            cb_jit_compiler->poll_gui_pump();
                            if (pump_safepoint.unpark() !=
                                cpu::jit::JitCodeArena::HostCallSafepoint::
                                    Resume::Continue) {
                                break;
                            }
                        }

                        (void)cb_jit_compiler->synchronize_executable_writes();
                        if (executed_block_start != cpu.rip) {
                            const auto thunk_out = try_chain_thunk_inline(
                                cpu, memory, thunk_registry, marshaler,
                                suspend_guard, cb_fast_thunks, *cb_block_cache,
                                cb_block, module_manager(), &chain_lease,
                                /*slow_hostcall_diag=*/false,
                                /*slow_hostcall_threshold_ms=*/0);
                            if (thunk_out == ChainThunkOutcome::Fatal ||
                                thunk_out == ChainThunkOutcome::BreakOuter) {
                                break;
                            }
                            if (thunk_out == ChainThunkOutcome::Handled) {
                                if (cpu.pending_raise.active ||
                                    cpu.pending_unwind.active) {
                                    break;
                                }
                                // ExitProcess from in-chain slow thunk must stop
                                // before the next native hop (same as outer).
                                if (process_mgr.has_exited()) {
                                    break;
                                }
                                if (!cb_block) {
                                    diagnostics::perf_note_chain_lookup_miss(
                                        cpu.rip);
                                    if (diagnostics::perf_window_active() &&
                                        cb_block_cache->find_covering(cpu.rip)) {
                                        diagnostics::perf_note_chain_miss_mid_block();
                                    }
                                    break;
                                }
                            } else {
                                cb_block = cb_block_cache->lookup_fast(cpu.rip);
                                if (!cb_block) {
                                    diagnostics::perf_note_chain_lookup_miss(
                                        cpu.rip);
                                    if (thunk_registry.is_in_thunk_region(
                                            cpu.rip)) {
                                        diagnostics::perf_note_chain_miss_thunk();
                                        if (follow_thunk_sled_jump(
                                                cpu, memory, thunk_registry)) {
                                            cb_block = cb_block_cache->lookup_fast(cpu.rip);
                                        }
                                        if (!cb_block) {
                                            maybe_skip_thunk_nop_sled(
                                                cpu, memory, thunk_registry);
                                            break;
                                        }
                                    } else {
                                        if (diagnostics::perf_window_active() &&
                                            cb_block_cache->find_covering(cpu.rip)) {
                                            diagnostics::perf_note_chain_miss_mid_block();
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                        if (pctrace_on && pctrace_overlaps(
                                cb_block->start_addr,
                                cb_block->byte_length)) {
                            break;
                        }

                        if (!cb_block->compiled_code) {
                            if (thunk_registry.is_in_thunk_region(
                                    cb_block->start_addr)) {
                                break;
                            }
                            if (cb_block->compile_failed) break;
                            // Cold successor: back to the dispatcher, which
                            // interprets and counts it (see the main loop).
                            if (!cpu::jit::JitCompiler::
                                    execution_reaches_hot_threshold(
                                        *cb_block)) {
                                break;
                            }
                            cb_block->execution_count++;
                            if (cb_block->instructions.size() !=
                                    cb_block->instruction_count &&
                                (cb_block_cache == nullptr ||
                                 !ensure_block_decoded(
                                     memory, *cb_block_cache, *cb_block))) {
                                cb_block = nullptr;
                                break;
                            }
                            (void)compile_and_publish_dispatch_block(
                                *cb_block, *cb_jit_compiler);
                            if (!cb_block->compiled_code) break;
                        }
                    } while (true);

                    // See main-loop note: do not settle lazy flags after every
                    // callback JIT chain. Dispatch entry materializes.
                    cb_jit_handled = !cb_jit_deopt;
                    }
                }
            }
        }
        if (cb_inchain_exited) {
            break;
        }
        // Pre-decoded block interpreter path (no JIT compiled code)
        const bool have_jit_exception =
            !have_pctrace_result && cb_jit_handled &&
            cpu::consume_pending_helper_raise(
                cpu, interp.context(), exec_result);
        if (cb_jit_handled && !have_jit_exception) {
            continue;
        }

        // The guest has already returned to the host: the callback loop's
        // top-of-iteration check is the ONLY consumer of the sentinel, and
        // nothing below may fetch it as code. A thunk tail-called from the
        // callback (a window procedure that does `jmp DefWindowProcW`) returns
        // straight to the sentinel from inside this iteration.
        if (!have_pctrace_result && !have_jit_exception &&
            cpu.rip == sentinel_check) {
            continue;
        }
        // A decoded block is only ever interpreted from its own start; a
        // block that lookup_fast handed back for some other RIP (a thunk
        // stub, or a stale entry) must go back to the top of the loop, where
        // the sled skip and the dispatch check own thunk-region addresses.
        if (!have_pctrace_result && !have_jit_exception && cb_block &&
            (cb_block->start_addr != cpu.rip ||
             thunk_registry.is_in_thunk_region(cb_block->start_addr))) {
            static std::atomic<int> s_reports{0};
            if (s_reports.fetch_add(1, std::memory_order_relaxed) < 4) {
                std::fprintf(stderr,
                    "[callback] refusing to interpret block 0x%llx at rip=0x%llx"
                    " (thunk-region=%d); re-dispatching\n",
                    static_cast<unsigned long long>(cb_block->start_addr),
                    static_cast<unsigned long long>(cpu.rip),
                    thunk_registry.is_in_thunk_region(cb_block->start_addr) ? 1 : 0);
            }
            cb_block = nullptr;
            continue;
        }
        // See the main loop: a released decoded array must be rebuilt before
        // the interpreter walks it.
        if (!have_pctrace_result && !have_jit_exception && cb_block &&
            cb_block->instruction_count > 0 &&
            cb_block->instructions.size() != cb_block->instruction_count) {
            if (cb_block_cache == nullptr ||
                !ensure_block_decoded(memory, *cb_block_cache, *cb_block)) {
                cb_block = nullptr;
            }
        }

        if (!have_pctrace_result && !have_jit_exception && cb_block &&
            cb_block->instruction_count > 0) {
            cpu.materialize_lazy_flags();
            auto& bctx = interp.context();
            exec_result = cpu::ExecResult::Continue;
            u32 cb_insns_run = 0;
            for (u32 bi = 0; bi < cb_block->instruction_count; ++bi) {
                const auto& instr = cb_block->instructions[bi];
                bctx.instr_length = instr.length;
                if (instr.prefixes.lock) {
                    cpu::LockPrefixGuard guard;
                    exec_result = cpu::execute_instruction(bctx, instr);
                } else {
                    exec_result = cpu::execute_instruction(bctx, instr);
                }
                if (exec_result == cpu::ExecResult::Continue) {
                    cpu.advance_ip(instr.length);
                    ++cb_insns_run;
                } else {
                    break;
                }
            }
            diagnostics::perf_add_interp_insns(cb_insns_run);
            if (exec_result == cpu::ExecResult::Continue ||
                exec_result == cpu::ExecResult::Branch) {
                continue;
            }
        } else if (!have_pctrace_result && !have_jit_exception) {
            cpu.materialize_lazy_flags();
            exec_result = interp.step();
            diagnostics::perf_add_interp_insns(1);
        }
        if (exec_result != cpu::ExecResult::Continue &&
            exec_result != cpu::ExecResult::Branch) {
            if (exec_result == cpu::ExecResult::Syscall) {
                const auto dispatch = dispatch_native_service(
                    cpu, thunk_registry, marshaler, suspend_guard);
                if (dispatch != NativeServiceDispatch::UnknownService) {
                    continue;
                }
                if (cpu.is_64bit()) {
                    std::fprintf(stderr,
                        "[callback] unknown x64 NT service number 0x%x\n",
                        cpu.gpr.get32(static_cast<u8>(cpu::RegIndex::RAX)));
                } else {
                    std::fprintf(stderr,
                        "[callback] unknown x86 SYSENTER NT service "
                        "number 0x%x\n",
                        cpu.gpr.get32(static_cast<u8>(cpu::RegIndex::RAX)));
                }
                break;
            }
            if (exec_result == cpu::ExecResult::Interrupt &&
                interp.context().interrupt_num == 0x2E && !cpu.is_64bit()) {
                cpu.rip += 2;
                const auto int2e_dispatch = dispatch_native_service(
                    cpu, thunk_registry, marshaler, suspend_guard);
                if (int2e_dispatch != NativeServiceDispatch::UnknownService) {
                    continue;
                }
                std::fprintf(stderr,
                    "[callback] unknown x86 INT 2E NT service "
                    "number 0x%x\n",
                    cpu.gpr.get32(static_cast<u8>(cpu::RegIndex::RAX)));
                break;
            }
            if (exec_result == cpu::ExecResult::Interrupt &&
                interp.context().interrupt_num == 0x29) {
                dump_fastfail_context(cpu, memory, cpu.gpr.rcx);
                process_mgr.exit_process(cpu::STATUS_STACK_BUFFER_OVERRUN);
                result = 0;
                break;
            }
            if (exec_result == cpu::ExecResult::Interrupt &&
                interp.context().interrupt_num == 0x2C && is_64bit) {
                const u64 address = cpu.rip;
                cpu.rip += 2;
                if (dispatch_x64_exception(
                        cpu, memory, thunk_registry, process_mgr,
                        cpu::STATUS_ASSERTION_FAILURE, 0, 0, nullptr,
                        address, module_manager())) continue;
                process_mgr.exit_process(cpu::STATUS_ASSERTION_FAILURE);
                result = 0;
                break;
            }
            // JIT helper INT3 is consumed into Interrupt (same as the main
            // loop). seh64/cb only runs at a thunk gateway, so a CHECK
            // inside callback JIT never reached dispatch_x64_exception —
            // the callback returned 0 and the process died silently.
            // Intel SDM INT3 / Learn STATUS_BREAKPOINT: VEH/SEH first;
            // unhandled kills the process with 0x80000003.
            if (exec_result == cpu::ExecResult::Interrupt &&
                interp.context().interrupt_num == 3 && is_64bit) {
                const u64 breakpoint_addr = interp.context().exception_addr;
                const bool handled = dispatch_x64_exception(
                    cpu, memory, thunk_registry, process_mgr,
                    cpu::STATUS_BREAKPOINT, 0, 0, nullptr,
                    breakpoint_addr, module_manager());
                if (handled) {
                    continue;
                }
                if (vitreo::detail::g_int3_nop) {
                    continue;
                }
                std::fprintf(stderr,
                    "[vitreo] unhandled INT3 (STATUS_BREAKPOINT) at rip=0x%llx"
                    " — terminating process (0x80000003)\n",
                    static_cast<unsigned long long>(cpu.rip));
                vitreo_hostcall_dump("int3-unhandled");
                process_mgr.exit_process(0x80000003u);
                result = 0;
                break;
            }

            VITREO_TRACE( "[callback] Execution stopped: %d at RIP=0x%llx\n",
                         static_cast<int>(exec_result),
                         static_cast<unsigned long long>(cpu.rip));

            if (exec_result == cpu::ExecResult::Exception) {
                // Hardware faults are dispatched through the same x64
                // VEH/.pdata path regardless of whether guest code is running
                // in the primary loop or inside a callback such as DllMain or
                // a TLS callback. Returning directly to the loader here
                // bypasses handlers that Windows invokes first-chance.
                if (cpu.is_64bit()) {
                    const auto& cbctx = interp.context();
                    const u32 status_code =
                        cpu::cpu_exception_to_status(cbctx.exception);
                    const u64 params[2] = {
                        static_cast<u64>(cbctx.exception_error),
                        cbctx.exception_addr,
                    };
                    const u32 num_params =
                        status_code == cpu::STATUS_ACCESS_VIOLATION ? 2u : cbctx.exception_num_params;
                    if (dispatch_x64_exception(
                            cpu, memory, thunk_registry, process_mgr,
                            status_code, 0, num_params,
                            num_params != 0 ? params : nullptr,
                            cpu.rip, module_manager())) {
                        if (process_mgr.has_exited()) {
                            result = 0;
                            break;
                        }
                        continue;
                    }
                }

                // Whether the 32-bit chain walk below already ran the
                // top-level filter (it does only for a non-empty chain).
                bool top_level_filter_ran = false;
                // Try 32-bit SEH dispatch in callback context too
                if (!cpu.is_64bit()) {
                    const auto& cbctx = interp.context();
                    const u32 status_code =
                        cpu::cpu_exception_to_status(cbctx.exception);
                    u32 params[2] = {
                        cbctx.exception_error,
                        static_cast<u32>(cbctx.exception_addr),
                    };
                    const u32 num_params =
                        status_code == cpu::STATUS_ACCESS_VIOLATION ? 2u : cbctx.exception_num_params;

                    // Microsoft Learn (Vectored Exception Handling): VEH is
                    // first-chance and is not tied to a stack frame. The
                    // callback's ordinary exception path used to gate all
                    // dispatch on a non-empty FS:[0] chain, silently skipping
                    // the guest's process-wide handler on worker threads with
                    // no frame registration.
                    if (cli::invoke_vectored_handlers_32(
                            cpu, memory, thunk_registry, process_mgr,
                            status_code, 0, num_params, params,
                            static_cast<u32>(cpu.rip))) {
                        continue;
                    }

                    auto chain_head = cpu::read_seh_chain_head(memory, cpu);
                    if (chain_head.is_ok() && chain_head.value() != 0xFFFFFFFF && chain_head.value() != 0) {
                        // Deep exception-path diagnostics. Every line below is
                        // debug output, and building it walks guest memory
                        // (fault bytes, object graphs, the stack, the TEB, the
                        // hostcall ring) solely so it can be printed. Gate the
                        // whole region — the reads, not just the writes —
                        // behind VITREO_TRACE; a counter or a printf gated
                        // further in still charges the caller for the work.
                        // Body indentation is intentionally unchanged so the
                        // gate reads as a diff of one brace pair.
                        if (::vitreo::detail::g_trace_enabled) {
                        u8 fault_bytes[8] = {};
                        u8 this_bytes[16] = {};
                        bool have_fault_bytes = memory.read_bytes(cpu.rip, fault_bytes, sizeof(fault_bytes)).is_ok();
                        bool have_this_bytes = memory.read_bytes(cpu.gpr.rcx, this_bytes, sizeof(this_bytes)).is_ok();

                        if (have_fault_bytes) {
                            std::fprintf(stderr,
                                "[seh-cb] func=0x%llx exception=%d status=0x%08x at RIP=0x%llx addr=0x%llx depth=%d EAX=%08x ECX=%08x EDX=%08x EBX=%08x ESI=%08x EDI=%08x EBP=%08x ESP=%08x bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                                static_cast<unsigned long long>(func_addr),
                                static_cast<int>(cbctx.exception), status_code,
                                static_cast<unsigned long long>(cpu.rip),
                                static_cast<unsigned long long>(cbctx.exception_addr),
                                s_callback_depth,
                                static_cast<unsigned>(cpu.gpr.rax),
                                static_cast<unsigned>(cpu.gpr.rcx),
                                static_cast<unsigned>(cpu.gpr.rdx),
                                static_cast<unsigned>(cpu.gpr.rbx),
                                static_cast<unsigned>(cpu.gpr.rsi),
                                static_cast<unsigned>(cpu.gpr.rdi),
                                static_cast<unsigned>(cpu.gpr.rbp),
                                static_cast<unsigned>(cpu.gpr.rsp),
                                static_cast<unsigned>(fault_bytes[0]),
                                static_cast<unsigned>(fault_bytes[1]),
                                static_cast<unsigned>(fault_bytes[2]),
                                static_cast<unsigned>(fault_bytes[3]),
                                static_cast<unsigned>(fault_bytes[4]),
                                static_cast<unsigned>(fault_bytes[5]),
                                static_cast<unsigned>(fault_bytes[6]),
                                static_cast<unsigned>(fault_bytes[7]));
                            // Name any register holding an import-thunk
                            // address. Vitreo's thunks are an 8-NOP sled
                            // followed by a dispatch gateway, so a register
                            // pointing at one reads as `90 90 ... cc cc` in a
                            // memory dump and looks like uninitialised
                            // padding. The registry knows the API; printing it
                            // turns an address in the thunk region into a name.
                            {
                                const u64 candidates[5] = {
                                    cpu.rip, cpu.gpr.rbx, cpu.gpr.rax,
                                    cpu.gpr.rcx, cpu.gpr.rdx};
                                static const char* kRegNames[5] = {
                                    "RIP", "EBX", "EAX", "ECX", "EDX"};
                                const auto exports =
                                    thunk_registry.enumerate_exports();
                                for (int ci = 0; ci < 5; ++ci) {
                                    const auto want =
                                        static_cast<GuestAddr>(candidates[ci]);
                                    if (want == 0) continue;
                                    for (const auto& e : exports) {
                                        // A thunk is a NOP sled plus a
                                        // gateway, so a register may point a
                                        // few bytes into it.
                                        if (want < e.thunk_address ||
                                            want >= e.thunk_address + 16) {
                                            continue;
                                        }
                                        std::fprintf(stderr,
                                            "[seh-cb] %s=0x%08x is import "
                                            "thunk %s!%s (+%u)\n",
                                            kRegNames[ci],
                                            static_cast<unsigned>(want),
                                            e.dll_name.c_str(),
                                            e.function_name.empty()
                                                ? "<ordinal>"
                                                : e.function_name.c_str(),
                                            static_cast<unsigned>(
                                                want - e.thunk_address));
                                        break;
                                    }
                                }
                            }
                            if (status_code == cpu::STATUS_ACCESS_VIOLATION) {
                                vitreo::diagnostics::dump_hostcalls(
                                    "seh-cb-av-before-dispatch");
                                auto dump_obj = [&](const char* label, u64 addr) {
                                    Byte bytes[32] = {};
                                    const bool ok = memory.read_bytes(
                                        static_cast<GuestAddr>(addr), bytes,
                                        sizeof(bytes)).is_ok();
                                    std::fprintf(stderr, "[seh-cb] %s%s=",
                                                 label, ok ? "" : " <unmapped>");
                                    if (ok) {
                                        for (usize i = 0; i < sizeof(bytes); i += 4) {
                                            u32 w = 0;
                                            std::memcpy(&w, bytes + i, 4);
                                            std::fprintf(stderr, " %08x", w);
                                        }
                                    }
                                    std::fputc('\n', stderr);
                                    return ok;
                                };
                                dump_obj("esi-obj", cpu.gpr.rsi);
                                dump_obj("ebx-obj", cpu.gpr.rbx);
                                dump_obj("ebp-obj", cpu.gpr.rbp);
                                dump_obj("ecx-obj", cpu.gpr.rcx);
                                dump_obj("edx-obj", cpu.gpr.rdx);
                                dump_obj("edi-obj", cpu.gpr.rdi);
                                auto dump_ascii = [&](const char* label,
                                                      GuestAddr addr) {
                                    char name[64] = {};
                                    if (addr < 0x10000u ||
                                        memory.read_bytes(
                                            addr,
                                            reinterpret_cast<Byte*>(name),
                                            sizeof(name) - 1).is_err()) {
                                        return;
                                    }
                                    std::fprintf(stderr, "[seh-cb] %s=", label);
                                    for (char c : name) {
                                        if (c == 0) break;
                                        std::fputc((c >= 0x20 && c < 0x7f) ? c
                                                                           : '.',
                                                   stderr);
                                    }
                                    std::fputc('\n', stderr);
                                };
                                auto read_u32_at = [&](GuestAddr addr) -> u32 {
                                    Byte b[4] = {};
                                    if (memory.read_bytes(addr, b, 4).is_err()) {
                                        return 0;
                                    }
                                    u32 w = 0;
                                    std::memcpy(&w, b, 4);
                                    return w;
                                };
                                {
                                    const GuestAddr imethod =
                                        static_cast<GuestAddr>(cpu.gpr.rsi);
                                    const u32 method = read_u32_at(imethod);
                                    std::fprintf(
                                        stderr,
                                        "[seh-cb] imethod+0x14=0x%08x +0x24=0x%08x "
                                        "+0x148=0x%08x method=0x%08x sig=0x%08x\n",
                                        read_u32_at(imethod + 0x14),
                                        read_u32_at(imethod + 0x24),
                                        read_u32_at(imethod + 0x148),
                                        method,
                                        method > 0x10000u
                                            ? read_u32_at(
                                                static_cast<GuestAddr>(method) +
                                                0xc)
                                            : 0u);
                                }
                                // Managed class object: +0x34 name, +0x38
                                // namespace, +0x14 init flags, +0x24 parent,
                                // +0xb0 related-class wrapper (parent at +4).
                                // Field helpers pass the class in EDI; the
                                // interpreter frame is in EBP.
                                const GuestAddr klass_edi =
                                    static_cast<GuestAddr>(cpu.gpr.rdi);
                                dump_ascii("edi-class-ns",
                                           read_u32_at(klass_edi + 0x38));
                                dump_ascii("edi-class-name",
                                           read_u32_at(klass_edi + 0x34));
                                std::fprintf(stderr,
                                    "[seh-cb] edi+0x14=0x%08x +0x24=0x%08x "
                                    "+0xc4=0x%08x\n",
                                    read_u32_at(klass_edi + 0x14),
                                    read_u32_at(klass_edi + 0x24),
                                    read_u32_at(klass_edi + 0xc4));
                                const GuestAddr image =
                                    static_cast<GuestAddr>(
                                        read_u32_at(
                                            static_cast<GuestAddr>(
                                                cpu.gpr.rsi) + 8));
                                if (image > 0x10000u) {
                                    std::fprintf(stderr,
                                        "[seh-cb] image=0x%08x +0x60=0x%08x\n",
                                        static_cast<unsigned>(image),
                                        read_u32_at(image + 0x60));
                                    dump_ascii("image-name",
                                               read_u32_at(image + 8));
                                }
                                const GuestAddr klass =
                                    static_cast<GuestAddr>(cpu.gpr.rbp);
                                dump_ascii("ebp-class-ns",
                                           read_u32_at(klass + 0x38));
                                dump_ascii("ebp-class-name",
                                           read_u32_at(klass + 0x34));
                                const u32 related = read_u32_at(klass + 0xb0);
                                std::fprintf(stderr,
                                    "[seh-cb] ebp+0x14=0x%08x +0xb0=0x%08x "
                                    "related+4=0x%08x\n",
                                    read_u32_at(klass + 0x14), related,
                                    related ? read_u32_at(
                                        static_cast<GuestAddr>(related) + 4)
                                            : 0u);
                                if (related) {
                                    dump_obj("related-obj", related);
                                }
                                {
                                    const GuestAddr fault =
                                        static_cast<GuestAddr>(
                                            cbctx.exception_addr);
                                    GuestAddr rbase = 0;
                                    usize rsize = 0;
                                    MemoryProtection rprot{};
                                    const bool mapped = memory.query_region(
                                        fault, rbase, rsize, rprot);
                                    const GuestAddr next =
                                        memory.next_region_base(fault);
                                    std::fprintf(
                                        stderr,
                                        "[seh-cb] fault-region mapped=%d "
                                        "base=0x%llx size=0x%zx next=0x%llx\n",
                                        mapped ? 1 : 0,
                                        static_cast<unsigned long long>(rbase),
                                        rsize,
                                        static_cast<unsigned long long>(next));
                                }
                                {
                                    std::fprintf(stderr, "[seh-cb] stack=");
                                    for (u32 i = 0; i < 24; ++i) {
                                        std::fprintf(
                                            stderr, " %08x",
                                            read_u32_at(
                                                static_cast<GuestAddr>(
                                                    cpu.gpr.rsp) +
                                                i * 4u));
                                    }
                                    std::fputc('\n', stderr);
                                }
                                {
                                    const GuestAddr teb =
                                        static_cast<GuestAddr>(
                                            cpu.seg.fs_desc.base);
                                    const u32 stack_base =
                                        read_u32_at(teb + 4);
                                    const u32 stack_limit =
                                        read_u32_at(teb + 8);
                                    const GuestAddr edi =
                                        static_cast<GuestAddr>(cpu.gpr.rdi);
                                    GuestAddr edi_base = 0;
                                    usize edi_size = 0;
                                    MemoryProtection edi_prot{};
                                    const bool edi_mapped = memory.query_region(
                                        edi, edi_base, edi_size, edi_prot);
                                    GuestAddr esp_base = 0;
                                    usize esp_size = 0;
                                    MemoryProtection esp_prot{};
                                    const bool esp_mapped = memory.query_region(
                                        static_cast<GuestAddr>(cpu.gpr.rsp),
                                        esp_base, esp_size, esp_prot);
                                    const u32 fo_head = read_u32_at(0xf0ec18);
                                    const u32 fo_count = read_u32_at(0xf0ec14);
                                    std::fprintf(
                                        stderr,
                                        "[seh-cb] teb=0x%08x stack=%08x..%08x "
                                        "edi-region mapped=%d base=0x%llx "
                                        "size=0x%zx esp-region base=0x%llx "
                                        "size=0x%zx fo_count=%u fo_head=0x%08x\n",
                                        static_cast<unsigned>(teb),
                                        stack_limit, stack_base,
                                        edi_mapped ? 1 : 0,
                                        static_cast<unsigned long long>(edi_base),
                                        edi_size,
                                        static_cast<unsigned long long>(esp_base),
                                        esp_size, fo_count, fo_head);
                                    if (fo_head > 0x10000u) {
                                        std::fprintf(stderr, "[seh-cb] fo_head=");
                                        for (u32 i = 0; i < 8; ++i) {
                                            std::fprintf(
                                                stderr, " %08x",
                                                read_u32_at(
                                                    static_cast<GuestAddr>(
                                                        fo_head) +
                                                    i * 4u));
                                        }
                                        std::fputc('\n', stderr);
                                    }
                                }
                                {
                                    const GuestAddr ebp =
                                        static_cast<GuestAddr>(cpu.gpr.rbp);
                                    const u32 table = read_u32_at(ebp - 0x14);
                                    const u32 frame = read_u32_at(ebp - 0x2c);
                                    std::fprintf(
                                        stderr,
                                        "[seh-cb] ebp-0x14=0x%08x ebp-0x10=0x%08x "
                                        "ebp-0x0c=0x%08x ebp-0x08=0x%08x "
                                        "ebp-0x24=0x%08x ebp-0x28=0x%08x "
                                        "ebp-0x2c=0x%08x\n",
                                        table,
                                        read_u32_at(ebp - 0x10),
                                        read_u32_at(ebp - 0x0c),
                                        read_u32_at(ebp - 0x08),
                                        read_u32_at(ebp - 0x24),
                                        read_u32_at(ebp - 0x28),
                                        frame);
                                    const u32 ldfld_obj = read_u32_at(ebp - 0x28);
                                    if (ldfld_obj > 0x10000u) {
                                        dump_obj("ldfld-obj", ldfld_obj);
                                        const u32 vt = read_u32_at(
                                            static_cast<GuestAddr>(ldfld_obj));
                                        std::fprintf(
                                            stderr,
                                            "[seh-cb] ldfld-vt=0x%08x vt-klass=0x%08x\n",
                                            vt,
                                            vt > 0x10000u
                                                ? read_u32_at(
                                                    static_cast<GuestAddr>(vt))
                                                : 0u);
                                    }
                                    std::fprintf(
                                        stderr,
                                        "[seh-cb] edi+0x34=0x%08x edi+0x38=0x%08x\n",
                                        read_u32_at(klass_edi + 0x34),
                                        read_u32_at(klass_edi + 0x38));
                                    if (table > 0x10000u) {
                                        dump_obj("interp-table", table);
                                    }
                                    if (frame > 0x10000u) {
                                        dump_obj("interp-frame", frame);
                                        std::fprintf(
                                            stderr,
                                            "[seh-cb] frame+0x10=0x%08x\n",
                                            read_u32_at(
                                                static_cast<GuestAddr>(frame) +
                                                0x10));
                                    }
                                }
                                u32 page_table = 0;
                                Byte table_bytes[4] = {};
                                const GuestAddr table_addr =
                                    static_cast<GuestAddr>(cpu.gpr.rdi) + 0xc4;
                                if (memory.read_bytes(table_addr, table_bytes,
                                                      sizeof(table_bytes)).is_ok()) {
                                    std::memcpy(&page_table, table_bytes, 4);
                                    std::fprintf(stderr,
                                        "[seh-cb] edi+0xc4=0x%08x\n", page_table);
                                }
                                u32 esi8 = 0;
                                Byte esi8_bytes[4] = {};
                                if (memory.read_bytes(
                                        static_cast<GuestAddr>(cpu.gpr.rsi) + 8,
                                        esi8_bytes, 4).is_ok()) {
                                    std::memcpy(&esi8, esi8_bytes, 4);
                                }
                                if (esi8 > 0x10000) {
                                    dump_obj("esi+8", esi8);
                                    char name[64] = {};
                                    if (memory.read_bytes(esi8,
                                            reinterpret_cast<Byte*>(name),
                                            sizeof(name) - 1).is_ok()) {
                                        std::fprintf(stderr, "[seh-cb] esi+8-ascii=");
                                        for (char c : name) {
                                            if (c == 0) break;
                                            std::fputc((c >= 0x20 && c < 0x7f) ? c : '.',
                                                       stderr);
                                        }
                                        std::fputc('\n', stderr);
                                    }
                                }
                            }
                            if (have_this_bytes) {
                                std::fprintf(stderr,
                                    "[seh-cb] this=0x%08x data=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                    static_cast<unsigned>(cpu.gpr.rcx),
                                    static_cast<unsigned>(this_bytes[0]),
                                    static_cast<unsigned>(this_bytes[1]),
                                    static_cast<unsigned>(this_bytes[2]),
                                    static_cast<unsigned>(this_bytes[3]),
                                    static_cast<unsigned>(this_bytes[4]),
                                    static_cast<unsigned>(this_bytes[5]),
                                    static_cast<unsigned>(this_bytes[6]),
                                    static_cast<unsigned>(this_bytes[7]),
                                    static_cast<unsigned>(this_bytes[8]),
                                    static_cast<unsigned>(this_bytes[9]),
                                    static_cast<unsigned>(this_bytes[10]),
                                    static_cast<unsigned>(this_bytes[11]),
                                    static_cast<unsigned>(this_bytes[12]),
                                    static_cast<unsigned>(this_bytes[13]),
                                    static_cast<unsigned>(this_bytes[14]),
                                    static_cast<unsigned>(this_bytes[15]));
                            } else {
                                std::fprintf(stderr,
                                    "[seh-cb] this=0x%08x <unmapped>\n",
                                    static_cast<unsigned>(cpu.gpr.rcx));
                            }
                        } else {
                            std::fprintf(stderr,
                                "[seh-cb] func=0x%llx exception=%d status=0x%08x at RIP=0x%llx addr=0x%llx depth=%d EAX=%08x ECX=%08x EDX=%08x EBX=%08x ESI=%08x EDI=%08x EBP=%08x ESP=%08x\n",
                                static_cast<unsigned long long>(func_addr),
                                static_cast<int>(cbctx.exception), status_code,
                                static_cast<unsigned long long>(cpu.rip),
                                static_cast<unsigned long long>(cbctx.exception_addr),
                                s_callback_depth,
                                static_cast<unsigned>(cpu.gpr.rax),
                                static_cast<unsigned>(cpu.gpr.rcx),
                                static_cast<unsigned>(cpu.gpr.rdx),
                                static_cast<unsigned>(cpu.gpr.rbx),
                                static_cast<unsigned>(cpu.gpr.rsi),
                                static_cast<unsigned>(cpu.gpr.rdi),
                                static_cast<unsigned>(cpu.gpr.rbp),
                                static_cast<unsigned>(cpu.gpr.rsp));
                            if (have_this_bytes) {
                                std::fprintf(stderr,
                                    "[seh-cb] this=0x%08x data=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                                    static_cast<unsigned>(cpu.gpr.rcx),
                                    static_cast<unsigned>(this_bytes[0]),
                                    static_cast<unsigned>(this_bytes[1]),
                                    static_cast<unsigned>(this_bytes[2]),
                                    static_cast<unsigned>(this_bytes[3]),
                                    static_cast<unsigned>(this_bytes[4]),
                                    static_cast<unsigned>(this_bytes[5]),
                                    static_cast<unsigned>(this_bytes[6]),
                                    static_cast<unsigned>(this_bytes[7]),
                                    static_cast<unsigned>(this_bytes[8]),
                                    static_cast<unsigned>(this_bytes[9]),
                                    static_cast<unsigned>(this_bytes[10]),
                                    static_cast<unsigned>(this_bytes[11]),
                                    static_cast<unsigned>(this_bytes[12]),
                                    static_cast<unsigned>(this_bytes[13]),
                                    static_cast<unsigned>(this_bytes[14]),
                                    static_cast<unsigned>(this_bytes[15]));
                            } else {
                                std::fprintf(stderr,
                                    "[seh-cb] this=0x%08x <unmapped>\n",
                                    static_cast<unsigned>(cpu.gpr.rcx));
                            }
                        }
                        } // VITREO_TRACE-gated diagnostics

                        u32 orig_esp = static_cast<u32>(cpu.gpr.rsp);
                        u32 scratch_esp = orig_esp - (cpu::CTX32_SIZE + cpu::EREC32_SIZE + 16);
                        scratch_esp &= ~0xF;
                        
                        u32 ctx_addr = scratch_esp;
                        u32 erec_addr = scratch_esp + cpu::CTX32_SIZE;
                        u32 disp_ctx_addr = erec_addr + cpu::EREC32_SIZE;
                        
                        cpu::write_exception_record_32(memory, erec_addr,
                            status_code, 0, static_cast<u32>(cpu.rip), num_params, params);
                        cpu::write_context_32(memory, ctx_addr, cpu);
                        (void)memory.write_u32(disp_ctx_addr, 0);
                        
                        u32 frame_addr = chain_head.value();
                        bool handled = false;
                        u32 chain_depth = 0;
                        
                        while (frame_addr != 0xFFFFFFFF && frame_addr != 0 && chain_depth < 256) {
                            if (!cpu::seh32_registration_dispatchable(
                                    memory, cpu, frame_addr)) {
                                std::fprintf(stderr,
                                    "[seh32/cb] registration 0x%08x is outside "
                                    "the thread stack (depth=%u) — chain "
                                    "corrupt, stopping dispatch\n",
                                    frame_addr, chain_depth);
                                break;
                            }
                            auto handler_info = cpu::read_seh_handler(memory, frame_addr);
                            if (handler_info.is_err()) break;
                            auto info = handler_info.value();

                            if (!seh_handler_looks_valid(memory, info.handler_addr)) {
                                std::fprintf(stderr,
                                    "[seh-cb] skipping bogus handler 0x%llx (frame=0x%08x depth=%u) — chain may be corrupt\n",
                                    static_cast<unsigned long long>(info.handler_addr), info.frame_addr, chain_depth);
                                frame_addr = info.next_frame;
                                ++chain_depth;
                                continue;
                            }

                            // Skip handlers already being dispatched (nested exception)
                            if (s_active_seh_handlers.count(info.handler_addr)) {
                                frame_addr = info.next_frame;
                                ++chain_depth;
                                continue;
                            }

                            // Per-dispatch tracing: the handler prologue read
                            // exists only to be printed, so it is gated too.
                            if (::vitreo::detail::g_trace_enabled) {
                            u8 handler_bytes[8] = {};
                            const bool have_handler_bytes =
                                memory.read_bytes(
                                    static_cast<GuestAddr>(info.handler_addr),
                                    handler_bytes, sizeof(handler_bytes)).is_ok();
                            std::fprintf(stderr,
                                "[seh-cb] dispatch handler=0x%08x frame=0x%08x "
                                "erec=0x%08x ctx=0x%08x info={%u,0x%08x} bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                                info.handler_addr, info.frame_addr,
                                erec_addr, ctx_addr,
                                params[0], params[1],
                                have_handler_bytes ? handler_bytes[0] : 0,
                                have_handler_bytes ? handler_bytes[1] : 0,
                                have_handler_bytes ? handler_bytes[2] : 0,
                                have_handler_bytes ? handler_bytes[3] : 0,
                                have_handler_bytes ? handler_bytes[4] : 0,
                                have_handler_bytes ? handler_bytes[5] : 0,
                                have_handler_bytes ? handler_bytes[6] : 0,
                                have_handler_bytes ? handler_bytes[7] : 0);
                            }

                            s_active_seh_handlers.insert(info.handler_addr);
                            auto disposition = invoke_seh32_frame_handler(
                                cpu, memory, thunk_registry, process_mgr,
                                info.handler_addr, info.frame_addr,
                                erec_addr, ctx_addr, disp_ctx_addr, scratch_esp);
                            s_active_seh_handlers.erase(info.handler_addr);

                            VITREO_TRACE(
                                "[seh-cb] handler=0x%08x disposition=%llu process_exited=%d\n",
                                info.handler_addr,
                                static_cast<unsigned long long>(disposition.disposition),
                                process_mgr.has_exited() ? 1 : 0);

                            // If the handler called RtlUnwind, it already set up the
                            // CPU state for the __except target — just mark handled.
                            if (apply_pending_unwind_32(cpu, memory, ctx_addr,
                                                        "seh32/cb-fault")) {
                                handled = true;
                                break;
                            }

                            if (disposition.disposition == cpu::ExceptionContinueExecution) {
                                if (!disposition.transferred) {
                                    (void)cli::resume_from_context_32(cpu, memory, thunk_registry, process_mgr,
                                                                 erec_addr, ctx_addr, scratch_esp);
                                }
                                handled = true;
                                break;
                            }
                            frame_addr = info.next_frame;
                            ++chain_depth;
                        }
                        
                        if (handled) {
                            continue;  // Resume callback execution
                        }
                        top_level_filter_ran = true;
                        if (invoke_unhandled_exception_filter_32(
                                cpu, memory, thunk_registry, process_mgr,
                                erec_addr, ctx_addr, scratch_esp)) {
                            continue;
                        }
                    }
                }

                // A start routine's exception that nothing took ends the
                // process (see unhandled_exit), after the top-level filter:
                // dispatch_x64_exception ran it on x64, the chain walk above
                // on x86 when the thread registered a frame.
                if (unhandled_exit.thread_entry &&
                    exec_result == cpu::ExecResult::Exception) {
                    const auto& cbctx = interp.context();
                    const u32 status_code =
                        cpu::cpu_exception_to_status(cbctx.exception);
                    if (!cpu.is_64bit() && !top_level_filter_ran) {
                        const u32 params[2] = {
                            cbctx.exception_error,
                            static_cast<u32>(cbctx.exception_addr),
                        };
                        const u32 num_params =
                            status_code == cpu::STATUS_ACCESS_VIOLATION
                                ? 2u : cbctx.exception_num_params;
                        if (thread_entry_filter_32(
                                status_code, 0, static_cast<u32>(cpu.rip),
                                num_params,
                                num_params != 0 ? params : nullptr)) {
                            continue;
                        }
                    }
                    unhandled_exit.arm(status_code);
                }

                // Unhandled — dump instruction trace and break
                hostcall_dump("callback crash");
                VITREO_TRACE( "[callback] Last %zu EIPs before crash:\n",
                             cb_trace_idx < CB_TRACE_SIZE ? cb_trace_idx : CB_TRACE_SIZE);
                {
                    usize start = cb_trace_idx >= CB_TRACE_SIZE ? cb_trace_idx - CB_TRACE_SIZE : 0;
                    for (usize i = start; i < cb_trace_idx; ++i) {
                        const u64 rip = cb_trace_rips[i % CB_TRACE_SIZE];
                        if (thunk_registry.is_in_thunk_region(rip)) {
                            const std::string name =
                                thunk_registry.resolve_name(static_cast<GuestAddr>(rip));
                            VITREO_TRACE( "  [%zu] 0x%08x [%s]\n",
                                         i, static_cast<unsigned>(rip), name.c_str());
                        } else {
                            VITREO_TRACE( "  [%zu] 0x%08x\n",
                                         i, static_cast<unsigned>(rip));
                        }
                    }
                }
                const auto& ctx = interp.context();
                VITREO_TRACE( "[callback] Exception type: %s (%u)\n",
                             exception_name(ctx.exception),
                             static_cast<unsigned>(ctx.exception));

                if (is_64bit) {
                    VITREO_TRACE(
                        "[callback] Register dump at exception (64-bit):\n"
                        "  RAX=%016llx  RCX=%016llx  RDX=%016llx  RBX=%016llx\n"
                        "  RSP=%016llx  RBP=%016llx  RSI=%016llx  RDI=%016llx\n"
                        "  R8 =%016llx  R9 =%016llx  R10=%016llx  R11=%016llx\n"
                        "  R12=%016llx  R13=%016llx  R14=%016llx  R15=%016llx\n"
                        "  RIP=%016llx\n",
                        (unsigned long long)cpu.gpr.rax, (unsigned long long)cpu.gpr.rcx,
                        (unsigned long long)cpu.gpr.rdx, (unsigned long long)cpu.gpr.rbx,
                        (unsigned long long)cpu.gpr.rsp, (unsigned long long)cpu.gpr.rbp,
                        (unsigned long long)cpu.gpr.rsi, (unsigned long long)cpu.gpr.rdi,
                        (unsigned long long)cpu.gpr.r8,  (unsigned long long)cpu.gpr.r9,
                        (unsigned long long)cpu.gpr.r10, (unsigned long long)cpu.gpr.r11,
                        (unsigned long long)cpu.gpr.r12, (unsigned long long)cpu.gpr.r13,
                        (unsigned long long)cpu.gpr.r14, (unsigned long long)cpu.gpr.r15,
                        (unsigned long long)cpu.rip);
                } else {
                    VITREO_TRACE(
                        "[callback] Register dump at exception:\n"
                        "  EAX=%08x  ECX=%08x  EDX=%08x  EBX=%08x\n"
                        "  ESP=%08x  EBP=%08x  ESI=%08x  EDI=%08x\n"
                        "  EIP=%08x\n",
                        static_cast<unsigned>(cpu.gpr.rax),
                        static_cast<unsigned>(cpu.gpr.rcx),
                        static_cast<unsigned>(cpu.gpr.rdx),
                        static_cast<unsigned>(cpu.gpr.rbx),
                        static_cast<unsigned>(cpu.gpr.rsp),
                        static_cast<unsigned>(cpu.gpr.rbp),
                        static_cast<unsigned>(cpu.gpr.rsi),
                        static_cast<unsigned>(cpu.gpr.rdi),
                        static_cast<unsigned>(cpu.rip));
                }

                u8 bytes[16] = {};
                auto rd = memory.read_bytes(cpu.rip, bytes, sizeof(bytes));
                if (rd.is_ok()) {
                    VITREO_TRACE( "[callback] Bytes at RIP: ");
                    for (u8 b : bytes) {
                        VITREO_TRACE( "%02x ", static_cast<unsigned>(b));
                    }
                    VITREO_TRACE( "\n");
                } else {
                    VITREO_TRACE( "[callback] Bytes at RIP: <unmapped>\n");
                }

                // If the handler crashed, signal ContinueSearch (1) so the
                // dispatch loop tries the next frame rather than treating
                // it as ContinueExecution.
                result = 1;
            }
            break;
        }
    }

callback_restore:
    // Learn _except_handler3: a selected __except continuation does not
    // return to the dispatcher. Its stack can overlap our former CONTEXT,
    // so neither serialize there nor restore the callback-entry registers.
    if (seh_abandon_ctx_addr != 0 && t_seh32_handler_left_dispatcher) {
        return result;
    }
    if (completed_normally != nullptr && !*completed_normally) {
        vitreo_pctrace_dump_ring("guest-callback-abnormal-termination");
        std::fprintf(stderr,
            "[callback] abnormal termination: func=0x%llx rip=0x%llx "
            "rsp=0x%llx process_exited=%d exit_code=0x%08x "
            "pending_unwind=%d depth=%d\n",
            static_cast<unsigned long long>(func_addr),
            static_cast<unsigned long long>(cpu.rip),
            static_cast<unsigned long long>(cpu.sp()),
            process_mgr.has_exited() ? 1 : 0,
            process_mgr.exit_code(),
            cpu.pending_unwind.active ? 1 : 0,
            s_callback_depth);
    }

    if (cpu.pending_unwind.active) {
        // RtlUnwind owns rip/rsp/rbp/rax via apply_pending_unwind_32, but the
        // frame handler must not permanently clobber the caller's nonvolatiles.
        // Restore them to callback-entry values (exception-time for SEH).
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RBX), saved_rbx);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RSI), saved_rsi);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RDI), saved_rdi);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R12), saved_r12);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R13), saved_r13);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R14), saved_r14);
        cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R15), saved_r15);
        if (is_64bit) {
            // x64: the unwind left this callback level at its sentinel
            // (winapi/kernel32/thread/unwind64.hpp). The host code that made
            // the call is back at its own call state -- its thunk's entry --
            // which is where the enclosing level goes on with the unwind, or
            // the exception dispatcher picks it up. Those frames have not been
            // walked yet.
            cpu.rip = saved_rip;
            cpu.set_sp(saved_rsp);
            cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RBP), saved_rbp);
            cpu.pending_unwind.level_walked = false;
        }
        return result;
    }

    // Restore the complete CPU state (all GPRs), including callee-saved
    // registers that the nested callback may have modified.
    cpu.rip = saved_rip;
    cpu.set_sp(saved_rsp);
    cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RAX), saved_rax);
    cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RCX), saved_rcx);
    cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RDX), saved_rdx);
    cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RBX), saved_rbx);
    cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RBP), saved_rbp);
    cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RSI), saved_rsi);
    cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::RDI), saved_rdi);
    cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R8),  saved_r8);
    cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R9),  saved_r9);
    cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R10), saved_r10);
    cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R11), saved_r11);
    cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R12), saved_r12);
    cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R13), saved_r13);
    cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R14), saved_r14);
    cpu.gpr.set64(static_cast<u8>(cpu::RegIndex::R15), saved_r15);
    cpu.flags = saved_flags;
    cpu.flag_op = saved_flag_op;
    cpu.flag_src = saved_flag_src;
    cpu.flag_dst = saved_flag_dst;
    cpu.sse = saved_sse;
    cpu.avx = saved_avx;

    return result;
}

} // namespace vitreo::cli
