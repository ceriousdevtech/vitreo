// SPDX-License-Identifier: LGPL-2.1-only

/// @file exception_dispatch32.cpp
/// @brief 32-bit SEH and C++ exception handling. See exception_dispatch32.hpp.
///
/// Split out of cli/commands/execution_loop.cpp. Everything declared in the
/// header is defined at namespace scope — an anonymous namespace would give it
/// internal linkage regardless of any `static`, and the loop calls all five.

#include "runtime/execution/exception_dispatch32.hpp"

#include "runtime/execution/guest_run_loop.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_block.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/system/time.hpp"
#include "winapi/media/winmm/winmm.hpp"
#include "graphics/common/perf_diag.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/memory/mem_watch.hpp"
#include "core/cpu/seh/seh32.hpp"
#include "winapi/kernel32/process/vectored_exception.hpp"
#include "winapi/msvcrt/runtime/cxx_eh.hpp"
#include <vitreo/diagnostics.hpp>
#include "runtime/diagnostics/pc_trace.hpp"
#include "runtime/diagnostics/hostcall_ring.hpp"
#include "runtime/execution/exception_dispatch.hpp"
#include "runtime/execution/block_decoder.hpp"
#include <atomic>
#include <array>
#include <cstdio>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <vector>
#include <mach/mach_time.h>
#include <csignal>

namespace vitreo::cli {

[[nodiscard]] bool seh_handler_looks_valid(GuestMemory& memory, u64 handler_addr) {
    // Anything inside the first 64 KiB is the NULL-page guard or trap-style
    // sentinel values (0, 1, 0xa, 0xff, ...); never a real DLL entry point.
    if (handler_addr < 0x10000ULL) return false;
    // Probe a single byte at the handler's first instruction; if the page
    // is not mapped/readable, we cannot run code there.
    auto info = memory.probe_access(static_cast<GuestAddr>(handler_addr), 1);
    if (!info.host_ptr || !info.readable) return false;
    return true;
}

/// Invoke a 32-bit frame SEH handler under a nested FS:[0] registration, matching
/// ntdll!RtlpExecuteHandlerForException. Packers that recover via
/// `mov ebx, fs:[0]; mov ebx, [ebx]` require Nested.Next == establisher frame.
[[nodiscard]] Seh32FrameHandlerResult invoke_seh32_frame_handler(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    winapi::ThunkRegistry& thunk_registry,
    winapi::ProcessManager& process_mgr,
    u32 handler_addr,
    u32 establisher_frame,
    u32 erec_addr,
    u32 ctx_addr,
    u32 disp_ctx_addr,
    u32 scratch_esp) {
    u64 args[4] = {
        static_cast<u64>(erec_addr),
        static_cast<u64>(establisher_frame),
        static_cast<u64>(ctx_addr),
        static_cast<u64>(disp_ctx_addr),
    };

    cpu::Seh32NestedExecution nested{};
    u32 handler_esp = scratch_esp;

    if (cpu::ensure_seh32_exception_protector(memory).is_ok()) {
        // Nested record lives just below CONTEXT/EREC scratch so the handler
        // stack (pushed by run_guest_callback) cannot overwrite it, and so
        // FS:[0] remains a stack-resident registration as Windows requires.
        const u32 nested_addr =
            (scratch_esp - cpu::SEH32_NESTED_RECORD_SIZE) & ~0x3u;
        if (cpu::install_seh32_nested_execution(
                memory, cpu, nested_addr, establisher_frame, nested)
                .is_ok()) {
            handler_esp = nested_addr;
            VITREO_TRACE(
                "[seh32] nested FS:[0]=0x%08x Next=0x%08x establisher=0x%08x "
                "protector=0x%08x\n",
                nested_addr, nested.previous_head, establisher_frame,
                cpu::seh32_exception_protector_addr());
        }
    }

    cpu.gpr.rsp = handler_esp;
    (void)consume_seh32_handler_left_dispatcher();  // no stale flag
    // Pass CONTEXT + establisher so a handler that abandons the dispatcher
    // stack (Petite: wipe stub → jmp OEP) becomes ContinueExecution with the
    // continuation register state, matching Windows (no ZwContinue).
    const u64 disposition = run_guest_callback(
        cpu, memory, thunk_registry, process_mgr,
        handler_addr, args, 4, false, nullptr,
        GuestCallbackKind::Nested, ctx_addr, establisher_frame);

    // A handler that left (a C++ catch continuation, an __except block, a
    // packer's jump to its OEP) never returns to RtlpExecuteHandlerForException,
    // so the epilogue that would pop this record never runs on Windows. By
    // then RtlUnwind has already unlinked it, and FS:[0] names the target
    // frame's own registration -- popping "one more" removed a live frame's
    // handler, so the next throw in that function found no catch.
    const bool left_dispatcher = consume_seh32_handler_left_dispatcher();
    if (nested.installed && !left_dispatcher) {
        (void)cpu::unlink_seh32_nested_execution(memory, cpu, nested);
    }
    return {disposition, left_dispatcher};
}

[[nodiscard]] bool apply_pending_unwind_32(cpu::CpuState& cpu,
                                           GuestMemory& memory,
                                           u32 ctx_addr,
                                           const char* trace_prefix) {
    if (!cpu.pending_unwind.active) return false;

    const u64 target_ip = cpu.pending_unwind.target_ip;
    const u64 target_frame = cpu.pending_unwind.target_frame;
    const u64 return_value = cpu.pending_unwind.return_value;
    const u64 resume_sp = cpu.pending_unwind.resume_sp;
    const u64 resume_bp = cpu.pending_unwind.resume_bp;
    cpu.pending_unwind.clear();

    // Match the x64 unwind path: reload the exception-time CONTEXT so
    // nonvolatiles (EBX/ESI/EDI) survive the frame handler, then overlay
    // RtlUnwind's continuation (TargetIp / adjusted ESP / EBP / EAX).
    // Leaving the handler's clobbered EDI behind breaks MSVC stack-watermark
    // epilogues (`cmp esp, edi` / `mov esp, edi`) after __except resumes.
    if (ctx_addr != 0) {
        (void)cpu::read_context_32(memory, ctx_addr, cpu);
    }

    VITREO_TRACE("[%s] RtlUnwind -> ip=0x%x frame=0x%x resume_sp=0x%x edi=0x%x\n",
                 trace_prefix, static_cast<unsigned>(target_ip),
                 static_cast<unsigned>(target_frame),
                 static_cast<unsigned>(resume_sp),
                 static_cast<unsigned>(cpu.gpr.rdi));
    if (target_ip != 0) cpu.rip = target_ip;
    if (resume_sp != 0) {
        cpu.set_sp(resume_sp);
        cpu.gpr.rbp = resume_bp;
    }
    cpu.gpr.rax = return_value;
    return true;
}

// -----------------------------------------------------------------------------
// MSVC C++ EH per-function __ehhandler thunk detection
// -----------------------------------------------------------------------------
// MSVC emits a per-function thunk that loads the FuncInfo pointer into EAX
// and tail-jumps to __CxxFrameHandler3:
//
//   __ehhandler$_F:
//     B8  FI FI FI FI    mov eax, offset FuncInfo$_F
//     E9  rr rr rr rr    jmp __CxxFrameHandler3  (rel32)
//
// We don't care WHERE the jmp goes — once we see this 10-byte signature at the
// handler address, we know the rest is the MSVC C++ EH personality routine.
// We can then dispatch the exception via our own cxx_eh code, which works
// regardless of whether __CxxFrameHandler3 is statically linked into the
// binary (the common case for an MSVC installer) or imported
// from msvcrt.dll.
//
// Returns the FuncInfo address on a thunk hit, or 0 if the handler isn't a
// recognised __ehhandler thunk.
//
// Two emission shapes are accepted:
//
//  1. Bare thunk (no /GS cookie):
//        B8 FI FI FI FI    mov eax, FuncInfo
//        E9 rr rr rr rr    jmp __CxxFrameHandler3
//
//  2. /GS-protected thunk (MSVC adds a __security_check_cookie call before
//     the `mov eax, FI; jmp` pair).  The cookie sequence is typically 16-20
//     bytes long and starts with `90 90 8b 54 24 08 8d 42 0c …`.  We scan
//     up to 32 bytes for the `B8 xx xx xx xx E9` signature, validating
//     that the dword immediate points at a real FuncInfo (magic check).
[[nodiscard]] u32 detect_cxx_ehhandler_funcinfo(GuestMemory& memory, u64 handler_addr) {
    // Scan up to 64 bytes — some /GS thunks have a doubled cookie check
    // (two `mov ecx,[edx-N]; xor ecx,eax; call __security_check_cookie`
    // sequences) that pushes the FuncInfo load to offset ~32.
    constexpr int SCAN_BYTES = 64;
    u8 bytes[SCAN_BYTES] = {};
    for (int i = 0; i < SCAN_BYTES; ++i) {
        auto r = memory.read_u8(static_cast<GuestAddr>(handler_addr + i));
        if (r.is_err()) return 0;
        bytes[i] = r.value();
    }
    for (int off = 0; off + 10 <= SCAN_BYTES; ++off) {
        if (bytes[off] != 0xB8) continue;
        if (bytes[off + 5] != 0xE9) continue;
        u32 fi = static_cast<u32>(bytes[off + 1])
               | (static_cast<u32>(bytes[off + 2]) << 8)
               | (static_cast<u32>(bytes[off + 3]) << 16)
               | (static_cast<u32>(bytes[off + 4]) << 24);
        if (fi < 0x10000u) continue;
        auto magic_r = memory.read_u32(static_cast<GuestAddr>(fi));
        if (magic_r.is_err()) continue;
        u32 m = magic_r.value();
        if (m == 0x19930520u || m == 0x19930521u || m == 0x19930522u) {
            return fi;
        }
    }
    return 0;
}

bool cxx_ehhandler_uses_host_personality(GuestMemory& memory,
                                         const winapi::ThunkRegistry& thunk_registry,
                                         u64 handler_addr) {
    // Same scan as detect_cxx_ehhandler_funcinfo: find `mov eax, FuncInfo;
    // jmp rel32` and take the jump.
    constexpr int SCAN_BYTES = 64;
    u8 bytes[SCAN_BYTES] = {};
    if (memory.read_bytes(static_cast<GuestAddr>(handler_addr), bytes,
                          SCAN_BYTES).is_err()) {
        return false;
    }
    u64 target = 0;
    for (int off = 0; off + 10 <= SCAN_BYTES; ++off) {
        if (bytes[off] != 0xB8 || bytes[off + 5] != 0xE9) continue;
        u32 rel = 0;
        std::memcpy(&rel, bytes + off + 6, 4);
        target = static_cast<u32>(handler_addr + static_cast<u64>(static_cast<u32>(off)) + 10 +
                                  static_cast<u64>(static_cast<i32>(rel)));
        break;
    }
    // Follow import slots (jmp [abs32]) and short veneers until the jump
    // chain ends.
    for (int hop = 0; target != 0 && hop < 4; ++hop) {
        if (thunk_registry.is_in_thunk_region(static_cast<GuestAddr>(target))) {
            return true;
        }
        u8 op[7] = {};
        if (memory.read_bytes(static_cast<GuestAddr>(target), op, 7).is_err()) {
            return false;
        }
        if (op[0] == 0xB8 && op[5] == 0xFF && op[6] == 0xE0) {
            // Synthetic-module EAT veneer: mov eax, thunk; jmp eax.
            u32 imm = 0;
            std::memcpy(&imm, op + 1, 4);
            target = imm;
        } else if (op[0] == 0xFF && op[1] == 0x25) {
            u32 slot = 0;
            std::memcpy(&slot, op + 2, 4);
            target = memory.read_u32(slot).value_or(0);
        } else if (op[0] == 0xE9) {
            u32 rel = 0;
            std::memcpy(&rel, op + 1, 4);
            target = static_cast<u32>(target + 5 +
                                      static_cast<u64>(static_cast<i32>(rel)));
        } else {
            return false;
        }
    }
    return false;
}

// Nesting depth of enter_cxx_catch_32 on this thread. A `throw;` from inside
// a catch funclet is dispatched while depth >= 1; when an *outer* catch then
// succeeds at depth > 1, the enclosing catch's run_guest_callback must be
// abandoned rather than continuing at the outer continuation (see
// t_cxx_catch_transfer).
thread_local int t_enter_cxx_catch_depth = 0;

// Set when a nested enter_cxx_catch_32 installs an outer catch's continuation
// while an inner catch funclet callback is still on the stack. The abandoned
// callback must exit without restoring its saved CPU state, and each enclosing
// enter_cxx_catch_32 must return success without re-patching its own frame.
thread_local bool t_cxx_catch_transfer = false;

[[nodiscard]] bool enter_cxx_catch_32(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    winapi::ThunkRegistry& thunk_registry,
    winapi::ProcessManager& process_mgr,
    const winapi::CxxDispatchResult& result,
    u32 frame_addr,
    u32 context_addr,
    u32 exception_record_addr,
    const char* trace_prefix)
{
    ++t_enter_cxx_catch_depth;
    struct CatchDepthGuard {
        ~CatchDepthGuard() { --t_enter_cxx_catch_depth; }
    } catch_depth_guard;

    if (!winapi::install_catch_param_32(memory, result, frame_addr)) {
        VITREO_TRACE("[%s] failed to install catch parameter\n", trace_prefix);
        return false;
    }

    // Selecting an outer catch unwinds every registration record below its
    // establisher frame. Leaving FS:[0] at the discarded inner record lets a
    // later EH prologue reuse that stack address and link the record to itself,
    // causing the dispatcher to revisit one handler until its depth limit.
    if (cpu::write_seh_chain_head(memory, cpu, frame_addr).is_err()) {
        VITREO_TRACE("[%s] failed to unwind SEH chain to frame=0x%08x\n",
                     trace_prefix, frame_addr);
        return false;
    }

    // ESP as captured when the exception was raised — the deepest live point
    // of the guest stack, and therefore below the exception object. Read it
    // before patch_context_for_catch_32 overwrites the field with the catch
    // function's resume stack.
    const u32 throw_site_sp =
        memory.read_u32(context_addr + cpu::CTX32_ESP).value_or(0);

    // A HandlerType points to a catch funclet, not directly to the code after
    // the catch. MSVC calls that funclet with the parent EBP; its EAX return is
    // the continuation address. Jumping to the funclet makes its RET consume
    // arbitrary throw-stack data as an instruction pointer.
    winapi::patch_context_for_catch_32(memory, result, frame_addr,
                                       context_addr);
    const u32 parent_ebp = frame_addr + 12;
    const auto callback_stack_top =
        winapi::catch_funclet_callback_stack_top_32(memory, frame_addr);
    if (!callback_stack_top.has_value()) {
        VITREO_TRACE(
            "[%s] invalid catch funclet stack for frame=0x%08x\n",
            trace_prefix, frame_addr);
        return false;
    }

    // Two stack levels matter, and a catch funclet may end at either one:
    //
    //   epilogue_sp — where MSVC's shared epilogue puts ESP via
    //                 `mov esp, [ebp-0x10]` before its RET, and the same dword
    //                 that epilogue later `pop edi`s;
    //   entry_sp    — where the funclet actually starts, which must be BELOW
    //                 the exception object so the catch body's own callees
    //                 cannot reuse the object's memory (see
    //                 catch_funclet_entry_stack_32).
    //
    // run_guest_callback plants its return sentinel at entry_sp-4, which
    // covers a funclet that ends in a plain RET. Mirror the sentinel at
    // epilogue_sp so the `mov esp, [ebp-0x10]` shape returns to it too, then
    // restore the original dword so the continuation's epilogue recovers the
    // caller's nonvolatiles.
    const u32 epilogue_sp = callback_stack_top.value() - sizeof(u32);
    u32 preserved_epilogue_slot = 0;
    const auto preserved_read = memory.read_u32(epilogue_sp);
    const bool have_preserved_slot = preserved_read.is_ok();
    if (have_preserved_slot) {
        preserved_epilogue_slot = preserved_read.value();
    }
    auto restore_epilogue_slot = [&]() {
        if (have_preserved_slot) {
            (void)memory.write_u32(epilogue_sp, preserved_epilogue_slot);
        }
    };

    const auto deep_entry = winapi::catch_funclet_entry_stack_32(
        memory, frame_addr, throw_site_sp);
    const u32 entry_sp = deep_entry.value_or(callback_stack_top.value());
    if (entry_sp != callback_stack_top.value()) {
        (void)memory.write_u32(
            epilogue_sp,
            static_cast<u32>(CALLBACK_RETURN_SENTINEL & 0xFFFFFFFFull));
    }

    // Whether the funclet actually got the deep stack is the difference
    // between a catch that works and one that eats its own exception object,
    // and it is invisible from the outside — report it with the EH diagnostics.
    static const bool s_catch_stack_diag =
        std::getenv("VITREO_CXX_EH_DIAG") != nullptr;
    if (s_catch_stack_diag) {
        std::fprintf(stderr,
            "[cxx-diag] catch funclet=0x%08x entry_sp=0x%08x epilogue_sp=0x%08x "
            "throw_site_sp=0x%08x %s\n",
            result.handler_addr, entry_sp, epilogue_sp, throw_site_sp,
            deep_entry.has_value() ? "deep" : "fallback");
        std::fflush(stderr);
    }

    cpu.gpr.rbp = parent_ebp;
    cpu.set_sp(entry_sp);

    // The funclet body is the catch block, so a `throw;` inside it raises
    // while this is on the stack. Record the exception being handled (and
    // snapshot its object) for the duration.
    winapi::note_cxx_exception_caught();
    winapi::push_active_cxx_exception(memory, exception_record_addr);
    bool completed = false;
    const u64 continuation = run_guest_callback(
        cpu, memory, thunk_registry, process_mgr, result.handler_addr,
        nullptr, 0, false, &completed);
    winapi::pop_active_cxx_exception();
    restore_epilogue_slot();

    // A catch body may end the process rather than resume after the try — a
    // top-level handler that reports an unrecoverable error and calls
    // ExitProcess is the common shape. The funclet then never returns a
    // continuation, so demanding one would misread a delivered exception as a
    // failed catch. Report success and let the run loop act on the exit.
    if (process_mgr.has_exited()) {
        VITREO_TRACE(
            "[%s] catch funclet 0x%08x exited the process (code=0x%08x)\n",
            trace_prefix, result.handler_addr, process_mgr.exit_code());
        return true;
    }

    // An outer catch was entered from inside this funclet (typically via
    // `throw;`). That nested enter_cxx_catch_32 already configured CPU for
    // its continuation and asked every enclosing catch callback to abandon.
    // Do not re-patch *this* frame — that would clobber the outer catch.
    if (t_cxx_catch_transfer) {
        VITREO_TRACE(
            "[%s] catch funclet 0x%08x abandoned after outer-catch transfer "
            "(depth=%d rip=0x%08x esp=0x%08x)\n",
            trace_prefix, result.handler_addr, t_enter_cxx_catch_depth,
            static_cast<unsigned>(cpu.rip),
            static_cast<unsigned>(cpu.gpr.rsp));
        // Clear only when returning to non-catch SEH (WndProc / main loop).
        // Intermediate abandoned catches keep the flag so their callbacks
        // also exit without restoring saved state.
        if (t_enter_cxx_catch_depth == 1) {
            t_cxx_catch_transfer = false;
        }
        return true;
    }

    const auto continuation_info = memory.probe_access(
        static_cast<GuestAddr>(continuation), 1);
    if (!completed || continuation < 0x10000u ||
        !continuation_info.host_ptr || !continuation_info.executable) {
        VITREO_TRACE(
            "[%s] catch funclet 0x%08x failed: completed=%d continuation=0x%08llx\n",
            trace_prefix, result.handler_addr, completed,
            static_cast<unsigned long long>(continuation));
        return false;
    }

    winapi::patch_context_for_catch_32(
        memory, result, frame_addr, context_addr,
        static_cast<u32>(continuation));
    if (cpu::read_context_32(memory, context_addr, cpu).is_err()) {
        VITREO_TRACE("[%s] failed to restore catch continuation context\n",
                     trace_prefix);
        return false;
    }

    // Nested inside another catch funclet (rethrow → outer catch). The
    // enclosing callback must not keep executing: leave the transfer flag set
    // so it returns without restoring its saved registers.
    if (t_enter_cxx_catch_depth > 1) {
        t_cxx_catch_transfer = true;
        VITREO_TRACE(
            "[%s] catch funclet=0x%08x continuation=0x%08llx ebp=0x%08x "
            "esp=0x%08x (nested — transferring out of enclosing catch)\n",
            trace_prefix, result.handler_addr,
            static_cast<unsigned long long>(continuation),
            static_cast<unsigned>(cpu.gpr.rbp),
            static_cast<unsigned>(cpu.gpr.rsp));
    } else {
        VITREO_TRACE(
            "[%s] catch funclet=0x%08x continuation=0x%08llx ebp=0x%08x "
            "esp=0x%08x\n",
            trace_prefix, result.handler_addr,
            static_cast<unsigned long long>(continuation),
            static_cast<unsigned>(cpu.gpr.rbp),
            static_cast<unsigned>(cpu.gpr.rsp));
    }
    return true;
}

bool cxx_catch_transfer_active() noexcept { return t_cxx_catch_transfer; }

bool resume_from_context_32(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    winapi::ThunkRegistry& thunk_registry,
    winapi::ProcessManager& process_mgr,
    u32 erec_addr,
    u32 ctx_addr,
    u32 scratch_esp) {
    const auto handlers = winapi::snapshot_vectored_continue_handlers();
    if (!handlers.empty()) {
        // EXCEPTION_POINTERS { ExceptionRecord, ContextRecord } — two dwords.
        const u32 ep_addr = (scratch_esp - 8u) & ~0xFu;
        (void)memory.write_u32(ep_addr, erec_addr);
        (void)memory.write_u32(ep_addr + 4, ctx_addr);
        for (u64 handler : handlers) {
            u64 args[1] = {static_cast<u64>(ep_addr)};
            cpu.gpr.rsp = ep_addr;
            const u64 disposition = run_guest_callback(
                cpu, memory, thunk_registry, process_mgr, handler, args, 1, false);
            if (process_mgr.has_exited()) return false;
            if (cpu.pending_raise.active && cpu.pending_raise.from_helper) return false;
            if (static_cast<u32>(disposition) == 0xFFFFFFFFu) break;
        }
    }
    (void)cpu::read_context_32(memory, ctx_addr, cpu);
    return true;
}

bool invoke_vectored_handlers_32(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    winapi::ThunkRegistry& thunk_registry,
    winapi::ProcessManager& process_mgr,
    u32 raise_code, u32 raise_flags, u32 raise_nargs, const u32* raise_args,
    u32 exception_address) {
    const auto handlers = winapi::snapshot_vectored_exception_handlers();
    if (handlers.empty()) {
        return false;   // the overwhelmingly common case — cost nothing
    }

    // A fault *inside* a vectored handler must not re-enter the list, or a
    // handler that touches bad memory recurses until the stack is gone.
    static thread_local int reentry = 0;
    if (reentry != 0) {
        return false;
    }
    ++reentry;

    const u32 orig_esp = static_cast<u32>(cpu.gpr.rsp);
    u32 scratch = orig_esp - (cpu::CTX32_SIZE + cpu::EREC32_SIZE + 32u);
    scratch &= ~0xFu;
    const u32 ctx_addr  = scratch;
    const u32 erec_addr = scratch + cpu::CTX32_SIZE;
    const u32 ep_addr   = erec_addr + cpu::EREC32_SIZE;

    (void)cpu::write_exception_record_32(memory, erec_addr, raise_code,
                                         raise_flags, exception_address,
                                         raise_nargs, raise_args);
    (void)cpu::write_context_32(memory, ctx_addr, cpu);
    // EXCEPTION_POINTERS { ExceptionRecord, ContextRecord } — two dwords.
    (void)memory.write_u32(ep_addr, erec_addr);
    (void)memory.write_u32(ep_addr + 4, ctx_addr);

    bool resume = false;
    for (u64 handler : handlers) {
        if (!seh_handler_looks_valid(memory, handler)) {
            continue;
        }
        u64 args[1] = {static_cast<u64>(ep_addr)};
        // Keep the handler's own frame clear of the records it is reading.
        cpu.gpr.rsp = ep_addr;
        const u64 disposition = run_guest_callback(
            cpu, memory, thunk_registry, process_mgr, handler, args, 1, false);

        VITREO_TRACE("[seh32] VEH handler=0x%llx code=0x%08x -> 0x%llx\n",
                     static_cast<unsigned long long>(handler), raise_code,
                     static_cast<unsigned long long>(disposition));

        if (process_mgr.has_exited()) {
            --reentry;
            return true;   // nothing left to dispatch into
        }
        // A handler that itself faulted leaves a fresh raise pending; let the
        // caller dispatch that one instead of continuing this walk.
        if (cpu.pending_raise.active && cpu.pending_raise.from_helper) {
            --reentry;
            return true;
        }
        if (static_cast<u32>(disposition) == 0xFFFFFFFFu) {  // CONTINUE_EXECUTION
            resume = true;
            break;
        }
    }

    // Either way the CONTEXT is authoritative: on resume it carries whatever
    // the handler (and the continue handlers after it) changed, and on a
    // declined search it restores the faulting state so the chain walk does
    // not inherit dispatcher scratch registers.
    if (resume) {
        if (!resume_from_context_32(cpu, memory, thunk_registry, process_mgr,
                                    erec_addr, ctx_addr, ctx_addr)) {
            --reentry;
            return true;
        }
    } else {
        (void)cpu::read_context_32(memory, ctx_addr, cpu);
        cpu.gpr.rsp = orig_esp;
    }
    --reentry;
    return resume;
}

bool invoke_unhandled_exception_filter_32(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    winapi::ThunkRegistry& thunk_registry,
    winapi::ProcessManager& process_mgr,
    u32 erec_addr,
    u32 ctx_addr,
    u32 scratch_esp) {
    const u64 filter = winapi::unhandled_exception_filter_guest();
    if (filter == 0 || !seh_handler_looks_valid(memory, filter)) {
        return false;
    }

    static thread_local int reentry = 0;
    if (reentry != 0) {
        return false;
    }
    ++reentry;

    // This path runs at most once per fatal exception, so log the record
    // unconditionally: by the time the guest's own filter has been reached the
    // process is going to die, and without the code and faulting address the
    // only evidence left is the terminator's return address, which identifies
    // the crash reporter rather than the crash.
    // EXCEPTION_RECORD: ExceptionCode(0) Flags(4) Record(8) Address(12)
    //                   NumberParameters(16) ExceptionInformation[](20)
    {
        const auto code  = memory.read_u32(erec_addr);
        const auto flags = memory.read_u32(erec_addr + 4);
        const auto addr  = memory.read_u32(erec_addr + 12);
        const auto nparm = memory.read_u32(erec_addr + 16);
        const auto info0 = memory.read_u32(erec_addr + 20);
        const auto info1 = memory.read_u32(erec_addr + 24);
        std::fprintf(stderr,
            "[seh32] UNHANDLED code=0x%08x flags=0x%x at=0x%08x nparm=%u "
            "info0=0x%08x info1=0x%08x eip=0x%08x esp=0x%08x\n",
            code.is_ok() ? code.value() : 0u,
            flags.is_ok() ? flags.value() : 0u,
            addr.is_ok() ? addr.value() : 0u,
            nparm.is_ok() ? nparm.value() : 0u,
            info0.is_ok() ? info0.value() : 0u,
            info1.is_ok() ? info1.value() : 0u,
            static_cast<unsigned>(cpu.rip),
            static_cast<unsigned>(cpu.gpr.rsp));

        // The integer registers from the CONTEXT the filter is about to see.
        // A fault address alone cannot distinguish "the base pointer was null"
        // from "the index ran away", and both shapes reach the same
        // instruction. CONTEXT (x86): Edi+0x9C Esi+0xA0 Ebx+0xA4 Edx+0xA8
        // Ecx+0xAC Eax+0xB0 Ebp+0xB4 Eip+0xB8.
        auto reg = [&](u32 off) -> u32 {
            auto r = memory.read_u32(ctx_addr + off);
            return r.is_ok() ? r.value() : 0u;
        };
        const u32 edi = reg(0x9C), esi = reg(0xA0), ebx = reg(0xA4);
        const u32 edx = reg(0xA8), ecx = reg(0xAC), eax = reg(0xB0);
        std::fprintf(stderr,
            "[seh32] UNHANDLED regs eax=0x%08x ecx=0x%08x edx=0x%08x "
            "ebx=0x%08x esi=0x%08x edi=0x%08x ebp=0x%08x\n",
            eax, ecx, edx, ebx, esi, edi, reg(0xB4));
    }
    {
        // Deref the two operands the faulting instruction shape depends on,
        // so the log says which link of the chain is broken rather than
        // leaving it to be re-derived from a later run.
        auto peek = [&](const char* what, u32 addr) {
            auto v = memory.read_u32(addr);
            std::fprintf(stderr, "[seh32] UNHANDLED  %s [0x%08x] = %s0x%08x\n",
                         what, addr, v.is_ok() ? "" : "<unreadable> ",
                         v.is_ok() ? v.value() : 0u);
        };
        auto reg = [&](u32 off) -> u32 {
            auto r = memory.read_u32(ctx_addr + off);
            return r.is_ok() ? r.value() : 0u;
        };
        const u32 edi = reg(0x9C), esi = reg(0xA0);
        if (edi != 0) peek("edi+0xC4", edi + 0xC4);
        if (esi != 0) peek("esi+0x1C", esi + 0x1C);

        // A jump into data leaves no clue at the fault site itself; the stack
        // above ESP still holds the return address of whoever transferred
        // there. Dump a short window so the caller is identifiable.
        const u32 esp = reg(0xC4);   // CONTEXT.Esp
        if (esp != 0) {
            for (u32 i = 0; i < 12; ++i) {
                auto v = memory.read_u32(esp + i * 4);
                std::fprintf(stderr,
                    "[seh32] UNHANDLED  [esp+0x%02x] = %s0x%08x\n",
                    i * 4, v.is_ok() ? "" : "<unreadable> ",
                    v.is_ok() ? v.value() : 0u);
            }
        }
    }

    // EXCEPTION_POINTERS { ExceptionRecord, ContextRecord } — two dwords on x86.
    const u32 ep_addr = (scratch_esp - 8u) & ~3u;
    (void)memory.write_u32(ep_addr, erec_addr);
    (void)memory.write_u32(ep_addr + 4, ctx_addr);

    u64 args[1] = {static_cast<u64>(ep_addr)};
    cpu.gpr.rsp = ep_addr;
    const u64 ret = run_guest_callback(
        cpu, memory, thunk_registry, process_mgr,
        filter, args, 1, false);

    --reentry;

    std::fprintf(stderr,
        "[seh32] UnhandledExceptionFilter handler=0x%llx ret=0x%llx\n",
        static_cast<unsigned long long>(filter),
        static_cast<unsigned long long>(ret));

    // Restore the faulting CONTEXT first so a declined filter does not leave
    // dispatcher scratch registers in the crash dump / terminator.
    if (static_cast<u32>(ret) == 0xFFFFFFFFu) {
        (void)resume_from_context_32(cpu, memory, thunk_registry, process_mgr,
                                     erec_addr, ctx_addr, ep_addr);
        return true;
    }
    (void)cpu::read_context_32(memory, ctx_addr, cpu);
    return false;
}

}  // namespace vitreo::cli
