// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_runtime_helpers.cpp
/// @brief Host helpers that emitted Tier 1 code calls by BLR: DIV/IDIV and the PUSHF/POPF flag image (declared in
///        ir_emit_internal.hpp).

#include "ir_emit_internal.hpp"

namespace vitreo::cpu::jit {

// DIV / IDIV at every width.
//
// Returns 0 on success and nonzero for #DE, and commits NOTHING on the #DE
// path -- a zero divisor or a quotient that does not fit the destination must
// leave the guest exactly as it was, which is why the decision and the two
// register writes live together in one helper instead of being assembled from
// IR arithmetic. The emitted code branches on the status and raises.
//
// The arithmetic mirrors `exec_div`/`exec_idiv` (Intel SDM Vol. 2 DIV/IDIV):
// the byte forms divide AX and write AL/AH, the word forms divide DX:AX and
// write AX/DX, the doubleword forms divide EDX:EAX and write EAX/EDX (a
// 32-bit write, so bits 63:32 of both are cleared), and the quadword forms
// divide RDX:RAX. Flags are UNDEFINED after all of them and are left alone.
extern "C" int jit_ir_divide(CpuState* cpu, u64 divisor_bits, u32 kind) {
    constexpr u8 kRax = 0;
    constexpr u8 kRdx = 2;
    switch (static_cast<ir::DivideKind>(kind)) {
    case ir::DivideKind::Div8: {
        const u8 divisor = static_cast<u8>(divisor_bits);
        if (divisor == 0) return 1;
        const u16 ax = cpu->gpr.get16(kRax);
        const u16 quotient = static_cast<u16>(ax / divisor);
        if (quotient > 0xFF) return 1;
        const u8 remainder = static_cast<u8>(ax % divisor);
        cpu->gpr.set8l(kRax, static_cast<u8>(quotient));
        cpu->gpr.set8h(kRax, remainder);
        return 0;
    }
    case ir::DivideKind::Idiv8: {
        const i8 divisor = static_cast<i8>(static_cast<u8>(divisor_bits));
        if (divisor == 0) return 1;
        const i16 ax = static_cast<i16>(cpu->gpr.get16(kRax));
        const i16 quotient = static_cast<i16>(ax / divisor);
        if (quotient > 127 || quotient < -128) return 1;
        const i8 remainder = static_cast<i8>(ax % divisor);
        cpu->gpr.set8l(kRax, static_cast<u8>(quotient));
        cpu->gpr.set8h(kRax, static_cast<u8>(remainder));
        return 0;
    }
    case ir::DivideKind::Div16: {
        const u16 divisor = static_cast<u16>(divisor_bits);
        if (divisor == 0) return 1;
        const u32 dividend =
            (static_cast<u32>(cpu->gpr.get16(kRdx)) << 16) | cpu->gpr.get16(kRax);
        const u32 quotient = dividend / divisor;
        if (quotient > 0xFFFF) return 1;
        const u16 remainder = static_cast<u16>(dividend % divisor);
        cpu->gpr.set16(kRax, static_cast<u16>(quotient));
        cpu->gpr.set16(kRdx, remainder);
        return 0;
    }
    case ir::DivideKind::Div32: {
        const u32 divisor = static_cast<u32>(divisor_bits);
        if (divisor == 0) return 1;
        const u64 dividend =
            (static_cast<u64>(cpu->gpr.get32(kRdx)) << 32) | cpu->gpr.get32(kRax);
        const u64 quotient = dividend / divisor;
        if (quotient > 0xFFFFFFFFull) return 1;
        cpu->gpr.set32(kRax, static_cast<u32>(quotient));
        cpu->gpr.set32(kRdx, static_cast<u32>(dividend % divisor));
        return 0;
    }
    case ir::DivideKind::Idiv32: {
        const i32 divisor = static_cast<i32>(static_cast<u32>(divisor_bits));
        if (divisor == 0) return 1;
        const i64 dividend = static_cast<i64>(
            (static_cast<u64>(cpu->gpr.get32(kRdx)) << 32) | cpu->gpr.get32(kRax));
        // INT64_MIN / -1 is undefined in C++; its quotient 2^63 does not fit
        // 32 bits either, so it is #DE like every other overflow.
        if (divisor == -1 && dividend == INT64_MIN) return 1;
        const i64 quotient = dividend / divisor;
        if (quotient > INT32_MAX || quotient < INT32_MIN) return 1;
        cpu->gpr.set32(kRax, static_cast<u32>(static_cast<i32>(quotient)));
        cpu->gpr.set32(kRdx, static_cast<u32>(static_cast<i32>(dividend % divisor)));
        return 0;
    }
    case ir::DivideKind::Div64: {
        using u128 = unsigned __int128;
        const u64 divisor = divisor_bits;
        if (divisor == 0) return 1;
        const u128 dividend =
            (static_cast<u128>(cpu->gpr.get64(kRdx)) << 64) | cpu->gpr.get64(kRax);
        const u128 quotient = dividend / divisor;
        if (quotient > UINT64_MAX) return 1;
        cpu->gpr.set64(kRax, static_cast<u64>(quotient));
        cpu->gpr.set64(kRdx, static_cast<u64>(dividend % divisor));
        return 0;
    }
    case ir::DivideKind::Idiv64: {
        using i128 = __int128;
        using u128 = unsigned __int128;
        const i64 divisor = static_cast<i64>(divisor_bits);
        if (divisor == 0) return 1;
        const i128 dividend = static_cast<i128>(
            (static_cast<u128>(cpu->gpr.get64(kRdx)) << 64) | cpu->gpr.get64(kRax));
        // The i128 analogue of the Idiv32 guard: -2^127 / -1 overflows i128.
        if (divisor == -1 && dividend == static_cast<i128>(u128{1} << 127))
            return 1;
        const i128 quotient = dividend / divisor;
        if (quotient > INT64_MAX || quotient < INT64_MIN) return 1;
        cpu->gpr.set64(kRax, static_cast<u64>(static_cast<i64>(quotient)));
        cpu->gpr.set64(kRdx, static_cast<u64>(static_cast<i64>(dividend % divisor)));
        return 0;
    }
    case ir::DivideKind::Idiv16: {
        const i16 divisor = static_cast<i16>(static_cast<u16>(divisor_bits));
        if (divisor == 0) return 1;
        const i32 dividend = static_cast<i32>(
            (static_cast<u32>(cpu->gpr.get16(kRdx)) << 16) | cpu->gpr.get16(kRax));
        const i32 quotient = dividend / divisor;
        if (quotient > 32767 || quotient < -32768) return 1;
        const i16 remainder = static_cast<i16>(dividend % divisor);
        cpu->gpr.set16(kRax, static_cast<u16>(quotient));
        cpu->gpr.set16(kRdx, static_cast<u16>(remainder));
        return 0;
    }
    }
    return 1;  // The verifier admits no other kind.
}

// PUSHF / POPF, as helper calls rather than emitted masking.
//
// Both rules live in exactly one place this way -- the same masks the
// interpreter applies (Intel SDM Vol. 2 PUSHF/POPF) -- instead of being
// re-derived in generated code, where a drift between the two tiers would be
// silent. Neither can fail, so neither is a may-fault op; they clobber the
// AAPCS caller-saved set like any other helper, which is why they end a
// segment.
extern "C" u64 jit_ir_flags_image(CpuState* cpu) {
    // A stale RFLAGS image is the whole hazard: PUSHF must observe the flags
    // the preceding arithmetic produced.
    cpu->materialize_lazy_flags();
    // Intel SDM Vol. 2 PUSHF: the reserved bits read zero in the pushed image
    // and bit 1 always reads one.
    return (cpu->flags.rflags & 0x00FCFFFFull) | 0x2ull;
}

extern "C" void jit_ir_flags_install(CpuState* cpu, u64 value) {
    // CF,PF,AF,ZF,SF,TF,IF,DF,OF,AC,ID -- the user-writable set. Everything
    // else keeps its current value.
    constexpr u64 kUserMask = 0x00244FD5ull;
    cpu->flags.rflags =
        ((cpu->flags.rflags & ~kUserMask) | (value & kUserMask)) | 0x2ull;
    // A POPF is a full flag write: whatever the block still owed is dead, and
    // the CF/OF overlays a partial writer left behind must not be re-applied.
    cpu->set_flag_op(CpuState::FlagOp::None);
}

}  // namespace vitreo::cpu::jit
