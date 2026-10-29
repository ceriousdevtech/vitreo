// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

#include "../state/cpu_state.hpp"
#include "../decode/decoder.hpp"
#include "core/memory/guest_memory.hpp"
#include <vitreo/guest_sentinels.hpp>

namespace vitreo::cpu {

// ============================================================================
// Execution Result Types
// ============================================================================

/// Result of executing a single instruction
enum class ExecResult : u8 {
    Continue,       ///< Continue to next instruction
    Branch,         ///< Branch taken (IP already updated)
    Halt,           ///< HLT instruction executed
    Exception,      ///< CPU exception occurred
    Interrupt,      ///< Software interrupt (INT)
    Syscall,        ///< SYSCALL instruction
    IOPort,         ///< I/O port access
};

/// CPU exception types
enum class CpuException : u8 {
    None = 0,
    DivideError = 0,
    Debug = 1,
    Nmi = 2,
    Breakpoint = 3,
    Overflow = 4,
    BoundRange = 5,
    InvalidOpcode = 6,
    DeviceNotAvailable = 7,
    DoubleFault = 8,
    InvalidTss = 10,
    SegmentNotPresent = 11,
    StackFault = 12,
    GeneralProtection = 13,
    PageFault = 14,
    FloatingPoint = 16,
    AlignmentCheck = 17,
    MachineCheck = 18,
    SimdFloatingPoint = 19,
    PrivilegedInstruction = 20,  // Virtual: maps to STATUS_PRIVILEGED_INSTRUCTION
    FloatingPointStackCheck = 21, // Virtual: x87 #IS delivered as STATUS_FLOAT_STACK_CHECK
    FloatingPointDenormalOperand = 22,
    FloatingPointDivideByZero = 23,
    FloatingPointInexactResult = 24,
    FloatingPointInvalidOperation = 25,
    FloatingPointOverflow = 26,
    FloatingPointUnderflow = 27,
};

/// Execution context with state references
struct ExecContext {
    CpuState& cpu;
    GuestMemory& mem;
    
    CpuException exception = CpuException::None;
    u32 exception_error = 0;
    u64 exception_addr = 0;
    /// Explicit Windows exception arguments carried by this instruction.
    u8 exception_num_params = 0;
    u8 interrupt_num = 0;
    u8 instr_length = 0;  ///< Current instruction length (for RIP-relative EA)
    bool address_size_override = false; ///< Current instruction, including delegates
    /// The instruction now executing reached a LEGACY handler from a VEX
    /// encoding, through `avx_sse_bridge`.
    ///
    /// The bridge clears `prefixes.vex` before calling the legacy handler --
    /// deliberately, so the handler runs in legacy mode -- which leaves that
    /// handler unable to tell a VEX form from a real legacy one. Exactly one
    /// rule needs the distinction: legacy SSE requires a 16-byte aligned
    /// 128-bit memory operand and VEX does not (Intel SDM Vol. 2: unaligned
    /// unless the opcode says otherwise). Set and restored around the bridged
    /// call, because a delegate can nest.
    bool vex_origin = false;
    
    ExecContext(CpuState& c, GuestMemory& m) noexcept
        : cpu(c), mem(m) {}
};

// ============================================================================
// Operand Access Functions
// ============================================================================

/// Read operand value from instruction
[[nodiscard]] Result<u64, Error> read_operand(
    ExecContext& ctx,
    const DecodedInstruction& instr,
    usize operand_idx
) noexcept;

/// Write value to operand
[[nodiscard]] VoidResult write_operand(
    ExecContext& ctx,
    const DecodedInstruction& instr,
    usize operand_idx,
    u64 value
) noexcept;

/// Push value onto stack
[[nodiscard]] VoidResult push_value(
    ExecContext& ctx,
    u64 value,
    u8 size
) noexcept;

/// Pop value from stack
[[nodiscard]] Result<u64, Error> pop_value(
    ExecContext& ctx,
    u8 size
) noexcept;

/// Record a failed `size`-byte guest READ at `addr` as the Windows
/// STATUS_ACCESS_VIOLATION parameters (ExceptionInformation[0] = 0 for a
/// read, [1] = the first inaccessible byte; a non-canonical address reads as
/// 0xFFFFFFFFFFFFFFFF), the rule read_operand already applies to every
/// faulting memory operand. For an implicit stack read such as RET's pop.
void record_read_access_violation(
    ExecContext& ctx,
    u64 addr,
    u8 size
) noexcept;

/// The write twin of record_read_access_violation ([0] = 1), for an implicit
/// stack write that does not go through push_value.
void record_write_access_violation(
    ExecContext& ctx,
    u64 addr,
    u8 size
) noexcept;

/// The CPU exception of a failed push_value/pop_value. Error::Overflow is
/// push_value's x64 stack-guard rule (StackLimit) and stays StackFault,
/// STATUS_STACK_OVERFLOW. Anything else is a page fault on the slot, which
/// push_value/pop_value have already recorded: STATUS_ACCESS_VIOLATION,
/// [0] = direction, [1] = the slot (tools/oracle_probe/
/// stack_access_fault_probe.expected) -- STATUS_STACK_OVERFLOW is the
/// guard's alone, never an ordinary bad stack pointer's.
[[nodiscard]] inline CpuException stack_access_exception(Error error) noexcept {
    return error == Error::Overflow ? CpuException::StackFault
                                    : CpuException::PageFault;
}

/// Intel SDM Vol. 2 JMP, CALL, RET, Jcc, LOOP, JrCXZ: in 64-bit mode a near
/// branch whose target is not canonical raises #GP(0) -- on Intel, which
/// Vitreo reports, AT THE BRANCH and with nothing committed (no RIP change,
/// no CALL push, no RET pop, no LOOP decrement). Windows reports it like a
/// non-canonical data access: STATUS_ACCESS_VIOLATION, [0] = 0,
/// [1] = 0xFFFFFFFFFFFFFFFF (tools/oracle_probe/
/// noncanonical_branch_probe.expected). Returns false, with that exception
/// recorded, for such a target; outside 64-bit mode every target is
/// canonical. The caller must not have committed anything it cannot undo.
///
/// Vitreo's two host return sentinels (vitreo/guest_sentinels.hpp) are
/// non-canonical BY DESIGN -- no guest page can ever hold them -- and are not
/// guest branch targets: a RET to one is the guest returning to the host,
/// which the run loops detect from RIP after the branch commits. They alone
/// pass.
[[nodiscard]] inline bool near_branch_target_canonical(ExecContext& ctx,
                                                       u64 target) noexcept {
    if (!ctx.cpu.is_64bit() ||
        static_cast<u64>(static_cast<i64>(target << 16) >> 16) == target ||
        target == kCallbackReturnSentinel || target == kProcessReturnSentinel) {
        return true;
    }
    ctx.exception = CpuException::GeneralProtection;
    ctx.exception_error = 0;
    ctx.exception_addr = ~u64{0};
    return false;
}

/// Execute a single decoded instruction
[[nodiscard]] ExecResult execute_instruction(
    ExecContext& ctx,
    const DecodedInstruction& instr
) noexcept;

// ============================================================================
// Diagnostic hooks
// ============================================================================

/// Optional callback used to dump an active PC-trace ring when the interpreter
/// detects a fault worth correlating against recent control flow.
///
/// The ring buffer lives in the CLI execution loop, which is not linked into
/// every host of this library (fuzz harnesses and unit tests use it standalone),
/// so the CPU layer only holds a pointer that such a host may install.
using PcTraceDumpHook = void (*)(const char* tag);
void set_pctrace_dump_hook(PcTraceDumpHook hook) noexcept;

/// Optional callback naming the export that owns a guest address, when that
/// address falls inside the WinAPI thunk region.
///
/// A fault whose faulting or last-executed address is a thunk slot is almost
/// always a bad import: the raw hex says nothing, the export name says which
/// API. The name table lives in the WinAPI layer, which the CPU layer must not
/// depend on, so the host installs a pointer here (same reasoning as
/// PcTraceDumpHook above). Returns false when the address is not a thunk.
using ThunkNameHook = bool (*)(u64 addr, char* out, unsigned out_size);
void set_thunk_name_hook(ThunkNameHook hook) noexcept;

// ============================================================================
// Interpreter Class
// ============================================================================

/// x86-64 interpreter for instruction execution
class Interpreter {
public:
    Interpreter(CpuState& cpu, GuestMemory& memory) noexcept
        : m_cpu(cpu), m_memory(memory), m_ctx(cpu, memory), m_instr_count(0) {}
    
    /// Execute a single instruction
    [[nodiscard]] ExecResult step() noexcept;

    /// Execute one already-decoded instruction, skipping the fetch+decode that
    /// step() performs. The caller must have set m_cpu.rip to the instruction's
    /// address (RIP-relative operands and the length advance depend on it) and
    /// guarantees `instr` matches the bytes at that rip (the JIT delegate
    /// supplies the block's own compile-time decode; SMC invalidation keeps it
    /// current). Mirrors step()'s post-decode tail exactly.
    [[nodiscard]] ExecResult execute_predecoded(
        const DecodedInstruction& instr) noexcept;

    /// Execute instructions until halt/exception or max_instructions reached
    [[nodiscard]] ExecResult run(u64 max_instructions = 0) noexcept;
    
    /// Get number of instructions executed
    [[nodiscard]] u64 instruction_count() const noexcept { return m_instr_count; }
    
    /// Get execution context
    [[nodiscard]] const ExecContext& context() const noexcept { return m_ctx; }
    [[nodiscard]] ExecContext& context() noexcept { return m_ctx; }
    
private:
    CpuState& m_cpu;
    GuestMemory& m_memory;
    ExecContext m_ctx;
    u64 m_instr_count;
};

} // namespace vitreo::cpu
