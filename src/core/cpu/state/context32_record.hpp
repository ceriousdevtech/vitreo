// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - the x86 (WOW64) Windows CONTEXT record for Get/SetThreadContext

#pragma once

/// @file context32_record.hpp
/// @brief Stores a CpuState into, and loads it from, a guest i386 CONTEXT
///        (winnt.h, 0x2CC bytes), honouring ContextFlags group by group.
///
/// Measured on Windows 11 WOW64 by tools/oracle_probe/thread_context32_probe.c
/// (.expected alongside):
///  - CONTEXT_i386 (0x10000) is implied: ORed into ContextFlags on output, and
///    a record without it is accepted.
///  - CONTROL: Ebp, Eip, SegCs (0x23), EFlags, Esp, SegSs (0x2B) -- EBP is a
///    CONTROL register on x86, not INTEGER.
///  - INTEGER: Edi, Esi, Ebx, Edx, Ecx, Eax.
///  - SEGMENTS: SegGs/SegEs/SegDs 0x2B, SegFs 0x53.
///  - FLOATING_POINT: FloatSave (FNSAVE form): ControlWord, StatusWord, the
///    full two-bit-per-register TagWord, ErrorOffset (FIP), ErrorSelector,
///    DataOffset, DataSelector, the eight ST(i) in stack order, Cr0NpxState 0.
///  - DEBUG_REGISTERS: Dr0-3/6/7, all zero (no hardware breakpoints).
///  - EXTENDED_REGISTERS: the FXSAVE image, of which Windows writes the first
///    0x120 bytes (x87/MXCSR header + XMM0-7).
///  - Set: with both FLOATING_POINT and EXTENDED_REGISTERS, the x87 state
///    comes from FloatSave and MXCSR/XMM from ExtendedRegisters. EFlags is
///    sanitised to (value & 0x00210FD5) | 0x200.
///  - Groups not requested are left exactly as the caller had them.

#include <vitreo/types.hpp>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::cpu {

struct CpuState;

inline constexpr u32 kContext32Size = 0x2CC;
inline constexpr u32 kContext32I386 = 0x00010000;
inline constexpr u32 kContext32Control = kContext32I386 | 0x01;
inline constexpr u32 kContext32Integer = kContext32I386 | 0x02;
inline constexpr u32 kContext32Segments = kContext32I386 | 0x04;
inline constexpr u32 kContext32FloatingPoint = kContext32I386 | 0x08;
inline constexpr u32 kContext32DebugRegisters = kContext32I386 | 0x10;
inline constexpr u32 kContext32ExtendedRegisters = kContext32I386 | 0x20;

enum class Context32Status {
    Ok,
    Fault,                      ///< record not fully readable / writable
    DebugRegistersUnsupported,  ///< Set asked to arm a DR7 breakpoint
};

/// Writes the groups selected by @p context_flags and stores
/// context_flags | CONTEXT_i386 as ContextFlags. @p cpu must have its lazy
/// flags materialised.
[[nodiscard]] Context32Status store_context32(GuestMemory& memory,
                                              GuestAddr record,
                                              const CpuState& cpu,
                                              u32 context_flags);

/// Loads the groups the record's own ContextFlags select into @p cpu (only
/// when the whole load succeeds). For a thread other than the caller.
[[nodiscard]] Context32Status load_context32(GuestMemory& memory,
                                             GuestAddr record, CpuState& cpu);

} // namespace vitreo::cpu
