// SPDX-License-Identifier: LGPL-2.1-only

/// @file exception_dispatch.cpp
/// @brief x64 table-based SEH dispatch.
///
/// See exception_dispatch.hpp. Split out of cli/commands/execution_loop.cpp.
///
/// Everything except dispatch_x64_exception stays file-local: the UNWIND_INFO
/// parser, the CONTEXT readers and writers, and the record builders are steps
/// of that one algorithm, not a general-purpose API.

#include "runtime/execution/exception_dispatch.hpp"
#include "core/cpu/state/context64_record.hpp"

// The one edge from runtime/ back into cli/: dispatch calls run_guest_callback
// to enter a language handler. That is not a layering mistake so much as a
// symptom — run_guest_callback is execution logic that still lives in
// cli/commands/execution_loop.cpp, and this include goes away when it moves to
// runtime/execution alongside the loop itself.
#include "runtime/execution/guest_run_loop.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_block.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/process/vectored_exception.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/system/time.hpp"
#include "winapi/media/winmm/winmm.hpp"
#include "graphics/common/perf_diag.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/thread/context64.hpp"
#include "winapi/kernel32/thread/restore_context64.hpp"
#include "winapi/kernel32/thread/unwind64.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/memory/mem_watch.hpp"
#include "core/cpu/seh/seh32.hpp"
#include "winapi/msvcrt/runtime/cxx_eh.hpp"
#include <vitreo/diagnostics.hpp>
#include "runtime/diagnostics/pc_trace.hpp"
#include "runtime/diagnostics/hostcall_ring.hpp"
#include <atomic>
#include <array>
#include <cstdio>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <vector>
#include <mach/mach_time.h>
#include <csignal>

namespace vitreo::cli {


// ============================================================================
// x64 table-based SEH helpers
// ============================================================================

// x64 CONTEXT layout
constexpr u32 CTX64_SIZE  = 0x4D0;
constexpr u32 CTX64_RAX = 0x078;  constexpr u32 CTX64_RCX = 0x080;
constexpr u32 CTX64_RDX = 0x088;  constexpr u32 CTX64_RBX = 0x090;
constexpr u32 CTX64_RSP = 0x098;  constexpr u32 CTX64_RBP = 0x0A0;
constexpr u32 CTX64_RSI = 0x0A8;  constexpr u32 CTX64_RDI = 0x0B0;
constexpr u32 CTX64_R8  = 0x0B8;  constexpr u32 CTX64_R9  = 0x0C0;
constexpr u32 CTX64_R10 = 0x0C8;  constexpr u32 CTX64_R11 = 0x0D0;
constexpr u32 CTX64_R12 = 0x0D8;  constexpr u32 CTX64_R13 = 0x0E0;
constexpr u32 CTX64_R14 = 0x0E8;  constexpr u32 CTX64_R15 = 0x0F0;
constexpr u32 CTX64_RIP = 0x0F8;
constexpr u32 CTX64_EFLAGS = 0x044;

// x64 EXCEPTION_RECORD layout
constexpr u32 EREC64_SIZE = 0x98;

// x64 DISPATCHER_CONTEXT layout
constexpr u32 DCTX64_SIZE = 0x50;
// The dispatcher's scratch frame holds, after the DISPATCHER_CONTEXT, the
// HandlerData / EstablisherFrame outputs and EXCEPTION_POINTERS (32 bytes),
// then the UNWIND_HISTORY_TABLE (winnt.h, 0xD8 bytes on x64) that
// DISPATCHER_CONTEXT.HistoryTable names. Windows passes a zeroed table from
// the dispatcher's own frame, never NULL (x64_dispatcher_context_probe
// HistoryTable_nonnull=1); it is a lookup cache, so a handler that hands it
// to RtlLookupFunctionEntry / RtlUnwindEx gets the same answers either way.
constexpr u32 DCTX64_OUTPUTS_SIZE = 32;
constexpr u32 UNWIND_HISTORY_TABLE64_SIZE = 0xD8;
constexpr u32 DCTX64_HISTORY_TABLE_OFFSET = DCTX64_SIZE + DCTX64_OUTPUTS_SIZE;

static bool is_canonical_x64_addr(u64 addr) {
    const u64 sign = (addr >> 47) & 1ULL;
    const u64 upper = addr >> 48;
    return sign ? (upper == 0xFFFFULL) : (upper == 0x0000ULL);
}

// The dispatcher's CONTEXT: CONTEXT_FULL written through the one x64 CONTEXT
// implementation (core/cpu/state/context64_record.hpp), so what a handler
// reads, what the unwinder updates and what a resume restores -- including
// the x87 image in FltSave -- is one layout.
static void write_context_64(GuestMemory& mem, GuestAddr a, const cpu::CpuState& c,
                             u32 context_flags) {
    u8 zeros[CTX64_SIZE] = {};
    (void)mem.write_bytes(a, zeros, CTX64_SIZE);
    (void)cpu::store_context64(mem, a, c, context_flags);
}

// The groups Windows fills in the CONTEXT a language handler is handed
// (tools/oracle_probe/x64_dispatcher_context_probe.expected): the faulting
// context carries segments and, for a hardware exception, the debug
// registers (0x10005F for #UD / AV / int3, 0x10004F for RaiseException);
// DISPATCHER_CONTEXT.ContextRecord, the unwind cursor, is 0x10000F. Windows
// also sets CONTEXT_XSTATE (0x40) with a CONTEXT_EX behind the record; Vitreo
// does not build that extended area, so it does not claim the bit.
constexpr u32 kHandlerContextFlags =
    cpu::kContext64Full | cpu::kContext64Segments;
constexpr u32 kHardwareExceptionContextFlags =
    kHandlerContextFlags | cpu::kContext64DebugRegisters;
constexpr u32 EXCEPTION_SOFTWARE_ORIGINATE_FLAG = 0x80u;
constexpr u64 RFLAGS_RF = 1ull << 16;

/// Intel SDM Vol. 3 17.3.1.1: the processor sets RF in the EFLAGS image it
/// saves for a fault (so the restarted instruction does not re-trigger an
/// instruction breakpoint), not for a trap. Measured: #UD and an access
/// violation report RF=1, int3 and RaiseException RF=0.
static bool is_fault_class(u32 code, u32 flags) {
    if ((flags & EXCEPTION_SOFTWARE_ORIGINATE_FLAG) != 0) return false;
    switch (code) {
        case 0x80000003u:  // STATUS_BREAKPOINT (int3, trap)
        case 0x80000004u:  // STATUS_SINGLE_STEP (trap)
        case 0xC0000095u:  // STATUS_INTEGER_OVERFLOW (INTO, trap)
            return false;
        default:
            return true;
    }
}

// Resuming from a CONTEXT restores the groups its ContextFlags select, as
// NtContinue does; @p c is the dispatching thread's own running state. RF
// only suppresses a breakpoint on the one resumed instruction, which Vitreo
// has none of, so it is not carried into the running flags.
static void read_context_64(GuestMemory& mem, GuestAddr a, cpu::CpuState& c) {
    (void)cpu::load_context64(mem, a, c, /*restoring_current_thread=*/true);
    c.flags.rflags &= ~RFLAGS_RF;
}

static void write_exception_record_64(
    GuestMemory& mem, GuestAddr a,
    u32 code, u32 flags, u64 addr,
    u32 nparams, const u64* params)
{
    u8 zeros[EREC64_SIZE] = {};
    (void)mem.write_bytes(a, zeros, EREC64_SIZE);
    (void)mem.write_u32(a + 0x00, code);
    (void)mem.write_u32(a + 0x04, flags);
    (void)mem.write_u64(a + 0x08, 0);     // ExceptionRecord (nullptr)
    (void)mem.write_u64(a + 0x10, addr);   // ExceptionAddress
    (void)mem.write_u32(a + 0x18, nparams);
    for (u32 i = 0; i < nparams && i < 15; ++i)
        (void)mem.write_u64(a + 0x20 + i * 8, params[i]);
}

static void write_dispatcher_context_64(
    GuestMemory& mem, GuestAddr a,
    u64 control_pc, u64 image_base, GuestAddr func_entry,
    u64 establisher_frame, u64 target_ip,
    GuestAddr ctx_record, u64 handler_addr, GuestAddr handler_data)
{
    u8 zeros[DCTX64_SIZE] = {};
    (void)mem.write_bytes(a, zeros, DCTX64_SIZE);
    (void)mem.write_u64(a + 0x00, control_pc);
    (void)mem.write_u64(a + 0x08, image_base);
    (void)mem.write_u64(a + 0x10, func_entry);
    (void)mem.write_u64(a + 0x18, establisher_frame);
    (void)mem.write_u64(a + 0x20, target_ip);
    (void)mem.write_u64(a + 0x28, ctx_record);
    (void)mem.write_u64(a + 0x30, handler_addr);
    (void)mem.write_u64(a + 0x38, handler_data);
    (void)mem.write_u64(a + 0x40, a + DCTX64_HISTORY_TABLE_OFFSET);  // HistoryTable
}

/// Dispatch x64 exception: walk .pdata, call handlers, handle unwind.
/// Returns true if the exception was handled (CPU state updated to resume).


/// The unwind half of x64 dispatch: a language handler called RtlUnwindEx
/// during the search, and its walk stopped at the host-callback boundary the
/// dispatcher called it through (winapi/kernel32/thread/unwind64.hpp). On
/// Windows that walk would go on through the dispatcher's frames into the
/// context the exception was raised in; here it restarts from that context,
/// with the same frame walk the RtlUnwindEx export uses.
///
/// Returns true when the CPU is positioned to resume (the target context was
/// restored, a handler raised, or the process ended); false when the walk
/// left the stack this dispatcher can see (the callback sentinel or the end
/// of the stack) and the caller should make the plain TargetIp transfer.
[[nodiscard]] static bool run_unwind_pass_64(
    cpu::CpuState& cpu, GuestMemory& memory,
    winapi::ThunkRegistry& thunk_registry, winapi::ProcessManager& process_mgr,
    winapi::ModuleManager* module_mgr, const cpu::CpuState& pre_dispatch_cpu,
    const winapi::Unwind64Scratch& scratch, u32 raise_code,
    const cpu::CpuState::PendingUnwind& unwind)
{
    if (module_mgr == nullptr) return false;

    // The search pass mutated the cursor frame by frame; the unwind pass
    // starts over from the state the exception was raised in.
    write_context_64(memory, scratch.cursor, pre_dispatch_cpu, kHandlerContextFlags);

    winapi::Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    // RtlUnwindEx hands the language handler the EXCEPTION_RECORD *it* was
    // given. MSVC matches that pointer against the one recorded in its frame
    // bookkeeping to tell "my exception" from a foreign unwind. A NULL record
    // is a STATUS_UNWIND whose address is RtlUnwindEx's return address.
    winapi::Unwind64Target target;
    target.frame = unwind.target_frame;
    target.ip = unwind.target_ip;
    target.return_value = unwind.return_value;
    target.record = static_cast<GuestAddr>(unwind.exception_record);
    if (target.record == 0) {
        winapi::write_status_unwind_record64(marshaler, scratch.local_record,
                                             unwind.null_record_address);
        target.record = scratch.local_record;
    }
    // The search-pass handler that called RtlUnwindEx has returned to us, but
    // its frames are still live on Windows until the unwind completes: MSVC's
    // consolidation record and the establisher-frame slot its
    // ExceptionInformation[1] points at live there, and the catch callback
    // reads them after the walk. The unwind handlers run below RtlUnwindEx's
    // caller, as continue_pending_unwind64 runs them, not over that stack.
    target.stack_floor = unwind.caller_sp;

    const winapi::Unwind64HandlerCall call_handler =
        [&](GuestAddr handler, const u64* args) -> std::optional<u64> {
            // After a collided unwind the nested RtlUnwindEx's caller (and its
            // record) is still live: run below it.
            if (target.stack_floor != 0 && cpu.sp() + 0x100 > target.stack_floor) {
                cpu.set_sp((target.stack_floor - 0x100) & ~u64{0xF});
            }
            u64 call_args[4] = {args[0], args[1], args[2], args[3]};
            const u64 disposition = run_guest_callback(
                cpu, memory, thunk_registry, process_mgr, handler, call_args, 4,
                true);
            if (raise_code == 0xE06D7363u) {
                VITREO_TRACE(
                    "[seh64] unwind handler=0x%llx establisher=0x%llx "
                    "disposition=%llu\n",
                    static_cast<unsigned long long>(handler),
                    static_cast<unsigned long long>(args[1]),
                    static_cast<unsigned long long>(disposition));
            }
            // A fault inside an unwind handler is a nested exception; leave it
            // for the outer loop exactly as the search pass does.
            if (cpu.pending_raise.active && cpu.pending_raise.from_helper) {
                return std::nullopt;
            }
            if (process_mgr.has_exited()) return std::nullopt;
            return disposition;
        };

    const winapi::Unwind64Stop stop = winapi::walk_unwind64(
        marshaler, *module_mgr, scratch, target,
        /*start_is_return_address=*/false, call_handler);
    switch (stop) {
    case winapi::Unwind64Stop::TargetReached: {
        // RtlUnwindEx ends in RtlRestoreContext(ContextRecord,
        // ExceptionRecord): the target frame's own context (not its
        // caller's), Rip = TargetIp, Rax = ReturnValue. The record decides
        // what that restore means: MSVC's catch blocks arrive as
        // STATUS_UNWIND_CONSOLIDATE, whose callback runs the catch body and
        // supplies the real continuation.
        u64 ceiling = unwind.caller_sp != 0 ? unwind.caller_sp : cpu.sp();
        if (target.stack_floor != 0 && target.stack_floor < ceiling) {
            ceiling = target.stack_floor;
        }
        if (!winapi::restore_context64_with_record(
                marshaler, *module_mgr, scratch.frame, target.record, ceiling)) {
            std::fprintf(stderr,
                "[seh64] RtlUnwindEx: cannot restore target context "
                "(record=0x%llx code=0x%08x)\n",
                static_cast<unsigned long long>(target.record),
                memory.read_u32(target.record).value_or(0));
            read_context_64(memory, scratch.frame, cpu);
        }
        VITREO_TRACE(
            "[seh64] unwind target reached: resume rip=0x%llx rsp=0x%llx "
            "(establisher=0x%llx)\n",
            static_cast<unsigned long long>(cpu.rip),
            static_cast<unsigned long long>(cpu.gpr.rsp),
            static_cast<unsigned long long>(target.frame));
        return true;
    }
    case winapi::Unwind64Stop::Interrupted:
        return true;
    case winapi::Unwind64Stop::BadStack:
    case winapi::Unwind64Stop::InvalidDisposition: {
        // RtlRaiseStatus from inside RtlUnwindEx: noncontinuable, raised in
        // ntdll. The handler frames that called it are gone, so it is
        // dispatched from the context the exception was raised in -- the
        // frames it can still reach are the same ones.
        const u32 status = stop == winapi::Unwind64Stop::BadStack
                               ? winapi::STATUS_BAD_STACK_CODE
                               : winapi::STATUS_INVALID_DISPOSITION_CODE;
        cpu = pre_dispatch_cpu;
        const u64 ntdll_code =
            thunk_registry.get_thunk_address("ntdll.dll", "RtlUnwindEx").value_or(0);
        if (!dispatch_x64_exception(cpu, memory, thunk_registry, process_mgr,
                                    status,
                                    winapi::EXCEPTION_NONCONTINUABLE_BIT |
                                        EXCEPTION_SOFTWARE_ORIGINATE_FLAG,
                                    0, nullptr, cpu.rip, module_mgr,
                                    ntdll_code)) {
            std::fprintf(stderr,
                "[seh64] unhandled 0x%08x raised by RtlUnwindEx; ending the "
                "process\n", status);
            process_mgr.exit_process(status);
        }
        return true;
    }
    case winapi::Unwind64Stop::CallbackBoundary:
    case winapi::Unwind64Stop::EndOfStack:
        return false;
    }
    return false;
}

/// Dispatch is about to resume from the CONTEXT at @p exception_ctx_addr:
/// run the vectored continue handlers on it first (vectored_continue_probe:
/// after a vectored handler, a frame-based handler or the top-level filter
/// continues, in list order, each seeing the previous one's CONTEXT edits,
/// until one returns EXCEPTION_CONTINUE_EXECUTION). False when a handler
/// raised a nested exception or ended the process: leave the CPU as it is.
static bool run_vectored_continue_handlers_64(
    cpu::CpuState& cpu, GuestMemory& memory,
    winapi::ThunkRegistry& thunk_registry, winapi::ProcessManager& process_mgr,
    GuestAddr eptrs_addr, GuestAddr stack) {
    for (u64 handler : winapi::snapshot_vectored_continue_handlers()) {
        cpu.set_sp(stack);
        u64 args[1] = {eptrs_addr};
        const u64 disposition = run_guest_callback(
            cpu, memory, thunk_registry, process_mgr, handler, args, 1, true);
        if (cpu.pending_raise.active && cpu.pending_raise.from_helper) return false;
        if (process_mgr.has_exited()) return false;
        if ((disposition & 0xFFFFFFFFull) == 0xFFFFFFFFull) break;
    }
    return true;
}

bool dispatch_x64_exception(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    winapi::ThunkRegistry& thunk_registry,
    winapi::ProcessManager& process_mgr,
    u32 raise_code, u32 raise_flags, u32 raise_nargs, const u64* raise_args,
    u64 exception_address,
    winapi::ModuleManager* module_mgr,
    u64 record_address)
{
    // A JIT registers its unwind information through
    // RtlInstallFunctionTableCallback, which resolves each PC by calling back
    // into GUEST code. Both the search and the unwind pass may need it, and
    // the callback must run on this thread's CpuState.
    const winapi::ModuleManager::RuntimeFunctionCallbackRunner callback_runner =
        [&](GuestAddr callback, GuestAddr control_pc,
            GuestAddr context) -> GuestAddr {
            u64 args[2] = {static_cast<u64>(control_pc),
                           static_cast<u64>(context)};
            return static_cast<GuestAddr>(run_guest_callback(
                cpu, memory, thunk_registry, process_mgr, callback, args, 2,
                true));
        };
    const winapi::ModuleManager::ScopedThreadCallbackRunner scoped_runner{
        &callback_runner};

    // Keep two CONTEXT records. The language-handler ContextRecord argument is
    // the context at which the exception was raised, while
    // DISPATCHER_CONTEXT.ContextRecord is the mutable virtual-unwind cursor.
    // RtlVirtualUnwind rewrites the latter to describe each caller frame; using
    // that same storage for both contracts makes handlers observe the caller's
    // RIP/registers instead of the fault context.
    // A thread that runs off its stack. Windows raises STATUS_STACK_OVERFLOW
    // for a write that reaches the stack's guard region -- the part between
    // the allocation base (TEB DeallocationStack) and the committed limit
    // (TEB StackLimit) -- and then lets the handler run on those pages, with
    // StackLimit lowered to one page above the base. Measured, not assumed:
    // tools/oracle_probe/stack_overflow_probe.c. Keyed on the TEB alone, so it
    // is exactly the kernel's rule and knows nothing about who is running.
    //
    // This has to happen BEFORE the scratch frame below is written: that
    // frame goes under RSP, which at this point is inside the guard, and it
    // was precisely those writes faulting that made every overflow silent.
    //
    // Two sources arrive here. The JIT (and any other hardware-protected
    // write) reports the guard touch as an ACCESS VIOLATION with the address;
    // the interpreter's own StackLimit check (push_value) raises
    // STATUS_STACK_OVERFLOW directly, before writing, with no parameters.
    // Both leave RSP in the guard, so both need the recovery pages opened
    // before anything is written below it -- and both leave with the two
    // parameters Windows supplies.
    // A breakpoint carries one parameter, 0 (BREAKPOINT_BREAK), whether from
    // CC or CD 03 (int3_record_probe: nparams=1 info=0).
    static constexpr u64 kBreakpointArgs[1] = {0};
    if (raise_code == 0x80000003u && raise_nargs == 0) {
        raise_nargs = 1;
        raise_args = kBreakpointArgs;
    }
    u64 stack_overflow_args[2] = {};
    const bool guard_access_violation =
        raise_code == 0xC0000005u && raise_nargs >= 2;
    const bool software_overflow = raise_code == 0xC00000FDu;
    if ((guard_access_violation || software_overflow) && cpu.is_64bit()) {
        const GuestAddr teb = static_cast<GuestAddr>(cpu.seg.gs_desc.base);
        const auto limit = memory.read_u64(teb + 0x10);
        const auto base = memory.read_u64(teb + 0x1478);
        // The interpreter checks before its push, so the address it was about
        // to write is the slot just under RSP.
        const u64 fault = guard_access_violation ? raise_args[1]
                                                 : cpu.sp() - 8;
        if (teb != 0 && limit.is_ok() && base.is_ok() && base.value() != 0 &&
            fault >= base.value() && fault < limit.value()) {
            constexpr u64 kPermanentGuardBytes = 0x1000;
            const u64 recovery_base = base.value() + kPermanentGuardBytes;
            if (limit.value() > recovery_base &&
                memory
                    .protect_region(
                        recovery_base,
                        static_cast<usize>(limit.value() - recovery_base),
                        MemoryProtection::ReadWrite)
                    .is_ok()) {
                (void)memory.write_u64(teb + 0x10, recovery_base);
                // [0] = access type (a stack overflow is a write), [1] = the
                // faulting address -- the shape measured on Windows.
                stack_overflow_args[0] =
                    guard_access_violation ? raise_args[0] : 1u;
                stack_overflow_args[1] = fault;
                raise_nargs = 2;
                raise_code = 0xC00000FDu;  // STATUS_STACK_OVERFLOW
                raise_args = stack_overflow_args;
                // The one moment the thread's recent host calls still
                // describe the runaway frames rather than whatever the
                // guest's handler or crash reporter does next. No-op unless
                // the hostcall ring is enabled; once per process, since a
                // recovered overflow can recur.
                static std::atomic<bool> dumped_stack_overflow{false};
                if (!dumped_stack_overflow.exchange(true)) {
                    hostcall_dump("STATUS_STACK_OVERFLOW");
                }
                VITREO_TRACE(
                    "[seh64] stack overflow: fault=0x%llx rsp=0x%llx "
                    "recovery=[0x%llx,0x%llx)\n",
                    static_cast<unsigned long long>(fault),
                    static_cast<unsigned long long>(cpu.sp()),
                    static_cast<unsigned long long>(recovery_base),
                    static_cast<unsigned long long>(limit.value()));
            }
        }
    }

    cpu::CpuState pre_dispatch_cpu = cpu;
    u64 orig_rsp = cpu.sp();
    // Layout: exception CONTEXT, cursor CONTEXT, EXCEPTION_RECORD,
    // DISPATCHER_CONTEXT + outputs + UNWIND_HISTORY_TABLE, then the unwind
    // pass's frame CONTEXT (16-aligned) and its STATUS_UNWIND record.
    u64 scratch = orig_rsp -
        (CTX64_SIZE * 3 + EREC64_SIZE * 2 + DCTX64_HISTORY_TABLE_OFFSET +
         UNWIND_HISTORY_TABLE64_SIZE + 16 + 64);
    scratch &= ~0xFULL;

    GuestAddr exception_ctx_addr = scratch;
    GuestAddr ctx_addr = exception_ctx_addr + CTX64_SIZE;
    GuestAddr erec_addr = ctx_addr + CTX64_SIZE;
    GuestAddr dctx_addr = erec_addr + EREC64_SIZE;
    const GuestAddr unwind_frame_ctx_addr =
        (dctx_addr + DCTX64_HISTORY_TABLE_OFFSET + UNWIND_HISTORY_TABLE64_SIZE +
         0xF) & ~GuestAddr{0xF};
    const GuestAddr unwind_local_erec_addr = unwind_frame_ctx_addr + CTX64_SIZE;

    // record_address: an exception the dispatcher raises itself names its
    // own code as ExceptionAddress while the walk starts at the raise site.
    write_exception_record_64(memory, erec_addr,
        raise_code, raise_flags,
        record_address != 0 ? record_address : exception_address,
        raise_nargs, raise_args);
    const bool software_raise = (raise_flags & EXCEPTION_SOFTWARE_ORIGINATE_FLAG) != 0;
    write_context_64(memory, exception_ctx_addr, cpu,
                     software_raise ? kHandlerContextFlags : kHardwareExceptionContextFlags);
    if (is_fault_class(raise_code, raise_flags)) {
        const u64 eflags = memory.read_u32(exception_ctx_addr + CTX64_EFLAGS).value_or(0);
        (void)memory.write_u32(exception_ctx_addr + CTX64_EFLAGS,
                               static_cast<u32>(eflags | RFLAGS_RF));
    }
    write_context_64(memory, ctx_addr, cpu, kHandlerContextFlags);
    // The #BP trap leaves RIP past the instruction; Windows hands handlers
    // the breakpoint address (end - 1) as the context RIP, so a handler that
    // continues without changing it executes the breakpoint again
    // (int3_record_probe ip=site+0 for CC, site+1 for CD 03). The running
    // state keeps the trap RIP for the unhandled paths.
    if (raise_code == 0x80000003u && exception_address + 1 == cpu.rip) {
        (void)memory.write_u64(exception_ctx_addr + CTX64_RIP, exception_address);
        (void)memory.write_u64(ctx_addr + CTX64_RIP, exception_address);
    }

    // Move SP below the scratch region so that handler callbacks (which
    // allocate their own stack frames starting from cpu.sp()) don't
    // stomp over our CONTEXT/EREC/DCTX data in guest memory.
    cpu.set_sp(scratch);

    winapi::Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const GuestAddr handler_data_out = dctx_addr + DCTX64_SIZE;
    const GuestAddr establisher_out = handler_data_out + 8;
    // EXCEPTION_POINTERS sits in the reserved pad after DCTX outputs.
    const GuestAddr eptrs_addr = establisher_out + 8;
    (void)memory.write_u64(eptrs_addr, erec_addr);
    (void)memory.write_u64(eptrs_addr + 8, exception_ctx_addr);
    {
        u8 zeros[UNWIND_HISTORY_TABLE64_SIZE] = {};
        (void)memory.write_bytes(dctx_addr + DCTX64_HISTORY_TABLE_OFFSET, zeros,
                                 UNWIND_HISTORY_TABLE64_SIZE);
    }

    // Learn: VEH runs after debugger first-chance and before the unwind walk.
    // PVECTORED_EXCEPTION_HANDLER returns EXCEPTION_CONTINUE_EXECUTION
    // (0xffffffff) or EXCEPTION_CONTINUE_SEARCH (0).
    const auto veh_handlers = winapi::snapshot_vectored_exception_handlers();
    // An access violation carries its access type and faulting address in the
    // two exception-record parameters; logging them turns a bare "AV at RIP"
    // into an attributable fault.
    VITREO_TRACE(
        "[seh64] VEH handlers=%zu code=0x%08x rip=0x%llx nargs=%u "
        "arg0=0x%llx arg1=0x%llx\n",
        veh_handlers.size(), raise_code,
        static_cast<unsigned long long>(exception_address),
        static_cast<unsigned>(raise_nargs),
        static_cast<unsigned long long>(raise_nargs > 0 ? raise_args[0] : 0),
        static_cast<unsigned long long>(raise_nargs > 1 ? raise_args[1] : 0));
    // An illegal instruction is attributable only by its bytes (the code may
    // have been written at run time, so the image on disk does not show it).
    if (raise_code == 0xC000001Du) {
        u8 bytes[16] = {};
        for (usize i = 0; i < sizeof(bytes); ++i) {
            bytes[i] = memory.read_u8(exception_address + i).value_or(0);
        }
        VITREO_TRACE(
            "[seh64] illegal instruction bytes at 0x%llx: %02x %02x %02x %02x %02x %02x %02x "
            "%02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
            static_cast<unsigned long long>(exception_address), bytes[0], bytes[1], bytes[2],
            bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10],
            bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    }
    if (raise_code == 0xC0000005u && raise_nargs > 1) {
        GuestAddr region_base = 0;
        usize region_size = 0;
        MemoryProtection region_prot{};
        const bool mapped = memory.query_region(
            static_cast<GuestAddr>(raise_args[1]), region_base, region_size,
            region_prot);
        VITREO_TRACE(
            "[seh64] fault region addr=0x%llx mapped=%d base=0x%llx "
            "size=0x%llx prot=0x%x\n",
            static_cast<unsigned long long>(raise_args[1]), mapped ? 1 : 0,
            static_cast<unsigned long long>(region_base),
            static_cast<unsigned long long>(region_size),
            static_cast<unsigned>(region_prot));
    }
    for (u64 handler : veh_handlers) {
        u64 veh_args[1] = {eptrs_addr};
        const u64 disposition = run_guest_callback(
            cpu, memory, thunk_registry, process_mgr, handler, veh_args, 1, true);
        VITREO_TRACE(
            "[seh64] VEH handler=0x%llx disposition=0x%llx context_rip=0x%llx\n",
            static_cast<unsigned long long>(handler),
            static_cast<unsigned long long>(disposition),
            static_cast<unsigned long long>(
                memory.read_u64(exception_ctx_addr + CTX64_RIP).value_or(0)));
        if (cpu.pending_raise.active && cpu.pending_raise.from_helper) {
            return true;
        }
        if (process_mgr.has_exited()) {
            return true;
        }
        if ((disposition & 0xFFFFFFFFull) == 0xFFFFFFFFull) {
            if (!run_vectored_continue_handlers_64(cpu, memory, thunk_registry,
                                                   process_mgr, eptrs_addr, scratch)) {
                return true;
            }
            read_context_64(memory, exception_ctx_addr, cpu);
            VITREO_TRACE(
                "[seh64] VEH continue applied rip=0x%llx rsp=0x%llx\n",
                static_cast<unsigned long long>(cpu.rip),
                static_cast<unsigned long long>(cpu.sp()));
            return true;
        }
    }

    // A vectored handler that continued the search may have edited the
    // exception context. Begin the table-based walk from that resulting state,
    // but keep its handler-facing record separate from the unwind cursor.
    u8 exception_context[CTX64_SIZE] = {};
    if (memory.read_bytes(exception_ctx_addr, exception_context,
                          CTX64_SIZE).is_ok()) {
        (void)memory.write_bytes(ctx_addr, exception_context, CTX64_SIZE);
        // The cursor never carries the debug-register group (0x10000F).
        (void)memory.write_u32(ctx_addr + cpu::kContext64FlagsOffset, kHandlerContextFlags);
    }

    if (!module_mgr) {
        VITREO_TRACE("[seh64] No module manager — cannot walk .pdata\n");
        std::fprintf(stderr,
            "[seh64] no module manager; cannot walk .pdata for code=0x%08x rip=0x%llx\n",
            raise_code, static_cast<unsigned long long>(exception_address));
        return false;
    }

    // Microsoft x64 exception handling: DISPATCHER_CONTEXT.ControlPc is the
    // exception address for the faulting frame. This differs from CONTEXT.Rip
    // for trap instructions such as INT3, whose saved RIP is already one byte
    // past the breakpoint. Looking up that post-trap address can cross a
    // RUNTIME_FUNCTION boundary and incorrectly discard the frame's handler.
    u64 walk_rip = exception_address;
    u64 walk_rsp = orig_rsp;  // Walk from original RSP, not the lowered one
    // A caller frame's RIP is a RETURN address: the call site is the byte
    // before it. Looking the frame up at the return address maps a `call` that
    // ends a try region (a noreturn call such as _CxxThrowException, which the
    // compiler follows with int3 padding) to the state AFTER the region, so the
    // language handler reports no catch. Windows resolves caller frames at
    // ControlPc-1; only the faulting frame uses its RIP verbatim.
    const bool lookup_bias =
        std::getenv("VITREO_X64_EH_NO_PC_BIAS") == nullptr;
    // A frame reached by resuming past the host-callback sentinel is entered at
    // the RIP the guest was interrupted on, which is a resume point, not a
    // return address. That RIP is routinely a function's first byte, so the
    // caller bias looks it up one byte before the RUNTIME_FUNCTION that covers
    // it and the walk aborts with "no RUNTIME_FUNCTION". Only frames whose RIP
    // came out of a stack slot written by `call` get biased.
    bool walk_rip_is_resume = false;

    // An unhandled walk is only actionable with the frames it visited: the
    // final RIP alone cannot say which frame's unwind produced a bad caller.
    struct WalkedFrame {
        u64 rip;
        u64 rsp;
        u64 runtime_func;
    };
    std::array<WalkedFrame, 64> walked{};
    u32 walked_count = 0;

    for (u32 depth = 0; depth < 64; ++depth) {
        const u64 lookup_rip =
            (depth > 0 && lookup_bias && !walk_rip_is_resume) ? walk_rip - 1
                                                              : walk_rip;
        GuestAddr image_base = 0;
        GuestAddr runtime_func = module_mgr->lookup_runtime_function_entry(
            lookup_rip, &image_base);
        if (walked_count < walked.size()) {
            walked[walked_count++] =
                WalkedFrame{walk_rip, walk_rsp,
                            static_cast<u64>(runtime_func)};
        }
        // A thunk stub is not guest code and has no prologue, so no
        // RUNTIME_FUNCTION describes it. It sits inside a synthesized module
        // whose .pdata can still answer the lookup, and unwinding a thunk
        // frame with a neighbouring function's prologue yields a garbage
        // caller — observed as `caller_rip=0x0` at the frame the callback
        // sentinel resumes onto, which stops the walk and reports an
        // exception the guest had a handler for as unhandled. Force the leaf
        // path below, which is what the frame actually is: the guest's return
        // address is at [RSP] because the guest reached the stub with `call`.
        if (runtime_func != 0 &&
            thunk_registry.is_in_thunk_region(
                static_cast<GuestAddr>(walk_rip))) {
            VITREO_TRACE("[seh64] RIP=0x%llx is a thunk stub — unwinding as"
                         " leaf, not with .pdata for 0x%llx\n",
                         static_cast<unsigned long long>(walk_rip),
                         static_cast<unsigned long long>(runtime_func));
            runtime_func = 0;
        }
        if (runtime_func == 0) {
            // Learn x64 exception handling: a function with no RUNTIME_FUNCTION
            // is a LEAF function. Its return address is at [RSP] and it has no
            // prologue to undo, which is exactly how RtlVirtualUnwind treats a
            // NULL function entry. Terminating the walk here instead made every
            // exception raised beneath such a frame unhandled — including ones
            // the guest had a perfectly good handler for further up.
            //
            // This fires in practice on Vitreo's own thunk frames: resuming
            // past the host-callback sentinel lands on a thunk RIP, which has
            // no .pdata because it is not real guest code. A browser renderer
            // died on exactly this path (access violation at null+8 inside a
            // callback), and the unwind stopped at the thunk rather than
            // reaching the guest's handler.
            VITREO_TRACE("[seh64] No RUNTIME_FUNCTION for RIP=0x%llx (depth=%u)"
                         " — unwinding as leaf\n",
                         static_cast<unsigned long long>(walk_rip), depth);
            const auto leaf_return = memory.read_u64(walk_rsp);
            if (leaf_return.is_err()) {
                std::fprintf(stderr,
                    "[seh64] leaf frame rip=0x%llx rsp=0x%llx unreadable"
                    " depth=%u code=0x%08x\n",
                    static_cast<unsigned long long>(walk_rip),
                    static_cast<unsigned long long>(walk_rsp), depth,
                    raise_code);
                break;
            }
            const u64 leaf_caller = leaf_return.value();
            // A sentinel or null return address is a genuine end of stack; let
            // the existing end-of-walk reporting handle it rather than chasing
            // a synthetic address into the RUNTIME_FUNCTION lookup.
            if (leaf_caller == 0 ||
                leaf_caller == CALLBACK_RETURN_SENTINEL ||
                leaf_caller == PROCESS_RETURN_SENTINEL) {
                std::fprintf(stderr,
                    "[seh64] leaf frame rip=0x%llx returns to sentinel 0x%llx"
                    " depth=%u code=0x%08x\n",
                    static_cast<unsigned long long>(walk_rip),
                    static_cast<unsigned long long>(leaf_caller), depth,
                    raise_code);
                break;
            }
            walk_rsp += 8;
            walk_rip = leaf_caller;
            // The new RIP came out of a stack slot written by `call`, so it is
            // a return address and must take the ControlPc-1 lookup bias.
            walk_rip_is_resume = false;
            (void)memory.write_u64(ctx_addr + CTX64_RIP, walk_rip);
            (void)memory.write_u64(ctx_addr + CTX64_RSP, walk_rsp);
            continue;
        }

        // Learn x64 EH: UWOP_PUSH_NONVOL / UWOP_SAVE_NONVOL restore the
        // register; UWOP_SET_FPREG then uses that frame pointer. Carry
        // state in the guest CONTEXT via RtlVirtualUnwind so later frames
        // do not reuse the exception-time RBP.
        GuestAddr handler_addr = 0;
        (void)memory.write_u64(handler_data_out, 0);
        (void)memory.write_u64(establisher_out, 0);
        if (!winapi::unwind_context64(
                marshaler, image_base, walk_rip, runtime_func, ctx_addr,
                handler_data_out, establisher_out, &handler_addr)) {
            const u32 begin =
                memory.read_u32(runtime_func + 0).value_or(0);
            const u32 end =
                memory.read_u32(runtime_func + 4).value_or(0);
            const u32 unwind =
                memory.read_u32(runtime_func + 8).value_or(0);
            u8 unwind_bytes[16]{};
            const bool have_unwind = memory.read_bytes(
                image_base + unwind, unwind_bytes,
                sizeof(unwind_bytes)).is_ok();
            VITREO_TRACE("[seh64] Failed to parse UNWIND_INFO for RIP=0x%llx\n",
                         static_cast<unsigned long long>(walk_rip));
            std::fprintf(stderr,
                "[seh64] UNWIND_INFO parse failed rip=0x%llx depth=%u "
                "image_base=0x%llx rf=0x%llx fields=[0x%x,0x%x,0x%x] "
                "bytes=",
                static_cast<unsigned long long>(walk_rip), depth,
                static_cast<unsigned long long>(image_base),
                static_cast<unsigned long long>(runtime_func),
                begin, end, unwind);
            if (have_unwind) {
                for (const u8 byte : unwind_bytes) {
                    std::fprintf(stderr, "%02x",
                                 static_cast<unsigned>(byte));
                }
            } else {
                std::fprintf(stderr, "<unmapped>");
            }
            std::fputc('\n', stderr);
            break;
        }

        const u64 establisher = memory.read_u64(establisher_out).value_or(0);
        const GuestAddr handler_data = memory.read_u64(handler_data_out).value_or(0);
        const u64 caller_rip = memory.read_u64(ctx_addr + CTX64_RIP).value_or(0);
        const u64 caller_rsp = memory.read_u64(ctx_addr + CTX64_RSP).value_or(0);

        VITREO_TRACE("[seh64] Walk depth=%u RIP=0x%llx RSP=0x%llx establisher=0x%llx caller_rip=0x%llx handler=0x%llx\n",
                     depth,
                     static_cast<unsigned long long>(walk_rip),
                     static_cast<unsigned long long>(walk_rsp),
                     static_cast<unsigned long long>(establisher),
                     static_cast<unsigned long long>(caller_rip),
                     static_cast<unsigned long long>(handler_addr));

        if (handler_addr != 0) {
            VITREO_TRACE("[seh64] Frame %u: RIP=0x%llx handler=0x%llx establisher=0x%llx handler_data=0x%llx\n",
                         depth,
                         static_cast<unsigned long long>(walk_rip),
                         static_cast<unsigned long long>(handler_addr),
                         static_cast<unsigned long long>(establisher),
                         static_cast<unsigned long long>(handler_data));

            write_dispatcher_context_64(memory, dctx_addr,
                walk_rip, image_base, runtime_func,
                establisher, 0, ctx_addr,
                handler_addr, handler_data);

            u64 seh_args[4] = {
                erec_addr,
                establisher,
                exception_ctx_addr,
                dctx_addr
            };

            u64 disposition = run_guest_callback(cpu, memory, thunk_registry,
                process_mgr, handler_addr, seh_args, 4, true);
            bool suppressed_fastfail_terminate = false;

            // A fault raised while executing the handler is a nested exception,
            // not a handler disposition. Preserve its CPU context for the outer
            // execution loop to dispatch (or terminate for fail-fast).
            if (cpu.pending_raise.active &&
                cpu.pending_raise.from_helper) {
                return true;
            }

            VITREO_TRACE("[seh64] Callback disposition=%llu after handler=0x%llx\n",
                         static_cast<unsigned long long>(disposition),
                         static_cast<unsigned long long>(handler_addr));

            // If the handler called TerminateProcess (or ExitProcess),
            // stop dispatching — the process is done.
            if (process_mgr.has_exited()) {
                // Compatibility: some x64 C++ EH paths call TerminateProcess
                // with STATUS_STACK_BUFFER_OVERRUN during probe/unwind logic
                // before user code has a chance to catch 0xE06D7363. Ignore
                // that specific termination and keep searching for handlers.
                const bool allow_0409_suppress = (std::getenv("VITREO_X64_EH_SUPPRESS_0409") != nullptr);
                if (allow_0409_suppress && raise_code == 0xE06D7363u && process_mgr.exit_code() == 0xC0000409u) {
                    VITREO_TRACE("[seh64] Suppressing TerminateProcess(0xC0000409) during C++ EH dispatch; continuing search\n");
                    process_mgr.clear_exit_request();
                    suppressed_fastfail_terminate = true;
                } else {
                VITREO_TRACE("[seh64] Process exited during handler callback (code=%u)\n",
                             process_mgr.exit_code());
                return true;
                }
            }

            if (suppressed_fastfail_terminate) {
                disposition = 1; // ExceptionContinueSearch
            }

            if (cpu.pending_unwind.active) {
                const auto unwind = cpu.pending_unwind;
                const u64 target_ip = unwind.target_ip;
                const u64 target_frame = unwind.target_frame;
                const u64 unwind_ret = unwind.return_value;
                cpu.pending_unwind.clear();

                VITREO_TRACE(
                    "[seh64] Unwind -> ip=0x%llx frame=0x%llx ret=0x%llx\n",
                    static_cast<unsigned long long>(target_ip),
                    static_cast<unsigned long long>(target_frame),
                    static_cast<unsigned long long>(unwind_ret));

                // Intel/Microsoft x64 EH: RtlUnwindEx does NOT jump straight
                // to TargetIp. It walks the frames from the unwind point down
                // to TargetFrame and calls each language handler a SECOND time
                // with EXCEPTION_UNWINDING set — plus EXCEPTION_TARGET_UNWIND
                // on TargetFrame itself. MSVC runs stack-object destructors on
                // those calls and executes the CATCH BODY on the target-frame
                // call, from which it transfers control itself. Jumping
                // straight to TargetIp skips every catch body and lands on
                // whatever ControlPc the CRT happened to pass — for a
                // `call _CxxThrowException` that is the int3 the compiler
                // emits after a noreturn call.
                winapi::Unwind64Scratch unwind_scratch;
                unwind_scratch.cursor = ctx_addr;
                unwind_scratch.frame = unwind_frame_ctx_addr;
                unwind_scratch.dispatcher = dctx_addr;
                unwind_scratch.handler_data_out = handler_data_out;
                unwind_scratch.establisher_out = establisher_out;
                unwind_scratch.history_table =
                    dctx_addr + DCTX64_HISTORY_TABLE_OFFSET;
                unwind_scratch.local_record = unwind_local_erec_addr;
                if (run_unwind_pass_64(
                        cpu, memory, thunk_registry, process_mgr, module_mgr,
                        pre_dispatch_cpu, unwind_scratch, raise_code, unwind)) {
                    return true;
                }

                // Read back context (handler may have modified it)
                read_context_64(memory, ctx_addr, cpu);
                cpu.rip = target_ip;
                if (target_frame != 0) cpu.set_sp(target_frame);
                cpu.gpr.rax = unwind_ret;
                return true;
            }

            VITREO_TRACE("[seh64] Handler returned disposition=%llu\n",
                         static_cast<unsigned long long>(disposition));
            if (raise_code == 0xE06D7363u) {
                std::fprintf(stderr,
                    "[seh64] handler disposition=%llu depth=%u rip=0x%llx handler=0x%llx\n",
                    static_cast<unsigned long long>(disposition), depth,
                    static_cast<unsigned long long>(walk_rip),
                    static_cast<unsigned long long>(handler_addr));
            }

            if (disposition == 0 && (raise_flags & 0x1u) != 0) {
                // Learn RaiseException: "Any attempt to continue execution
                // after a noncontinuable exception causes the
                // EXCEPTION_NONCONTINUABLE_EXCEPTION exception." Measured
                // (c_specific_handler_probe continue_noncontinuable): code
                // 0xC0000025, flags NONCONTINUABLE | software (0x81), no
                // parameters, no chained record, raised inside ntdll, and
                // dispatched to the same frames from the original context.
                // The exception CONTEXT is the raise site (what continuing
                // would have resumed); the walk starts from it again.
                read_context_64(memory, exception_ctx_addr, cpu);
                // ExceptionAddress is ntdll code, not the raise site.
                const u64 ntdll_code =
                    thunk_registry.get_thunk_address("ntdll.dll", "RtlUnwindEx").value_or(0);
                return dispatch_x64_exception(
                    cpu, memory, thunk_registry, process_mgr, 0xC0000025u,
                    0x1u | EXCEPTION_SOFTWARE_ORIGINATE_FLAG, 0, nullptr, cpu.rip,
                    module_mgr, ntdll_code);
            }
            if (disposition == 0) { // ExceptionContinueExecution
                if (!run_vectored_continue_handlers_64(cpu, memory, thunk_registry,
                                                       process_mgr, eptrs_addr, scratch)) {
                    return true;
                }
                read_context_64(memory, exception_ctx_addr, cpu);
                VITREO_TRACE("[seh64] continue execution at rip=0x%llx rsp=0x%llx\n",
                             static_cast<unsigned long long>(cpu.rip),
                             static_cast<unsigned long long>(cpu.sp()));
                return true;
            }
            // ExceptionContinueSearch (1) — try next frame
        }

        // Move to caller's frame (CONTEXT already holds restored nonvols).
        if (caller_rip == CALLBACK_RETURN_SENTINEL) {
            Seh64HostCallbackFrame interrupted{};
            if (seh64_interrupted_guest_frame(&interrupted) &&
                interrupted.rip != 0 &&
                interrupted.rip != CALLBACK_RETURN_SENTINEL) {
                std::fprintf(stderr,
                    "[seh64] resume past callback sentinel -> rip=0x%llx rsp=0x%llx depth=%u\n",
                    static_cast<unsigned long long>(interrupted.rip),
                    static_cast<unsigned long long>(interrupted.rsp), depth);
                walk_rip = interrupted.rip;
                walk_rsp = interrupted.rsp;
                walk_rip_is_resume = true;
                (void)memory.write_u64(ctx_addr + CTX64_RIP, interrupted.rip);
                (void)memory.write_u64(ctx_addr + CTX64_RSP, interrupted.rsp);
                (void)memory.write_u64(ctx_addr + CTX64_RAX, interrupted.gpr[0]);
                (void)memory.write_u64(ctx_addr + CTX64_RCX, interrupted.gpr[1]);
                (void)memory.write_u64(ctx_addr + CTX64_RDX, interrupted.gpr[2]);
                (void)memory.write_u64(ctx_addr + CTX64_RBX, interrupted.gpr[3]);
                (void)memory.write_u64(ctx_addr + CTX64_RBP, interrupted.gpr[5]);
                (void)memory.write_u64(ctx_addr + CTX64_RSI, interrupted.gpr[6]);
                (void)memory.write_u64(ctx_addr + CTX64_RDI, interrupted.gpr[7]);
                (void)memory.write_u64(ctx_addr + CTX64_R8,  interrupted.gpr[8]);
                (void)memory.write_u64(ctx_addr + CTX64_R9,  interrupted.gpr[9]);
                (void)memory.write_u64(ctx_addr + CTX64_R10, interrupted.gpr[10]);
                (void)memory.write_u64(ctx_addr + CTX64_R11, interrupted.gpr[11]);
                (void)memory.write_u64(ctx_addr + CTX64_R12, interrupted.gpr[12]);
                (void)memory.write_u64(ctx_addr + CTX64_R13, interrupted.gpr[13]);
                (void)memory.write_u64(ctx_addr + CTX64_R14, interrupted.gpr[14]);
                (void)memory.write_u64(ctx_addr + CTX64_R15, interrupted.gpr[15]);
                continue;
            }
        }
        if (caller_rip == PROCESS_RETURN_SENTINEL) {
            std::fprintf(stderr,
                "[seh64] end of stack (process sentinel) rip=0x%llx depth=%u handler=0x%llx\n",
                static_cast<unsigned long long>(walk_rip), depth,
                static_cast<unsigned long long>(handler_addr));
            break;
        }
        if (caller_rip == 0 || !is_canonical_x64_addr(caller_rip)) {
            std::fprintf(stderr,
                "[seh64] walk stop: caller_rip=0x%llx rip=0x%llx depth=%u handler=0x%llx\n",
                static_cast<unsigned long long>(caller_rip),
                static_cast<unsigned long long>(walk_rip), depth,
                static_cast<unsigned long long>(handler_addr));
            break;
        }
        walk_rip = caller_rip;
        walk_rsp = caller_rsp;
        walk_rip_is_resume = false;
    }

    VITREO_TRACE("[seh64] No handler caught exception 0x%08x at RIP=0x%llx\n",
                 raise_code, static_cast<unsigned long long>(exception_address));
    std::fprintf(stderr,
        "[seh64] unhandled code=0x%08x exception_rip=0x%llx last_walk_rip=0x%llx\n",
        raise_code,
        static_cast<unsigned long long>(exception_address),
        static_cast<unsigned long long>(walk_rip));
    // A PCTRACE ring (VITREO_PCTRACE_RING) holds the instructions leading here.
    vitreo_pctrace_dump_ring("unhandled-x64-exception");
    for (u32 i = 0; i < walked_count; ++i) {
        std::fprintf(stderr,
            "[seh64-frame] depth=%u rip=0x%llx rsp=0x%llx runtime_func=0x%llx\n",
            i, static_cast<unsigned long long>(walked[i].rip),
            static_cast<unsigned long long>(walked[i].rsp),
            static_cast<unsigned long long>(walked[i].runtime_func));
    }

    // Learn (SetUnhandledExceptionFilter): once frame-based handling finds no
    // handler, the process's top-level filter runs on the faulting thread with
    // the same EXCEPTION_POINTERS. EXCEPTION_CONTINUE_EXECUTION (-1) resumes
    // from the possibly edited context; anything else leaves the exception
    // unhandled and the process ends.
    //
    // Only the 32-bit dispatcher did this. On x64 the filter was never called,
    // so every crash reporter a 64-bit guest installs was silently skipped,
    // and a crash left no trace in the guest's own log.
    {
        static thread_local bool in_top_level_filter = false;
        const u64 filter = winapi::unhandled_exception_filter_guest();
        GuestAddr filter_region = 0;
        usize filter_size = 0;
        MemoryProtection filter_prot{};
        const bool filter_executable =
            filter != 0 &&
            memory.query_region(static_cast<GuestAddr>(filter),
                                filter_region, filter_size, filter_prot) &&
            (static_cast<u8>(filter_prot) &
             static_cast<u8>(MemoryProtection::Execute)) != 0;
        if (filter_executable && !in_top_level_filter) {
            in_top_level_filter = true;
            cpu.set_sp(scratch);
            u64 filter_args[1] = {eptrs_addr};
            const u64 disposition = run_guest_callback(
                cpu, memory, thunk_registry, process_mgr, filter, filter_args,
                1, true);
            in_top_level_filter = false;
            std::fprintf(stderr,
                "[seh64] UnhandledExceptionFilter handler=0x%llx "
                "code=0x%08x ret=0x%llx\n",
                static_cast<unsigned long long>(filter), raise_code,
                static_cast<unsigned long long>(disposition));
            if (process_mgr.has_exited()) {
                return true;
            }
            if ((disposition & 0xFFFFFFFFull) == 0xFFFFFFFFull) {
                if (run_vectored_continue_handlers_64(cpu, memory, thunk_registry,
                                                      process_mgr, eptrs_addr, scratch)) {
                    read_context_64(memory, exception_ctx_addr, cpu);
                }
                return true;
            }
        }
    }

    // No handler consumed the exception. Restore the pre-dispatch context so
    // compatibility paths that continue execution do not run with a scratch
    // stack pointer or clobbered volatile registers.
    cpu = pre_dispatch_cpu;
    return false;
}

PendingUnwind64Action apply_pending_unwind_64(cpu::CpuState& cpu,
                                              GuestMemory& memory,
                                              winapi::ModuleManager* module_mgr,
                                              winapi::ProcessManager& process_mgr,
                                              bool in_callback) {
    if (!cpu.pending_unwind.active) return PendingUnwind64Action::Resume;
    if (cpu.pending_unwind.level_walked) {
        if (in_callback) return PendingUnwind64Action::LeaveLevel;
        // A thread's outermost level has no host-callback sentinel for a
        // walk to stop at; nothing encloses it to hand the unwind to.
        std::fprintf(stderr,
            "[seh64] RtlUnwindEx: unwind left the outermost level at a "
            "callback boundary (target=0x%llx); ending the process\n",
            static_cast<unsigned long long>(cpu.pending_unwind.target_frame));
        cpu.pending_unwind.clear();
        process_mgr.exit_process(winapi::STATUS_BAD_STACK_CODE);
        return PendingUnwind64Action::Stop;
    }

    // Language handlers this walk calls may resolve unwind data through a
    // RtlInstallFunctionTableCallback, which runs guest code on this thread.
    winapi::Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const winapi::RtlUnwind64Result result = winapi::continue_pending_unwind64(
        marshaler, module_mgr, &process_mgr);
    if (process_mgr.has_exited()) return PendingUnwind64Action::Stop;
    switch (result) {
    case winapi::RtlUnwind64Result::Transferred:
        return PendingUnwind64Action::Resume;
    case winapi::RtlUnwind64Result::Deferred:
        if (in_callback) return PendingUnwind64Action::LeaveLevel;
        std::fprintf(stderr,
            "[seh64] RtlUnwindEx: outermost level reached a callback "
            "boundary; ending the process\n");
        cpu.pending_unwind.clear();
        process_mgr.exit_process(winapi::STATUS_BAD_STACK_CODE);
        return PendingUnwind64Action::Stop;
    case winapi::RtlUnwind64Result::Raised:
        return PendingUnwind64Action::Raise;
    case winapi::RtlUnwind64Result::Interrupted:
        // A handler's own control transfer (or nested raise) owns the CPU.
        return cpu.pending_raise.active ? PendingUnwind64Action::Raise
                                        : PendingUnwind64Action::Resume;
    case winapi::RtlUnwind64Result::Terminated:
        return PendingUnwind64Action::Stop;
    }
    return PendingUnwind64Action::Stop;
}

}  // namespace vitreo::cli
