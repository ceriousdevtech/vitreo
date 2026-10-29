// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

#include "flags.hpp"
#include "vitreo/types.hpp"

#include <array>
#include <atomic>
#include <cstdlib>

namespace vitreo::cpu {

/// Standalone lazy flag evaluation state extracted from CpuState.
/// Instead of computing all 6 flags after every ALU instruction,
/// stores the operation type and operands. Flags are computed on demand
/// only when read (Jcc, CMOVcc, SETcc, ADC, SBB, PUSHF).
struct FlagState {
    enum class FlagOp : u8 {
        None = 0,   // flags.rflags is authoritative (interpreter mode)
        Add8,  Add16,  Add32,  Add64,
        Sub8,  Sub16,  Sub32,  Sub64,
        And8,  And16,  And32,  And64,
        Or8,   Or16,   Or32,   Or64,
        Xor8,  Xor16,  Xor32,  Xor64,
        Inc8,  Inc16,  Inc32,  Inc64,
        Dec8,  Dec16,  Dec32,  Dec64,
        Shl8,  Shl16,  Shl32,  Shl64,  Shl64_1,
        Shr8,  Shr16,  Shr32,  Shr64,  Shr64_1,
        Sar8,  Sar16,  Sar32,  Sar64,  Sar64_1,
        Neg8,  Neg16,  Neg32,  Neg64,
        Mul32, Imul16, Imul32, Imul64, Imul8,
        Sbb8,  // flag_src = (cf_in << 32) | src; flag_dst = result
        Sbb16, // flag_src = (cf_in << 32) | src; flag_dst = result
        Sbb32, // flag_src = (cf_in << 32) | src; flag_dst = result
        Adc32, // flag_src = (cf_in << 32) | src; flag_dst = result
        Adc8,  // flag_src = (cf_in << 32) | src; flag_dst = result
        Adc16, // flag_src = (cf_in << 32) | src; flag_dst = result
        // ROL/ROR 64-bit. flag_dst = result.
        //   _1 variants are by-1 (both CF and OF defined).
        //   Multi-bit variants only define CF; OF is left unchanged.
        Rol64, Rol64_1,
        Ror64, Ror64_1,
        // No narrow or 32-bit rotate records, deliberately. A rotate is a
        // PARTIAL flag writer -- it moves CF and OF and leaves ZF/SF/PF/AF
        // exactly as they were -- and a lazy record cannot express that: only
        // one record survives, so publishing a rotate would discard the
        // pending record a preceding instruction owed, and ZSP would
        // materialize from stale rflags. The stencil compiler writes CF/OF
        // directly and installs `cf_overlay`/`of_overlay` for precisely this
        // reason. Adding records here without overlay support would look
        // complete and be wrong; see the M11b notes in
        // .docs/perf/dbt-completion-milestones.md.
    };

    FlagOp flag_op{FlagOp::None};
    u64 flag_src{0};            // Source operand (b for add/sub)
    u64 flag_dst{0};            // Result value

    /// Materialize lazy flags into the provided Flags struct.
    /// After this call flag_op == FlagOp::None.
    void materialize(Flags& flags) noexcept {
        if (flag_op == FlagOp::None) return;

        u32 src32 = static_cast<u32>(flag_src);
        u32 dst32 = static_cast<u32>(flag_dst);

        switch (flag_op) {
        // --- ADD ---
        case FlagOp::Add8: {
            u8 b = static_cast<u8>(flag_src);
            u8 r = static_cast<u8>(flag_dst);
            u8 a = r - b;
            flags.update_flags_add(a, b, r);
            break;
        }
        case FlagOp::Add32: {
            u32 a = dst32 - src32;
            flags.update_flags_add(a, src32, dst32);
            break;
        }
        // --- SUB ---
        case FlagOp::Sub32: {
            u32 a = dst32 + src32;
            flags.update_flags_sub(a, src32, dst32);
            break;
        }
        // --- SBB (carry-in packed in upper 32 bits of flag_src) ---
        case FlagOp::Sbb8: {
            u8 src = static_cast<u8>(flag_src);
            u8 cf_in = static_cast<u8>(flag_src >> 32);
            u8 result = static_cast<u8>(flag_dst);
            u8 a = static_cast<u8>(result + src + cf_in);
            flags.update_zsp(result);
            flags.set_cf(static_cast<u16>(a) <
                         static_cast<u16>(src) + cf_in);
            constexpr u8 sign_bit = u8(1) << 7;
            flags.set_of(((a ^ src) & (a ^ result) & sign_bit) != 0);
            flags.set_af(((a ^ src ^ result) & 0x10) != 0);
            break;
        }
        case FlagOp::Sbb16: {
            u16 src = static_cast<u16>(flag_src);
            u16 cf_in = static_cast<u16>(flag_src >> 32);
            u16 result = static_cast<u16>(flag_dst);
            u16 a = static_cast<u16>(result + src + cf_in);
            flags.update_zsp(result);
            flags.set_cf(static_cast<u32>(a) <
                         static_cast<u32>(src) + cf_in);
            constexpr u16 sign_bit = u16(1) << 15;
            flags.set_of(((a ^ src) & (a ^ result) & sign_bit) != 0);
            flags.set_af(((a ^ src ^ result) & 0x10) != 0);
            break;
        }
        case FlagOp::Sbb32: {
            u32 src = static_cast<u32>(flag_src);
            u32 cf_in = static_cast<u32>(flag_src >> 32);
            u32 result = dst32;
            u32 a = result + src + cf_in; // reconstruct original dst
            flags.update_zsp(result);
            flags.set_cf(static_cast<u64>(a) < static_cast<u64>(src) + cf_in);
            constexpr u32 sign_bit = u32(1) << 31;
            flags.set_of(((a ^ src) & (a ^ result) & sign_bit) != 0);
            flags.set_af(((a ^ src ^ result) & 0x10) != 0);
            break;
        }
        // --- ADC (carry-in packed in upper 32 bits of flag_src) ---
        // Intel SDM Vol. 2A ADC: DEST := DEST + SRC + CF; OF uses SRC (not
        // SRC+CF). Adc8/Adc16 mirror Sbb8's packing for the PE32 JIT path.
        case FlagOp::Adc8: {
            u8 src = static_cast<u8>(flag_src);
            u8 cf_in = static_cast<u8>(flag_src >> 32);
            u8 result = static_cast<u8>(flag_dst);
            u8 a = static_cast<u8>(result - src - cf_in);
            flags.update_zsp(result);
            u16 wide = static_cast<u16>(a) + static_cast<u16>(src) + cf_in;
            flags.set_cf(wide > 0xFF);
            constexpr u8 sign_bit = u8(1) << 7;
            flags.set_of(((a ^ result) & (src ^ result) & sign_bit) != 0);
            flags.set_af(((a ^ src ^ result) & 0x10) != 0);
            break;
        }
        case FlagOp::Adc16: {
            u16 src = static_cast<u16>(flag_src);
            u16 cf_in = static_cast<u16>(flag_src >> 32);
            u16 result = static_cast<u16>(flag_dst);
            u16 a = static_cast<u16>(result - src - cf_in);
            flags.update_zsp(result);
            u32 wide = static_cast<u32>(a) + static_cast<u32>(src) + cf_in;
            flags.set_cf(wide > 0xFFFF);
            constexpr u16 sign_bit = u16(1) << 15;
            flags.set_of(((a ^ result) & (src ^ result) & sign_bit) != 0);
            flags.set_af(((a ^ src ^ result) & 0x10) != 0);
            break;
        }
        case FlagOp::Adc32: {
            u32 src = static_cast<u32>(flag_src);
            u32 cf_in = static_cast<u32>(flag_src >> 32);
            u32 result = dst32;
            u32 a = result - src - cf_in; // reconstruct original dst
            flags.update_zsp(result);
            u64 wide = static_cast<u64>(a) + static_cast<u64>(src) + cf_in;
            flags.set_cf(wide > 0xFFFFFFFFULL);
            constexpr u32 sign_bit = u32(1) << 31;
            flags.set_of(((a ^ result) & (src ^ result) & sign_bit) != 0);
            flags.set_af(((a ^ src ^ result) & 0x10) != 0);
            break;
        }
        // --- SUB 8-bit ---
        case FlagOp::Sub8: {
            u8 s = static_cast<u8>(flag_src);
            u8 r = static_cast<u8>(flag_dst);
            u8 a = r + s;
            flags.update_flags_sub(a, s, r);
            break;
        }
        // --- Logic ---
        case FlagOp::And8:
        case FlagOp::Or8:
        case FlagOp::Xor8:
            flags.update_flags_logic(static_cast<u8>(flag_dst));
            break;
        case FlagOp::And32:
        case FlagOp::Or32:
        case FlagOp::Xor32:
            flags.update_flags_logic(dst32);
            break;
        // --- INC (preserves CF) ---
        case FlagOp::Inc8: {
            const bool cf = ((flag_src >> 32) & 1u) != 0;
            const u8 original = static_cast<u8>(flag_src);
            const u8 result = static_cast<u8>(flag_dst);
            flags.update_flags_add(original, static_cast<u8>(1), result);
            flags.set_cf(cf);
            break;
        }
        case FlagOp::Inc16: {
            const bool cf = ((flag_src >> 32) & 1u) != 0;
            const u16 original = static_cast<u16>(flag_src);
            const u16 result = static_cast<u16>(flag_dst);
            flags.update_flags_add(original, static_cast<u16>(1), result);
            flags.set_cf(cf);
            break;
        }
        case FlagOp::Inc32: {
            bool cf = ((flag_src >> 32) & 1u) != 0;
            flags.update_flags_add(src32, static_cast<u32>(1), dst32);
            flags.set_cf(cf);
            break;
        }
        // --- DEC (preserves CF) ---
        case FlagOp::Dec8: {
            const bool cf = ((flag_src >> 32) & 1u) != 0;
            const u8 original = static_cast<u8>(flag_src);
            const u8 result = static_cast<u8>(flag_dst);
            flags.update_flags_sub(original, static_cast<u8>(1), result);
            flags.set_cf(cf);
            break;
        }
        case FlagOp::Dec16: {
            const bool cf = ((flag_src >> 32) & 1u) != 0;
            const u16 original = static_cast<u16>(flag_src);
            const u16 result = static_cast<u16>(flag_dst);
            flags.update_flags_sub(original, static_cast<u16>(1), result);
            flags.set_cf(cf);
            break;
        }
        case FlagOp::Dec32: {
            bool cf = ((flag_src >> 32) & 1u) != 0;
            flags.update_flags_sub(src32, static_cast<u32>(1), dst32);
            flags.set_cf(cf);
            break;
        }
        // --- NEG ---
        case FlagOp::Neg32:
            flags.update_flags_sub(static_cast<u32>(0), src32, dst32);
            break;
        // NEG is `0 - original`, at the operand's own width. These two had no
        // arm at all, so publishing one left the PREVIOUS definition's flags
        // live and claimed by this instruction.
        case FlagOp::Neg8:
            flags.update_flags_sub(static_cast<u8>(0), static_cast<u8>(flag_src),
                                   static_cast<u8>(flag_dst));
            break;
        case FlagOp::Neg16:
            flags.update_flags_sub(static_cast<u16>(0), static_cast<u16>(flag_src),
                                   static_cast<u16>(flag_dst));
            break;
        // --- SHL ---
        // Convention: flag_src = original, flag_dst = (count << 32) | result
        case FlagOp::Shl32: {
            u32 original = static_cast<u32>(flag_src);
            u32 result = static_cast<u32>(flag_dst);
            u32 cnt = static_cast<u32>(flag_dst >> 32) & 31;
            if (cnt == 0) break;
            flags.update_zsp(result);
            flags.set_cf((original >> (32 - cnt)) & 1);
            if (cnt == 1)
                flags.set_of(((result >> 31) ^ (original >> 31)) != 0);
            else
                flags.set_of(false);
            flags.set_af(false);
            break;
        }
        // --- SHR ---
        case FlagOp::Shr32: {
            u32 original = static_cast<u32>(flag_src);
            u32 result = static_cast<u32>(flag_dst);
            u32 cnt = static_cast<u32>(flag_dst >> 32) & 31;
            if (cnt == 0) break;
            flags.update_zsp(result);
            flags.set_cf((original >> (cnt - 1)) & 1);
            if (cnt == 1)
                flags.set_of((original >> 31) & 1);
            else
                flags.set_of(false);
            flags.set_af(false);
            break;
        }
        // --- SAR ---
        case FlagOp::Sar32: {
            u32 original = static_cast<u32>(flag_src);
            u32 result = static_cast<u32>(flag_dst);
            u32 cnt = static_cast<u32>(flag_dst >> 32) & 31;
            if (cnt == 0) break;
            flags.update_zsp(result);
            flags.set_cf((original >> (cnt - 1)) & 1);
            flags.set_of(0);
            flags.set_af(false);
            break;
        }
        // --- MUL ---
        case FlagOp::Mul32: {
            // flag_src = low32 (EAX), flag_dst = high32 (EDX)
            flags.set_cf(dst32 != 0);
            flags.set_of(dst32 != 0);
            flags.set_zf(src32 == 0 && dst32 == 0);
            flags.set_sf(false);
            flags.set_pf(false);
            flags.set_af(false);
            break;
        }
        // --- IMUL ---
        case FlagOp::Imul8: {
            // flag_src = full signed product of the two 8-bit operands.
            // Intel SDM Vol. 2A, IMUL r/m8: CF/OF when truncating to AL
            // changes the signed value; other flags undefined.
            i64 full = static_cast<i64>(flag_src);
            i8 lo = static_cast<i8>(static_cast<u8>(flag_src));
            bool overflow = (full != static_cast<i64>(lo));
            flags.set_cf(overflow);
            flags.set_of(overflow);
            break;
        }
        case FlagOp::Imul16: {
            // flag_src = full signed product of the two 16-bit operands.
            // Intel SDM Vol. 2A, IMUL: CF/OF are set when truncating the
            // product to 16 bits changes its signed value; other flags are
            // undefined and therefore retain their prior values here.
            i64 full = static_cast<i64>(flag_src);
            i16 lo = static_cast<i16>(static_cast<u16>(flag_src));
            bool overflow = (full != static_cast<i64>(lo));
            flags.set_cf(overflow);
            flags.set_of(overflow);
            break;
        }
        case FlagOp::Imul32: {
            // flag_src = full 64-bit signed result
            // Intel: two/three-operand IMUL only defines CF and OF.
            // SF/ZF/PF/AF are undefined — leave them unchanged.
            i64 full = static_cast<i64>(flag_src);
            i32 lo = static_cast<i32>(static_cast<u32>(flag_src));
            bool overflow = (full != static_cast<i64>(lo));
            flags.set_cf(overflow);
            flags.set_of(overflow);
            break;
        }
        case FlagOp::Imul64: {
            // flag_src = high 64 bits (SMULH), flag_dst = low 64 bits (MUL)
            // Intel: two/three-operand IMUL only defines CF and OF.
            // SF/ZF/PF/AF are undefined — leave them unchanged.
            i64 hi = static_cast<i64>(flag_src);
            i64 lo = static_cast<i64>(flag_dst);
            bool overflow = (hi != (lo >> 63));
            flags.set_cf(overflow);
            flags.set_of(overflow);
            break;
        }
        // --- ADD 16-bit ---
        case FlagOp::Add16: {
            u16 s = static_cast<u16>(flag_src);
            u16 r = static_cast<u16>(flag_dst);
            u16 a = r - s;
            flags.update_flags_add(a, s, r);
            break;
        }
        // --- SUB 16-bit ---
        case FlagOp::Sub16: {
            u16 s = static_cast<u16>(flag_src);
            u16 r = static_cast<u16>(flag_dst);
            u16 a = r + s;
            flags.update_flags_sub(a, s, r);
            break;
        }
        // --- Logic 16-bit ---
        case FlagOp::And16:
        case FlagOp::Or16:
        case FlagOp::Xor16:
            flags.update_flags_logic(static_cast<u16>(flag_dst));
            break;
        // --- SHR 16-bit ---
        // Convention shared with Shr32: flag_src = zero-extended original,
        // flag_dst = (count << 32) | zero-extended result.
        case FlagOp::Shr16: {
            const u16 original = static_cast<u16>(flag_src);
            const u16 result = static_cast<u16>(flag_dst);
            const u32 cnt = static_cast<u32>(flag_dst >> 32) & 31u;
            if (cnt == 0) break;
            flags.update_zsp(result);
            flags.set_cf(cnt <= 16u &&
                         ((original >> (cnt - 1u)) & 1u) != 0);
            if (cnt == 1)
                flags.set_of(((original >> 15u) & 1u) != 0);
            break;
        }
        // --- SHL/SAR 16-bit ---
        // AF is architecturally UNDEFINED after a shift. These clear it, the
        // same deterministic choice Shl32/Sar32 make, because a lazy record
        // that left it alone could not preserve it anyway: only ONE record
        // survives, so a preceding logic op's AF is gone by materialization.
        // Tests must exclude AF when comparing a shift, not chase it.
        // Same convention as Shl32/Sar32. These forced CF to false and
        // ignored the count, which is not "approximate" -- it is a different
        // answer from the interpreter's for every shift that moves a set bit
        // out. The count is already packed by the publishers.
        case FlagOp::Shl16: {
            const u16 original = static_cast<u16>(flag_src);
            const u16 result = static_cast<u16>(flag_dst);
            const u32 cnt = static_cast<u32>(flag_dst >> 32) & 31u;
            if (cnt == 0) break;
            flags.update_zsp(result);
            // Bit (width - cnt) leaves last. Past the operand width every bit
            // has already gone, so nothing is carried out.
            flags.set_cf(cnt <= 16u && ((original >> (16u - cnt)) & 1u) != 0);
            if (cnt == 1)
                flags.set_of((((result >> 15u) ^ (original >> 15u)) & 1u) != 0);
            else
                flags.set_of(false);
            flags.set_af(false);
            break;
        }
        case FlagOp::Sar16: {
            const u16 original = static_cast<u16>(flag_src);
            const u16 result = static_cast<u16>(flag_dst);
            const u32 cnt = static_cast<u32>(flag_dst >> 32) & 31u;
            if (cnt == 0) break;
            flags.update_zsp(result);
            // Bit (cnt - 1) leaves last. Past the width SAR is replicating the
            // sign, so the carry settles on the sign bit.
            flags.set_cf(((original >> (cnt <= 16u ? cnt - 1u : 15u)) & 1u) != 0);
            flags.set_of(false);
            flags.set_af(false);
            break;
        }
        // --- SHR 8-bit ---
        // Convention shared with Shr16/Shr32: flag_src = zero-extended
        // original, flag_dst = (count << 32) | zero-extended result.
        case FlagOp::Shr8: {
            const u8 original = static_cast<u8>(flag_src);
            const u8 result = static_cast<u8>(flag_dst);
            const u32 cnt = static_cast<u32>(flag_dst >> 32) & 31u;
            if (cnt == 0) break;
            flags.update_zsp(result);
            flags.set_cf(cnt <= 8u &&
                         ((original >> (cnt - 1u)) & 1u) != 0);
            if (cnt == 1)
                flags.set_of(((original >> 7u) & 1u) != 0);
            break;
        }
        // --- SHL/SAR 8-bit --- (see the 16-bit pair above)
        case FlagOp::Shl8: {
            const u8 original = static_cast<u8>(flag_src);
            const u8 result = static_cast<u8>(flag_dst);
            const u32 cnt = static_cast<u32>(flag_dst >> 32) & 31u;
            if (cnt == 0) break;
            flags.update_zsp(result);
            flags.set_cf(cnt <= 8u && ((original >> (8u - cnt)) & 1u) != 0);
            if (cnt == 1)
                flags.set_of((((result >> 7u) ^ (original >> 7u)) & 1u) != 0);
            else
                flags.set_of(false);
            flags.set_af(false);
            break;
        }
        case FlagOp::Sar8: {
            const u8 original = static_cast<u8>(flag_src);
            const u8 result = static_cast<u8>(flag_dst);
            const u32 cnt = static_cast<u32>(flag_dst >> 32) & 31u;
            if (cnt == 0) break;
            flags.update_zsp(result);
            flags.set_cf(((original >> (cnt <= 8u ? cnt - 1u : 7u)) & 1u) != 0);
            flags.set_of(false);
            flags.set_af(false);
            break;
        }
        // ====================================================================
        // 64-bit variants (used by 64-bit JIT)
        // ====================================================================
        // --- ADD 64-bit ---
        case FlagOp::Add64: {
            u64 s = flag_src;
            u64 r = flag_dst;
            u64 a = r - s;
            // ZF/SF/PF on 64-bit result
            flags.set_zf(r == 0);
            flags.set_sf((r >> 63) & 1);
            flags.set_pf(__builtin_parity(static_cast<u8>(r)) == 0);
            // CF: unsigned overflow
            flags.set_cf(r < a);
            // OF: signed overflow
            constexpr u64 sign_bit = u64(1) << 63;
            flags.set_of((((a ^ r) & (s ^ r)) & sign_bit) != 0);
            // AF: bit 4 carry
            flags.set_af(((a ^ s ^ r) & 0x10) != 0);
            break;
        }
        // --- SUB 64-bit ---
        case FlagOp::Sub64: {
            u64 s = flag_src;
            u64 r = flag_dst;
            u64 a = r + s;
            flags.set_zf(r == 0);
            flags.set_sf((r >> 63) & 1);
            flags.set_pf(__builtin_parity(static_cast<u8>(r)) == 0);
            flags.set_cf(a < s);
            constexpr u64 sign_bit = u64(1) << 63;
            flags.set_of((((a ^ s) & (a ^ r)) & sign_bit) != 0);
            flags.set_af(((a ^ s ^ r) & 0x10) != 0);
            break;
        }
        // --- Logic 64-bit (AND/OR/XOR/TEST) ---
        case FlagOp::And64:
        case FlagOp::Or64:
        case FlagOp::Xor64: {
            u64 r = flag_dst;
            flags.set_zf(r == 0);
            flags.set_sf((r >> 63) & 1);
            flags.set_pf(__builtin_parity(static_cast<u8>(r)) == 0);
            flags.set_cf(false);
            flags.set_of(false);
            flags.set_af(false);
            break;
        }
        // --- INC 64-bit (preserves CF) ---
        case FlagOp::Inc64: {
            const bool cf = ((flag_src >> 32) & 1u) != 0;
            u64 r = flag_dst;
            u64 a = r - 1;
            flags.set_zf(r == 0);
            flags.set_sf((r >> 63) & 1);
            flags.set_pf(__builtin_parity(static_cast<u8>(r)) == 0);
            constexpr u64 sign_bit = u64(1) << 63;
            flags.set_of((((a ^ r) & (1ULL ^ r)) & sign_bit) != 0);
            flags.set_af(((a ^ 1 ^ r) & 0x10) != 0);
            flags.set_cf(cf);
            break;
        }
        // --- DEC 64-bit (preserves CF) ---
        case FlagOp::Dec64: {
            const bool cf = ((flag_src >> 32) & 1u) != 0;
            u64 r = flag_dst;
            u64 a = r + 1;
            flags.set_zf(r == 0);
            flags.set_sf((r >> 63) & 1);
            flags.set_pf(__builtin_parity(static_cast<u8>(r)) == 0);
            constexpr u64 sign_bit = u64(1) << 63;
            flags.set_of((((a ^ 1) & (a ^ r)) & sign_bit) != 0);
            flags.set_af(((a ^ 1 ^ r) & 0x10) != 0);
            flags.set_cf(cf);
            break;
        }
        // --- NEG 64-bit ---
        case FlagOp::Neg64: {
            u64 s = flag_src;
            u64 r = flag_dst;
            flags.set_zf(r == 0);
            flags.set_sf((r >> 63) & 1);
            flags.set_pf(__builtin_parity(static_cast<u8>(r)) == 0);
            flags.set_cf(s != 0);
            constexpr u64 sign_bit = u64(1) << 63;
            flags.set_of(((0ULL ^ s) & (0ULL ^ r) & sign_bit) != 0);
            flags.set_af((s & 0x0F) != 0);
            break;
        }
        // --- SHL/SHR/SAR 64-bit, masked count 2..63 ---
        // Convention: flag_src = original, flag_dst = result, both full 64-bit,
        // so there is no room for the count, and this record alone does not
        // determine CF. That is a contract between the publishers and this
        // case, not an approximation (Intel SDM Vol. 2 SAL/SAR/SHL/SHR):
        //   * ZF/SF/PF follow the result, and are set here.
        //   * CF, the last bit shifted out, is written by every publisher of
        //     these three records into rflags.CF and cf_overlay
        //     (JitCompiler::emit_shift64_cf / emit_shift64_cf_imm; the IR's
        //     SetCf), so it is left alone here.
        //   * OF is defined only for a count of 1, which publishes the
        //     Shl64_1 / Shr64_1 / Sar64_1 records instead -- the CL forms
        //     switch at run time. Above 1 OF is undefined and is left as it
        //     was, which is what the interpreter does.
        //   * AF is undefined after a shift and is cleared, the same
        //     deterministic choice every narrower shift record makes.
        //   * A zero count publishes nothing (it changes no flag).
        // try_cf()/try_of() return -1 for these records for the same reason.
        case FlagOp::Shl64:
        case FlagOp::Shr64:
        case FlagOp::Sar64: {
            u64 r = flag_dst;
            flags.set_zf(r == 0);
            flags.set_sf((r >> 63) & 1);
            flags.set_pf(__builtin_parity(static_cast<u8>(r)) == 0);
            flags.set_af(false);
            break;
        }
        // --- By-1 shift variants: CF/OF are fully defined for count == 1 ---
        // Convention: flag_src = original value, flag_dst = result.
        case FlagOp::Shl64_1: {
            u64 orig = flag_src;
            u64 r    = flag_dst;
            bool cf = (orig >> 63) != 0;          // last bit shifted out (bit 63)
            bool of = cf ^ ((r >> 63) != 0);       // OF = sign change
            flags.set_cf(cf);
            flags.set_of(of);
            flags.set_zf(r == 0);
            flags.set_sf((r >> 63) & 1);
            flags.set_pf(__builtin_parity(static_cast<u8>(r)) == 0);
            flags.set_af(false);
            break;
        }
        case FlagOp::Shr64_1: {
            u64 orig = flag_src;
            u64 r    = flag_dst;
            bool cf = (orig & 1) != 0;             // last bit shifted out (bit 0)
            bool of = (orig >> 63) != 0;            // OF = original sign bit (sign changed to 0)
            flags.set_cf(cf);
            flags.set_of(of);
            flags.set_zf(r == 0);
            flags.set_sf((r >> 63) & 1);
            flags.set_pf(__builtin_parity(static_cast<u8>(r)) == 0);
            flags.set_af(false);
            break;
        }
        case FlagOp::Sar64_1: {
            u64 orig = flag_src;
            u64 r    = flag_dst;
            bool cf = (orig & 1) != 0;             // last bit shifted out (bit 0)
            flags.set_cf(cf);
            flags.set_of(false);                   // SAR by 1 never sets OF
            flags.set_zf(r == 0);
            flags.set_sf((r >> 63) & 1);
            flags.set_pf(__builtin_parity(static_cast<u8>(r)) == 0);
            flags.set_af(false);
            break;
        }
        // --- ROL/ROR 64-bit ---
        // Intel: ROL/ROR only define CF; OF is defined only when count == 1.
        // ZF/SF/PF/AF are untouched. flag_dst = result.
        case FlagOp::Rol64: {
            // CF = LSB of result.
            flags.set_cf((flag_dst & 1) != 0);
            break;
        }
        case FlagOp::Rol64_1: {
            // CF = result bit 0; OF = MSB(result) XOR CF.
            u64 r = flag_dst;
            bool cf = (r & 1) != 0;
            bool msb = (r >> 63) & 1;
            flags.set_cf(cf);
            flags.set_of(msb ^ cf);
            break;
        }
        case FlagOp::Ror64: {
            // CF = MSB of result.
            flags.set_cf(((flag_dst >> 63) & 1) != 0);
            break;
        }
        case FlagOp::Ror64_1: {
            // CF = MSB(result); OF = MSB XOR (MSB-1) of result.
            u64 r = flag_dst;
            bool msb   = (r >> 63) & 1;
            bool msb_1 = (r >> 62) & 1;
            flags.set_cf(msb);
            flags.set_of(msb ^ msb_1);
            break;
        }
        default:
            // Unreachable in JIT — interpreter writes flags directly.
            break;
        }

        flag_op = FlagOp::None;
    }

    // ====================================================================
    // Direct condition evaluation
    // ====================================================================
    // Jcc/SETcc/CMOVcc read one or two flag bits, but materialize() computes
    // all six and does a read-modify-write on rflags. On flag-dense guest code
    // (hash and decompression inner loops) that difference dominates JIT CPU
    // time. The queries below derive only the bits a condition needs and leave
    // the lazy state untouched, so a later full read still materializes exactly
    // the same values.
    //
    // Every query returns -1 when the recorded op does not determine that bit,
    // in which case the caller must fall back to materialize(). "Does not
    // determine" covers three cases that must not be guessed at: Intel leaves
    // the flag undefined (MUL/IMUL result flags), Intel leaves it unchanged
    // (ROL/ROR ZF/SF/PF, multi-bit rotate OF, a shift with count 0), or this
    // lazy encoding simply does not record enough to recover it (Shl64/Shr64/
    // Sar64 carry out). In all of those materialize() leaves the prior rflags
    // bit in place, which only the caller can read.

    /// Shift count, as recorded in the high half of flag_dst by the
    /// Shl32/Shr32/Sar32/Shr16/Shr8 lazy encodings.
    [[nodiscard]] u32 shift_count() const noexcept {
        return static_cast<u32>(flag_dst >> 32) & 31u;
    }

    /// Width in bits of the architectural result recorded in flag_dst, or 0
    /// when ZF/SF/PF do not follow from it.
    [[nodiscard]] u32 result_width() const noexcept {
        switch (flag_op) {
        case FlagOp::Add8:  case FlagOp::Sub8:
        case FlagOp::And8:  case FlagOp::Or8:  case FlagOp::Xor8:
        case FlagOp::Inc8:  case FlagOp::Dec8:
        case FlagOp::Sbb8:
        case FlagOp::Adc8:
        case FlagOp::Neg8:
            return 8;
        case FlagOp::Shl8: case FlagOp::Sar8: case FlagOp::Shr8:
            return shift_count() == 0 ? 0 : 8;
        case FlagOp::Add16: case FlagOp::Sub16:
        case FlagOp::And16: case FlagOp::Or16: case FlagOp::Xor16:
        case FlagOp::Inc16: case FlagOp::Dec16:
        case FlagOp::Sbb16:
        case FlagOp::Adc16:
        case FlagOp::Neg16:
            return 16;
        case FlagOp::Shl16: case FlagOp::Sar16: case FlagOp::Shr16:
            return shift_count() == 0 ? 0 : 16;
        case FlagOp::Add32: case FlagOp::Sub32:
        case FlagOp::And32: case FlagOp::Or32: case FlagOp::Xor32:
        case FlagOp::Inc32: case FlagOp::Dec32: case FlagOp::Neg32:
        case FlagOp::Sbb32: case FlagOp::Adc32:
            return 32;
        case FlagOp::Shl32: case FlagOp::Shr32: case FlagOp::Sar32:
            return shift_count() == 0 ? 0 : 32;
        case FlagOp::Add64: case FlagOp::Sub64:
        case FlagOp::And64: case FlagOp::Or64: case FlagOp::Xor64:
        case FlagOp::Inc64: case FlagOp::Dec64: case FlagOp::Neg64:
        case FlagOp::Shl64: case FlagOp::Shr64: case FlagOp::Sar64:
        case FlagOp::Shl64_1: case FlagOp::Shr64_1: case FlagOp::Sar64_1:
            return 64;
        default:
            // None; Mul32/Imul* (result flags undefined); Rol*/Ror*
            // (unchanged).
            return 0;
        }
    }

    /// The architectural result, masked to `width` bits.
    [[nodiscard]] u64 result_bits(u32 width) const noexcept {
        return width == 64 ? flag_dst : (flag_dst & ((1ull << width) - 1));
    }

    /// CF, or -1 when the lazy state does not determine it.
    [[nodiscard]] int try_cf() const noexcept {
        const u64 s = flag_src;
        const u64 r = flag_dst;
        switch (flag_op) {
        // ADD: update_flags_add computes cf = result < original.
        case FlagOp::Add8: {
            const u8 b = static_cast<u8>(s), res = static_cast<u8>(r);
            return res < static_cast<u8>(res - b);
        }
        case FlagOp::Add16: {
            const u16 b = static_cast<u16>(s), res = static_cast<u16>(r);
            return res < static_cast<u16>(res - b);
        }
        case FlagOp::Add32: {
            const u32 b = static_cast<u32>(s), res = static_cast<u32>(r);
            return res < static_cast<u32>(res - b);
        }
        case FlagOp::Add64:
            return r < static_cast<u64>(r - s);
        // SUB: update_flags_sub computes cf = original < src (borrow).
        case FlagOp::Sub8: {
            const u8 b = static_cast<u8>(s), res = static_cast<u8>(r);
            return static_cast<u8>(res + b) < b;
        }
        case FlagOp::Sub16: {
            const u16 b = static_cast<u16>(s), res = static_cast<u16>(r);
            return static_cast<u16>(res + b) < b;
        }
        case FlagOp::Sub32: {
            const u32 b = static_cast<u32>(s), res = static_cast<u32>(r);
            return static_cast<u32>(res + b) < b;
        }
        case FlagOp::Sub64:
            return static_cast<u64>(r + s) < s;
        // Logic clears CF. The narrow shifts used to be grouped here because
        // they reused the logic materializer; they carry a real bit out now.
        case FlagOp::And8:  case FlagOp::And16:
        case FlagOp::And32: case FlagOp::And64:
        case FlagOp::Or8:   case FlagOp::Or16:
        case FlagOp::Or32:  case FlagOp::Or64:
        case FlagOp::Xor8:  case FlagOp::Xor16:
        case FlagOp::Xor32: case FlagOp::Xor64:
            return 0;
        // Narrow SHL/SAR. A zero count changes nothing, so the lazy state does
        // not determine CF and the caller must keep what it has.
        case FlagOp::Shl8: {
            const u32 cnt = shift_count();
            if (cnt == 0) return -1;
            return cnt <= 8u && ((static_cast<u8>(s) >> (8u - cnt)) & 1u) != 0;
        }
        case FlagOp::Shl16: {
            const u32 cnt = shift_count();
            if (cnt == 0) return -1;
            return cnt <= 16u && ((static_cast<u16>(s) >> (16u - cnt)) & 1u) != 0;
        }
        case FlagOp::Sar8: {
            const u32 cnt = shift_count();
            if (cnt == 0) return -1;
            return ((static_cast<u8>(s) >> (cnt <= 8u ? cnt - 1u : 7u)) & 1u) != 0;
        }
        case FlagOp::Sar16: {
            const u32 cnt = shift_count();
            if (cnt == 0) return -1;
            return ((static_cast<u16>(s) >> (cnt <= 16u ? cnt - 1u : 15u)) & 1u) != 0;
        }
        // INC/DEC preserve CF; the incoming value rides in flag_src bit 32.
        case FlagOp::Inc8:  case FlagOp::Inc16:
        case FlagOp::Inc32: case FlagOp::Inc64:
        case FlagOp::Dec8:  case FlagOp::Dec16:
        case FlagOp::Dec32: case FlagOp::Dec64:
            return static_cast<int>((s >> 32) & 1u);
        // NEG: CF = (source != 0) at the operand's width (Intel SDM Vol. 2).
        case FlagOp::Neg8:
            return static_cast<u8>(s) != 0;
        case FlagOp::Neg16:
            return static_cast<u16>(s) != 0;
        case FlagOp::Neg32:
            return static_cast<u32>(s) != 0;
        case FlagOp::Neg64:
            return s != 0;
        // Shifts: CF is the last bit shifted out, undefined at count 0 —
        // where materialize() leaves the prior CF in place.
        case FlagOp::Shl32: {
            const u32 c = shift_count();
            if (c == 0) return -1;
            return static_cast<int>((static_cast<u32>(s) >> (32u - c)) & 1u);
        }
        case FlagOp::Shr32: case FlagOp::Sar32: {
            const u32 c = shift_count();
            if (c == 0) return -1;
            return static_cast<int>((static_cast<u32>(s) >> (c - 1u)) & 1u);
        }
        case FlagOp::Shr16: {
            const u32 c = shift_count();
            if (c == 0) return -1;
            return c <= 16u &&
                   ((static_cast<u16>(s) >> (c - 1u)) & 1u) != 0;
        }
        case FlagOp::Shr8: {
            const u32 c = shift_count();
            if (c == 0) return -1;
            return c <= 8u &&
                   ((static_cast<u8>(s) >> (c - 1u)) & 1u) != 0;
        }
        case FlagOp::Shl64_1:
            return static_cast<int>((s >> 63) & 1u);
        case FlagOp::Shr64_1: case FlagOp::Sar64_1:
            return static_cast<int>(s & 1u);
        case FlagOp::Sbb8: {
            const u8 src = static_cast<u8>(s);
            const u8 cf_in = static_cast<u8>(s >> 32);
            const u8 res = static_cast<u8>(r);
            const u8 a = static_cast<u8>(res + src + cf_in);
            return static_cast<u16>(a) < static_cast<u16>(src) + cf_in;
        }
        case FlagOp::Sbb16: {
            const u16 src = static_cast<u16>(s);
            const u16 cf_in = static_cast<u16>(s >> 32);
            const u16 res = static_cast<u16>(r);
            const u16 a = static_cast<u16>(res + src + cf_in);
            return static_cast<u32>(a) < static_cast<u32>(src) + cf_in;
        }
        case FlagOp::Sbb32: {
            const u32 src = static_cast<u32>(s);
            const u32 cf_in = static_cast<u32>(s >> 32);
            const u32 res = static_cast<u32>(r);
            const u32 a = res + src + cf_in;
            return static_cast<u64>(a) < static_cast<u64>(src) + cf_in;
        }
        case FlagOp::Adc8: {
            const u8 src = static_cast<u8>(s);
            const u8 cf_in = static_cast<u8>(s >> 32);
            const u8 res = static_cast<u8>(r);
            const u8 a = static_cast<u8>(res - src - cf_in);
            return static_cast<u16>(a) + src + cf_in > 0xFF;
        }
        case FlagOp::Adc16: {
            const u16 src = static_cast<u16>(s);
            const u16 cf_in = static_cast<u16>(s >> 32);
            const u16 res = static_cast<u16>(r);
            const u16 a = static_cast<u16>(res - src - cf_in);
            return static_cast<u32>(a) + src + cf_in > 0xFFFF;
        }
        case FlagOp::Adc32: {
            const u32 src = static_cast<u32>(s);
            const u32 cf_in = static_cast<u32>(s >> 32);
            const u32 res = static_cast<u32>(r);
            const u32 a = res - src - cf_in;
            return static_cast<u64>(a) + src + cf_in > 0xFFFFFFFFull;
        }
        case FlagOp::Mul32:
            return static_cast<u32>(r) != 0;
        case FlagOp::Imul8: {
            const i64 full = static_cast<i64>(s);
            return full != static_cast<i64>(
                               static_cast<i8>(static_cast<u8>(s)));
        }
        case FlagOp::Imul16: {
            const i64 full = static_cast<i64>(s);
            return full != static_cast<i64>(
                               static_cast<i16>(static_cast<u16>(s)));
        }
        case FlagOp::Imul32: {
            const i64 full = static_cast<i64>(s);
            return full != static_cast<i64>(
                               static_cast<i32>(static_cast<u32>(s)));
        }
        case FlagOp::Imul64:
            return static_cast<i64>(s) != (static_cast<i64>(r) >> 63);
        case FlagOp::Rol64: case FlagOp::Rol64_1:
            return static_cast<int>(r & 1u);
        case FlagOp::Ror64: case FlagOp::Ror64_1:
            return static_cast<int>((r >> 63) & 1u);
        default:
            // None; Shl64/Shr64/Sar64 (carry out not recorded -- the
            // publisher writes it to rflags.CF and cf_overlay).
            return -1;
        }
    }

    /// OF, or -1 when the lazy state does not determine it.
    [[nodiscard]] int try_of() const noexcept {
        const u64 s = flag_src;
        const u64 r = flag_dst;
        switch (flag_op) {
        case FlagOp::Add8:
            return of_add<u8>(static_cast<u8>(r) - static_cast<u8>(s),
                              static_cast<u8>(s), static_cast<u8>(r));
        case FlagOp::Add16:
            return of_add<u16>(static_cast<u16>(r) - static_cast<u16>(s),
                               static_cast<u16>(s), static_cast<u16>(r));
        case FlagOp::Add32:
            return of_add<u32>(static_cast<u32>(r) - static_cast<u32>(s),
                               static_cast<u32>(s), static_cast<u32>(r));
        case FlagOp::Add64:
            return of_add<u64>(r - s, s, r);
        case FlagOp::Sub8:
            return of_sub<u8>(static_cast<u8>(r) + static_cast<u8>(s),
                              static_cast<u8>(s), static_cast<u8>(r));
        case FlagOp::Sub16:
            return of_sub<u16>(static_cast<u16>(r) + static_cast<u16>(s),
                               static_cast<u16>(s), static_cast<u16>(r));
        case FlagOp::Sub32:
            return of_sub<u32>(static_cast<u32>(r) + static_cast<u32>(s),
                               static_cast<u32>(s), static_cast<u32>(r));
        case FlagOp::Sub64:
            return of_sub<u64>(r + s, s, r);
        case FlagOp::And8:  case FlagOp::And16:
        case FlagOp::And32: case FlagOp::And64:
        case FlagOp::Or8:   case FlagOp::Or16:
        case FlagOp::Or32:  case FlagOp::Or64:
        case FlagOp::Xor8:  case FlagOp::Xor16:
        case FlagOp::Xor32: case FlagOp::Xor64:
        case FlagOp::Sar64_1:
            return 0;
        // SAR clears OF for a by-1 shift and leaves it undefined above that;
        // `materialize` settles on 0 either way. SHL's OF is the result's sign
        // against the carry out, which only a by-1 shift defines.
        case FlagOp::Sar8: case FlagOp::Sar16:
            return shift_count() == 0 ? -1 : 0;
        case FlagOp::Shl8: {
            const u32 cnt = shift_count();
            if (cnt == 0) return -1;
            if (cnt != 1) return 0;
            return (((static_cast<u8>(r) >> 7u) ^ (static_cast<u8>(s) >> 7u)) & 1u) != 0;
        }
        case FlagOp::Shl16: {
            const u32 cnt = shift_count();
            if (cnt == 0) return -1;
            if (cnt != 1) return 0;
            return (((static_cast<u16>(r) >> 15u) ^ (static_cast<u16>(s) >> 15u)) & 1u) != 0;
        }
        // INC/DEC: an add of 1 / subtract of 1. The 8/16/32-bit encodings
        // carry the pre-op value in the low half of flag_src and materialize()
        // uses it directly; only the 64-bit forms rederive it as result -+ 1.
        // Mirror that split exactly so the two paths cannot disagree.
        case FlagOp::Inc8:
            return of_add<u8>(static_cast<u8>(s), 1u, static_cast<u8>(r));
        case FlagOp::Inc16:
            return of_add<u16>(static_cast<u16>(s), 1u, static_cast<u16>(r));
        case FlagOp::Inc32:
            return of_add<u32>(static_cast<u32>(s), 1u, static_cast<u32>(r));
        case FlagOp::Inc64:
            return of_add<u64>(r - 1u, 1u, r);
        case FlagOp::Dec8:
            return of_sub<u8>(static_cast<u8>(s), 1u, static_cast<u8>(r));
        case FlagOp::Dec16:
            return of_sub<u16>(static_cast<u16>(s), 1u, static_cast<u16>(r));
        case FlagOp::Dec32:
            return of_sub<u32>(static_cast<u32>(s), 1u, static_cast<u32>(r));
        case FlagOp::Dec64:
            return of_sub<u64>(r + 1u, 1u, r);
        // NEG is a subtract from zero.
        case FlagOp::Neg8:
            return of_sub<u8>(0u, static_cast<u8>(s), static_cast<u8>(r));
        case FlagOp::Neg16:
            return of_sub<u16>(0u, static_cast<u16>(s), static_cast<u16>(r));
        case FlagOp::Neg32:
            return of_sub<u32>(0u, static_cast<u32>(s), static_cast<u32>(r));
        case FlagOp::Neg64:
            return of_sub<u64>(0u, s, r);
        // Shifts define OF only at count 1; materialize() leaves the prior
        // OF in place otherwise for Shr8/Shr16, and clears it for the 32-bit
        // forms, so those two groups differ.
        case FlagOp::Shl32: {
            const u32 c = shift_count();
            if (c == 0) return -1;
            if (c != 1) return 0;
            return static_cast<int>(((static_cast<u32>(r) >> 31) ^
                                     (static_cast<u32>(s) >> 31)) & 1u);
        }
        case FlagOp::Shr32: {
            const u32 c = shift_count();
            if (c == 0) return -1;
            return c == 1 ? static_cast<int>((static_cast<u32>(s) >> 31) & 1u)
                          : 0;
        }
        case FlagOp::Sar32:
            return shift_count() == 0 ? -1 : 0;
        case FlagOp::Shr16: {
            if (shift_count() != 1) return -1;
            return static_cast<int>((static_cast<u16>(s) >> 15) & 1u);
        }
        case FlagOp::Shr8: {
            if (shift_count() != 1) return -1;
            return static_cast<int>((static_cast<u8>(s) >> 7) & 1u);
        }
        case FlagOp::Shl64_1:
            return static_cast<int>(((s >> 63) ^ (r >> 63)) & 1u);
        case FlagOp::Shr64_1:
            return static_cast<int>((s >> 63) & 1u);
        case FlagOp::Sbb8: {
            const u8 src = static_cast<u8>(s);
            const u8 cf_in = static_cast<u8>(s >> 32);
            const u8 res = static_cast<u8>(r);
            const u8 a = static_cast<u8>(res + src + cf_in);
            return ((a ^ src) & (a ^ res) & u8(0x80)) != 0;
        }
        case FlagOp::Sbb16: {
            const u16 src = static_cast<u16>(s);
            const u16 cf_in = static_cast<u16>(s >> 32);
            const u16 res = static_cast<u16>(r);
            const u16 a = static_cast<u16>(res + src + cf_in);
            return ((a ^ src) & (a ^ res) & (u16(1) << 15)) != 0;
        }
        case FlagOp::Sbb32: {
            const u32 src = static_cast<u32>(s);
            const u32 cf_in = static_cast<u32>(s >> 32);
            const u32 res = static_cast<u32>(r);
            const u32 a = res + src + cf_in;
            return ((a ^ src) & (a ^ res) & (u32(1) << 31)) != 0;
        }
        case FlagOp::Adc8: {
            const u8 src = static_cast<u8>(s);
            const u8 cf_in = static_cast<u8>(s >> 32);
            const u8 res = static_cast<u8>(r);
            const u8 a = static_cast<u8>(res - src - cf_in);
            return ((a ^ res) & (src ^ res) & u8(0x80)) != 0;
        }
        case FlagOp::Adc16: {
            const u16 src = static_cast<u16>(s);
            const u16 cf_in = static_cast<u16>(s >> 32);
            const u16 res = static_cast<u16>(r);
            const u16 a = static_cast<u16>(res - src - cf_in);
            return ((a ^ res) & (src ^ res) & (u16(1) << 15)) != 0;
        }
        case FlagOp::Adc32: {
            const u32 src = static_cast<u32>(s);
            const u32 cf_in = static_cast<u32>(s >> 32);
            const u32 res = static_cast<u32>(r);
            const u32 a = res - src - cf_in;
            return ((a ^ res) & (src ^ res) & (u32(1) << 31)) != 0;
        }
        case FlagOp::Mul32:
            return static_cast<u32>(r) != 0;
        case FlagOp::Imul8: case FlagOp::Imul16:
        case FlagOp::Imul32: case FlagOp::Imul64:
            return try_cf();  // IMUL sets OF == CF.
        case FlagOp::Rol64_1: {
            const int cf = static_cast<int>(r & 1u);
            return static_cast<int>((r >> 63) & 1u) ^ cf;
        }
        case FlagOp::Ror64_1:
            return static_cast<int>(((r >> 63) ^ (r >> 62)) & 1u);
        default:
            // None; Shl64/Shr64/Sar64 (OF undefined above count 1, left as
            // it was); multi-bit Rol64/Ror64 (Intel leaves OF unchanged for a
            // rotate count other than 1).
            return -1;
        }
    }

    /// Evaluate x86 condition code `cc` (0x0..0xF) straight from the lazy
    /// state. Returns 0 or 1, or -1 when the caller must materialize instead.
    [[nodiscard]] int try_condition(u8 cc) const noexcept {
        if (flag_op == FlagOp::None) return -1;
        switch (cc & 0xF) {
        case 0x0: return try_of();                       // O
        case 0x1: { const int v = try_of(); return v < 0 ? -1 : (v ^ 1); }
        case 0x2: return try_cf();                       // B  (CF)
        case 0x3: { const int v = try_cf(); return v < 0 ? -1 : (v ^ 1); }
        case 0x4: case 0x5:                              // Z  / NZ
        case 0x8: case 0x9:                              // S  / NS
        case 0xA: case 0xB: {                            // P  / NP
            const u32 w = result_width();
            if (w == 0) return -1;
            const u64 res = result_bits(w);
            int v;
            if ((cc & 0xE) == 0x4)      v = static_cast<int>(res == 0);
            else if ((cc & 0xE) == 0x8) v = static_cast<int>((res >> (w - 1)) & 1u);
            else v = static_cast<int>(
                     __builtin_parity(static_cast<u8>(res)) == 0);
            return (cc & 1) ? (v ^ 1) : v;
        }
        case 0x6: case 0x7: {                            // BE / A  (CF | ZF)
            const u32 w = result_width();
            if (w == 0) return -1;
            const int cf = try_cf();
            if (cf < 0) return -1;
            const int v = cf | static_cast<int>(result_bits(w) == 0);
            return (cc & 1) ? (v ^ 1) : v;
        }
        default: {                                       // L/GE/LE/G
            const u32 w = result_width();
            if (w == 0) return -1;
            const int of = try_of();
            if (of < 0) return -1;
            const u64 res = result_bits(w);
            int v = static_cast<int>((res >> (w - 1)) & 1u) ^ of;   // SF != OF
            if ((cc & 0xF) >= 0xE) v |= static_cast<int>(res == 0); // LE adds ZF
            return (cc & 1) ? (v ^ 1) : v;
        }
        }
    }

private:
    template <typename T>
    static int of_add(T a, T b, T result) noexcept {
        constexpr T sign_bit = static_cast<T>(1) << (sizeof(T) * 8 - 1);
        return ((a ^ result) & (b ^ result) & sign_bit) != 0;
    }
    template <typename T>
    static int of_sub(T a, T b, T result) noexcept {
        constexpr T sign_bit = static_cast<T>(1) << (sizeof(T) * 8 - 1);
        return ((a ^ b) & (a ^ result) & sign_bit) != 0;
    }
};

} // namespace vitreo::cpu
