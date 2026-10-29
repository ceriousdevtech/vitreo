#include "core/cpu/seh/seh32.hpp"
#include "core/cpu/seh/seh32_nested_handler.generated.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cstring>

namespace vitreo::cpu {

// Helper: propagate write errors from VoidResult-returning memory operations
#define SEH_TRY_WRITE(expr)                                     \
    do {                                                         \
        auto _wr = (expr);                                       \
        if (_wr.is_err()) {                                      \
            return VoidResult::err(std::move(_wr).error());      \
        }                                                        \
    } while (0)

// =============================================================================
// cpu_exception_to_status
// =============================================================================

u32 cpu_exception_to_status(CpuException ex) {
    switch (ex) {
        case CpuException::Breakpoint:
            return STATUS_BREAKPOINT;
        case CpuException::DivideError:
            return STATUS_INTEGER_DIVIDE_BY_ZERO;
        case CpuException::Overflow:
            return STATUS_INTEGER_OVERFLOW;
        case CpuException::GeneralProtection:
            return STATUS_ACCESS_VIOLATION;
        case CpuException::PageFault:
            return STATUS_ACCESS_VIOLATION;
        case CpuException::StackFault:
            return STATUS_STACK_OVERFLOW;
        case CpuException::InvalidOpcode:
            return STATUS_ILLEGAL_INSTRUCTION;
        case CpuException::PrivilegedInstruction:
            return STATUS_PRIVILEGED_INSTRUCTION;
        case CpuException::FloatingPointStackCheck:
            return STATUS_FLOAT_STACK_CHECK;
        case CpuException::FloatingPointDenormalOperand:
            return STATUS_FLOAT_DENORMAL_OPERAND;
        case CpuException::FloatingPointDivideByZero:
            return STATUS_FLOAT_DIVIDE_BY_ZERO;
        case CpuException::FloatingPointInexactResult:
            return STATUS_FLOAT_INEXACT_RESULT;
        case CpuException::FloatingPointInvalidOperation:
            return STATUS_FLOAT_INVALID_OPERATION;
        case CpuException::FloatingPointOverflow:
            return STATUS_FLOAT_OVERFLOW;
        case CpuException::FloatingPointUnderflow:
            return STATUS_FLOAT_UNDERFLOW;
        default:
            return STATUS_ACCESS_VIOLATION;
    }
}

bool consume_pending_helper_raise(
    CpuState& cpu, ExecContext& context, ExecResult& exec_result) noexcept {
    if (!cpu.pending_raise.active || !cpu.pending_raise.from_helper) {
        return false;
    }

    const auto pending = cpu.pending_raise;
    cpu.pending_raise.clear();

    // Mirror EXCEPTION_RECORD.ExceptionInformation for ACCESS_VIOLATION:
    // args[0] = access type (0=read, 1=write, 8=DEP), args[1] = fault VA.
    context.exception_num_params = static_cast<u8>(std::min(pending.num_args, 2u));
    context.exception_error =
        pending.num_args >= 1 ? static_cast<u32>(pending.args[0]) : 0;
    context.exception_addr =
        pending.num_args >= 2 ? pending.args[1] : cpu.rip;

    if (pending.code == STATUS_BREAKPOINT) {
        context.exception = CpuException::Breakpoint;
        context.interrupt_num = 3;
        // INT3 is a trap. The delegated interpreter has already advanced the
        // instruction pointer; retain the instruction address separately for
        // ExceptionAddress.
        context.exception_addr = cpu.rip > 0 ? cpu.rip - 1 : 0;
        exec_result = ExecResult::Interrupt;
        return true;
    }

    // __fastfail / INT 0x29 (Learn: /cpp/intrinsics/fastfail): user-mode
    // fast fail is STATUS_STACK_BUFFER_OVERRUN and "no exception handlers
    // are invoked". Surface it as Interrupt 0x29 so the run loop terminates
    // instead of mapping the helper raise onto #GP / SEH.
    if (pending.code == STATUS_STACK_BUFFER_OVERRUN) {
        context.exception = CpuException::GeneralProtection;
        context.interrupt_num = 0x29;
        context.exception_error =
            pending.num_args >= 1 ? static_cast<u32>(pending.args[0]) : 0;
        context.exception_addr = cpu.rip;
        exec_result = ExecResult::Interrupt;
        return true;
    }

    switch (pending.code) {
        case STATUS_INTEGER_DIVIDE_BY_ZERO:
            context.exception = CpuException::DivideError;
            break;
        case STATUS_INTEGER_OVERFLOW:
            context.exception = CpuException::Overflow;
            break;
        case STATUS_ILLEGAL_INSTRUCTION:
            context.exception = CpuException::InvalidOpcode;
            break;
        case STATUS_PRIVILEGED_INSTRUCTION:
            context.exception = CpuException::PrivilegedInstruction;
            break;
        case STATUS_STACK_OVERFLOW:
            context.exception = CpuException::StackFault;
            break;
        case STATUS_FLOAT_DENORMAL_OPERAND:
            context.exception = CpuException::FloatingPointDenormalOperand;
            break;
        case STATUS_FLOAT_DIVIDE_BY_ZERO:
            context.exception = CpuException::FloatingPointDivideByZero;
            break;
        case STATUS_FLOAT_INEXACT_RESULT:
            context.exception = CpuException::FloatingPointInexactResult;
            break;
        case STATUS_FLOAT_INVALID_OPERATION:
            context.exception = CpuException::FloatingPointInvalidOperation;
            break;
        case STATUS_FLOAT_OVERFLOW:
            context.exception = CpuException::FloatingPointOverflow;
            break;
        case STATUS_FLOAT_STACK_CHECK:
            context.exception = CpuException::FloatingPointStackCheck;
            break;
        case STATUS_FLOAT_UNDERFLOW:
            context.exception = CpuException::FloatingPointUnderflow;
            break;
        case STATUS_ACCESS_VIOLATION:
        default:
            // Both #GP and #PF map to STATUS_ACCESS_VIOLATION in the current
            // x86 exception model. GeneralProtection preserves that contract.
            context.exception = CpuException::GeneralProtection;
            break;
    }

    exec_result = ExecResult::Exception;
    return true;
}

// =============================================================================
// write_exception_record_32
// =============================================================================

VoidResult write_exception_record_32(GuestMemory& memory, u32 addr,
    u32 exception_code, u32 exception_flags, u32 exception_address,
    u32 num_params, const u32* params) {

    // Zero out the full EXCEPTION_RECORD (80 bytes)
    Byte zeros[EREC32_SIZE] = {};
    SEH_TRY_WRITE(memory.write_bytes(static_cast<GuestAddr>(addr), zeros, EREC32_SIZE));

    // ExceptionCode
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + EREC32_CODE), exception_code));

    // ExceptionFlags
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + EREC32_FLAGS), exception_flags));

    // ExceptionRecord (chained, usually 0)
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + EREC32_RECORD), 0));

    // ExceptionAddress
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + EREC32_ADDRESS), exception_address));

    // NumberParameters
    u32 clamped_params = (num_params > 15) ? 15 : num_params;
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + EREC32_NUM_PARAMS), clamped_params));

    // ExceptionInformation array
    if (params && clamped_params > 0) {
        for (u32 i = 0; i < clamped_params; ++i) {
            SEH_TRY_WRITE(memory.write_u32(
                static_cast<GuestAddr>(addr + EREC32_PARAMS + i * 4), params[i]));
        }
    }

    return VoidResult::ok();
}

// =============================================================================
// write_context_32
// =============================================================================

VoidResult write_context_32(GuestMemory& memory, u32 addr, CpuState& cpu) {
    // Ensure lazy flags (from JIT) are materialized into cpu.flags.rflags
    // before snapshotting EFLAGS into the CONTEXT record.
    cpu.materialize_lazy_flags();
    // Zero out the full CONTEXT (716 bytes)
    Byte zeros[CTX32_SIZE] = {};
    SEH_TRY_WRITE(memory.write_bytes(static_cast<GuestAddr>(addr), zeros, CTX32_SIZE));

    // ContextFlags
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + CTX32_FLAGS), CONTEXT_FULL_32));

    // Segment registers
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + CTX32_SEGGS), 0));
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + CTX32_SEGFS), 0x3B));
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + CTX32_SEGES), 0x23));
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + CTX32_SEGDS), 0x23));

    // General-purpose registers
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + CTX32_EDI), static_cast<u32>(cpu.gpr.rdi)));
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + CTX32_ESI), static_cast<u32>(cpu.gpr.rsi)));
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + CTX32_EBX), static_cast<u32>(cpu.gpr.rbx)));
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + CTX32_EDX), static_cast<u32>(cpu.gpr.rdx)));
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + CTX32_ECX), static_cast<u32>(cpu.gpr.rcx)));
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + CTX32_EAX), static_cast<u32>(cpu.gpr.rax)));

    // EBP, EIP, EFLAGS, ESP
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + CTX32_EBP), static_cast<u32>(cpu.gpr.rbp)));
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + CTX32_EIP), static_cast<u32>(cpu.rip)));
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + CTX32_EFLAGS), static_cast<u32>(cpu.flags.rflags)));
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + CTX32_ESP), static_cast<u32>(cpu.gpr.rsp)));

    // CS and SS segment registers
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + CTX32_SEGCS), 0x1B));
    SEH_TRY_WRITE(memory.write_u32(static_cast<GuestAddr>(addr + CTX32_SEGSS), 0x23));

    return VoidResult::ok();
}

// =============================================================================
// read_context_32
// =============================================================================

VoidResult read_context_32(GuestMemory& memory, u32 addr, CpuState& cpu) {
    // Read general-purpose registers
    auto eax_r = memory.read_u32(static_cast<GuestAddr>(addr + CTX32_EAX));
    if (eax_r.is_err()) return VoidResult::err(std::move(eax_r).error());
    auto ecx_r = memory.read_u32(static_cast<GuestAddr>(addr + CTX32_ECX));
    if (ecx_r.is_err()) return VoidResult::err(std::move(ecx_r).error());
    auto edx_r = memory.read_u32(static_cast<GuestAddr>(addr + CTX32_EDX));
    if (edx_r.is_err()) return VoidResult::err(std::move(edx_r).error());
    auto ebx_r = memory.read_u32(static_cast<GuestAddr>(addr + CTX32_EBX));
    if (ebx_r.is_err()) return VoidResult::err(std::move(ebx_r).error());
    auto esp_r = memory.read_u32(static_cast<GuestAddr>(addr + CTX32_ESP));
    if (esp_r.is_err()) return VoidResult::err(std::move(esp_r).error());
    auto ebp_r = memory.read_u32(static_cast<GuestAddr>(addr + CTX32_EBP));
    if (ebp_r.is_err()) return VoidResult::err(std::move(ebp_r).error());
    auto esi_r = memory.read_u32(static_cast<GuestAddr>(addr + CTX32_ESI));
    if (esi_r.is_err()) return VoidResult::err(std::move(esi_r).error());
    auto edi_r = memory.read_u32(static_cast<GuestAddr>(addr + CTX32_EDI));
    if (edi_r.is_err()) return VoidResult::err(std::move(edi_r).error());

    // Read instruction pointer and flags
    auto eip_r = memory.read_u32(static_cast<GuestAddr>(addr + CTX32_EIP));
    if (eip_r.is_err()) return VoidResult::err(std::move(eip_r).error());
    auto eflags_r = memory.read_u32(static_cast<GuestAddr>(addr + CTX32_EFLAGS));
    if (eflags_r.is_err()) return VoidResult::err(std::move(eflags_r).error());

    // Apply to CPU state (zero-extend into 64-bit registers)
    cpu.gpr.rax = eax_r.value();
    cpu.gpr.rcx = ecx_r.value();
    cpu.gpr.rdx = edx_r.value();
    cpu.gpr.rbx = ebx_r.value();
    cpu.gpr.rsp = esp_r.value();
    cpu.gpr.rbp = ebp_r.value();
    cpu.gpr.rsi = esi_r.value();
    cpu.gpr.rdi = edi_r.value();
    cpu.rip = eip_r.value();
    cpu.flags = Flags(static_cast<u64>(eflags_r.value()));

    return VoidResult::ok();
}

// =============================================================================
// read_seh_chain_head
// =============================================================================

VResult<u32> read_seh_chain_head(GuestMemory& memory, const CpuState& cpu) {
    // FS:[0] is the ExceptionList field of the TEB
    GuestAddr teb_base = cpu.seg.fs_desc.base;
    auto head_r = memory.read_u32(teb_base + 0x00);
    if (head_r.is_err()) return VResult<u32>::err(std::move(head_r).error());
    return VResult<u32>::ok(head_r.value());
}

// =============================================================================
// write_seh_chain_head
// =============================================================================

VoidResult write_seh_chain_head(
    GuestMemory& memory, const CpuState& cpu, u32 frame_addr) {
    // FS:[0] is the ExceptionList field of the TEB.
    return memory.write_u32(cpu.seg.fs_desc.base + 0x00, frame_addr);
}

// =============================================================================
// read_seh_handler
// =============================================================================

bool seh32_registration_dispatchable(
    GuestMemory& memory, const CpuState& cpu, u32 frame_addr) {
    if ((frame_addr & 3u) != 0) return false;

    // NT_TIB32: StackBase at +0x04 (high address), StackLimit at +0x08 (low).
    const GuestAddr teb_base = cpu.seg.fs_desc.base;
    auto base_r = memory.read_u32(teb_base + 0x04);
    auto limit_r = memory.read_u32(teb_base + 0x08);
    if (base_r.is_err() || limit_r.is_err()) return true;

    const u64 stack_high = base_r.value();
    const u64 stack_low = limit_r.value();
    // A thread whose TEB has no usable stack description cannot be judged, so
    // dispatch proceeds as before rather than refusing every handler.
    if (stack_high <= stack_low) return true;

    constexpr u64 REGISTRATION_SIZE = 8;  // Next + Handler
    return frame_addr >= stack_low &&
           static_cast<u64>(frame_addr) + REGISTRATION_SIZE <= stack_high;
}

VResult<SehHandlerInfo> read_seh_handler(GuestMemory& memory, u32 frame_addr) {
    auto next_r = memory.read_u32(static_cast<GuestAddr>(frame_addr + 0));
    if (next_r.is_err()) return VResult<SehHandlerInfo>::err(std::move(next_r).error());
    auto handler_r = memory.read_u32(static_cast<GuestAddr>(frame_addr + 4));
    if (handler_r.is_err()) return VResult<SehHandlerInfo>::err(std::move(handler_r).error());

    SehHandlerInfo info;
    info.frame_addr   = frame_addr;
    info.handler_addr = handler_r.value();
    info.next_frame   = next_r.value();

    return VResult<SehHandlerInfo>::ok(info);
}

// =============================================================================
// Nested FS:[0] registration (RtlpExecuteHandlerForException contract)
// =============================================================================

namespace {

// Immediately below SYNTHETIC_MODULE_BASE_X86 (0x7C000000). Not on the stack —
// Windows rejects EXCEPTION_REGISTRATION_RECORD.Handler values that point into
// stack. It moves with the module arena so this page does not sit alone in the
// middle of the guest's address space fragmenting it.
constexpr GuestAddr kSeh32ProtectorPage = 0x7BFE0000;
constexpr u32 kSeh32ProtectorPageSize = 0x1000;

u32 g_seh32_protector_addr = 0;

} // namespace

u32 seh32_exception_protector_addr() noexcept {
    return g_seh32_protector_addr;
}

VoidResult ensure_seh32_exception_protector(GuestMemory& memory) {
    // Present in THIS guest memory, not merely installed once somewhere.
    if (g_seh32_protector_addr != 0 &&
        memory.read_u8(kSeh32ProtectorPage).value_or(0) == kSeh32NestedHandler[0]) {
        return VoidResult::ok();
    }

    // Generated from the C helper and native Windows behavior measurements.
    // See tools/oracle_probe/seh32_nested_protector_probe.expected.

    auto mapped = memory.map_region(
        kSeh32ProtectorPage, kSeh32ProtectorPageSize,
        MemoryProtection::ReadWrite);
    if (mapped.is_err()) {
        // Page may already exist from a prior ensure; try writing in place.
        auto probe = memory.probe_access(kSeh32ProtectorPage, 1);
        if (!probe.host_ptr || !probe.writable) {
            return VoidResult::err(mapped.error());
        }
    }

    auto wr = memory.write_bytes(
        kSeh32ProtectorPage, kSeh32NestedHandler, sizeof(kSeh32NestedHandler));
    if (wr.is_err()) {
        return VoidResult::err(std::move(wr).error());
    }

    (void)memory.protect_region(
        kSeh32ProtectorPage, kSeh32ProtectorPageSize,
        MemoryProtection::ReadExecute);

    g_seh32_protector_addr = static_cast<u32>(kSeh32ProtectorPage);
    return VoidResult::ok();
}

VoidResult install_seh32_nested_execution(
    GuestMemory& memory, CpuState& cpu,
    u32 nested_addr, u32 establisher_frame,
    Seh32NestedExecution& out) {
    out = {};
    if (nested_addr == 0 || g_seh32_protector_addr == 0) {
        return VoidResult::err(Error::InvalidArgument);
    }

    auto head_r = read_seh_chain_head(memory, cpu);
    if (head_r.is_err()) {
        return VoidResult::err(std::move(head_r).error());
    }
    const u32 previous = head_r.value();

    // Nested dispatch can re-enter with the same dispatcher scratch pointer.
    // A second synthetic record at that address would overwrite the first and
    // make Next point to itself, so the chain could never reach the guest's
    // establisher frame. Windows stack registrations grow downward: place the
    // new record immediately below the overlapping head. The writes below
    // remain the authority for stack accessibility.
    const u64 previous_end =
        static_cast<u64>(previous) + SEH32_NESTED_RECORD_SIZE;
    const u64 nested_end =
        static_cast<u64>(nested_addr) + SEH32_NESTED_RECORD_SIZE;
    const bool overlaps_previous =
        previous != 0 && previous != 0xffffffffu &&
        static_cast<u64>(previous) < nested_end &&
        static_cast<u64>(nested_addr) < previous_end;
    if (overlaps_previous) {
        const u32 lower = std::min(previous, nested_addr);
        if (lower < SEH32_NESTED_RECORD_SIZE) {
            return VoidResult::err(Error::InvalidArgument);
        }
        nested_addr =
            (lower - SEH32_NESTED_RECORD_SIZE) & ~u32{3};
    }

    // Match RtlpExecuteHandler2: Next, Protector, EstablisherFrame.
    SEH_TRY_WRITE(memory.write_u32(
        static_cast<GuestAddr>(nested_addr + 0), previous));
    SEH_TRY_WRITE(memory.write_u32(
        static_cast<GuestAddr>(nested_addr + 4), g_seh32_protector_addr));
    SEH_TRY_WRITE(memory.write_u32(
        static_cast<GuestAddr>(nested_addr + 8), establisher_frame));

    auto link = write_seh_chain_head(memory, cpu, nested_addr);
    if (link.is_err()) {
        return link;
    }

    out.nested_addr = nested_addr;
    out.previous_head = previous;
    out.installed = true;
    return VoidResult::ok();
}

VoidResult unlink_seh32_nested_execution(
    GuestMemory& memory, CpuState& cpu,
    const Seh32NestedExecution& nested) {
    if (!nested.installed) {
        return VoidResult::ok();
    }

    // RtlpExecuteHandler2:
    //   mov esp, [fs:0]
    //   pop [fs:0]          ; FS:[0] = *[FS:[0]]
    //   mov esp, ebp        ; (ESP restored by caller)
    auto head_r = read_seh_chain_head(memory, cpu);
    if (head_r.is_err()) {
        return VoidResult::err(std::move(head_r).error());
    }
    const u32 head = head_r.value();

    if (head == nested.nested_addr) {
        return write_seh_chain_head(memory, cpu, nested.previous_head);
    }

    // Handler rewired FS:[0] (common for packers that unlink themselves).
    // Pop Next from whatever registration is currently at the head.
    if (head == 0 || head == 0xFFFFFFFFu) {
        return VoidResult::ok();
    }
    auto next_r = memory.read_u32(static_cast<GuestAddr>(head));
    if (next_r.is_err()) {
        return VoidResult::err(std::move(next_r).error());
    }
    return write_seh_chain_head(memory, cpu, next_r.value());
}

} // namespace vitreo::cpu
