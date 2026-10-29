// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// F16C VCVTPH2PS / VCVTPS2PH and ADX ADCX / ADOX (Intel SDM Vol. 2)

#include "exec_ops.hpp"
#include "helpers.hpp"
#include "../../sse_float_semantics.hpp"

#include <cstring>

namespace vitreo::cpu::ops {

namespace {

u32 f16_to_f32_bits(u16 h) noexcept {
    const u32 sign = static_cast<u32>(h & 0x8000u) << 16;
    const u32 exp = (h >> 10) & 0x1F;
    const u32 man = h & 0x3FF;
    if (exp == 0) {
        if (man == 0) return sign;
        // Normalize subnormal
        u32 m = man;
        u32 e = 127 - 15 + 1;
        while ((m & 0x400) == 0) {
            m <<= 1;
            --e;
        }
        m &= 0x3FF;
        return sign | (e << 23) | (m << 13);
    }
    if (exp == 0x1F) {
        // Infinity, or a NaN whose payload widens into the single's upper
        // mantissa bits. An SNaN is returned QUIETENED (Intel SDM Vol. 2
        // VCVTPH2PS, non-numerical behaviour: SNaN -> QNaN with the quiet bit
        // set, #I), exactly as every x86 widening conversion does; leaving
        // the quiet bit clear produced an SNaN single.
        return sign | 0x7F800000u | (man << 13) | (man != 0 ? 0x00400000u : 0u);
    }
    return sign | ((exp + (127 - 15)) << 23) | (man << 13);
}

// Intel SDM Vol. 2 VCVTPS2PH Table 5-13: imm[2]=0 → imm[1:0], else MXCSR.RC.
// 00 nearest-even, 01 toward −∞, 10 toward +∞, 11 toward zero.
bool f16_round_up(u8 rc, bool neg, u32 kept, bool round_bit, bool sticky) noexcept {
    if (!round_bit && !sticky) return false;
    switch (rc) {
    case 0: return round_bit && (sticky || (kept & 1u) != 0);
    case 1: return neg;
    case 2: return !neg;
    default: return false;
    }
}

u16 f16_overflow(u32 sign, u8 rc) noexcept {
    // IEEE overflow: RNE/RU(+)/RD(−) → Inf; otherwise max finite.
    const bool to_inf = (rc == 0) || (rc == 2 && sign == 0) || (rc == 1 && sign != 0);
    return static_cast<u16>(sign | (to_inf ? 0x7C00u : 0x7BFFu));
}

u16 f32_to_f16_bits(u32 f, u8 imm_rc, u32& mxcsr) noexcept {
    const u8 rc = (imm_rc & 4u)
                      ? static_cast<u8>((mxcsr >> 13) & 3u)
                      : static_cast<u8>(imm_rc & 3u);
    const u32 sign = (f >> 16) & 0x8000u;
    u32 exp = (f >> 23) & 0xFF;
    u32 man = f & 0x7FFFFF;
    u16 h;
    if (exp == 0xFF) {
        h = static_cast<u16>(sign | 0x7C00 | (man ? 0x200 : 0));
    } else if (exp == 0 && (man == 0 || (mxcsr & (1u << 6)) != 0)) {
        // ±0, or DAZ: denormal source is a signed zero before convert.
        h = static_cast<u16>(sign);
    } else {
        u32 sig;
        i32 unbiased;
        if (exp == 0) {
            sig = man;
            unbiased = -126;
        } else {
            sig = (1u << 23) | man;
            unbiased = static_cast<i32>(exp) - 127;
        }
        i32 f16_exp = unbiased + 15;
        const bool neg = sign != 0;
        if (f16_exp >= 0x1F) {
            h = f16_overflow(sign, rc);
        } else if (f16_exp > 0) {
            u32 kept = (sig >> 13) & 0x3FFu;
            const u32 rest = sig & 0x1FFFu;
            const bool round_bit = (rest & 0x1000u) != 0;
            const bool sticky = (rest & 0x0FFFu) != 0;
            if (f16_round_up(rc, neg, kept, round_bit, sticky)) {
                ++kept;
                if (kept == 0x400u) {
                    kept = 0;
                    ++f16_exp;
                }
            }
            h = (f16_exp >= 0x1F)
                    ? f16_overflow(sign, rc)
                    : static_cast<u16>(sign | (static_cast<u32>(f16_exp) << 10) |
                                       kept);
        } else {
            // SDM: tiny results become denormals; MXCSR.FTZ is ignored.
            const i32 shift = -(unbiased + 1);
            u32 kept = 0;
            bool round_bit = false;
            bool sticky = false;
            if (shift >= 32) {
                sticky = sig != 0;
            } else if (shift > 0) {
                kept = sig >> shift;
                round_bit = ((sig >> (shift - 1)) & 1u) != 0;
                sticky = (sig & ((1u << (shift - 1)) - 1u)) != 0;
            } else {
                kept = sig << -shift;
            }
            if (f16_round_up(rc, neg, kept, round_bit, sticky)) {
                ++kept;
            }
            h = (kept >= 0x400u)
                    ? static_cast<u16>(sign | (1u << 10))
                    : static_cast<u16>(sign | kept);
        }
    }
    // Intel SDM Vol. 2 VCVTPS2PH: Precision when the half is inexact.
    f32 src = 0.0f;
    std::memcpy(&src, &f, sizeof(src));
    const u32 rec_bits = f16_to_f32_bits(h);
    f32 rec = 0.0f;
    std::memcpy(&rec, &rec_bits, sizeof(rec));
    sse_cvt_narrow_pe(src, rec, mxcsr);
    return h;
}

ExecResult adcx_adox(ExecContext& ctx, const DecodedInstruction& instr, bool is_of) noexcept {
    auto src = read_operand(ctx, instr, 1);
    auto dst = read_operand(ctx, instr, 0);
    if (!src || !dst) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    const bool wide = instr.opcode == Opcode::ADCX_R64_RM64
        || instr.opcode == Opcode::ADOX_R64_RM64;
    // Read the carry-in through the lazy machinery, not raw RFLAGS.
    //
    // Measured on real x86 silicon (Windows; Rosetta #UDs both of these, so
    // it cannot adjudicate them): ADCX writes ONLY CF and ADOX writes ONLY OF.
    // Everything else -- SF, ZF, AF, PF, and the other one of CF/OF -- is
    // preserved, including ZF when the result is zero. That is exactly the
    // cf_overlay / of_overlay contract, so these must not retire the pending
    // FlagOp that owns the flags they preserve.
    //
    // Reading ctx.cpu.flags.cf() directly saw stale RFLAGS whenever a lazy
    // FlagOp was pending, and set_cf() wrote a value that the next reader
    // would recompute away from that same pending op. `ADD; ADCX` is enough
    // to hit it -- which is the bignum shape ADX exists for.
    u64 cin = 0;
    if (is_of) {
        // No lazy_of() accessor exists, and OF needs the full computation for
        // most FlagOps, so settle first. ADOX is rare enough for this to be
        // the right trade.
        ctx.cpu.materialize_lazy_flags();
        cin = ctx.cpu.flags.of() ? 1ull : 0ull;
    } else {
        cin = ctx.cpu.lazy_cf() ? 1ull : 0ull;
    }
    if (wide) {
        const __uint128_t sum =
            static_cast<__uint128_t>(dst.value()) + src.value() + cin;
        auto wr = write_operand(ctx, instr, 0, static_cast<u64>(sum));
        if (!wr) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        const bool cout = (sum >> 64) != 0;
        if (is_of) ctx.cpu.write_of_only(cout);
        else ctx.cpu.write_cf_only(cout);
    } else {
        const u64 sum = static_cast<u64>(static_cast<u32>(dst.value()))
            + static_cast<u32>(src.value()) + static_cast<u32>(cin);
        auto wr = write_operand(ctx, instr, 0, static_cast<u32>(sum));
        if (!wr) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        const bool cout = (sum >> 32) != 0;
        if (is_of) ctx.cpu.write_of_only(cout);
        else ctx.cpu.write_cf_only(cout);
    }
    return ExecResult::Continue;
}

}  // namespace

ExecResult exec_f16c_adx(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    switch (instr.opcode) {
        case Opcode::ADCX_R32_RM32:
        case Opcode::ADCX_R64_RM64:
            return adcx_adox(ctx, instr, false);
        case Opcode::ADOX_R32_RM32:
        case Opcode::ADOX_R64_RM64:
            return adcx_adox(ctx, instr, true);
        case Opcode::VCVTPH2PS_XMM_XMMM64:
        case Opcode::VCVTPH2PS_YMM_XMMM128: {
            u16 src[8]{};
            const u32 n = (instr.opcode == Opcode::VCVTPH2PS_YMM_XMMM128) ? 8 : 4;
            const auto& op = instr.operands[1];
            if (op.type == OperandType::Reg128) {
                for (u32 i = 0; i < n; ++i) {
                    src[i] = ctx.cpu.sse.xmm[op.reg & 15].u16_[i];
                }
            } else {
                auto addr = calc_effective_address(ctx, op.mem);
                if (!addr) {
                    ctx.exception = CpuException::GeneralProtection;
                    return ExecResult::Exception;
                }
                auto rd = ctx.mem.read_bytes(addr.value(), reinterpret_cast<Byte*>(src), n * 2);
                if (!rd) {
                    ctx.exception = CpuException::GeneralProtection;
                    return ExecResult::Exception;
                }
            }
            u8 dst = instr.operands[0].reg & 15;
            for (u32 i = 0; i < 4 && i < n; ++i) {
                ctx.cpu.sse.xmm[dst].u32_[i] = f16_to_f32_bits(src[i]);
            }
            if (n == 8) {
                for (u32 i = 0; i < 4; ++i) {
                    ctx.cpu.avx.ymm_high[dst].u32_[i] = f16_to_f32_bits(src[4 + i]);
                }
            } else {
                ctx.cpu.avx.ymm_high[dst].clear();
            }
            return ExecResult::Continue;
        }
        case Opcode::VCVTPS2PH_XMMM64_XMM_IMM8:
        case Opcode::VCVTPS2PH_XMMM128_YMM_IMM8: {
            const u8 imm = static_cast<u8>(instr.operands[2].imm);
            const u32 n = (instr.opcode == Opcode::VCVTPS2PH_XMMM128_YMM_IMM8) ? 8 : 4;
            u8 src = instr.operands[1].reg & 15;
            u16 out[8]{};
            for (u32 i = 0; i < 4 && i < n; ++i) {
                out[i] = f32_to_f16_bits(ctx.cpu.sse.xmm[src].u32_[i], imm,
                                         ctx.cpu.sse.mxcsr);
            }
            if (n == 8) {
                for (u32 i = 0; i < 4; ++i) {
                    out[4 + i] = f32_to_f16_bits(
                        ctx.cpu.avx.ymm_high[src].u32_[i], imm,
                        ctx.cpu.sse.mxcsr);
                }
            }
            const auto& dst = instr.operands[0];
            if (is_register_operand(dst.type)) {
                // Intel SDM Vol. 2 VCVTPS2PH: packed HP lands in DEST[n*16-1:0];
                // DEST[127:64] := 0 for the xmm form; DEST[MAXVL-1:128] := 0.
                for (u32 i = 0; i < n; ++i) {
                    ctx.cpu.sse.xmm[dst.reg & 15].u16_[i] = out[i];
                }
                if (n == 4) {
                    ctx.cpu.sse.xmm[dst.reg & 15].u64_[1] = 0;
                }
                ctx.cpu.avx.ymm_high[dst.reg & 15].clear();
            } else {
                auto addr = calc_effective_address(ctx, dst.mem);
                if (!addr) {
                    ctx.exception = CpuException::GeneralProtection;
                    return ExecResult::Exception;
                }
                auto wr = ctx.mem.write_bytes(addr.value(),
                    reinterpret_cast<const Byte*>(out), n * 2);
                if (!wr) {
                    ctx.exception = CpuException::GeneralProtection;
                    return ExecResult::Exception;
                }
            }
            return ExecResult::Continue;
        }
        default:
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
    }
}

}  // namespace vitreo::cpu::ops
