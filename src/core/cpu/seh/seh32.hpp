#pragma once

/// @file seh32.hpp
/// @brief 32-bit frame-based Structured Exception Handling (SEH)

#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/cpu/interpreter/interpreter.hpp"

namespace vitreo { class GuestMemory; }

namespace vitreo::cpu {

// =============================================================================
// Win32 STATUS codes (32-bit SEH)
// =============================================================================
constexpr u32 STATUS_BREAKPOINT             = 0x80000003;
constexpr u32 STATUS_ASSERTION_FAILURE      = 0xC0000420;
constexpr u32 STATUS_ACCESS_VIOLATION       = 0xC0000005;
constexpr u32 STATUS_INTEGER_DIVIDE_BY_ZERO = 0xC0000094;
constexpr u32 STATUS_INTEGER_OVERFLOW       = 0xC0000095;
constexpr u32 STATUS_FLOAT_DENORMAL_OPERAND = 0xC000008D;
constexpr u32 STATUS_FLOAT_DIVIDE_BY_ZERO   = 0xC000008E;
constexpr u32 STATUS_FLOAT_INEXACT_RESULT   = 0xC000008F;
constexpr u32 STATUS_FLOAT_INVALID_OPERATION = 0xC0000090;
constexpr u32 STATUS_FLOAT_OVERFLOW         = 0xC0000091;
constexpr u32 STATUS_FLOAT_STACK_CHECK      = 0xC0000092;
constexpr u32 STATUS_FLOAT_UNDERFLOW        = 0xC0000093;
constexpr u32 STATUS_PRIVILEGED_INSTRUCTION = 0xC0000096;
constexpr u32 STATUS_ILLEGAL_INSTRUCTION    = 0xC000001D;
constexpr u32 STATUS_STACK_OVERFLOW         = 0xC00000FD;
constexpr u32 STATUS_STACK_BUFFER_OVERRUN   = 0xC0000409;

// _EXCEPTION_DISPOSITION values (returned by low-level SEH handlers)
constexpr u32 ExceptionContinueExecution = 0;
constexpr u32 ExceptionContinueSearch    = 1;
constexpr u32 ExceptionNestedException   = 2;
constexpr u32 ExceptionCollidedUnwind    = 3;

// Nested registration pushed by RtlpExecuteHandlerForException / ForUnwind:
//   +0 Next, +4 Protector, +8 EstablisherFrame (RegistrationPointer)
constexpr u32 SEH32_NESTED_RECORD_SIZE = 12;

// CONTEXT_FULL for 32-bit
constexpr u32 CONTEXT_i386    = 0x00010000;
constexpr u32 CONTEXT_FULL_32 = CONTEXT_i386 | 0x07;  // CONTROL | INTEGER | SEGMENTS

// =============================================================================
// 32-bit EXCEPTION_RECORD layout offsets
// =============================================================================
constexpr u32 EREC32_CODE       = 0x00;
constexpr u32 EREC32_FLAGS      = 0x04;
constexpr u32 EREC32_RECORD     = 0x08;
constexpr u32 EREC32_ADDRESS    = 0x0C;
constexpr u32 EREC32_NUM_PARAMS = 0x10;
constexpr u32 EREC32_PARAMS     = 0x14;
constexpr u32 EREC32_SIZE       = 0x50;  // 80 bytes

// =============================================================================
// 32-bit CONTEXT layout offsets
// =============================================================================
constexpr u32 CTX32_FLAGS    = 0x00;
constexpr u32 CTX32_DR0      = 0x04;
constexpr u32 CTX32_FLOAT    = 0x1C;  // FLOATING_SAVE_AREA (112 bytes)
constexpr u32 CTX32_SEGGS    = 0x8C;
constexpr u32 CTX32_SEGFS    = 0x90;
constexpr u32 CTX32_SEGES    = 0x94;
constexpr u32 CTX32_SEGDS    = 0x98;
constexpr u32 CTX32_EDI      = 0x9C;
constexpr u32 CTX32_ESI      = 0xA0;
constexpr u32 CTX32_EBX      = 0xA4;
constexpr u32 CTX32_EDX      = 0xA8;
constexpr u32 CTX32_ECX      = 0xAC;
constexpr u32 CTX32_EAX      = 0xB0;
constexpr u32 CTX32_EBP      = 0xB4;
constexpr u32 CTX32_EIP      = 0xB8;
constexpr u32 CTX32_SEGCS    = 0xBC;
constexpr u32 CTX32_EFLAGS   = 0xC0;
constexpr u32 CTX32_ESP      = 0xC4;
constexpr u32 CTX32_SEGSS    = 0xC8;
constexpr u32 CTX32_SIZE     = 0x2CC;  // 716 bytes

// =============================================================================
// Functions
// =============================================================================

/// Map CpuException to Win32 STATUS code
u32 cpu_exception_to_status(CpuException ex);

/// Convert a CPU exception surfaced by a JIT helper back into the interpreter
/// result/context form consumed by the shared x86/x64 SEH paths.
///
/// Returns false without changing state when no helper raise is pending.
/// On success, clears cpu.pending_raise and returns the corresponding
/// Exception or Interrupt result through exec_result.
bool consume_pending_helper_raise(
    CpuState& cpu, ExecContext& context, ExecResult& exec_result) noexcept;

/// Information about a single SEH handler in the chain
struct SehHandlerInfo {
    u32 frame_addr;    // address of EXCEPTION_REGISTRATION_RECORD
    u32 handler_addr;  // address of handler function
    u32 next_frame;    // address of next record (0xFFFFFFFF = end)
};

/// Write a 32-bit EXCEPTION_RECORD to guest memory
VoidResult write_exception_record_32(GuestMemory& memory, u32 addr,
    u32 exception_code, u32 exception_flags, u32 exception_address,
    u32 num_params = 0, const u32* params = nullptr);

/// Write a 32-bit CONTEXT to guest memory from current CPU state
VoidResult write_context_32(GuestMemory& memory, u32 addr, CpuState& cpu);

/// Read a 32-bit CONTEXT from guest memory back into CPU state
VoidResult read_context_32(GuestMemory& memory, u32 addr, CpuState& cpu);

/// Read the top of the SEH chain from FS:[0]
VResult<u32> read_seh_chain_head(GuestMemory& memory, const CpuState& cpu);

/// Replace the top of the SEH chain in FS:[0].
///
/// Used after dispatch selects an outer frame: records below that frame have
/// been unwound and must no longer remain visible to subsequent exceptions.
VoidResult write_seh_chain_head(
    GuestMemory& memory, const CpuState& cpu, u32 frame_addr);

/// Read a single SEH handler entry from the chain
VResult<SehHandlerInfo> read_seh_handler(GuestMemory& memory, u32 frame_addr);

/// True when an EXCEPTION_REGISTRATION_RECORD address is dispatchable.
///
/// Windows validates every record before calling its handler: the record must
/// lie wholly inside the current thread's stack (TEB StackLimit..StackBase) and
/// be 4-byte aligned.  A record that fails this test means the chain is
/// corrupt, and dispatch stops rather than calling a handler with an
/// establisher frame that points at arbitrary memory.
[[nodiscard]] bool seh32_registration_dispatchable(
    GuestMemory& memory, const CpuState& cpu, u32 frame_addr);

/// Guest VA of Vitreo's RtlpExceptionProtector equivalent (0 if unmapped).
u32 seh32_exception_protector_addr() noexcept;

/// Map a permanent RX stub implementing the nested-exception protector.
/// Idempotent; required before install_seh32_nested_execution.
VoidResult ensure_seh32_exception_protector(GuestMemory& memory);

/// State saved across a nested FS:[0] registration around a frame handler.
struct Seh32NestedExecution {
    u32 nested_addr = 0;
    u32 previous_head = 0xFFFFFFFF;
    bool installed = false;
};

/// Install the nested EXCEPTION_REGISTRATION_RECORD that Windows places on
/// FS:[0] while executing a frame handler (RtlpExecuteHandlerForException).
/// Handlers that recover via `mov ebx, fs:[0]; mov ebx, [ebx]` need Nested.Next
/// to be the establisher frame — without this they see the outer chain link.
VoidResult install_seh32_nested_execution(
    GuestMemory& memory, CpuState& cpu,
    u32 nested_addr, u32 establisher_frame,
    Seh32NestedExecution& out);

/// Unlink the nested registration using RtlpExecuteHandler2 epilogue rules:
/// if FS:[0] still names the nested record, restore Nested.Next; otherwise
/// the handler rewired the chain and FS:[0] becomes *[FS:[0]].Next.
VoidResult unlink_seh32_nested_execution(
    GuestMemory& memory, CpuState& cpu,
    const Seh32NestedExecution& nested);

} // namespace vitreo::cpu
