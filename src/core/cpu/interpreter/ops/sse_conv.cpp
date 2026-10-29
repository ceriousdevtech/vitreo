// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE conversion instruction execution (CPU-033)
//
// Integration checklist (modifications to existing files):
//
// 1. exec_ops.hpp — add declaration:
//      [[nodiscard]] ExecResult exec_sse_conv(ExecContext& ctx,
//          const DecodedInstruction& instr) noexcept;
//
// 2. interpreter.cpp — add cases in execute_instruction switch:
//      case Opcode::CVTSI2SS_XMM_RM32: case Opcode::CVTSI2SS_XMM_RM64:
//      case Opcode::CVTSI2SD_XMM_RM32: case Opcode::CVTSI2SD_XMM_RM64:
//      case Opcode::CVTSS2SI_R32_XMMM32: case Opcode::CVTSS2SI_R64_XMMM32:
//      case Opcode::CVTSD2SI_R32_XMMM64: case Opcode::CVTSD2SI_R64_XMMM64:
//      case Opcode::CVTTSS2SI_R32_XMMM32: case Opcode::CVTTSS2SI_R64_XMMM32:
//      case Opcode::CVTTSD2SI_R32_XMMM64: case Opcode::CVTTSD2SI_R64_XMMM64:
//      case Opcode::CVTSS2SD_XMM_XMMM32: case Opcode::CVTSD2SS_XMM_XMMM64:
//      case Opcode::CVTPS2DQ_XMM_XMMM128: case Opcode::CVTDQ2PS_XMM_XMMM128:
//      case Opcode::CVTPD2DQ_XMM_XMMM128: case Opcode::CVTDQ2PD_XMM_XMMM64:
//      case Opcode::CVTPS2PD_XMM_XMMM64: case Opcode::CVTPD2PS_XMM_XMMM128:
//      case Opcode::CVTTPD2DQ_XMM_XMMM128: case Opcode::CVTTPS2DQ_XMM_XMMM128:
//          return exec_sse_conv(ctx, instr);
//
// 3. src/CMakeLists.txt — add to vitreo_cpu_interpreter sources:
//      cpu/interpreter/ops/sse_conv.cpp

#include "exec_ops.hpp"
#include "core/math/host_fenv.hpp"
#include "helpers.hpp"
#include "../../sse_float_semantics.hpp"
#include <cmath>
#include <cstring>
#include <limits>

namespace vitreo::cpu::ops {

// ============================================================================
// Helpers
// ============================================================================

static XmmReg& get_xmm(ExecContext& ctx, u8 index) noexcept {
    return ctx.cpu.sse.xmm[index & 15];
}

/// Read scalar f32 from operand (register=XMM low lane, or memory 4 bytes)
static Result<f32, Error> read_ss(ExecContext& ctx, const Operand& op) noexcept {
    if (op.type == OperandType::Reg128) {
        return Result<f32, Error>::ok(ctx.cpu.sse.xmm[op.reg & 15].f32_[0]);
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<f32, Error>::err(addr.error());
    auto val = read_memory(ctx, addr.value(), 4);
    if (!val) return Result<f32, Error>::err(val.error());
    f32 f;
    u32 bits = static_cast<u32>(val.value());
    std::memcpy(&f, &bits, 4);
    return Result<f32, Error>::ok(f);
}

/// Read scalar f64 from operand (register=XMM low lane, or memory 8 bytes)
static Result<f64, Error> read_sd(ExecContext& ctx, const Operand& op) noexcept {
    if (op.type == OperandType::Reg128) {
        return Result<f64, Error>::ok(ctx.cpu.sse.xmm[op.reg & 15].f64_[0]);
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<f64, Error>::err(addr.error());
    auto val = read_memory(ctx, addr.value(), 8);
    if (!val) return Result<f64, Error>::err(val.error());
    f64 d;
    u64 bits = val.value();
    std::memcpy(&d, &bits, 8);
    return Result<f64, Error>::ok(d);
}

/// Read packed 128 bits from operand into XmmReg
static Result<XmmReg, Error> read_pack(ExecContext& ctx, const Operand& op,
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

/// Read 64-bit (8 bytes) from operand into XmmReg low half (high zeroed)
static Result<XmmReg, Error> read_pack64(ExecContext& ctx, const Operand& op) noexcept {
    if (op.type == OperandType::Reg128) {
        return Result<XmmReg, Error>::ok(ctx.cpu.sse.xmm[op.reg & 15]);
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<XmmReg, Error>::err(addr.error());
    XmmReg tmp{};
    auto r = ctx.mem.read_bytes(addr.value(), reinterpret_cast<Byte*>(&tmp), 8);
    if (!r) return Result<XmmReg, Error>::err(r.error());
    return Result<XmmReg, Error>::ok(tmp);
}

/// Read 64 bits from an MMX register or a quadword memory operand.
/// MM0-MM7 alias the x87 register mantissas (FpuState::get_mm).
static Result<u64, Error> read_mmm64(ExecContext& ctx, const Operand& op) noexcept {
    if (op.type == OperandType::RegMM) {
        ctx.cpu.fpu.mmx_enter();
        return Result<u64, Error>::ok(ctx.cpu.fpu.get_mm(op.reg & 7));
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<u64, Error>::err(addr.error());
    return read_memory(ctx, addr.value(), 8);
}

/// Write 64 bits to an MMX register (also sets the x87 "MMX in use" tag).
static void write_mm(ExecContext& ctx, u8 reg, u64 value) noexcept {
    ctx.cpu.fpu.set_mm(reg & 7, value);
    ctx.cpu.fpu.mmx_enter();
}

/// Pack two signed dwords into an MMX quadword (low dword first).
static u64 pack_i32_pair(i32 low, i32 high) noexcept {
    return static_cast<u64>(static_cast<u32>(low)) |
           (static_cast<u64>(static_cast<u32>(high)) << 32);
}

/// Read i32 from GP register or 32-bit memory operand
static Result<i32, Error> read_rm32(ExecContext& ctx, const Operand& op) noexcept {
    if (op.type == OperandType::Reg32) {
        u32 v = ctx.cpu.gpr.get32(op.reg);
        return Result<i32, Error>::ok(static_cast<i32>(v));
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<i32, Error>::err(addr.error());
    auto val = read_memory(ctx, addr.value(), 4);
    if (!val) return Result<i32, Error>::err(val.error());
    return Result<i32, Error>::ok(static_cast<i32>(static_cast<u32>(val.value())));
}

/// Read i64 from GP register or 64-bit memory operand
static Result<i64, Error> read_rm64(ExecContext& ctx, const Operand& op) noexcept {
    if (op.type == OperandType::Reg64) {
        u64 v = ctx.cpu.gpr.get64(op.reg);
        return Result<i64, Error>::ok(static_cast<i64>(v));
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<i64, Error>::err(addr.error());
    auto val = read_memory(ctx, addr.value(), 8);
    if (!val) return Result<i64, Error>::err(val.error());
    return Result<i64, Error>::ok(static_cast<i64>(val.value()));
}

// ============================================================================
// Overflow-safe float/double → integer conversion
// ============================================================================

/// Round `val` to an integral value using the rounding mode selected by
/// MXCSR.RC (bits 14:13), computed explicitly.
///
/// std::lrint / std::nearbyint follow the *host* FPU rounding mode, which is
/// round-to-nearest under normal execution and has nothing to do with the
/// guest's MXCSR. Every CVTSS2SI / CVTSD2SI / CVTPS2DQ therefore rounded to
/// nearest no matter what the guest had programmed, and a guest that set
/// round-down or round-up silently got the wrong integer. Only the CVTT*
/// (truncating) forms were unaffected, because truncation happens to be what
/// a static_cast does.
[[nodiscard]] inline double sse_round_by_mxcsr(double val, u32 mxcsr) noexcept {
    switch ((mxcsr >> 13) & 3u) {
    case 1:  return std::floor(val);   // round toward -inf
    case 2:  return std::ceil(val);    // round toward +inf
    case 3:  return std::trunc(val);   // round toward zero
    default: break;                    // 0: round to nearest, ties to even
    }
    // Compared against the midpoint rather than through val - lower: for a
    // positive denormal that difference is itself denormal, and under
    // MXCSR.FTZ (host FPCR.FZ) computing it raises underflow, which no CVT*
    // instruction signals (Intel SDM Vol. 2 CVTPD2DQ: #I, #P only).
    const double lower = std::floor(val);
    if (lower == val) return val;
    const double mid = lower + 0.5;  // exact: a non-integral val is < 2^52
    if (val > mid) return lower + 1.0;
    if (val < mid) return lower;
    // Exact tie: pick the even neighbour.
    return (std::fmod(lower, 2.0) == 0.0) ? lower : lower + 1.0;
}

/// Convert f32 → i32 with rounding (lrintf). Returns 0x80000000 on overflow/NaN.
static i32 cvt_f32_to_i32_round(f32 val, u32& mxcsr) noexcept {
    if (std::isnan(val) || val >= static_cast<f32>(INT32_MAX) + 1.0f ||
        val < static_cast<f32>(INT32_MIN)) {
        return static_cast<i32>(0x80000000u);
    }
    const i32 r = static_cast<i32>(sse_round_by_mxcsr(val, mxcsr));
    sse_cvt_int_pe(val, r, mxcsr);
    return r;
}

/// Convert f64 → i32 with rounding (lrint). Returns 0x80000000 on overflow/NaN.
static i32 cvt_f64_to_i32_round(f64 val, u32& mxcsr) noexcept {
    if (std::isnan(val) || val >= static_cast<f64>(INT32_MAX) + 1.0 ||
        val < static_cast<f64>(INT32_MIN)) {
        return static_cast<i32>(0x80000000u);
    }
    const i32 r = static_cast<i32>(sse_round_by_mxcsr(val, mxcsr));
    sse_cvt_int_pe(val, r, mxcsr);
    return r;
}

/// Convert f32 → i64 with rounding. Returns 0x8000000000000000 on overflow/NaN.
static i64 cvt_f32_to_i64_round(f32 val, u32& mxcsr) noexcept {
    if (std::isnan(val) || val >= static_cast<f32>(INT64_MAX) ||
        val < static_cast<f32>(INT64_MIN)) {
        return static_cast<i64>(0x8000000000000000ull);
    }
    const i64 r = static_cast<i64>(sse_round_by_mxcsr(val, mxcsr));
    sse_cvt_int_pe(val, r, mxcsr);
    return r;
}

/// Convert f64 → i64 with rounding. Returns 0x8000000000000000 on overflow/NaN.
static i64 cvt_f64_to_i64_round(f64 val, u32& mxcsr) noexcept {
    if (std::isnan(val) || val >= static_cast<f64>(INT64_MAX) ||
        val < static_cast<f64>(INT64_MIN)) {
        return static_cast<i64>(0x8000000000000000ull);
    }
    const i64 r = static_cast<i64>(sse_round_by_mxcsr(val, mxcsr));
    sse_cvt_int_pe(val, r, mxcsr);
    return r;
}

/// Convert f32 → i32 with truncation. Returns 0x80000000 on overflow/NaN.
static i32 cvt_f32_to_i32_trunc(f32 val, u32& mxcsr) noexcept {
    if (std::isnan(val) || val >= static_cast<f32>(INT32_MAX) + 1.0f ||
        val < static_cast<f32>(INT32_MIN)) {
        return static_cast<i32>(0x80000000u);
    }
    const i32 r = static_cast<i32>(std::truncf(val));
    sse_cvt_int_pe(val, r, mxcsr);
    return r;
}

/// Convert f64 → i32 with truncation. Returns 0x80000000 on overflow/NaN.
static i32 cvt_f64_to_i32_trunc(f64 val, u32& mxcsr) noexcept {
    if (std::isnan(val) || val >= static_cast<f64>(INT32_MAX) + 1.0 ||
        val < static_cast<f64>(INT32_MIN)) {
        return static_cast<i32>(0x80000000u);
    }
    const i32 r = static_cast<i32>(std::trunc(val));
    sse_cvt_int_pe(val, r, mxcsr);
    return r;
}

/// Convert f32 → i64 with truncation. Returns 0x8000000000000000 on overflow/NaN.
static i64 cvt_f32_to_i64_trunc(f32 val, u32& mxcsr) noexcept {
    if (std::isnan(val) || val >= static_cast<f32>(INT64_MAX) ||
        val < static_cast<f32>(INT64_MIN)) {
        return static_cast<i64>(0x8000000000000000ull);
    }
    const i64 r = static_cast<i64>(std::truncf(val));
    sse_cvt_int_pe(val, r, mxcsr);
    return r;
}

/// Convert f64 → i64 with truncation. Returns 0x8000000000000000 on overflow/NaN.
static i64 cvt_f64_to_i64_trunc(f64 val, u32& mxcsr) noexcept {
    if (std::isnan(val) || val >= static_cast<f64>(INT64_MAX) ||
        val < static_cast<f64>(INT64_MIN)) {
        return static_cast<i64>(0x8000000000000000ull);
    }
    const i64 r = static_cast<i64>(std::trunc(val));
    sse_cvt_int_pe(val, r, mxcsr);
    return r;
}

/// Generic GP-fault helper
static ExecResult gp_fault(ExecContext& ctx) noexcept {
    ctx.exception = CpuException::GeneralProtection;
    return ExecResult::Exception;
}

/// SIMD fault helper
static ExecResult simd_fault(ExecContext& ctx) noexcept {
    ctx.exception = CpuException::SimdFloatingPoint;
    return ExecResult::Exception;
}

// ============================================================================
// Main SSE conversion dispatcher
// ============================================================================

ExecResult exec_sse_conv(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // The float<->int and float<->float conversions round per MXCSR.RC just as
    // the arithmetic does (only the CVTT* forms truncate unconditionally), so
    // this class needs the host mode installed too -- exec_sse_arith is not
    // the only SSE entry point. Missing it here showed up as a JIT<->
    // interpreter divergence on CVTPI2PS: the JIT installed MXCSR.RC and the
    // interpreter inherited whatever x87 had left behind, 1 ULP apart.
    math::apply_sse_rounding(ctx.cpu.sse.mxcsr);
    // A VEX-encoded form carries no 16-byte alignment requirement (Intel SDM
    // Vol. 2); `ctx.vex_origin` catches the ones the AVX bridge re-dispatches
    // here with the VEX prefix already stripped.
    const bool aligned_form = !instr.prefixes.vex && !ctx.vex_origin;
    const auto& dst_op = instr.operands[0];
    const auto& src_op = instr.operands[1];
    u32& mxcsr = ctx.cpu.sse.mxcsr;

    switch (instr.opcode) {

    // ========================================================================
    // CVTSI2SS: int → float, preserve upper XMM bits
    // ========================================================================
    case Opcode::CVTSI2SS_XMM_RM32: {
        auto src = read_rm32(ctx, src_op);
        if (!src) return simd_fault(ctx);
        const f32 narrow = static_cast<f32>(src.value());
        sse_cvt_int_to_f32_pe(src.value(), narrow, mxcsr);
        get_xmm(ctx, dst_op.reg).f32_[0] = narrow;
        return ExecResult::Continue;
    }
    case Opcode::CVTSI2SS_XMM_RM64: {
        auto src = read_rm64(ctx, src_op);
        if (!src) return simd_fault(ctx);
        const f32 narrow = static_cast<f32>(src.value());
        sse_cvt_int_to_f32_pe(src.value(), narrow, mxcsr);
        get_xmm(ctx, dst_op.reg).f32_[0] = narrow;
        return ExecResult::Continue;
    }

    // ========================================================================
    // CVTSI2SD: int → double, preserve upper XMM bits
    // ========================================================================
    case Opcode::CVTSI2SD_XMM_RM32: {
        auto src = read_rm32(ctx, src_op);
        if (!src) return simd_fault(ctx);
        get_xmm(ctx, dst_op.reg).f64_[0] = static_cast<f64>(src.value());
        return ExecResult::Continue;
    }
    case Opcode::CVTSI2SD_XMM_RM64: {
        auto src = read_rm64(ctx, src_op);
        if (!src) return simd_fault(ctx);
        get_xmm(ctx, dst_op.reg).f64_[0] = static_cast<f64>(src.value());
        return ExecResult::Continue;
    }

    // ========================================================================
    // CVTSS2SI: float → int (rounding per MXCSR.RC / host mode)
    // ========================================================================
    case Opcode::CVTSS2SI_R32_XMMM32: {
        auto src = read_ss(ctx, src_op);
        if (!src) return simd_fault(ctx);
        i32 result = cvt_f32_to_i32_round(src.value(), mxcsr);
        ctx.cpu.gpr.set32(dst_op.reg, static_cast<u32>(result));
        return ExecResult::Continue;
    }
    case Opcode::CVTSS2SI_R64_XMMM32: {
        auto src = read_ss(ctx, src_op);
        if (!src) return simd_fault(ctx);
        i64 result = cvt_f32_to_i64_round(src.value(), mxcsr);
        ctx.cpu.gpr.set64(dst_op.reg, static_cast<u64>(result));
        return ExecResult::Continue;
    }

    // ========================================================================
    // CVTSD2SI: double → int (rounding per MXCSR.RC / host mode)
    // ========================================================================
    case Opcode::CVTSD2SI_R32_XMMM64: {
        auto src = read_sd(ctx, src_op);
        if (!src) return simd_fault(ctx);
        i32 result = cvt_f64_to_i32_round(src.value(), mxcsr);
        ctx.cpu.gpr.set32(dst_op.reg, static_cast<u32>(result));
        return ExecResult::Continue;
    }
    case Opcode::CVTSD2SI_R64_XMMM64: {
        auto src = read_sd(ctx, src_op);
        if (!src) return simd_fault(ctx);
        i64 result = cvt_f64_to_i64_round(src.value(), mxcsr);
        ctx.cpu.gpr.set64(dst_op.reg, static_cast<u64>(result));
        return ExecResult::Continue;
    }

    // ========================================================================
    // CVTTSS2SI: float → int (truncation)
    // ========================================================================
    case Opcode::CVTTSS2SI_R32_XMMM32: {
        auto src = read_ss(ctx, src_op);
        if (!src) return simd_fault(ctx);
        i32 result = cvt_f32_to_i32_trunc(src.value(), mxcsr);
        ctx.cpu.gpr.set32(dst_op.reg, static_cast<u32>(result));
        return ExecResult::Continue;
    }
    case Opcode::CVTTSS2SI_R64_XMMM32: {
        auto src = read_ss(ctx, src_op);
        if (!src) return simd_fault(ctx);
        i64 result = cvt_f32_to_i64_trunc(src.value(), mxcsr);
        ctx.cpu.gpr.set64(dst_op.reg, static_cast<u64>(result));
        return ExecResult::Continue;
    }

    // ========================================================================
    // CVTTSD2SI: double → int (truncation)
    // ========================================================================
    case Opcode::CVTTSD2SI_R32_XMMM64: {
        auto src = read_sd(ctx, src_op);
        if (!src) return simd_fault(ctx);
        i32 result = cvt_f64_to_i32_trunc(src.value(), mxcsr);
        ctx.cpu.gpr.set32(dst_op.reg, static_cast<u32>(result));
        return ExecResult::Continue;
    }
    case Opcode::CVTTSD2SI_R64_XMMM64: {
        auto src = read_sd(ctx, src_op);
        if (!src) return simd_fault(ctx);
        i64 result = cvt_f64_to_i64_trunc(src.value(), mxcsr);
        ctx.cpu.gpr.set64(dst_op.reg, static_cast<u64>(result));
        return ExecResult::Continue;
    }

    // ========================================================================
    // CVTSS2SD: float → double (exact), preserve upper 64 bits
    // ========================================================================
    case Opcode::CVTSS2SD_XMM_XMMM32: {
        auto src = read_ss(ctx, src_op);
        if (!src) return simd_fault(ctx);
        get_xmm(ctx, dst_op.reg).f64_[0] = static_cast<f64>(src.value());
        return ExecResult::Continue;
    }

    // ========================================================================
    // CVTSD2SS: double → float (may lose precision), preserve upper bits
    // ========================================================================
    case Opcode::CVTSD2SS_XMM_XMMM64: {
        auto src = read_sd(ctx, src_op);
        if (!src) return simd_fault(ctx);
        const f32 narrow = static_cast<f32>(src.value());
        sse_cvt_narrow_pe(src.value(), narrow, mxcsr);
        get_xmm(ctx, dst_op.reg).f32_[0] = narrow;
        return ExecResult::Continue;
    }

    // ========================================================================
    // CVTPS2DQ: 4×f32 → 4×i32 (rounding)
    // ========================================================================
    case Opcode::CVTPS2DQ_XMM_XMMM128: {
        auto src = read_pack(ctx, src_op, aligned_form);
        if (!src) return simd_fault(ctx);
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 4; ++i) {
            dst.i32_[i] = cvt_f32_to_i32_round(src.value().f32_[i], mxcsr);
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // CVTDQ2PS: 4×i32 → 4×f32
    // ========================================================================
    case Opcode::CVTDQ2PS_XMM_XMMM128: {
        auto src = read_pack(ctx, src_op, aligned_form);
        if (!src) return simd_fault(ctx);
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 4; ++i) {
            const f32 narrow = static_cast<f32>(src.value().i32_[i]);
            sse_cvt_int_to_f32_pe(src.value().i32_[i], narrow, mxcsr);
            dst.f32_[i] = narrow;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // CVTTPS2DQ: 4×f32 → 4×i32 (truncation)
    // ========================================================================
    case Opcode::CVTTPS2DQ_XMM_XMMM128: {
        auto src = read_pack(ctx, src_op, aligned_form);
        if (!src) return simd_fault(ctx);
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 4; ++i) {
            dst.i32_[i] = cvt_f32_to_i32_trunc(src.value().f32_[i], mxcsr);
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // CVTPD2DQ: 2×f64 → 2×i32 in low 64 bits, zero upper 64
    // ========================================================================
    case Opcode::CVTPD2DQ_XMM_XMMM128: {
        auto src = read_pack(ctx, src_op, aligned_form);
        if (!src) return simd_fault(ctx);
        auto& dst = get_xmm(ctx, dst_op.reg);
        dst.i32_[0] = cvt_f64_to_i32_round(src.value().f64_[0], mxcsr);
        dst.i32_[1] = cvt_f64_to_i32_round(src.value().f64_[1], mxcsr);
        dst.u64_[1] = 0;  // zero upper 64 bits
        return ExecResult::Continue;
    }

    // ========================================================================
    // CVTTPD2DQ: 2×f64 → 2×i32 in low 64 bits (truncation), zero upper 64
    // ========================================================================
    case Opcode::CVTTPD2DQ_XMM_XMMM128: {
        auto src = read_pack(ctx, src_op, aligned_form);
        if (!src) return simd_fault(ctx);
        auto& dst = get_xmm(ctx, dst_op.reg);
        dst.i32_[0] = cvt_f64_to_i32_trunc(src.value().f64_[0], mxcsr);
        dst.i32_[1] = cvt_f64_to_i32_trunc(src.value().f64_[1], mxcsr);
        dst.u64_[1] = 0;  // zero upper 64 bits
        return ExecResult::Continue;
    }

    // ========================================================================
    // CVTDQ2PD: 2×i32 from low 64 → 2×f64
    // ========================================================================
    case Opcode::CVTDQ2PD_XMM_XMMM64: {
        auto src = read_pack64(ctx, src_op);
        if (!src) return simd_fault(ctx);
        auto& dst = get_xmm(ctx, dst_op.reg);
        // Read the two i32 values before writing (src and dst may alias)
        i32 v0 = src.value().i32_[0];
        i32 v1 = src.value().i32_[1];
        dst.f64_[0] = static_cast<f64>(v0);
        dst.f64_[1] = static_cast<f64>(v1);
        return ExecResult::Continue;
    }

    // ========================================================================
    // CVTPS2PD: 2×f32 from low 64 → 2×f64
    // ========================================================================
    case Opcode::CVTPS2PD_XMM_XMMM64: {
        auto src = read_pack64(ctx, src_op);
        if (!src) return simd_fault(ctx);
        auto& dst = get_xmm(ctx, dst_op.reg);
        // Read the two f32 values before writing (src and dst may alias)
        f32 v0 = src.value().f32_[0];
        f32 v1 = src.value().f32_[1];
        dst.f64_[0] = static_cast<f64>(v0);
        dst.f64_[1] = static_cast<f64>(v1);
        return ExecResult::Continue;
    }

    // ========================================================================
    // CVTPD2PS: 2×f64 → 2×f32 in low 64, zero upper 64
    // ========================================================================
    case Opcode::CVTPD2PS_XMM_XMMM128: {
        auto src = read_pack(ctx, src_op, aligned_form);
        if (!src) return simd_fault(ctx);
        auto& dst = get_xmm(ctx, dst_op.reg);
        const f32 n0 = static_cast<f32>(src.value().f64_[0]);
        const f32 n1 = static_cast<f32>(src.value().f64_[1]);
        sse_cvt_narrow_pe(src.value().f64_[0], n0, mxcsr);
        sse_cvt_narrow_pe(src.value().f64_[1], n1, mxcsr);
        dst.f32_[0] = n0;
        dst.f32_[1] = n1;
        dst.u64_[1] = 0;  // zero upper 64 bits
        return ExecResult::Continue;
    }

    // ========================================================================
    // CVTPI2PS: 2×i32 from MM → 2×f32 in low 64 of XMM, upper 64 unchanged
    // ========================================================================
    case Opcode::CVTPI2PS_XMM_MMM64: {
        auto src = read_mmm64(ctx, src_op);
        if (!src) return simd_fault(ctx);
        i32 v0 = static_cast<i32>(static_cast<u32>(src.value()));
        i32 v1 = static_cast<i32>(static_cast<u32>(src.value() >> 32));
        auto& dst = get_xmm(ctx, dst_op.reg);
        dst.f32_[0] = static_cast<f32>(v0);
        dst.f32_[1] = static_cast<f32>(v1);
        return ExecResult::Continue;
    }

    // ========================================================================
    // CVTPI2PD: 2×i32 from MM → 2×f64 (writes all 128 bits)
    // ========================================================================
    case Opcode::CVTPI2PD_XMM_MMM64: {
        auto src = read_mmm64(ctx, src_op);
        if (!src) return simd_fault(ctx);
        i32 v0 = static_cast<i32>(static_cast<u32>(src.value()));
        i32 v1 = static_cast<i32>(static_cast<u32>(src.value() >> 32));
        auto& dst = get_xmm(ctx, dst_op.reg);
        dst.f64_[0] = static_cast<f64>(v0);
        dst.f64_[1] = static_cast<f64>(v1);
        return ExecResult::Continue;
    }

    // ========================================================================
    // CVTPS2PI / CVTTPS2PI: 2×f32 from low 64 of XMM/m64 → 2×i32 in MM
    // ========================================================================
    case Opcode::CVTPS2PI_MM_XMMM64:
    case Opcode::CVTTPS2PI_MM_XMMM64: {
        auto src = read_pack64(ctx, src_op);
        if (!src) return simd_fault(ctx);
        const bool truncate = instr.opcode == Opcode::CVTTPS2PI_MM_XMMM64;
        i32 v0 = truncate ? cvt_f32_to_i32_trunc(src.value().f32_[0], mxcsr)
                          : cvt_f32_to_i32_round(src.value().f32_[0], mxcsr);
        i32 v1 = truncate ? cvt_f32_to_i32_trunc(src.value().f32_[1], mxcsr)
                          : cvt_f32_to_i32_round(src.value().f32_[1], mxcsr);
        write_mm(ctx, dst_op.reg, pack_i32_pair(v0, v1));
        return ExecResult::Continue;
    }

    // ========================================================================
    // CVTPD2PI / CVTTPD2PI: 2×f64 from XMM/m128 → 2×i32 in MM
    // ========================================================================
    case Opcode::CVTPD2PI_MM_XMMM128:
    case Opcode::CVTTPD2PI_MM_XMMM128: {
        auto src = read_pack(ctx, src_op, aligned_form);
        if (!src) return simd_fault(ctx);
        const bool truncate = instr.opcode == Opcode::CVTTPD2PI_MM_XMMM128;
        i32 v0 = truncate ? cvt_f64_to_i32_trunc(src.value().f64_[0], mxcsr)
                          : cvt_f64_to_i32_round(src.value().f64_[0], mxcsr);
        i32 v1 = truncate ? cvt_f64_to_i32_trunc(src.value().f64_[1], mxcsr)
                          : cvt_f64_to_i32_round(src.value().f64_[1], mxcsr);
        write_mm(ctx, dst_op.reg, pack_i32_pair(v0, v1));
        return ExecResult::Continue;
    }

    default:
        ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }
}

} // namespace vitreo::cpu::ops
