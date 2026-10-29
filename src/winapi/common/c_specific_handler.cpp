/// @file c_specific_handler.cpp
/// @brief x64 __C_specific_handler: the MSVC/clang SEH scope-table walker.
///
/// Layouts are winnt.h's (Windows SDK 10.0.26100): DISPATCHER_CONTEXT and
/// SCOPE_TABLE_AMD64; the prototype and the meaning of each DISPATCHER_CONTEXT
/// field are Learn's "x64 exception handling" (Language-specific handler).
/// The walk itself is measured: tools/oracle_probe/c_specific_handler_probe
/// (MSVC and clang builds, the handler taken from vcruntime, ucrtbase and
/// ntdll, all identical on Windows).

#include "winapi/common/c_specific_handler.hpp"

#include <optional>
#include <utility>

#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>

namespace vitreo::winapi {

namespace {

// winnt.h DISPATCHER_CONTEXT (x64).
constexpr u64 kDcControlPc = 0x00;
constexpr u64 kDcImageBase = 0x08;
constexpr u64 kDcTargetIp = 0x20;
constexpr u64 kDcHandlerData = 0x38;
constexpr u64 kDcScopeIndex = 0x48;

// winnt.h SCOPE_TABLE_AMD64: DWORD Count, then {Begin, End, Handler, JumpTarget}.
constexpr u64 kScopeRecords = 4;
constexpr u64 kScopeRecordSize = 16;

// winnt.h exception flags.
constexpr u32 kExceptionUnwinding = 0x2;
constexpr u32 kExceptionExitUnwind = 0x4;
constexpr u32 kExceptionTargetUnwind = 0x20;
constexpr u32 kExceptionUnwind = kExceptionUnwinding | kExceptionExitUnwind | kExceptionTargetUnwind;

// excpt.h.
constexpr u64 kExceptionContinueExecution = 0;
constexpr u64 kExceptionContinueSearch = 1;
constexpr u32 kExceptionExecuteHandler = 1;

SehGuestCall& guest_call() {
    static SehGuestCall call;
    return call;
}

struct ScopeRecord {
    u32 begin = 0;
    u32 end = 0;
    u32 handler = 0;
    u32 jump_target = 0;
};

ScopeRecord read_scope(Marshaler& m, GuestAddr table, u32 index) {
    const GuestAddr at = table + kScopeRecords + static_cast<u64>(index) * kScopeRecordSize;
    return ScopeRecord{m.read_u32(at + 0).value_or(0), m.read_u32(at + 4).value_or(0),
                       m.read_u32(at + 8).value_or(0), m.read_u32(at + 12).value_or(0)};
}

/// Run a guest funclet below the handler's own frame. Returns nullopt when it
/// did not return normally (a nested raise, or an unwind it requested).
std::optional<u64> call_funclet(cpu::CpuState& cpu, GuestAddr fn, const u64* args, usize count) {
    auto& call = guest_call();
    if (!call) return std::nullopt;
    bool completed = false;
    const u64 result = call(cpu, fn, args, count, &completed);
    if (!completed || cpu.pending_unwind.active) return std::nullopt;
    return result;
}

}  // namespace

void set_seh_guest_call(SehGuestCall call) {
    guest_call() = std::move(call);
}

const SehGuestCall& seh_guest_call() {
    return guest_call();
}

void request_x64_unwind(cpu::CpuState& cpu, u64 target_frame, u64 target_ip,
                        u64 exception_record, u64 return_value) {
    cpu.pending_unwind.exception_record = exception_record;
    cpu.pending_unwind.active = true;
    cpu.pending_unwind.target_ip = target_ip;
    cpu.pending_unwind.target_frame = target_frame;
    cpu.pending_unwind.return_value = return_value;
    cpu.pending_unwind.caller_sp = cpu.gpr.rsp;
    cpu.gpr.rax = return_value;
}

ExportEntry make_c_specific_handler_export() {
    return ExportEntry{"__C_specific_handler", 0, [](Marshaler& m) {
        auto* cpu = m.cpu();
        const GuestAddr record = m.arg_ptr(0);
        const u64 establisher = m.arg(1);
        const GuestAddr context = m.arg_ptr(2);
        const GuestAddr dispatch = m.arg_ptr(3);
        if (cpu == nullptr || record == 0 || dispatch == 0) {
            m.set_return(kExceptionContinueSearch);
            return;
        }
        const u64 image_base = m.read_u64(dispatch + kDcImageBase).value_or(0);
        const u64 control_rva = m.read_u64(dispatch + kDcControlPc).value_or(0) - image_base;
        const GuestAddr table = m.read_u64(dispatch + kDcHandlerData).value_or(0);
        const u32 count = table != 0 ? m.read_u32(table).value_or(0) : 0;
        const u32 code = m.read_u32(record + 0).value_or(0);
        const u32 flags = m.read_u32(record + 4).value_or(0);
        VITREO_TRACE("[seh64] __C_specific_handler code=0x%08x flags=0x%x control_rva=0x%llx "
                     "scopes=%u frame=0x%llx\n", code, flags,
                     static_cast<unsigned long long>(control_rva), count,
                     static_cast<unsigned long long>(establisher));
        const auto in_scope = [&](const ScopeRecord& s) {
            return control_rva >= s.begin && control_rva < s.end;
        };

        if ((flags & kExceptionUnwind) == 0) {
            // Search pass: __except scopes (JumpTarget != 0) around ControlPc,
            // innermost first (filter_returns_*, nested_scopes_one_function).
            for (u32 i = 0; i < count; ++i) {
                const ScopeRecord s = read_scope(m, table, i);
                if (s.jump_target == 0 || !in_scope(s)) continue;
                i32 verdict = static_cast<i32>(kExceptionExecuteHandler);
                if (s.handler != kExceptionExecuteHandler) {
                    // The filter funclet takes (EXCEPTION_POINTERS*, EstablisherFrame).
                    // EXCEPTION_POINTERS lives below the handler's frame for the call.
                    const u64 saved_rsp = cpu->gpr.rsp;
                    const GuestAddr pointers = (saved_rsp - 0x10) & ~u64{0xF};
                    (void)m.write_u64(pointers + 0, record);
                    (void)m.write_u64(pointers + 8, context);
                    cpu->gpr.rsp = pointers;
                    const u64 args[2] = {pointers, establisher};
                    const auto result = call_funclet(*cpu, image_base + s.handler, args, 2);
                    cpu->gpr.rsp = saved_rsp;
                    if (cpu->pending_unwind.active) return;  // the filter unwound past us
                    // A filter that did not return cannot have chosen a handler.
                    verdict = result ? static_cast<i32>(static_cast<u32>(*result)) : 0;
                    VITREO_TRACE("[seh64]   scope %u filter 0x%x -> %d%s\n", i, s.handler,
                                 verdict, result ? "" : " (did not return)");
                }
                if (verdict < 0) {
                    // filter_returns_minus1 / _minus2: resume at the fault.
                    m.set_return(kExceptionContinueExecution);
                    return;
                }
                if (verdict > 0) {
                    // filter_returns_1 / _2: unwind to the __except block; its
                    // code reads the exception code from RAX (GetExceptionCode).
                    VITREO_TRACE("[seh64]   scope %u unwind to 0x%llx\n", i,
                                 static_cast<unsigned long long>(image_base + s.jump_target));
                    request_x64_unwind(*cpu, establisher, image_base + s.jump_target, record, code);
                    return;
                }
            }
            m.set_return(kExceptionContinueSearch);
            return;
        }

        // Unwind pass: __finally scopes (JumpTarget == 0) around ControlPc run
        // with AbnormalTermination() TRUE (finally_during_unwind, across_frames).
        // In the target frame the walk stops at the __except being unwound to,
        // so only __finally blocks inside it run (nested_scopes_one_function).
        const u64 target_rva = m.read_u64(dispatch + kDcTargetIp).value_or(0) - image_base;
        const u32 first = m.read_u32(dispatch + kDcScopeIndex).value_or(0);
        for (u32 i = first; i < count; ++i) {
            const ScopeRecord s = read_scope(m, table, i);
            if (!in_scope(s)) continue;
            if ((flags & kExceptionTargetUnwind) != 0 && s.jump_target != 0 &&
                s.jump_target == target_rva) {
                break;
            }
            if (s.jump_target != 0) continue;
            // A collided unwind resumes after the termination handler that ran.
            (void)m.write_u32(dispatch + kDcScopeIndex, i + 1);
            const u64 args[2] = {1u, establisher};
            (void)call_funclet(*cpu, image_base + s.handler, args, 2);
            if (cpu->pending_unwind.active) return;
        }
        m.set_return(kExceptionContinueSearch);
    }};
}

}  // namespace vitreo::winapi
