// SPDX-License-Identifier: LGPL-2.1-only

/// @file unwind64.cpp
/// @brief x64 RtlUnwindEx: frame walk, handler protocol, final transfer.
///
/// See unwind64.hpp. The algorithm follows Microsoft Learn, "x64 exception
/// handling" (unwind procedure, language-specific handler,
/// DISPATCHER_CONTEXT), RtlUnwindEx, RtlVirtualUnwind and RtlRestoreContext:
///
///  - The walk starts at RtlUnwindEx's caller (RtlCaptureContext semantics:
///    Rip = return address, Rsp past it).
///  - A NULL ExceptionRecord becomes a local STATUS_UNWIND record whose
///    ExceptionAddress is that return address.
///  - Each frame with a RUNTIME_FUNCTION is virtually unwound with
///    UNW_FLAG_UHANDLER; one without is a leaf (return address at [Rsp]).
///  - An establisher frame below StackLimit, above StackBase, not 8-byte
///    aligned, or above TargetFrame is STATUS_BAD_STACK.
///  - The language handler gets ExceptionFlags | EXCEPTION_UNWINDING
///    (| EXCEPTION_EXIT_UNWIND for a NULL TargetFrame, | EXCEPTION_TARGET_UNWIND
///    on the target frame, | EXCEPTION_COLLIDED_UNWIND when re-entered after a
///    collision), the frame's own context with Rax = ReturnValue, and a
///    DISPATCHER_CONTEXT whose TargetIp is the unwind target.
///  - ExceptionContinueSearch goes on; ExceptionCollidedUnwind resumes from
///    the frame the returned DISPATCHER_CONTEXT names; anything else is
///    STATUS_INVALID_DISPOSITION.
///  - At TargetFrame: Rax = ReturnValue, Rip = TargetIp unless the record is
///    STATUS_UNWIND_CONSOLIDATE, then RtlRestoreContext(context, record).
///  - No TargetFrame found (or an exit unwind): the record is raised as a
///    second-chance exception, which ends the process with its code.

#include "winapi/kernel32/thread/unwind64.hpp"

#include "winapi/common/c_specific_handler.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/context64.hpp"
#include "winapi/kernel32/thread/restore_context64.hpp"
#include "core/cpu/state/context64_record.hpp"
#include "core/cpu/seh/seh32.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include <vitreo/diagnostics.hpp>
#include <vitreo/guest_sentinels.hpp>

#include <atomic>
#include <cstdio>
#include <vector>

namespace vitreo::winapi {
namespace {

// winnt.h AMD64 CONTEXT.
constexpr u32 kCtxSize = 0x4D0;
constexpr u32 kCtxRax = 0x78;
constexpr u32 kCtxRsp = 0x98;
constexpr u32 kCtxRip = 0xF8;

// winnt.h EXCEPTION_RECORD (64-bit).
constexpr u32 kErecSize = 0x98;
constexpr u32 kErecCode = 0x00;
constexpr u32 kErecFlags = 0x04;
constexpr u32 kErecAddress = 0x10;

// winnt.h DISPATCHER_CONTEXT (AMD64).
constexpr u32 kDcSize = 0x50;
constexpr u32 kDcControlPc = 0x00;
constexpr u32 kDcImageBase = 0x08;
constexpr u32 kDcFunctionEntry = 0x10;
constexpr u32 kDcEstablisherFrame = 0x18;
constexpr u32 kDcTargetIp = 0x20;
constexpr u32 kDcContextRecord = 0x28;
constexpr u32 kDcLanguageHandler = 0x30;
constexpr u32 kDcHandlerData = 0x38;
constexpr u32 kDcHistoryTable = 0x40;
constexpr u32 kDcScopeIndex = 0x48;

// winnt.h UNWIND_HISTORY_TABLE (AMD64).
constexpr u32 kHistoryTableSize = 0xD8;

// winnt.h UNW_FLAG_*.
constexpr u32 UNW_FLAG_NHANDLER = 0x0;
constexpr u32 UNW_FLAG_UHANDLER = 0x2;

// excpt.h EXCEPTION_DISPOSITION.
constexpr u32 kExceptionContinueSearch = 1;
constexpr u32 kExceptionCollidedUnwind = 3;

// TEB (NT_TIB64) stack bounds.
constexpr u32 kTebStackBase = 0x08;
constexpr u32 kTebStackLimit = 0x10;
constexpr u32 kTebDeallocationStack = 0x1478;

// A walk is bounded by the stack itself; this only guards a corrupt chain.
constexpr u32 kMaxFrames = 1u << 16;

[[nodiscard]] bool is_canonical(u64 addr) {
    const u64 upper = addr >> 47;
    return upper == 0 || upper == 0x1FFFFu;
}

[[nodiscard]] bool copy_context(GuestMemory& memory, GuestAddr to, GuestAddr from) {
    if (to == from) return true;
    u8 bytes[kCtxSize];
    return memory.read_bytes(from, bytes, kCtxSize).is_ok() &&
           memory.write_bytes(to, bytes, kCtxSize).is_ok();
}

void write_dispatcher_context(GuestMemory& memory, GuestAddr dc, u64 control_pc,
                              u64 image_base, u64 function_entry,
                              u64 establisher, u64 target_ip, GuestAddr context,
                              u64 handler, u64 handler_data, u64 history,
                              u32 scope_index) {
    u8 zeros[kDcSize] = {};
    (void)memory.write_bytes(dc, zeros, kDcSize);
    (void)memory.write_u64(dc + kDcControlPc, control_pc);
    (void)memory.write_u64(dc + kDcImageBase, image_base);
    (void)memory.write_u64(dc + kDcFunctionEntry, function_entry);
    (void)memory.write_u64(dc + kDcEstablisherFrame, establisher);
    (void)memory.write_u64(dc + kDcTargetIp, target_ip);
    (void)memory.write_u64(dc + kDcContextRecord, context);
    (void)memory.write_u64(dc + kDcLanguageHandler, handler);
    (void)memory.write_u64(dc + kDcHandlerData, handler_data);
    (void)memory.write_u64(dc + kDcHistoryTable, history);
    (void)memory.write_u32(dc + kDcScopeIndex, scope_index);
}

/// The flags RtlUnwindEx writes into the record before calling a handler.
/// The record's other flags are kept, except EXCEPTION_NONCONTINUABLE, which
/// describes the search the unwind ends (the dispatcher's existing contract).
[[nodiscard]] u32 unwind_flags(u32 original, const Unwind64Target& target) {
    u32 flags = (original & ~(EXCEPTION_NONCONTINUABLE_BIT |
                              EXCEPTION_TARGET_UNWIND_BIT |
                              EXCEPTION_COLLIDED_UNWIND_BIT)) |
                EXCEPTION_UNWINDING_BIT;
    if (target.frame == 0) flags |= EXCEPTION_EXIT_UNWIND_BIT;
    return flags;
}

}  // namespace

void write_status_unwind_record64(Marshaler& m, GuestAddr record, u64 address) {
    u8 zeros[kErecSize] = {};
    (void)m.memory().write_bytes(record, zeros, kErecSize);
    (void)m.memory().write_u32(record + kErecCode, STATUS_UNWIND_CODE);
    (void)m.memory().write_u64(record + kErecAddress, address);
}

Unwind64Stop walk_unwind64(Marshaler& m, ModuleManager& modules,
                           const Unwind64Scratch& s, Unwind64Target& target,
                           bool start_is_return_address,
                           const Unwind64HandlerCall& call_handler) {
    cpu::CpuState& cpu = *m.cpu();
    GuestMemory& memory = m.memory();
    const ThunkRegistry& thunks = modules.registry();

    // RtlpGetStackLimits: the thread's NT_TIB. Vitreo commits the whole
    // reserve, so the low bound is DeallocationStack whenever it is lower than
    // the recorded StackLimit. A stack the TEB does not describe (the walk
    // starting outside it) is not checked rather than rejected wholesale.
    const GuestAddr teb = static_cast<GuestAddr>(cpu.seg.gs_desc.base);
    u64 stack_high = 0;
    u64 stack_low = 0;
    if (teb != 0) {
        stack_high = memory.read_u64(teb + kTebStackBase).value_or(0);
        stack_low = memory.read_u64(teb + kTebStackLimit).value_or(0);
        const u64 deallocation = memory.read_u64(teb + kTebDeallocationStack).value_or(0);
        if (deallocation != 0 && deallocation < stack_low) stack_low = deallocation;
    }
    const u64 start_rsp = memory.read_u64(s.cursor + kCtxRsp).value_or(0);
    const bool limits_known = stack_high > stack_low && start_rsp >= stack_low &&
                              start_rsp <= stack_high;

    u32 original_flags = memory.read_u32(target.record + kErecFlags).value_or(0);
    bool bias = start_is_return_address;
    u64 establisher = 0;

    auto bad_establisher = [&](u64 frame) {
        return (limits_known && (frame < stack_low || frame > stack_high)) ||
               (frame & 7u) != 0 ||
               (target.frame != 0 && frame > target.frame);
    };

    for (u32 depth = 0; depth < kMaxFrames; ++depth) {
        u64 control_pc = memory.read_u64(s.cursor + kCtxRip).value_or(0);
        if (control_pc == kCallbackReturnSentinel) {
            return Unwind64Stop::CallbackBoundary;
        }
        if (control_pc == 0 || control_pc == kProcessReturnSentinel ||
            !is_canonical(control_pc)) {
            return Unwind64Stop::EndOfStack;
        }

        // A caller frame's Rip is a return address: the call is the byte
        // before it, and a noreturn call that ends a function would otherwise
        // be looked up in the next one (exception_dispatch.cpp's search pass
        // does the same).
        GuestAddr image_base = 0;
        GuestAddr function_entry = modules.lookup_runtime_function_entry(
            static_cast<GuestAddr>(bias ? control_pc - 1 : control_pc), &image_base);
        bias = true;
        // A thunk stub is host code with no prologue; whatever .pdata of the
        // synthesized module covers it does not describe it. It is reached by
        // `call`, so it unwinds as a leaf.
        if (function_entry != 0 &&
            thunks.is_in_thunk_region(static_cast<GuestAddr>(control_pc))) {
            function_entry = 0;
        }

        if (function_entry == 0) {
            // Learn: no RUNTIME_FUNCTION is a leaf function; its return
            // address is at [Rsp] and it has no establisher frame.
            const u64 rsp = memory.read_u64(s.cursor + kCtxRsp).value_or(0);
            const auto ret = memory.read_u64(rsp);
            if (ret.is_err()) {
                std::fprintf(stderr,
                    "[seh64] RtlUnwindEx: leaf frame rip=0x%llx rsp=0x%llx unreadable\n",
                    static_cast<unsigned long long>(control_pc),
                    static_cast<unsigned long long>(rsp));
                return Unwind64Stop::EndOfStack;
            }
            (void)memory.write_u64(s.cursor + kCtxRip, ret.value());
            (void)memory.write_u64(s.cursor + kCtxRsp, rsp + 8);
            continue;
        }

        // The frame's own context is what its handler sees and, if it is the
        // target, what resumes; the cursor becomes its caller's.
        if (!copy_context(memory, s.frame, s.cursor)) return Unwind64Stop::EndOfStack;
        GuestAddr handler = 0;
        (void)memory.write_u64(s.handler_data_out, 0);
        (void)memory.write_u64(s.establisher_out, 0);
        if (!unwind_context64(m, image_base, static_cast<GuestAddr>(control_pc),
                              function_entry, s.cursor, s.handler_data_out,
                              s.establisher_out, &handler, UNW_FLAG_UHANDLER)) {
            std::fprintf(stderr,
                "[seh64] RtlUnwindEx: unreadable UNWIND_INFO rip=0x%llx "
                "function_entry=0x%llx\n",
                static_cast<unsigned long long>(control_pc),
                static_cast<unsigned long long>(function_entry));
            return Unwind64Stop::EndOfStack;
        }
        establisher = memory.read_u64(s.establisher_out).value_or(0);
        u64 handler_data = memory.read_u64(s.handler_data_out).value_or(0);

        if (bad_establisher(establisher)) {
            std::fprintf(stderr,
                "[seh64] RtlUnwindEx: STATUS_BAD_STACK establisher=0x%llx "
                "target=0x%llx stack=[0x%llx,0x%llx] rip=0x%llx\n",
                static_cast<unsigned long long>(establisher),
                static_cast<unsigned long long>(target.frame),
                static_cast<unsigned long long>(stack_low),
                static_cast<unsigned long long>(stack_high),
                static_cast<unsigned long long>(control_pc));
            return Unwind64Stop::BadStack;
        }

        if (handler != 0) {
            u64 history = s.history_table;
            u32 scope_index = 0;
            u32 collided = 0;
            for (;;) {
                const bool is_target = target.frame != 0 && establisher == target.frame;
                const u32 flags = unwind_flags(original_flags, target) | collided |
                                  (is_target ? EXCEPTION_TARGET_UNWIND_BIT : 0u);
                (void)memory.write_u32(target.record + kErecFlags, flags);
                (void)memory.write_u64(s.frame + kCtxRax, target.return_value);
                write_dispatcher_context(memory, s.dispatcher, control_pc, image_base,
                                         function_entry, establisher, target.ip,
                                         s.frame, handler, handler_data, history,
                                         scope_index);
                const u64 args[4] = {target.record, establisher, s.frame, s.dispatcher};
                VITREO_TRACE(
                    "[seh64] unwind handler=0x%llx rip=0x%llx establisher=0x%llx "
                    "flags=0x%x target=0x%llx\n",
                    static_cast<unsigned long long>(handler),
                    static_cast<unsigned long long>(control_pc),
                    static_cast<unsigned long long>(establisher), flags,
                    static_cast<unsigned long long>(target.frame));
                const std::optional<u64> result = call_handler(handler, args);
                if (!result.has_value()) return Unwind64Stop::Interrupted;
                const u32 disposition = static_cast<u32>(*result);

                if (cpu.pending_unwind.active) {
                    // The handler called RtlUnwindEx itself (a __finally that
                    // longjmps, a nested __except): its walk went through the
                    // handler's frames to the callback boundary. On Windows
                    // the next frame is RtlpExecuteHandlerForUnwind, whose
                    // handler answers ExceptionCollidedUnwind with this
                    // frame: the new unwind takes over here, re-entering this
                    // handler with EXCEPTION_COLLIDED_UNWIND and the scope
                    // index it left in the DISPATCHER_CONTEXT.
                    const auto pending = cpu.pending_unwind;
                    cpu.pending_unwind.clear();
                    target.frame = pending.target_frame;
                    target.ip = pending.target_ip;
                    target.return_value = pending.return_value;
                    if (pending.caller_sp != 0 &&
                        (target.stack_floor == 0 || pending.caller_sp < target.stack_floor)) {
                        target.stack_floor = pending.caller_sp;
                    }
                    if (pending.exception_record != 0) {
                        target.record = static_cast<GuestAddr>(pending.exception_record);
                    } else {
                        write_status_unwind_record64(m, s.local_record,
                                                     pending.null_record_address);
                        target.record = s.local_record;
                    }
                    original_flags = memory.read_u32(target.record + kErecFlags).value_or(0);
                    scope_index = memory.read_u32(s.dispatcher + kDcScopeIndex).value_or(0);
                    collided = EXCEPTION_COLLIDED_UNWIND_BIT;
                    if (bad_establisher(establisher)) return Unwind64Stop::BadStack;
                    continue;
                }
                if (disposition == kExceptionContinueSearch) break;
                if (disposition == kExceptionCollidedUnwind) {
                    // The handler returned the DISPATCHER_CONTEXT of the unwind
                    // it collided with: go on from the frame it names, with
                    // that frame's handler, data and scope index.
                    control_pc = memory.read_u64(s.dispatcher + kDcControlPc).value_or(0);
                    image_base = static_cast<GuestAddr>(
                        memory.read_u64(s.dispatcher + kDcImageBase).value_or(0));
                    function_entry = static_cast<GuestAddr>(
                        memory.read_u64(s.dispatcher + kDcFunctionEntry).value_or(0));
                    const GuestAddr context = static_cast<GuestAddr>(
                        memory.read_u64(s.dispatcher + kDcContextRecord).value_or(0));
                    establisher = memory.read_u64(s.dispatcher + kDcEstablisherFrame).value_or(0);
                    handler = static_cast<GuestAddr>(
                        memory.read_u64(s.dispatcher + kDcLanguageHandler).value_or(0));
                    handler_data = memory.read_u64(s.dispatcher + kDcHandlerData).value_or(0);
                    history = memory.read_u64(s.dispatcher + kDcHistoryTable).value_or(0);
                    scope_index = memory.read_u32(s.dispatcher + kDcScopeIndex).value_or(0);
                    GuestAddr ignored = 0;
                    if (context == 0 || !copy_context(memory, s.frame, context) ||
                        !copy_context(memory, s.cursor, s.frame) ||
                        !unwind_context64(m, image_base, static_cast<GuestAddr>(control_pc),
                                          function_entry, s.cursor, s.handler_data_out,
                                          s.establisher_out, &ignored, UNW_FLAG_NHANDLER)) {
                        return Unwind64Stop::EndOfStack;
                    }
                    collided = EXCEPTION_COLLIDED_UNWIND_BIT;
                    if (handler == 0) break;
                    continue;
                }
                std::fprintf(stderr,
                    "[seh64] RtlUnwindEx: handler 0x%llx returned disposition %u "
                    "-> STATUS_INVALID_DISPOSITION\n",
                    static_cast<unsigned long long>(handler), disposition);
                return Unwind64Stop::InvalidDisposition;
            }
        }

        if (target.frame != 0 && establisher == target.frame) {
            (void)memory.write_u64(s.frame + kCtxRax, target.return_value);
            const u32 code = memory.read_u32(target.record + kErecCode).value_or(0);
            if (code != STATUS_UNWIND_CONSOLIDATE_CODE) {
                (void)memory.write_u64(s.frame + kCtxRip, target.ip);
            }
            return Unwind64Stop::TargetReached;
        }
        if (limits_known && establisher >= stack_high) {
            return Unwind64Stop::EndOfStack;
        }
    }
    std::fprintf(stderr, "[seh64] RtlUnwindEx: more than %u frames\n", kMaxFrames);
    return Unwind64Stop::EndOfStack;
}

namespace {

/// RtlRaiseStatus from inside RtlUnwindEx: a noncontinuable exception raised
/// from the unwinder, dispatched from its caller once the thunk returns.
void raise_from_unwinder(cpu::CpuState& cpu, u32 code, u32 nargs = 0,
                         u64 arg0 = 0, u64 arg1 = 0) {
    cpu.pending_raise.active = true;
    cpu.pending_raise.from_helper = false;
    cpu.pending_raise.code = code;
    cpu.pending_raise.flags = EXCEPTION_NONCONTINUABLE_BIT;
    cpu.pending_raise.num_args = nargs;
    cpu.pending_raise.args[0] = arg0;
    cpu.pending_raise.args[1] = arg1;
}

/// Where a handler's frame starts: below the walk's scratch and below
/// anything a collided unwind still needs.
[[nodiscard]] u64 handler_stack_top(u64 scratch_base, u64 stack_floor) {
    constexpr u64 kMargin = 0x100;
    if (stack_floor != 0 && stack_floor - kMargin < scratch_base) {
        return (stack_floor - kMargin) & ~u64{0xF};
    }
    return scratch_base;
}

/// The thunk's CPU as it entered, with the exception bookkeeping it carried.
void restore_entry(cpu::CpuState& cpu, const cpu::CpuState& entry) {
    const auto pending_raise = cpu.pending_raise;
    cpu = entry;
    cpu.pending_raise = pending_raise;
    cpu.pending_unwind.clear();
    cpu.thunk_control_transfer = false;
}

}  // namespace

RtlUnwind64Result rtl_unwind_ex64(Marshaler& m, ModuleManager* modules,
                                  ProcessManager* processes,
                                  const RtlUnwind64Request& request) {
    cpu::CpuState* cpu_ptr = m.cpu();
    if (cpu_ptr == nullptr) return RtlUnwind64Result::Interrupted;
    cpu::CpuState& cpu = *cpu_ptr;
    GuestMemory& memory = m.memory();

    cpu.pending_unwind.clear();
    const cpu::CpuState entry = cpu;
    const u64 entry_sp = cpu.sp();
    const auto return_address = memory.read_u64(entry_sp);
    if (return_address.is_err()) {
        // RtlCaptureContext reading its own return address faults.
        raise_from_unwinder(cpu, cpu::STATUS_ACCESS_VIOLATION, 2, 0, entry_sp);
        return RtlUnwind64Result::Raised;
    }
    const SehGuestCall& guest_call = seh_guest_call();
    if (modules == nullptr || !guest_call) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            std::fprintf(stderr,
                "[vitreo] UNIMPLEMENTED ntdll!RtlUnwindEx: no module table or "
                "guest-call hook to walk frames with\n");
        }
        raise_from_unwinder(cpu, STATUS_BAD_STACK_CODE);
        return RtlUnwind64Result::Raised;
    }

    // Scratch below everything the caller still owns. Handlers run below it.
    constexpr u64 kAlign = 0xF;
    auto align16 = [](u64 v) { return (v + kAlign) & ~kAlign; };
    const u64 cursor_size = request.original_context != 0 ? 0 : align16(kCtxSize);
    const u64 total = cursor_size + align16(kCtxSize) + align16(kErecSize) +
                      align16(kDcSize + 16) + align16(kHistoryTableSize) + 0x40;
    // Everything an earlier level of this unwind still needs (its record,
    // a jump buffer) lies at or above request.stack_floor.
    const u64 scratch_top =
        request.stack_floor != 0 && request.stack_floor - 0x100 < entry_sp
            ? request.stack_floor - 0x100
            : entry_sp;
    const u64 base = (scratch_top - total) & ~kAlign;
    Unwind64Scratch s;
    u64 at = base;
    if (request.original_context != 0) {
        s.cursor = request.original_context;
    } else {
        s.cursor = at;
        at += align16(kCtxSize);
    }
    s.frame = at;                 at += align16(kCtxSize);
    s.local_record = at;          at += align16(kErecSize);
    s.dispatcher = at;
    s.handler_data_out = at + kDcSize;
    s.establisher_out = at + kDcSize + 8;
    at += align16(kDcSize + 16);
    s.history_table = request.history_table != 0 ? request.history_table : at;
    {
        u8 zeros[kHistoryTableSize] = {};
        (void)memory.write_bytes(at, zeros, kHistoryTableSize);
    }

    // RtlCaptureContext in RtlUnwindEx: the caller's state after the return.
    {
        cpu::CpuState caller = entry;
        caller.materialize_lazy_flags();
        caller.rip = return_address.value();
        caller.set_sp(entry_sp + 8);
        u8 zeros[kCtxSize] = {};
        if (memory.write_bytes(s.cursor, zeros, kCtxSize).is_err() ||
            cpu::store_context64(memory, s.cursor, caller,
                                 cpu::kContext64Full | cpu::kContext64Segments) !=
                cpu::Context64Status::Ok) {
            raise_from_unwinder(cpu, cpu::STATUS_ACCESS_VIOLATION, 2, 1, s.cursor);
            return RtlUnwind64Result::Raised;
        }
    }

    Unwind64Target target;
    target.frame = request.target_frame;
    target.ip = request.target_ip;
    target.return_value = request.return_value;
    target.record = request.record;
    target.stack_floor = request.stack_floor;
    const u64 null_address = request.null_record_address != 0
                                 ? request.null_record_address
                                 : return_address.value();
    if (target.record == 0) {
        write_status_unwind_record64(m, s.local_record, null_address);
        target.record = s.local_record;
    }

    VITREO_TRACE(
        "[seh64] RtlUnwindEx(frame=0x%llx ip=0x%llx record=0x%llx ret=0x%llx) "
        "from 0x%llx\n",
        static_cast<unsigned long long>(request.target_frame),
        static_cast<unsigned long long>(request.target_ip),
        static_cast<unsigned long long>(request.record),
        static_cast<unsigned long long>(request.return_value),
        static_cast<unsigned long long>(return_address.value()));

    const Unwind64HandlerCall call_handler =
        [&](GuestAddr handler, const u64* args) -> std::optional<u64> {
            cpu.set_sp(handler_stack_top(base, target.stack_floor));
            bool completed = false;
            const u64 result = guest_call(cpu, handler, args, 4, &completed);
            if (processes != nullptr && processes->has_exited()) return std::nullopt;
            if (cpu.pending_raise.active && cpu.pending_raise.from_helper) {
                return std::nullopt;
            }
            return result;
        };
    const Unwind64Stop stop =
        walk_unwind64(m, *modules, s, target, /*start_is_return_address=*/true,
                      call_handler);

    switch (stop) {
    case Unwind64Stop::TargetReached: {
        restore_entry(cpu, entry);
        // RtlRestoreContext(ContextRecord, ExceptionRecord). The frames from
        // the caller up stay live until then (the throw object of a
        // consolidation lives in them), so the ceiling is the entry RSP.
        if (!restore_context64_with_record(
                m, *modules, s.frame, target.record,
                target.stack_floor != 0 && target.stack_floor < entry_sp
                    ? target.stack_floor
                    : entry_sp)) {
            std::fprintf(stderr,
                "[seh64] RtlUnwindEx: cannot restore the target context "
                "(record=0x%llx)\n",
                static_cast<unsigned long long>(target.record));
            restore_entry(cpu, entry);
            raise_from_unwinder(cpu, cpu::STATUS_ACCESS_VIOLATION, 2, 0, target.record);
            return RtlUnwind64Result::Raised;
        }
        VITREO_TRACE("[seh64] RtlUnwindEx: resume rip=0x%llx rsp=0x%llx\n",
                     static_cast<unsigned long long>(cpu.rip),
                     static_cast<unsigned long long>(cpu.sp()));
        cpu.thunk_control_transfer = true;
        return RtlUnwind64Result::Transferred;
    }
    case Unwind64Stop::CallbackBoundary:
        restore_entry(cpu, entry);
        cpu.pending_unwind.active = true;
        cpu.pending_unwind.target_frame = target.frame;
        cpu.pending_unwind.target_ip = target.ip;
        cpu.pending_unwind.return_value = target.return_value;
        cpu.pending_unwind.exception_record =
            target.record == s.local_record ? 0 : target.record;
        cpu.pending_unwind.null_record_address =
            target.record == s.local_record
                ? memory.read_u64(s.local_record + kErecAddress).value_or(null_address)
                : 0;
        cpu.pending_unwind.caller_sp =
            target.stack_floor != 0 && target.stack_floor < entry_sp
                ? target.stack_floor
                : entry_sp;
        cpu.pending_unwind.level_walked = true;
        cpu.gpr.rax = target.return_value;
        return RtlUnwind64Result::Deferred;
    case Unwind64Stop::EndOfStack: {
        // ZwRaiseException(ExceptionRecord, Context, FirstChance = FALSE):
        // no frame handler runs again; with no debugger the process ends
        // with the exception code.
        const u32 code = memory.read_u32(target.record + kErecCode).value_or(STATUS_UNWIND_CODE);
        std::fprintf(stderr,
            "[vitreo] RtlUnwindEx: %s; second-chance exception 0x%08x ends the "
            "process\n",
            target.frame == 0 ? "exit unwind reached the end of the stack"
                              : "target frame is not on the stack",
            code);
        restore_entry(cpu, entry);
        if (processes == nullptr) {
            raise_from_unwinder(cpu, STATUS_BAD_STACK_CODE);
            return RtlUnwind64Result::Raised;
        }
        processes->exit_process(code);
        return RtlUnwind64Result::Terminated;
    }
    case Unwind64Stop::BadStack:
        restore_entry(cpu, entry);
        raise_from_unwinder(cpu, STATUS_BAD_STACK_CODE);
        return RtlUnwind64Result::Raised;
    case Unwind64Stop::InvalidDisposition:
        restore_entry(cpu, entry);
        raise_from_unwinder(cpu, STATUS_INVALID_DISPOSITION_CODE);
        return RtlUnwind64Result::Raised;
    case Unwind64Stop::Interrupted:
        return RtlUnwind64Result::Interrupted;
    }
    return RtlUnwind64Result::Interrupted;
}

void rtl_unwind_export64(Marshaler& m, ModuleManager* modules,
                         ProcessManager* processes, bool is_ex) {
    RtlUnwind64Request request;
    request.target_frame = m.arg(0);
    request.target_ip = m.arg(1);
    request.record = m.arg_ptr(2);
    request.return_value = m.arg(3);
    if (is_ex) {
        request.original_context = m.arg_ptr(4);
        request.history_table = m.arg_ptr(5);
    }
    (void)rtl_unwind_ex64(m, modules, processes, request);
}

RtlUnwind64Result continue_pending_unwind64(Marshaler& m, ModuleManager* modules,
                                            ProcessManager* processes) {
    cpu::CpuState* cpu = m.cpu();
    if (cpu == nullptr || !cpu->pending_unwind.active) {
        return RtlUnwind64Result::Interrupted;
    }
    const auto pending = cpu->pending_unwind;
    cpu->pending_unwind.clear();
    RtlUnwind64Request request;
    request.target_frame = pending.target_frame;
    request.target_ip = pending.target_ip;
    request.record = static_cast<GuestAddr>(pending.exception_record);
    request.return_value = pending.return_value;
    request.null_record_address = pending.null_record_address;
    request.stack_floor = pending.caller_sp;
    const RtlUnwind64Result result = rtl_unwind_ex64(m, modules, processes, request);
    // The run loop resumes at rip itself; there is no thunk epilogue here.
    cpu->thunk_control_transfer = false;
    return result;
}

} // namespace vitreo::winapi
