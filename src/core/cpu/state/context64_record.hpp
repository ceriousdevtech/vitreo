// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - the x64 Windows CONTEXT record, one implementation

#pragma once

/// @file context64_record.hpp
/// @brief Stores a CpuState into, and loads it from, a guest x64 CONTEXT
///        (winnt.h, 0x4D0 bytes), honouring ContextFlags group by group.
///
/// Every producer and consumer of an x64 CONTEXT -- GetThreadContext /
/// SetThreadContext, NtGet/NtSetContextThread, RtlCaptureContext, the
/// exception dispatcher and NtContinue / RtlRestoreContext -- goes through this
/// pair, so a record one of them writes is exactly what another reads back.
///
/// The group contents were measured on Windows 11 x64
/// (tools/oracle_probe/thread_context_probe.c / .expected):
///  - CONTEXT_AMD64 is implied: it is ORed into ContextFlags on output, and a
///    record without it is accepted.
///  - CONTROL: SegCs (0x33) at 0x38, SegSs (0x2B) at 0x42, EFlags at 0x44 with
///    IF and bit 1 set, Rsp, Rip. The other selectors are untouched.
///  - INTEGER: Rax..R15 without Rsp.
///  - SEGMENTS: SegDs/SegEs/SegGs 0x2B, SegFs 0x53.
///  - FLOATING_POINT: MxCsr at 0x34 and the 512-byte FXSAVE image (32-bit FIP /
///    FDP form, selectors 0) as FltSave at 0x100; Xmm0-15 are inside it. On a
///    load, MXCSR comes from the 0x34 field, not FltSave.MxCsr.
///  - DEBUG_REGISTERS: Dr0-3/6/7 (0x48..0x70) and the four LastBranch /
///    LastException fields (0x4B0..0x4C8), all zero -- Vitreo exposes no
///    hardware breakpoints and records no branch trace.
///  - Groups not requested are left exactly as the caller had them.

#include <vitreo/types.hpp>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::cpu {

struct CpuState;

inline constexpr u32 kContext64Size = 0x4D0;
inline constexpr u32 kContext64FlagsOffset = 0x30;

inline constexpr u32 kContext64Amd64 = 0x00100000;
inline constexpr u32 kContext64Control = kContext64Amd64 | 0x01;
inline constexpr u32 kContext64Integer = kContext64Amd64 | 0x02;
inline constexpr u32 kContext64Segments = kContext64Amd64 | 0x04;
inline constexpr u32 kContext64FloatingPoint = kContext64Amd64 | 0x08;
inline constexpr u32 kContext64DebugRegisters = kContext64Amd64 | 0x10;
inline constexpr u32 kContext64Full =
    kContext64Control | kContext64Integer | kContext64FloatingPoint;

enum class Context64Status {
    Ok,
    /// The record is not fully readable / writable guest memory.
    Fault,
    /// SetThreadContext asked to arm a hardware breakpoint (a DR7 local or
    /// global enable bit). Vitreo has no debug-register support; this is
    /// reported, never silently dropped.
    DebugRegistersUnsupported,
};

/// Writes the groups selected by @p context_flags (CONTEXT_AMD64 implied) and
/// stores context_flags | CONTEXT_AMD64 as the record's ContextFlags. @p cpu
/// must have its lazy flags materialised.
[[nodiscard]] Context64Status store_context64(GuestMemory& memory,
                                              GuestAddr record,
                                              const CpuState& cpu,
                                              u32 context_flags);

/// Loads the groups selected by the record's own ContextFlags into @p cpu.
/// @p cpu is updated only when the whole load succeeds.
/// @param restoring_current_thread true when @p cpu is the calling host
///        thread's own running state (NtContinue, RtlRestoreContext, exception
///        resume); a restored FP image then also discards the host FPU's
///        accumulated exceptions, which belong to the state being replaced.
[[nodiscard]] Context64Status load_context64(GuestMemory& memory,
                                             GuestAddr record, CpuState& cpu,
                                             bool restoring_current_thread);

} // namespace vitreo::cpu
