// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE2 integer arithmetic instruction execution (CPU-037)

#include <cstdio>
#include "exec_ops.hpp"
#include "helpers.hpp"
#include <cstring>

namespace vitreo::cpu::ops {

// ============================================================================
// SSE register / operand helpers (same as sse_arith.cpp)
// ============================================================================

static XmmReg& get_xmm(ExecContext& ctx, u8 index) noexcept {
    return ctx.cpu.sse.xmm[index & 15];
}

/// Read packed 128-bit from operand into XmmReg.
///
/// A 128-bit MEMORY operand must be 16-byte aligned, and that is a property of
/// the OPERAND rather than of the opcode -- which is what makes it one check
/// here instead of an audit of all forty-three call sites. Measured on silicon,
/// `tools/oracle_probe/sse_align_probe.c` with its recorded output beside it:
/// on a real Alder Lake core every 128-bit operand faults when misaligned
/// (PADDB, PADDD, PSUBQ, PMULLW, PMADDWD, PMINUB, PAVGB, PSADBW, PANDN,
/// PACKSSWB, PSLLW, PSRAD, PCMPEQB were all checked, alongside the bitwise and
/// copy forms), while every NARROWER one does not (MOVD m32, MOVQ m64,
/// MOVSS m32, MOVSD m64, MOVLPS m64 all complete). The explicitly unaligned
/// spellings -- MOVUPS, MOVUPD, MOVDQU, LDDQU -- are the copies, and none of
/// them reads through here.
///
/// The `Mem128` guard is what keeps that distinction honest: a caller passing
/// a narrower operand would be over-reading anyway, and must not inherit a
/// rule its width does not have.
///
/// `aligned_form` is false for VEX encodings, which are unaligned unless the
/// opcode says otherwise (Intel SDM Vol. 2) -- the same exemption `avx.cpp`
/// and the Tier 0 macros make.
static Result<XmmReg, Error> read_ps(ExecContext& ctx, const Operand& op,
                                     bool aligned_form = true) noexcept {
    if (op.type == OperandType::Reg128) {
        return Result<XmmReg, Error>::ok(ctx.cpu.sse.xmm[op.reg & 15]);
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<XmmReg, Error>::err(addr.error());
    if (aligned_form && op.type == OperandType::Mem128 &&
        (addr.value() & 0xF) != 0) {
        ctx.exception = CpuException::GeneralProtection;
        return Result<XmmReg, Error>::err(Error::InvalidArgument);
    }
    XmmReg tmp{};
    auto r = ctx.mem.read_bytes(addr.value(), reinterpret_cast<Byte*>(&tmp), 16);
    if (!r) return Result<XmmReg, Error>::err(r.error());
    return Result<XmmReg, Error>::ok(tmp);
}

// ============================================================================
// Main SSE2 integer arithmetic dispatcher
// ============================================================================

ExecResult exec_sse_int(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    const auto& dst_op = instr.operands[0];
    const auto& src_op = instr.operands[1];
    // VEX encodings do not carry the legacy 16-byte alignment requirement.
    const bool aligned_form = !instr.prefixes.vex && !ctx.vex_origin;

    switch (instr.opcode) {

    // ========================================================================
    // PADDB / PADDW / PADDD / PADDQ — wrapping add
    // ========================================================================
    case Opcode::PADDB_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 16; ++i)
            dst.u8_[i] = static_cast<u8>(dst.u8_[i] + src.value().u8_[i]);
        return ExecResult::Continue;
    }
    case Opcode::PADDW_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 8; ++i)
            dst.u16_[i] = static_cast<u16>(dst.u16_[i] + src.value().u16_[i]);
        return ExecResult::Continue;
    }
    case Opcode::PADDD_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 4; ++i)
            dst.u32_[i] = dst.u32_[i] + src.value().u32_[i];
        return ExecResult::Continue;
    }
    case Opcode::PADDQ_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 2; ++i)
            dst.u64_[i] = dst.u64_[i] + src.value().u64_[i];
        return ExecResult::Continue;
    }

    // ========================================================================
    // PSUBB / PSUBW / PSUBD / PSUBQ — wrapping subtract
    // ========================================================================
    case Opcode::PSUBB_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 16; ++i)
            dst.u8_[i] = static_cast<u8>(dst.u8_[i] - src.value().u8_[i]);
        return ExecResult::Continue;
    }
    case Opcode::PSUBW_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 8; ++i)
            dst.u16_[i] = static_cast<u16>(dst.u16_[i] - src.value().u16_[i]);
        return ExecResult::Continue;
    }
    case Opcode::PSUBD_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 4; ++i)
            dst.u32_[i] = dst.u32_[i] - src.value().u32_[i];
        return ExecResult::Continue;
    }
    case Opcode::PSUBQ_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 2; ++i)
            dst.u64_[i] = dst.u64_[i] - src.value().u64_[i];
        return ExecResult::Continue;
    }

    // ========================================================================
    // PADDUSB / PADDUSW — unsigned add with saturation
    // ========================================================================
    case Opcode::PADDUSB_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 16; ++i) {
            u16 sum = static_cast<u16>(dst.u8_[i]) + static_cast<u16>(src.value().u8_[i]);
            dst.u8_[i] = (sum > 255) ? 255 : static_cast<u8>(sum);
        }
        return ExecResult::Continue;
    }
    case Opcode::PADDUSW_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 8; ++i) {
            u32 sum = static_cast<u32>(dst.u16_[i]) + static_cast<u32>(src.value().u16_[i]);
            dst.u16_[i] = (sum > 65535) ? 65535 : static_cast<u16>(sum);
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PSUBUSB / PSUBUSW — unsigned subtract with saturation
    // ========================================================================
    case Opcode::PSUBUSB_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 16; ++i) {
            i16 diff = static_cast<i16>(dst.u8_[i]) - static_cast<i16>(src.value().u8_[i]);
            dst.u8_[i] = (diff < 0) ? 0 : static_cast<u8>(diff);
        }
        return ExecResult::Continue;
    }
    case Opcode::PSUBUSW_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 8; ++i) {
            i32 diff = static_cast<i32>(dst.u16_[i]) - static_cast<i32>(src.value().u16_[i]);
            dst.u16_[i] = (diff < 0) ? 0 : static_cast<u16>(diff);
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PADDSB / PADDSW — signed add with saturation
    // ========================================================================
    case Opcode::PADDSB_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 16; ++i) {
            i32 sum = static_cast<i32>(dst.i8_[i]) + static_cast<i32>(src.value().i8_[i]);
            if (sum > 127) sum = 127; else if (sum < -128) sum = -128;
            dst.i8_[i] = static_cast<i8>(sum);
        }
        return ExecResult::Continue;
    }
    case Opcode::PADDSW_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 8; ++i) {
            i32 sum = static_cast<i32>(dst.i16_[i]) + static_cast<i32>(src.value().i16_[i]);
            if (sum > 32767) sum = 32767; else if (sum < -32768) sum = -32768;
            dst.i16_[i] = static_cast<i16>(sum);
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PSUBSB / PSUBSW — signed subtract with saturation
    // ========================================================================
    case Opcode::PSUBSB_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 16; ++i) {
            i32 diff = static_cast<i32>(dst.i8_[i]) - static_cast<i32>(src.value().i8_[i]);
            if (diff > 127) diff = 127; else if (diff < -128) diff = -128;
            dst.i8_[i] = static_cast<i8>(diff);
        }
        return ExecResult::Continue;
    }
    case Opcode::PSUBSW_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 8; ++i) {
            i32 diff = static_cast<i32>(dst.i16_[i]) - static_cast<i32>(src.value().i16_[i]);
            if (diff > 32767) diff = 32767; else if (diff < -32768) diff = -32768;
            dst.i16_[i] = static_cast<i16>(diff);
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PACKSSWB — pack 16-bit signed -> 8-bit signed with saturation
    // dst[0..7]  = sat_i8(dst.i16[0..7]); dst[8..15] = sat_i8(src.i16[0..7])
    // ========================================================================
    case Opcode::PACKSSWB_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        i8 out_bytes[16];
        auto sat_i8 = [](i32 v) -> i8 {
            if (v > 127) return 127; if (v < -128) return -128; return static_cast<i8>(v);
        };
        for (int i = 0; i < 8; ++i) out_bytes[i]     = sat_i8(dst.i16_[i]);
        for (int i = 0; i < 8; ++i) out_bytes[i + 8] = sat_i8(src.value().i16_[i]);
        for (int i = 0; i < 16; ++i) dst.i8_[i] = out_bytes[i];
        return ExecResult::Continue;
    }

    // ========================================================================
    // PACKSSDW — pack 32-bit signed -> 16-bit signed with saturation
    // ========================================================================
    case Opcode::PACKSSDW_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        i16 out_words[8];
        auto sat_i16 = [](i32 v) -> i16 {
            if (v > 32767) return 32767; if (v < -32768) return -32768; return static_cast<i16>(v);
        };
        for (int i = 0; i < 4; ++i) out_words[i]     = sat_i16(dst.i32_[i]);
        for (int i = 0; i < 4; ++i) out_words[i + 4] = sat_i16(src.value().i32_[i]);
        for (int i = 0; i < 8; ++i) dst.i16_[i] = out_words[i];
        return ExecResult::Continue;
    }

    // ========================================================================
    // PACKUSWB — pack 16-bit signed -> 8-bit unsigned with saturation
    // ========================================================================
    case Opcode::PACKUSWB_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        u8 out_bytes[16];
        auto sat_u8 = [](i32 v) -> u8 {
            if (v > 255) return 255; if (v < 0) return 0; return static_cast<u8>(v);
        };
        for (int i = 0; i < 8; ++i) out_bytes[i]     = sat_u8(dst.i16_[i]);
        for (int i = 0; i < 8; ++i) out_bytes[i + 8] = sat_u8(src.value().i16_[i]);
        for (int i = 0; i < 16; ++i) dst.u8_[i] = out_bytes[i];
        return ExecResult::Continue;
    }

    // ========================================================================
    // PMULLW — signed multiply, keep low 16 bits
    // ========================================================================
    case Opcode::PMULLW_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 8; ++i) {
            i32 prod = static_cast<i32>(dst.i16_[i]) * static_cast<i32>(src.value().i16_[i]);
            dst.i16_[i] = static_cast<i16>(prod & 0xFFFF);
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PMULHW — signed multiply, keep high 16 bits
    // ========================================================================
    case Opcode::PMULHW_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 8; ++i) {
            i32 prod = static_cast<i32>(dst.i16_[i]) * static_cast<i32>(src.value().i16_[i]);
            dst.i16_[i] = static_cast<i16>((prod >> 16) & 0xFFFF);
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PMULHUW — unsigned multiply, keep high 16 bits
    // ========================================================================
    case Opcode::PMULHUW_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 8; ++i) {
            u32 prod = static_cast<u32>(dst.u16_[i]) * static_cast<u32>(src.value().u16_[i]);
            dst.u16_[i] = static_cast<u16>((prod >> 16) & 0xFFFF);
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PMADDWD — multiply-accumulate: i32[i] = i16[2i]*i16[2i] + i16[2i+1]*i16[2i+1]
    // ========================================================================
    case Opcode::PMADDWD_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        auto sv = src.value();
        for (int i = 0; i < 4; ++i) {
            i32 lo = static_cast<i32>(dst.i16_[2 * i]) * static_cast<i32>(sv.i16_[2 * i]);
            i32 hi = static_cast<i32>(dst.i16_[2 * i + 1]) * static_cast<i32>(sv.i16_[2 * i + 1]);
            dst.i32_[i] = lo + hi;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PMULUDQ — unsigned dword multiply → qword
    // u64[0] = u32[0] * src.u32[0], u64[1] = u32[2] * src.u32[2]
    // ========================================================================
    case Opcode::PMULUDQ_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        auto sv = src.value();
        u64 lo = static_cast<u64>(dst.u32_[0]) * static_cast<u64>(sv.u32_[0]);
        u64 hi = static_cast<u64>(dst.u32_[2]) * static_cast<u64>(sv.u32_[2]);
        dst.u64_[0] = lo;
        dst.u64_[1] = hi;
        return ExecResult::Continue;
    }

    // ========================================================================
    // PMINUB / PMAXUB — unsigned byte min/max
    // ========================================================================
    case Opcode::PMINUB_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 16; ++i)
            dst.u8_[i] = (dst.u8_[i] < src.value().u8_[i]) ? dst.u8_[i] : src.value().u8_[i];
        return ExecResult::Continue;
    }
    case Opcode::PMAXUB_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 16; ++i)
            dst.u8_[i] = (dst.u8_[i] > src.value().u8_[i]) ? dst.u8_[i] : src.value().u8_[i];
        return ExecResult::Continue;
    }

    // ========================================================================
    // PMINSW / PMAXSW — signed word min/max
    // ========================================================================
    case Opcode::PMINSW_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 8; ++i)
            dst.i16_[i] = (dst.i16_[i] < src.value().i16_[i]) ? dst.i16_[i] : src.value().i16_[i];
        return ExecResult::Continue;
    }
    case Opcode::PMAXSW_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 8; ++i)
            dst.i16_[i] = (dst.i16_[i] > src.value().i16_[i]) ? dst.i16_[i] : src.value().i16_[i];
        return ExecResult::Continue;
    }

    // ========================================================================
    // PAVGB / PAVGW — average with rounding: (a + b + 1) >> 1
    // ========================================================================
    case Opcode::PAVGB_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 16; ++i) {
            u16 sum = static_cast<u16>(dst.u8_[i]) + static_cast<u16>(src.value().u8_[i]) + 1;
            dst.u8_[i] = static_cast<u8>(sum >> 1);
        }
        return ExecResult::Continue;
    }
    case Opcode::PAVGW_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 8; ++i) {
            u32 sum = static_cast<u32>(dst.u16_[i]) + static_cast<u32>(src.value().u16_[i]) + 1;
            dst.u16_[i] = static_cast<u16>(sum >> 1);
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PSADBW — sum of absolute differences
    // ========================================================================
    case Opcode::PSADBW_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        auto sv = src.value();

        // Low 8 bytes → u64_[0]
        u64 sad0 = 0;
        for (int i = 0; i < 8; ++i) {
            i16 diff = static_cast<i16>(dst.u8_[i]) - static_cast<i16>(sv.u8_[i]);
            sad0 += static_cast<u64>(diff < 0 ? -diff : diff);
        }
        // High 8 bytes → u64_[1]
        u64 sad1 = 0;
        for (int i = 8; i < 16; ++i) {
            i16 diff = static_cast<i16>(dst.u8_[i]) - static_cast<i16>(sv.u8_[i]);
            sad1 += static_cast<u64>(diff < 0 ? -diff : diff);
        }

        // Zero entire register, then set results
        dst.u64_[0] = sad0;
        dst.u64_[1] = sad1;
        return ExecResult::Continue;
    }

    // ========================================================================
    // PAND / POR / PXOR — bitwise on 128-bit
    // ========================================================================
    case Opcode::PAND_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        dst.u64_[0] &= src.value().u64_[0];
        dst.u64_[1] &= src.value().u64_[1];
        return ExecResult::Continue;
    }
    case Opcode::POR_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        dst.u64_[0] |= src.value().u64_[0];
        dst.u64_[1] |= src.value().u64_[1];
        return ExecResult::Continue;
    }
    case Opcode::PXOR_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        dst.u64_[0] ^= src.value().u64_[0];
        dst.u64_[1] ^= src.value().u64_[1];
        return ExecResult::Continue;
    }

    // ========================================================================
    // PANDN — ~dst & src
    // ========================================================================
    case Opcode::PANDN_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        dst.u64_[0] = ~dst.u64_[0] & src.value().u64_[0];
        dst.u64_[1] = ~dst.u64_[1] & src.value().u64_[1];
        return ExecResult::Continue;
    }

    // ========================================================================
    // MOVD xmm, r/m32 — zero-extend 32-bit into XMM
    // ========================================================================
    case Opcode::MOVD_XMM_RM32: {
        auto& dst = get_xmm(ctx, dst_op.reg);
        u32 src;
        if (src_op.type == OperandType::Reg32) {
            src = static_cast<u32>(read_register(ctx, src_op.type, src_op.reg));
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto r = read_memory(ctx, addr.value(), 4);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            src = static_cast<u32>(r.value());
        }
        dst.clear();
        dst.u32_[0] = src;
        return ExecResult::Continue;
    }

    // ========================================================================
    // MOVD r/m32, xmm — store low 32 bits of XMM
    // ========================================================================
    case Opcode::MOVD_RM32_XMM: {
        u32 val = get_xmm(ctx, src_op.reg).u32_[0];
        if (dst_op.type == OperandType::Reg32) {
            write_register(ctx, dst_op.type, dst_op.reg, val);
        } else {
            auto addr = calc_effective_address(ctx, dst_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto r = write_memory(ctx, addr.value(), val, 4);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // MOVQ xmm, r/m64 — load 64-bit GPR/mem into XMM (REX.W variant of MOVD)
    // ========================================================================
    case Opcode::MOVQ_XMM_RM64: {
        auto& dst = get_xmm(ctx, dst_op.reg);
        u64 src;
        if (src_op.type == OperandType::Reg64) {
            src = read_register(ctx, src_op.type, src_op.reg);
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto r = read_memory(ctx, addr.value(), 8);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            src = r.value();
        }
        dst.clear();
        dst.u64_[0] = src;
        return ExecResult::Continue;
    }

    // ========================================================================
    // MOVQ r/m64, xmm — store low 64 bits of XMM to GPR/mem (REX.W variant of MOVD)
    // ========================================================================
    case Opcode::MOVQ_RM64_XMM: {
        u64 val = get_xmm(ctx, src_op.reg).u64_[0];
        if (dst_op.type == OperandType::Reg64) {
            write_register(ctx, dst_op.type, dst_op.reg, val);
        } else {
            auto addr = calc_effective_address(ctx, dst_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto r = write_memory(ctx, addr.value(), val, 8);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PMOVMSKB r32, xmm — extract MSB of each byte into GPR
    // ========================================================================
    case Opcode::PMOVMSKB_R32_XMM: {
        const auto& src = get_xmm(ctx, src_op.reg);
        u32 mask = 0;
        for (u8 i = 0; i < 16; ++i) {
            if (src.u8_[i] & 0x80u) mask |= (1u << i);
        }
        write_register(ctx, OperandType::Reg32, dst_op.reg, mask);
        return ExecResult::Continue;
    }

    // ========================================================================
    // MOVQ xmm, xmm/m64 — zero-extend 64-bit into XMM
    // ========================================================================
    case Opcode::MOVQ_XMM_XMMM64: {
        auto& dst = get_xmm(ctx, dst_op.reg);
        u64 src;
        if (src_op.type == OperandType::Reg128) {
            src = get_xmm(ctx, src_op.reg).u64_[0];
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto r = read_memory(ctx, addr.value(), 8);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            src = r.value();
        }
        dst.clear();
        dst.u64_[0] = src;
        return ExecResult::Continue;
    }

    // ========================================================================
    // MOVQ xmm/m64, xmm — store low 64 bits of XMM
    // ========================================================================
    case Opcode::MOVQ_XMMM64_XMM: {
        u64 val = get_xmm(ctx, src_op.reg).u64_[0];
        if (dst_op.type == OperandType::Reg128) {
            auto& dst = get_xmm(ctx, dst_op.reg);
            dst.clear();
            dst.u64_[0] = val;
        } else {
            auto addr = calc_effective_address(ctx, dst_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto r = write_memory(ctx, addr.value(), val, 8);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // Packed shift left logical — word / dword / qword / dqword by imm8
    // ========================================================================
    case Opcode::PSLLW_XMM_IMM8: {
        auto& xmm = get_xmm(ctx, dst_op.reg);
        u8 shift = static_cast<u8>(src_op.imm);
        if (shift > 15) { for (int i = 0; i < 8; ++i) xmm.u16_[i] = 0; }
        else { for (int i = 0; i < 8; ++i) xmm.u16_[i] <<= shift; }
        return ExecResult::Continue;
    }
    case Opcode::PSLLD_XMM_IMM8: {
        auto& xmm = get_xmm(ctx, dst_op.reg);
        u8 shift = static_cast<u8>(src_op.imm);
        if (shift > 31) { for (int i = 0; i < 4; ++i) xmm.u32_[i] = 0; }
        else { for (int i = 0; i < 4; ++i) xmm.u32_[i] <<= shift; }
        return ExecResult::Continue;
    }
    case Opcode::PSLLQ_XMM_IMM8: {
        auto& xmm = get_xmm(ctx, dst_op.reg);
        u8 shift = static_cast<u8>(src_op.imm);
        if (shift > 63) { xmm.u64_[0] = 0; xmm.u64_[1] = 0; }
        else { xmm.u64_[0] <<= shift; xmm.u64_[1] <<= shift; }
        return ExecResult::Continue;
    }
    case Opcode::PSLLDQ_XMM_IMM8: {
        auto& xmm = get_xmm(ctx, dst_op.reg);
        u8 shift = static_cast<u8>(src_op.imm);
        if (shift > 15) { xmm.clear(); }
        else {
            // Byte shift left: move bytes up by shift positions
            for (int i = 15; i >= shift; --i)
                xmm.u8_[i] = xmm.u8_[i - shift];
            for (int i = shift - 1; i >= 0; --i)
                xmm.u8_[i] = 0;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // Packed shift right logical — word / dword / qword / dqword by imm8
    // ========================================================================
    case Opcode::PSRLW_XMM_IMM8: {
        auto& xmm = get_xmm(ctx, dst_op.reg);
        u8 shift = static_cast<u8>(src_op.imm);
        if (shift > 15) { for (int i = 0; i < 8; ++i) xmm.u16_[i] = 0; }
        else { for (int i = 0; i < 8; ++i) xmm.u16_[i] >>= shift; }
        return ExecResult::Continue;
    }
    case Opcode::PSRLD_XMM_IMM8: {
        auto& xmm = get_xmm(ctx, dst_op.reg);
        u8 shift = static_cast<u8>(src_op.imm);
        if (shift > 31) { for (int i = 0; i < 4; ++i) xmm.u32_[i] = 0; }
        else { for (int i = 0; i < 4; ++i) xmm.u32_[i] >>= shift; }
        return ExecResult::Continue;
    }
    case Opcode::PSRLQ_XMM_IMM8: {
        auto& xmm = get_xmm(ctx, dst_op.reg);
        u8 shift = static_cast<u8>(src_op.imm);
        if (shift > 63) { xmm.u64_[0] = 0; xmm.u64_[1] = 0; }
        else { xmm.u64_[0] >>= shift; xmm.u64_[1] >>= shift; }
        return ExecResult::Continue;
    }
    case Opcode::PSRLDQ_XMM_IMM8: {
        auto& xmm = get_xmm(ctx, dst_op.reg);
        u8 shift = static_cast<u8>(src_op.imm);
        if (shift > 15) { xmm.clear(); }
        else {
            // Byte shift right: move bytes down by shift positions
            for (int i = 0; i < 16 - shift; ++i)
                xmm.u8_[i] = xmm.u8_[i + shift];
            for (int i = 16 - shift; i < 16; ++i)
                xmm.u8_[i] = 0;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // Packed shift right arithmetic — word / dword by imm8
    // ========================================================================
    case Opcode::PSRAW_XMM_IMM8: {
        auto& xmm = get_xmm(ctx, dst_op.reg);
        u8 shift = static_cast<u8>(src_op.imm);
        if (shift > 15) shift = 15;
        for (int i = 0; i < 8; ++i)
            xmm.i16_[i] >>= shift;
        return ExecResult::Continue;
    }
    case Opcode::PSRAD_XMM_IMM8: {
        auto& xmm = get_xmm(ctx, dst_op.reg);
        u8 shift = static_cast<u8>(src_op.imm);
        if (shift > 31) shift = 31;
        for (int i = 0; i < 4; ++i)
            xmm.i32_[i] >>= shift;
        return ExecResult::Continue;
    }

    // ========================================================================
    // Packed shift by register (shift count from low 64 bits of src xmm)
    // ========================================================================
    case Opcode::PSRLW_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        u64 count = src.value().u64_[0];
        if (count > 15) { for (int i = 0; i < 8; ++i) dst.u16_[i] = 0; }
        else { for (int i = 0; i < 8; ++i) dst.u16_[i] >>= count; }
        return ExecResult::Continue;
    }
    case Opcode::PSRLD_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        u64 count = src.value().u64_[0];
        if (count > 31) { for (int i = 0; i < 4; ++i) dst.u32_[i] = 0; }
        else { for (int i = 0; i < 4; ++i) dst.u32_[i] >>= count; }
        return ExecResult::Continue;
    }
    case Opcode::PSRLQ_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        u64 count = src.value().u64_[0];
        if (count > 63) { dst.u64_[0] = 0; dst.u64_[1] = 0; }
        else { dst.u64_[0] >>= count; dst.u64_[1] >>= count; }
        return ExecResult::Continue;
    }
    case Opcode::PSRAW_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        u64 count = src.value().u64_[0];
        if (count > 15) count = 15;
        for (int i = 0; i < 8; ++i) dst.i16_[i] >>= count;
        return ExecResult::Continue;
    }
    case Opcode::PSRAD_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        u64 count = src.value().u64_[0];
        if (count > 31) count = 31;
        for (int i = 0; i < 4; ++i) dst.i32_[i] >>= count;
        return ExecResult::Continue;
    }
    case Opcode::PSLLW_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        u64 count = src.value().u64_[0];
        if (count > 15) { for (int i = 0; i < 8; ++i) dst.u16_[i] = 0; }
        else { for (int i = 0; i < 8; ++i) dst.u16_[i] <<= count; }
        return ExecResult::Continue;
    }
    case Opcode::PSLLD_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        u64 count = src.value().u64_[0];
        if (count > 31) { for (int i = 0; i < 4; ++i) dst.u32_[i] = 0; }
        else { for (int i = 0; i < 4; ++i) dst.u32_[i] <<= count; }
        return ExecResult::Continue;
    }
    case Opcode::PSLLQ_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        u64 count = src.value().u64_[0];
        if (count > 63) { dst.u64_[0] = 0; dst.u64_[1] = 0; }
        else { dst.u64_[0] <<= count; dst.u64_[1] <<= count; }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PINSRW — Insert 16-bit word into XMM at position imm8 & 7
    // ========================================================================
    case Opcode::PINSRW_XMM_R32M16_IMM8: {
        auto& dst = get_xmm(ctx, dst_op.reg);
        u16 word;
        if (src_op.type == OperandType::Reg32) {
            word = static_cast<u16>(read_register(ctx, src_op.type, src_op.reg));
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto r = read_memory(ctx, addr.value(), 2);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            word = static_cast<u16>(r.value());
        }
        u8 sel = static_cast<u8>(instr.operands[2].imm) & 7;
        dst.u16_[sel] = word;
        return ExecResult::Continue;
    }

    // ========================================================================
    // PINSRD — Insert dword from r/m32 into XMM at position imm8[1:0]
    // ========================================================================
    case Opcode::PINSRD_XMM_RM32_IMM8: {
        auto& dst = get_xmm(ctx, dst_op.reg);
        u32 dword;
        if (src_op.type == OperandType::Reg32) {
            dword = static_cast<u32>(read_register(ctx, src_op.type, src_op.reg));
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto r = read_memory(ctx, addr.value(), 4);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            dword = static_cast<u32>(r.value());
        }
        u8 sel = static_cast<u8>(instr.operands[2].imm) & 3;
        dst.u32_[sel] = dword;
        return ExecResult::Continue;
    }

    // ========================================================================
    // PEXTRW — Extract 16-bit word from XMM at position imm8 & 7.
    // Register dest: zero-extend to r32. Memory dest: store 2 bytes (SSE4.1 m16).
    // ========================================================================
    case Opcode::PEXTRW_R32_XMM_IMM8: {
        const auto& src = get_xmm(ctx, src_op.reg);
        u8 sel = static_cast<u8>(instr.operands[2].imm) & 7;
        u32 val = src.u16_[sel];
        if (dst_op.type == OperandType::Reg32) {
            write_register(ctx, OperandType::Reg32, dst_op.reg, val);
        } else {
            auto addr = calc_effective_address(ctx, dst_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            if (!write_memory(ctx, addr.value(), val, 2)) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        }
        return ExecResult::Continue;
    }

    // MASKMOVDQU xmm1, xmm2 — store selected bytes of xmm1 to DS:EDI/RDI
    // using the MSB of each xmm2 byte. EDI is not incremented.
    case Opcode::MASKMOVDQU_XMM_XMM: {
        const auto& data = get_xmm(ctx, dst_op.reg);
        const auto& mask = get_xmm(ctx, src_op.reg);
        auto addr = maskmov_ds_edi_address(ctx, instr);
        if (!addr) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        for (int lane = 0; lane < 16; ++lane) {
            if ((mask.u8_[lane] & 0x80u) == 0)
                continue;
            if (!write_memory(ctx, addr.value() + static_cast<u64>(lane),
                              data.u8_[lane], 1)) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PEXTRD — Extract dword from XMM at position imm8[1:0] → r/m32
    // ========================================================================
    case Opcode::PEXTRD_RM32_XMM_IMM8: {
        const auto& src = get_xmm(ctx, src_op.reg);
        u8 sel = static_cast<u8>(instr.operands[2].imm) & 3;
        u32 val = src.u32_[sel];
        if (dst_op.type == OperandType::Reg32) {
            write_register(ctx, OperandType::Reg32, dst_op.reg, val);
        } else {
            auto addr = calc_effective_address(ctx, dst_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            if (!write_memory(ctx, addr.value(), val, 4)) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PEXTRQ — Extract qword from XMM at position imm8[0] → r/m64
    // ========================================================================
    case Opcode::PEXTRQ_RM64_XMM_IMM8: {
        const auto& src = get_xmm(ctx, src_op.reg);
        u8 sel = static_cast<u8>(instr.operands[2].imm) & 1;
        u64 val = src.u64_[sel];
        if (dst_op.type == OperandType::Reg64) {
            write_register(ctx, OperandType::Reg64, dst_op.reg, val);
        } else {
            auto addr = calc_effective_address(ctx, dst_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            if (!write_memory(ctx, addr.value(), val, 8)) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PINSRQ — Insert qword from r/m64 into XMM at position imm8[0]
    // ========================================================================
    case Opcode::PINSRQ_XMM_RM64_IMM8: {
        auto& dst = get_xmm(ctx, dst_op.reg);
        u64 qword;
        if (src_op.type == OperandType::Reg64) {
            qword = read_register(ctx, src_op.type, src_op.reg);
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto r = read_memory(ctx, addr.value(), 8);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            qword = r.value();
        }
        u8 sel = static_cast<u8>(instr.operands[2].imm) & 1;
        dst.u64_[sel] = qword;
        return ExecResult::Continue;
    }

    default:
        ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }
}

} // namespace vitreo::cpu::ops
