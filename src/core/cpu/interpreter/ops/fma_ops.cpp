// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// FMA3 instruction execution (VFMADD, VFMSUB, VFNMADD, VFNMSUB, VFMADDSUB, VFMSUBADD)

#include "exec_ops.hpp"
#include "helpers.hpp"
#include <cstring>
#include <cmath>
#include <type_traits>

#include "core/math/host_fenv.hpp"

namespace vitreo::cpu::ops {

namespace {

inline XmmReg& get_xmm(ExecContext& ctx, u8 reg) noexcept {
    return ctx.cpu.sse.xmm[reg & 15];
}

Result<XmmReg, Error> read_xmm_or_m128(ExecContext& ctx, const Operand& op) noexcept {
    if (op.type == OperandType::Reg128) {
        return Result<XmmReg, Error>::ok(ctx.cpu.sse.xmm[op.reg & 15]);
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<XmmReg, Error>::err(addr.error());
    XmmReg tmp{};
    auto r = ctx.mem.read_bytes(addr.value(), reinterpret_cast<Byte*>(&tmp), 16);
    if (!r) return Result<XmmReg, Error>::err(r.error());
    return Result<XmmReg, Error>::ok(tmp);
}

Result<float, Error> read_f32_src(ExecContext& ctx, const Operand& op) noexcept {
    if (op.type == OperandType::Reg128) {
        return Result<float, Error>::ok(ctx.cpu.sse.xmm[op.reg & 15].f32_[0]);
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<float, Error>::err(addr.error());
    auto val = read_memory(ctx, addr.value(), 4);
    if (!val) return Result<float, Error>::err(val.error());
    float f;
    u32 u = static_cast<u32>(val.value());
    std::memcpy(&f, &u, 4);
    return Result<float, Error>::ok(f);
}

Result<double, Error> read_f64_src(ExecContext& ctx, const Operand& op) noexcept {
    if (op.type == OperandType::Reg128) {
        return Result<double, Error>::ok(ctx.cpu.sse.xmm[op.reg & 15].f64_[0]);
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<double, Error>::err(addr.error());
    XmmReg tmp{};
    auto r = ctx.mem.read_bytes(addr.value(), reinterpret_cast<Byte*>(&tmp), 8);
    if (!r) return Result<double, Error>::err(r.error());
    return Result<double, Error>::ok(tmp.f64_[0]);
}

// x86 FMA3 element semantics that the host's fused multiply-add does not give:
//
// * NaN inputs: the result is the FIRST NaN among the multiplicands a, b and
//   then the addend c (in the form's arithmetic order: 132 = op1*op3+op2,
//   213 = op2*op1+op3, 231 = op2*op3+op1), quieted, with its sign and payload
//   kept. SNaN versus QNaN does not change the priority, and the negated forms
//   (VFMSUB, VFNMADD, VFNMSUB) never flip a NaN's sign. ARM FMADD instead
//   prefers the addend and prefers SNaNs, and negating an operand in C flips a
//   NaN's sign. Measured on silicon: tools/oracle_probe/fma_nan_priority_probe.
// * An invalid operation with no NaN input (inf * 0) yields the x86 default
//   "QNaN floating-point indefinite", whose sign bit is SET; ARM's default NaN
//   has it clear.
template <typename F>
F x86_fma(F a, F b, F c, bool negate_product, bool negate_addend) noexcept {
    using Bits = std::conditional_t<sizeof(F) == 4, u32, u64>;
    Bits kQuiet{}, kIndefinite{};
    if constexpr (sizeof(F) == 4) {
        kQuiet = 0x00400000u;
        kIndefinite = 0xFFC00000u;
    } else {
        kQuiet = 0x0008000000000000ull;
        kIndefinite = 0xFFF8000000000000ull;
    }
    auto quiet = [&](F v) {
        Bits bits;
        std::memcpy(&bits, &v, sizeof(v));
        bits |= kQuiet;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    };
    if (std::isnan(a)) return quiet(a);
    if (std::isnan(b)) return quiet(b);
    if (std::isnan(c)) return quiet(c);
    F r = std::fma(negate_product ? -a : a, b, negate_addend ? -c : c);
    if (std::isnan(r)) {
        std::memcpy(&r, &kIndefinite, sizeof(r));
    }
    return r;
}

// FMA operation types
enum class FmaOp { ADD, SUB, NMADD, NMSUB };
// FMA form: which operands go where
enum class FmaForm { F132, F213, F231 };

// Packed single-precision FMA
ExecResult exec_fma_ps(ExecContext& ctx, const DecodedInstruction& instr,
                       FmaOp op, FmaForm form) noexcept {
    XmmReg& dst = get_xmm(ctx, instr.operands[0].reg);
    const XmmReg& vvvv = get_xmm(ctx, instr.prefixes.vex_vvvv);
    auto src3_r = read_xmm_or_m128(ctx, instr.operands[1]);
    if (!src3_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
    XmmReg src3 = src3_r.value();

    float a[4], b[4], c[4];
    switch (form) {
        case FmaForm::F132:
            for (int i = 0; i < 4; i++) { a[i] = dst.f32_[i]; b[i] = src3.f32_[i]; c[i] = vvvv.f32_[i]; }
            break;
        case FmaForm::F213:
            for (int i = 0; i < 4; i++) { a[i] = vvvv.f32_[i]; b[i] = dst.f32_[i]; c[i] = src3.f32_[i]; }
            break;
        case FmaForm::F231:
            for (int i = 0; i < 4; i++) { a[i] = vvvv.f32_[i]; b[i] = src3.f32_[i]; c[i] = dst.f32_[i]; }
            break;
    }

    for (int i = 0; i < 4; i++) {
        switch (op) {
            case FmaOp::ADD:   dst.f32_[i] = x86_fma(a[i], b[i], c[i], false, false); break;
            case FmaOp::SUB:   dst.f32_[i] = x86_fma(a[i], b[i], c[i], false, true); break;
            case FmaOp::NMADD: dst.f32_[i] = x86_fma(a[i], b[i], c[i], true, false); break;
            case FmaOp::NMSUB: dst.f32_[i] = x86_fma(a[i], b[i], c[i], true, true); break;
        }
    }
    if (instr.prefixes.vex_l) {
        const u8 di = instr.operands[0].reg & 15;
        const u8 vi = instr.prefixes.vex_vvvv & 15;
        XmmReg src3h{};
        if (instr.operands[1].type == OperandType::Reg128) {
            src3h = ctx.cpu.avx.ymm_high[instr.operands[1].reg & 15];
        } else {
            auto addr = calc_effective_address(ctx, instr.operands[1].mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto r = ctx.mem.read_bytes(addr.value() + 16, reinterpret_cast<Byte*>(&src3h), 16);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        }
        XmmReg& dsth = ctx.cpu.avx.ymm_high[di];
        const XmmReg& vvvvh = ctx.cpu.avx.ymm_high[vi];
        float ah[4], bh[4], ch[4];
        switch (form) {
            case FmaForm::F132:
                for (int i = 0; i < 4; i++) { ah[i] = dsth.f32_[i]; bh[i] = src3h.f32_[i]; ch[i] = vvvvh.f32_[i]; }
                break;
            case FmaForm::F213:
                for (int i = 0; i < 4; i++) { ah[i] = vvvvh.f32_[i]; bh[i] = dsth.f32_[i]; ch[i] = src3h.f32_[i]; }
                break;
            case FmaForm::F231:
                for (int i = 0; i < 4; i++) { ah[i] = vvvvh.f32_[i]; bh[i] = src3h.f32_[i]; ch[i] = dsth.f32_[i]; }
                break;
        }
        for (int i = 0; i < 4; i++) {
            switch (op) {
                case FmaOp::ADD:   dsth.f32_[i] = x86_fma(ah[i], bh[i], ch[i], false, false); break;
                case FmaOp::SUB:   dsth.f32_[i] = x86_fma(ah[i], bh[i], ch[i], false, true); break;
                case FmaOp::NMADD: dsth.f32_[i] = x86_fma(ah[i], bh[i], ch[i], true, false); break;
                case FmaOp::NMSUB: dsth.f32_[i] = x86_fma(ah[i], bh[i], ch[i], true, true); break;
            }
        }
    } else {
        ctx.cpu.avx.ymm_high[instr.operands[0].reg & 15].clear();
    }
    return ExecResult::Continue;
}

// Packed double-precision FMA
ExecResult exec_fma_pd(ExecContext& ctx, const DecodedInstruction& instr,
                       FmaOp op, FmaForm form) noexcept {
    XmmReg& dst = get_xmm(ctx, instr.operands[0].reg);
    const XmmReg& vvvv = get_xmm(ctx, instr.prefixes.vex_vvvv);
    auto src3_r = read_xmm_or_m128(ctx, instr.operands[1]);
    if (!src3_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
    XmmReg src3 = src3_r.value();

    double a[2], b[2], c[2];
    switch (form) {
        case FmaForm::F132:
            for (int i = 0; i < 2; i++) { a[i] = dst.f64_[i]; b[i] = src3.f64_[i]; c[i] = vvvv.f64_[i]; }
            break;
        case FmaForm::F213:
            for (int i = 0; i < 2; i++) { a[i] = vvvv.f64_[i]; b[i] = dst.f64_[i]; c[i] = src3.f64_[i]; }
            break;
        case FmaForm::F231:
            for (int i = 0; i < 2; i++) { a[i] = vvvv.f64_[i]; b[i] = src3.f64_[i]; c[i] = dst.f64_[i]; }
            break;
    }

    for (int i = 0; i < 2; i++) {
        switch (op) {
            case FmaOp::ADD:   dst.f64_[i] = x86_fma(a[i], b[i], c[i], false, false); break;
            case FmaOp::SUB:   dst.f64_[i] = x86_fma(a[i], b[i], c[i], false, true); break;
            case FmaOp::NMADD: dst.f64_[i] = x86_fma(a[i], b[i], c[i], true, false); break;
            case FmaOp::NMSUB: dst.f64_[i] = x86_fma(a[i], b[i], c[i], true, true); break;
        }
    }
    if (instr.prefixes.vex_l) {
        const u8 di = instr.operands[0].reg & 15;
        const u8 vi = instr.prefixes.vex_vvvv & 15;
        XmmReg src3h{};
        if (instr.operands[1].type == OperandType::Reg128) {
            src3h = ctx.cpu.avx.ymm_high[instr.operands[1].reg & 15];
        } else {
            auto addr = calc_effective_address(ctx, instr.operands[1].mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto r = ctx.mem.read_bytes(addr.value() + 16, reinterpret_cast<Byte*>(&src3h), 16);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        }
        XmmReg& dsth = ctx.cpu.avx.ymm_high[di];
        const XmmReg& vvvvh = ctx.cpu.avx.ymm_high[vi];
        double ah[2], bh[2], ch[2];
        switch (form) {
            case FmaForm::F132:
                for (int i = 0; i < 2; i++) { ah[i] = dsth.f64_[i]; bh[i] = src3h.f64_[i]; ch[i] = vvvvh.f64_[i]; }
                break;
            case FmaForm::F213:
                for (int i = 0; i < 2; i++) { ah[i] = vvvvh.f64_[i]; bh[i] = dsth.f64_[i]; ch[i] = src3h.f64_[i]; }
                break;
            case FmaForm::F231:
                for (int i = 0; i < 2; i++) { ah[i] = vvvvh.f64_[i]; bh[i] = src3h.f64_[i]; ch[i] = dsth.f64_[i]; }
                break;
        }
        for (int i = 0; i < 2; i++) {
            switch (op) {
                case FmaOp::ADD:   dsth.f64_[i] = x86_fma(ah[i], bh[i], ch[i], false, false); break;
                case FmaOp::SUB:   dsth.f64_[i] = x86_fma(ah[i], bh[i], ch[i], false, true); break;
                case FmaOp::NMADD: dsth.f64_[i] = x86_fma(ah[i], bh[i], ch[i], true, false); break;
                case FmaOp::NMSUB: dsth.f64_[i] = x86_fma(ah[i], bh[i], ch[i], true, true); break;
            }
        }
    } else {
        ctx.cpu.avx.ymm_high[instr.operands[0].reg & 15].clear();
    }
    return ExecResult::Continue;
}

// Scalar single-precision FMA
ExecResult exec_fma_ss(ExecContext& ctx, const DecodedInstruction& instr,
                       FmaOp op, FmaForm form) noexcept {
    XmmReg& dst = get_xmm(ctx, instr.operands[0].reg);
    const XmmReg& vvvv = get_xmm(ctx, instr.prefixes.vex_vvvv);
    auto src3_r = read_f32_src(ctx, instr.operands[1]);
    if (!src3_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
    float s3 = src3_r.value();

    float a, b, c;
    switch (form) {
        case FmaForm::F132: a = dst.f32_[0]; b = s3;          c = vvvv.f32_[0]; break;
        case FmaForm::F213: a = vvvv.f32_[0]; b = dst.f32_[0]; c = s3;          break;
        case FmaForm::F231: a = vvvv.f32_[0]; b = s3;          c = dst.f32_[0]; break;
    }

    // Intel SDM Vol. 2 VFMADDxxxSS (VEX): DEST[31:0] := fma; DEST[127:32]
    // unchanged; DEST[MAXVL-1:128] := 0. Not the VADDSS vvvv-merge.
    switch (op) {
        case FmaOp::ADD:   dst.f32_[0] = x86_fma(a, b, c, false, false); break;
        case FmaOp::SUB:   dst.f32_[0] = x86_fma(a, b, c, false, true); break;
        case FmaOp::NMADD: dst.f32_[0] = x86_fma(a, b, c, true, false); break;
        case FmaOp::NMSUB: dst.f32_[0] = x86_fma(a, b, c, true, true); break;
    }
    ctx.cpu.avx.ymm_high[instr.operands[0].reg & 15].clear();
    return ExecResult::Continue;
}

// Scalar double-precision FMA
ExecResult exec_fma_sd(ExecContext& ctx, const DecodedInstruction& instr,
                       FmaOp op, FmaForm form) noexcept {
    XmmReg& dst = get_xmm(ctx, instr.operands[0].reg);
    const XmmReg& vvvv = get_xmm(ctx, instr.prefixes.vex_vvvv);
    auto src3_r = read_f64_src(ctx, instr.operands[1]);
    if (!src3_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
    double s3 = src3_r.value();

    double a, b, c;
    switch (form) {
        case FmaForm::F132: a = dst.f64_[0]; b = s3;          c = vvvv.f64_[0]; break;
        case FmaForm::F213: a = vvvv.f64_[0]; b = dst.f64_[0]; c = s3;          break;
        case FmaForm::F231: a = vvvv.f64_[0]; b = s3;          c = dst.f64_[0]; break;
    }

    // Intel SDM Vol. 2 VFMADDxxxSD (VEX): DEST[63:0] := fma; DEST[127:64]
    // unchanged; DEST[MAXVL-1:128] := 0. Not the VADDSD vvvv-merge.
    switch (op) {
        case FmaOp::ADD:   dst.f64_[0] = x86_fma(a, b, c, false, false); break;
        case FmaOp::SUB:   dst.f64_[0] = x86_fma(a, b, c, false, true); break;
        case FmaOp::NMADD: dst.f64_[0] = x86_fma(a, b, c, true, false); break;
        case FmaOp::NMSUB: dst.f64_[0] = x86_fma(a, b, c, true, true); break;
    }
    ctx.cpu.avx.ymm_high[instr.operands[0].reg & 15].clear();
    return ExecResult::Continue;
}

// VFMADDSUB (odd elements add, even subtract) and VFMSUBADD (odd subtract,
// even add), Intel SDM Vol. 2. The VEX.256 forms repeat the pattern across
// all eight/four elements, so the upper lane is computed exactly like the
// lower one; it used to be cleared, which zeroed half of every 256-bit
// result (measured against silicon by the VEX sweep).
template <typename F>
ExecResult exec_fm_alternating(ExecContext& ctx, const DecodedInstruction& instr,
                               FmaForm form, bool odd_elements_add) noexcept {
    constexpr int kN = 16 / static_cast<int>(sizeof(F));
    const u8 di = instr.operands[0].reg & 15;
    const u8 vi = instr.prefixes.vex_vvvv & 15;
    const bool ymm = instr.prefixes.vex_l;

    // Read every source for both lanes before writing anything.
    XmmReg src3[2]{};
    if (instr.operands[1].type == OperandType::Reg128) {
        src3[0] = ctx.cpu.sse.xmm[instr.operands[1].reg & 15];
        if (ymm) src3[1] = ctx.cpu.avx.ymm_high[instr.operands[1].reg & 15];
    } else {
        auto addr = calc_effective_address(ctx, instr.operands[1].mem);
        if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto r = ctx.mem.read_bytes(addr.value(), reinterpret_cast<Byte*>(&src3[0]), 16);
        if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        if (ymm) {
            r = ctx.mem.read_bytes(addr.value() + 16, reinterpret_cast<Byte*>(&src3[1]), 16);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        }
    }
    const XmmReg dst_in[2] = {ctx.cpu.sse.xmm[di], ctx.cpu.avx.ymm_high[di]};
    const XmmReg vvvv_in[2] = {ctx.cpu.sse.xmm[vi], ctx.cpu.avx.ymm_high[vi]};

    XmmReg out[2]{};
    for (int lane = 0; lane < (ymm ? 2 : 1); ++lane) {
        for (int i = 0; i < kN; ++i) {
            F d, v, m;
            std::memcpy(&d, dst_in[lane].u8_ + i * sizeof(F), sizeof(F));
            std::memcpy(&v, vvvv_in[lane].u8_ + i * sizeof(F), sizeof(F));
            std::memcpy(&m, src3[lane].u8_ + i * sizeof(F), sizeof(F));
            F a{}, b{}, c{};
            switch (form) {
                case FmaForm::F132: a = d; b = m; c = v; break;
                case FmaForm::F213: a = v; b = d; c = m; break;
                case FmaForm::F231: a = v; b = m; c = d; break;
            }
            const bool add = ((i & 1) != 0) == odd_elements_add;
            const F r = x86_fma(a, b, c, false, !add);
            std::memcpy(out[lane].u8_ + i * sizeof(F), &r, sizeof(F));
        }
    }
    ctx.cpu.sse.xmm[di] = out[0];
    if (ymm) {
        ctx.cpu.avx.ymm_high[di] = out[1];
    } else {
        ctx.cpu.avx.ymm_high[di].clear();
    }
    return ExecResult::Continue;
}

ExecResult exec_fmaddsub_ps(ExecContext& ctx, const DecodedInstruction& instr,
                            FmaForm form) noexcept {
    return exec_fm_alternating<float>(ctx, instr, form, /*odd_elements_add=*/true);
}
ExecResult exec_fmaddsub_pd(ExecContext& ctx, const DecodedInstruction& instr,
                            FmaForm form) noexcept {
    return exec_fm_alternating<double>(ctx, instr, form, /*odd_elements_add=*/true);
}
ExecResult exec_fmsubadd_ps(ExecContext& ctx, const DecodedInstruction& instr,
                            FmaForm form) noexcept {
    return exec_fm_alternating<float>(ctx, instr, form, /*odd_elements_add=*/false);
}
ExecResult exec_fmsubadd_pd(ExecContext& ctx, const DecodedInstruction& instr,
                            FmaForm form) noexcept {
    return exec_fm_alternating<double>(ctx, instr, form, /*odd_elements_add=*/false);
}

} // anonymous namespace

ExecResult exec_fma_ops(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // FMA is SSE-class arithmetic, so MXCSR.RC governs it — the same contract
    // exec_sse_arith installs for ADDSD/DIVSS/etc. This file never did, so
    // every FMA inherited whichever rounding mode was installed last, which
    // after any x87 instruction is FCW.RC. Two independent consequences:
    //
    //   * the guest's SSE rounding mode was ignored by FMA outright, and
    //   * host_fenv could not attribute FMA's sticky exception flags, because
    //     attribution happens at exactly this call. Without it the flags were
    //     left unowned and leaked into MXCSR, which is how the interpreter came
    //     to report IE on `VFMADD132SS` with infinity operands where hardware
    //     raises nothing.
    math::apply_sse_rounding(ctx.cpu.sse.mxcsr);
    switch (instr.opcode) {
    // VFMADD 132
    case Opcode::VFMADD132PS_XMM_XMM_XMMM128: return exec_fma_ps(ctx, instr, FmaOp::ADD, FmaForm::F132);
    case Opcode::VFMADD132PD_XMM_XMM_XMMM128: return exec_fma_pd(ctx, instr, FmaOp::ADD, FmaForm::F132);
    case Opcode::VFMADD132SS_XMM_XMM_XMMM32:  return exec_fma_ss(ctx, instr, FmaOp::ADD, FmaForm::F132);
    case Opcode::VFMADD132SD_XMM_XMM_XMMM64:  return exec_fma_sd(ctx, instr, FmaOp::ADD, FmaForm::F132);
    // VFMADD 213
    case Opcode::VFMADD213PS_XMM_XMM_XMMM128: return exec_fma_ps(ctx, instr, FmaOp::ADD, FmaForm::F213);
    case Opcode::VFMADD213PD_XMM_XMM_XMMM128: return exec_fma_pd(ctx, instr, FmaOp::ADD, FmaForm::F213);
    case Opcode::VFMADD213SS_XMM_XMM_XMMM32:  return exec_fma_ss(ctx, instr, FmaOp::ADD, FmaForm::F213);
    case Opcode::VFMADD213SD_XMM_XMM_XMMM64:  return exec_fma_sd(ctx, instr, FmaOp::ADD, FmaForm::F213);
    // VFMADD 231
    case Opcode::VFMADD231PS_XMM_XMM_XMMM128: return exec_fma_ps(ctx, instr, FmaOp::ADD, FmaForm::F231);
    case Opcode::VFMADD231PD_XMM_XMM_XMMM128: return exec_fma_pd(ctx, instr, FmaOp::ADD, FmaForm::F231);
    case Opcode::VFMADD231SS_XMM_XMM_XMMM32:  return exec_fma_ss(ctx, instr, FmaOp::ADD, FmaForm::F231);
    case Opcode::VFMADD231SD_XMM_XMM_XMMM64:  return exec_fma_sd(ctx, instr, FmaOp::ADD, FmaForm::F231);

    // VFMSUB 132
    case Opcode::VFMSUB132PS_XMM_XMM_XMMM128: return exec_fma_ps(ctx, instr, FmaOp::SUB, FmaForm::F132);
    case Opcode::VFMSUB132PD_XMM_XMM_XMMM128: return exec_fma_pd(ctx, instr, FmaOp::SUB, FmaForm::F132);
    case Opcode::VFMSUB132SS_XMM_XMM_XMMM32:  return exec_fma_ss(ctx, instr, FmaOp::SUB, FmaForm::F132);
    case Opcode::VFMSUB132SD_XMM_XMM_XMMM64:  return exec_fma_sd(ctx, instr, FmaOp::SUB, FmaForm::F132);
    // VFMSUB 213
    case Opcode::VFMSUB213PS_XMM_XMM_XMMM128: return exec_fma_ps(ctx, instr, FmaOp::SUB, FmaForm::F213);
    case Opcode::VFMSUB213PD_XMM_XMM_XMMM128: return exec_fma_pd(ctx, instr, FmaOp::SUB, FmaForm::F213);
    case Opcode::VFMSUB213SS_XMM_XMM_XMMM32:  return exec_fma_ss(ctx, instr, FmaOp::SUB, FmaForm::F213);
    case Opcode::VFMSUB213SD_XMM_XMM_XMMM64:  return exec_fma_sd(ctx, instr, FmaOp::SUB, FmaForm::F213);
    // VFMSUB 231
    case Opcode::VFMSUB231PS_XMM_XMM_XMMM128: return exec_fma_ps(ctx, instr, FmaOp::SUB, FmaForm::F231);
    case Opcode::VFMSUB231PD_XMM_XMM_XMMM128: return exec_fma_pd(ctx, instr, FmaOp::SUB, FmaForm::F231);
    case Opcode::VFMSUB231SS_XMM_XMM_XMMM32:  return exec_fma_ss(ctx, instr, FmaOp::SUB, FmaForm::F231);
    case Opcode::VFMSUB231SD_XMM_XMM_XMMM64:  return exec_fma_sd(ctx, instr, FmaOp::SUB, FmaForm::F231);

    // VFNMADD 132
    case Opcode::VFNMADD132PS_XMM_XMM_XMMM128: return exec_fma_ps(ctx, instr, FmaOp::NMADD, FmaForm::F132);
    case Opcode::VFNMADD132PD_XMM_XMM_XMMM128: return exec_fma_pd(ctx, instr, FmaOp::NMADD, FmaForm::F132);
    case Opcode::VFNMADD132SS_XMM_XMM_XMMM32:  return exec_fma_ss(ctx, instr, FmaOp::NMADD, FmaForm::F132);
    case Opcode::VFNMADD132SD_XMM_XMM_XMMM64:  return exec_fma_sd(ctx, instr, FmaOp::NMADD, FmaForm::F132);
    // VFNMADD 213
    case Opcode::VFNMADD213PS_XMM_XMM_XMMM128: return exec_fma_ps(ctx, instr, FmaOp::NMADD, FmaForm::F213);
    case Opcode::VFNMADD213PD_XMM_XMM_XMMM128: return exec_fma_pd(ctx, instr, FmaOp::NMADD, FmaForm::F213);
    case Opcode::VFNMADD213SS_XMM_XMM_XMMM32:  return exec_fma_ss(ctx, instr, FmaOp::NMADD, FmaForm::F213);
    case Opcode::VFNMADD213SD_XMM_XMM_XMMM64:  return exec_fma_sd(ctx, instr, FmaOp::NMADD, FmaForm::F213);
    // VFNMADD 231
    case Opcode::VFNMADD231PS_XMM_XMM_XMMM128: return exec_fma_ps(ctx, instr, FmaOp::NMADD, FmaForm::F231);
    case Opcode::VFNMADD231PD_XMM_XMM_XMMM128: return exec_fma_pd(ctx, instr, FmaOp::NMADD, FmaForm::F231);
    case Opcode::VFNMADD231SS_XMM_XMM_XMMM32:  return exec_fma_ss(ctx, instr, FmaOp::NMADD, FmaForm::F231);
    case Opcode::VFNMADD231SD_XMM_XMM_XMMM64:  return exec_fma_sd(ctx, instr, FmaOp::NMADD, FmaForm::F231);

    // VFNMSUB 132
    case Opcode::VFNMSUB132PS_XMM_XMM_XMMM128: return exec_fma_ps(ctx, instr, FmaOp::NMSUB, FmaForm::F132);
    case Opcode::VFNMSUB132PD_XMM_XMM_XMMM128: return exec_fma_pd(ctx, instr, FmaOp::NMSUB, FmaForm::F132);
    case Opcode::VFNMSUB132SS_XMM_XMM_XMMM32:  return exec_fma_ss(ctx, instr, FmaOp::NMSUB, FmaForm::F132);
    case Opcode::VFNMSUB132SD_XMM_XMM_XMMM64:  return exec_fma_sd(ctx, instr, FmaOp::NMSUB, FmaForm::F132);
    // VFNMSUB 213
    case Opcode::VFNMSUB213PS_XMM_XMM_XMMM128: return exec_fma_ps(ctx, instr, FmaOp::NMSUB, FmaForm::F213);
    case Opcode::VFNMSUB213PD_XMM_XMM_XMMM128: return exec_fma_pd(ctx, instr, FmaOp::NMSUB, FmaForm::F213);
    case Opcode::VFNMSUB213SS_XMM_XMM_XMMM32:  return exec_fma_ss(ctx, instr, FmaOp::NMSUB, FmaForm::F213);
    case Opcode::VFNMSUB213SD_XMM_XMM_XMMM64:  return exec_fma_sd(ctx, instr, FmaOp::NMSUB, FmaForm::F213);
    // VFNMSUB 231
    case Opcode::VFNMSUB231PS_XMM_XMM_XMMM128: return exec_fma_ps(ctx, instr, FmaOp::NMSUB, FmaForm::F231);
    case Opcode::VFNMSUB231PD_XMM_XMM_XMMM128: return exec_fma_pd(ctx, instr, FmaOp::NMSUB, FmaForm::F231);
    case Opcode::VFNMSUB231SS_XMM_XMM_XMMM32:  return exec_fma_ss(ctx, instr, FmaOp::NMSUB, FmaForm::F231);
    case Opcode::VFNMSUB231SD_XMM_XMM_XMMM64:  return exec_fma_sd(ctx, instr, FmaOp::NMSUB, FmaForm::F231);

    // VFMADDSUB 132/213/231 PS
    case Opcode::VFMADDSUB132PS_XMM_XMM_XMMM128: return exec_fmaddsub_ps(ctx, instr, FmaForm::F132);
    case Opcode::VFMADDSUB213PS_XMM_XMM_XMMM128: return exec_fmaddsub_ps(ctx, instr, FmaForm::F213);
    case Opcode::VFMADDSUB231PS_XMM_XMM_XMMM128: return exec_fmaddsub_ps(ctx, instr, FmaForm::F231);
    // VFMADDSUB 132/213/231 PD
    case Opcode::VFMADDSUB132PD_XMM_XMM_XMMM128: return exec_fmaddsub_pd(ctx, instr, FmaForm::F132);
    case Opcode::VFMADDSUB213PD_XMM_XMM_XMMM128: return exec_fmaddsub_pd(ctx, instr, FmaForm::F213);
    case Opcode::VFMADDSUB231PD_XMM_XMM_XMMM128: return exec_fmaddsub_pd(ctx, instr, FmaForm::F231);

    // VFMSUBADD 132/213/231 PS
    case Opcode::VFMSUBADD132PS_XMM_XMM_XMMM128: return exec_fmsubadd_ps(ctx, instr, FmaForm::F132);
    case Opcode::VFMSUBADD213PS_XMM_XMM_XMMM128: return exec_fmsubadd_ps(ctx, instr, FmaForm::F213);
    case Opcode::VFMSUBADD231PS_XMM_XMM_XMMM128: return exec_fmsubadd_ps(ctx, instr, FmaForm::F231);
    // VFMSUBADD 132/213/231 PD
    case Opcode::VFMSUBADD132PD_XMM_XMM_XMMM128: return exec_fmsubadd_pd(ctx, instr, FmaForm::F132);
    case Opcode::VFMSUBADD213PD_XMM_XMM_XMMM128: return exec_fmsubadd_pd(ctx, instr, FmaForm::F213);
    case Opcode::VFMSUBADD231PD_XMM_XMM_XMMM128: return exec_fmsubadd_pd(ctx, instr, FmaForm::F231);

    default:
        ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }
}

} // namespace vitreo::cpu::ops
