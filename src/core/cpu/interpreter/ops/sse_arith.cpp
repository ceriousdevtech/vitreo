// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE arithmetic instruction execution (CPU-032)

#include "exec_ops.hpp"
#include "core/math/host_fenv.hpp"
#include "helpers.hpp"
#include "core/cpu/sse_float_semantics.hpp"
#include <cmath>
#include <cstring>
#include <bit>
#include <limits>

namespace vitreo::cpu::ops {

// ============================================================================
// SSE register / operand helpers
// ============================================================================

static XmmReg& get_xmm(ExecContext& ctx, u8 index) noexcept {
    return ctx.cpu.sse.xmm[index & 15];
}

/// Read scalar f32 from operand (register or memory)
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

/// Read scalar f64 from operand (register or memory)
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

/// Read packed 4×f32 from operand into XmmReg.
///
/// A 128-bit MEMORY operand must be 16-byte aligned, and that is a property of
/// the OPERAND rather than of the opcode -- so it is one guarded check here
/// rather than a decision per form. Measured on silicon,
/// `tools/oracle_probe/sse_align_probe.c` with its recorded output beside it:
/// every legacy 128-bit operand faults when misaligned and every narrower one
/// does not. The explicitly unaligned spellings (MOVUPS/MOVUPD/MOVDQU/LDDQU)
/// are copies and do not read through here.
///
/// `aligned_form` is false for VEX encodings, which are unaligned unless the
/// opcode says otherwise (Intel SDM Vol. 2). `ctx.vex_origin` is how that
/// survives `avx_sse_bridge`, which clears `prefixes.vex` before calling the
/// legacy handler.
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

/// Read packed 2×f64 from operand into XmmReg (same bytes, just aliased differently)
static Result<XmmReg, Error> read_pd(ExecContext& ctx, const Operand& op,
                                     bool aligned_form = true) noexcept {
    return read_ps(ctx, op, aligned_form);  // Same 128-bit read
}

// ============================================================================
// COMISS / COMISD / UCOMISS / UCOMISD flag helper
// ============================================================================

static void set_comiss_flags(ExecContext& ctx, f32 a, f32 b, bool signaling) noexcept {
    // COMISS/UCOMISS write ALL six arithmetic flags (Intel SDM Vol. 2), so
    // they are full-flag writers: any pending lazy FlagOp is dead and the
    // CF/OF overlays a partial writer (STC/CLC/CMC/BT/ROR) left behind must
    // not survive to be re-applied by the next materialize_lazy_flags.
    ctx.cpu.set_flag_op(CpuState::FlagOp::None);
    auto& f = ctx.cpu.flags;
    // Clear OF, SF, AF unconditionally
    f.set_of(false);
    f.set_sf(false);
    f.set_af(false);

    // Intel SDM Vol. 2 COMISS: "Invalid (if SNaN or QNaN operands)";
    // UCOMISS: "Invalid (if SNaN Operands)". Raised in the host accumulator,
    // where the JIT's FCMPE/FCMP put it (sse_raise_invalid).
    if ((signaling && (std::isnan(a) || std::isnan(b))) ||
        sse_is_snan(a) || sse_is_snan(b)) {
        sse_raise_invalid();
    }
    if (std::isnan(a) || std::isnan(b)) {
        // Unordered
        f.set_zf(true);
        f.set_pf(true);
        f.set_cf(true);
    } else if (a > b) {
        f.set_zf(false);
        f.set_pf(false);
        f.set_cf(false);
    } else if (a < b) {
        f.set_zf(false);
        f.set_pf(false);
        f.set_cf(true);
    } else {
        // Equal
        f.set_zf(true);
        f.set_pf(false);
        f.set_cf(false);
    }
}

static void set_comisd_flags(ExecContext& ctx, f64 a, f64 b, bool signaling) noexcept {
    // Full-flag writer — see set_comiss_flags.
    ctx.cpu.set_flag_op(CpuState::FlagOp::None);
    auto& f = ctx.cpu.flags;
    f.set_of(false);
    f.set_sf(false);
    f.set_af(false);

    // COMISD / UCOMISD: as COMISS / UCOMISS above.
    if ((signaling && (std::isnan(a) || std::isnan(b))) ||
        sse_is_snan(a) || sse_is_snan(b)) {
        sse_raise_invalid();
    }
    if (std::isnan(a) || std::isnan(b)) {
        f.set_zf(true);
        f.set_pf(true);
        f.set_cf(true);
    } else if (a > b) {
        f.set_zf(false);
        f.set_pf(false);
        f.set_cf(false);
    } else if (a < b) {
        f.set_zf(false);
        f.set_pf(false);
        f.set_cf(true);
    } else {
        f.set_zf(true);
        f.set_pf(false);
        f.set_cf(false);
    }
}

// ============================================================================
// Main SSE arithmetic dispatcher
// ============================================================================

namespace {
// Intel SDM Vol. 2 MINPS/MAXPS/MINPD/MAXPD; Vol. 1 10.2.3.3-4,
// 11.5.2.1-2, 11.5.3.1-3. These instructions SELECT an input: FTZ never
// applies, NaNs (including signaling payloads) are returned unchanged from
// SRC2, equal signed zeros take SRC2, and DAZ converts inputs before selection.
// Integer ordering avoids inheriting the host's FTZ or NaN arithmetic rules.
template<class F, class I>
I minmax_lane(I a, I b, bool minimum, u32 control, u32& raised) noexcept {
    constexpr I sign = std::bit_cast<I>(F{-0.0});
    constexpr I infinity = std::bit_cast<I>(std::numeric_limits<F>::infinity());
    constexpr I normal = std::bit_cast<I>(std::numeric_limits<F>::min());
    I am = a & ~sign, bm = b & ~sign;
    if (am > infinity || bm > infinity) {
        raised |= MXCSR_IE;
        // DAZ still applies to the returned SRC2 when SRC1 is a NaN.
        if ((control & MXCSR_DAZ) && bm != 0 && bm < normal) b &= sign;
        return b;
    }
    if (control & MXCSR_DAZ) {
        if (am != 0 && am < normal) { a &= sign; am = 0; }
        if (bm != 0 && bm < normal) { b &= sign; bm = 0; }
    } else if ((am != 0 && am < normal) || (bm != 0 && bm < normal)) {
        raised |= MXCSR_DE;
    }
    if (a == b || (am == 0 && bm == 0)) return b;
    const bool a_negative = (a & sign) != 0;
    const bool b_negative = (b & sign) != 0;
    const bool less = a_negative != b_negative ? a_negative
                                              : (a_negative ? am > bm : am < bm);
    return (minimum ? less : !less) ? a : b;
}
} // namespace

ExecResult exec_sse_minmax(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // The scalar forms are the same selection on lane 0 alone, with the same
    // SIMD exceptions, and every other lane of the destination taken from
    // SRC1 (Intel SDM Vol. 2 MINSS/MINSD/MAXSS/MAXSD). Measured on silicon,
    // flags, #XM record and upper lanes included:
    // tools/oracle_probe/sse_minmax_scalar_probe.
    const bool scalar = instr.opcode == Opcode::MINSS_XMM_XMMM32 ||
                        instr.opcode == Opcode::MINSD_XMM_XMMM64 ||
                        instr.opcode == Opcode::MAXSS_XMM_XMMM32 ||
                        instr.opcode == Opcode::MAXSD_XMM_XMMM64;
    const bool pd = instr.opcode == Opcode::MINPD_XMM_XMMM128 ||
                    instr.opcode == Opcode::MAXPD_XMM_XMMM128 ||
                    instr.opcode == Opcode::MINSD_XMM_XMMM64 ||
                    instr.opcode == Opcode::MAXSD_XMM_XMMM64;
    const bool minimum = instr.opcode == Opcode::MINPD_XMM_XMMM128 ||
                         instr.opcode == Opcode::MINPS_XMM_XMMM128 ||
                         instr.opcode == Opcode::MINSS_XMM_XMMM32 ||
                         instr.opcode == Opcode::MINSD_XMM_XMMM64;
    const bool vex = instr.prefixes.vex;
    // The scalar forms are VEX.LIG: L is ignored (opcodes.cpp,
    // vex_l_is_reserved; the decoder clears it), so they are never 256-bit.
    const bool wide = vex && instr.prefixes.vex_l && !scalar;
    const u8 dst = instr.operands[0].reg & 15;
    const u8 src1 = vex ? instr.prefixes.vex_vvvv & 15 : dst;
    const auto& operand = instr.operands[1];
    const usize halves = wide ? 2 : 1;
    XmmReg first[2]{ctx.cpu.sse.xmm[src1],ctx.cpu.avx.ymm_high[src1]};
    XmmReg second[2]{};
    if (is_register_operand(operand.type)) {
        const u8 src2 = operand.reg & 15;
        second[0] = ctx.cpu.sse.xmm[src2];
        second[1] = ctx.cpu.avx.ymm_high[src2];
    } else if (scalar) {
        // m32/m64: no alignment requirement in any encoding.
        auto address = calc_effective_address(ctx,operand.mem);
        if (!address) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        const usize bytes = pd ? sizeof(u64) : sizeof(u32);
        auto value = read_memory(ctx,address.value(),bytes);
        if (!value) {
            ctx.exception = CpuException::PageFault;
            ctx.exception_addr = address.value();
            // Native Windows reports the first inaccessible byte.
            for (usize byte=0;byte<bytes;++byte) {
                if (address.value() > std::numeric_limits<u64>::max()-byte ||
                    ctx.mem.read_u8(address.value()+byte).is_err()) {
                    ctx.exception_addr = address.value()+byte;
                    break;
                }
            }
            return ExecResult::Exception;
        }
        if (pd) second[0].u64_[0] = value.value();
        else second[0].u32_[0] = static_cast<u32>(value.value());
    } else {
        auto address = calc_effective_address(ctx,operand.mem);
        if (!address) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        if (!vex && !ctx.vex_origin && (address.value() & (sizeof(XmmReg)-1)) != 0) {
            ctx.exception = CpuException::GeneralProtection;
            // Native Windows #GP has no CR2 fault address.
            ctx.exception_addr = ctx.cpu.is_64bit() ? std::numeric_limits<u64>::max()
                                                   : std::numeric_limits<u32>::max();
            return ExecResult::Exception;
        }
        // Read the whole operand before touching flags or destination. The
        // common reader owns page faults and x86/x64 address translation.
        for (usize half=0;half<halves;++half) for (usize word=0;word<2;++word) {
            auto value = read_memory(ctx,address.value()+half*sizeof(XmmReg)+word*sizeof(u64),sizeof(u64));
            if (!value) {
                ctx.exception = CpuException::PageFault;
                const u64 start = address.value()+half*sizeof(XmmReg)+word*sizeof(u64);
                ctx.exception_addr = start;
                // Native Windows reports the first inaccessible byte, not
                // the start of a qword spanning a protection boundary.
                for (usize byte=0;byte<sizeof(u64);++byte) {
                    if (start > std::numeric_limits<u64>::max()-byte ||
                        ctx.mem.read_u8(start+byte).is_err()) {
                        ctx.exception_addr = start+byte;
                        break;
                    }
                }
                return ExecResult::Exception;
            }
            second[half].u64_[word] = value.value();
        }
    }
    XmmReg result[2]{};
    u32 raised = 0;
    if (scalar) {
        result[0] = first[0];
        if (pd) {
            result[0].u64_[0] = minmax_lane<f64,u64>(first[0].u64_[0],
                second[0].u64_[0],minimum,ctx.cpu.sse.mxcsr,raised);
        } else {
            result[0].u32_[0] = minmax_lane<f32,u32>(first[0].u32_[0],
                second[0].u32_[0],minimum,ctx.cpu.sse.mxcsr,raised);
        }
    }
    for (usize half=0;half<halves && !scalar;++half) {
        if (pd) {
            for (usize lane=0;lane<2;++lane)
                result[half].u64_[lane] = minmax_lane<f64,u64>(first[half].u64_[lane],
                    second[half].u64_[lane],minimum,ctx.cpu.sse.mxcsr,raised);
        } else {
            for (usize lane=0;lane<4;++lane)
                result[half].u32_[lane] = minmax_lane<f32,u32>(first[half].u32_[lane],
                    second[half].u32_[lane],minimum,ctx.cpu.sse.mxcsr,raised);
        }
    }
    ctx.cpu.sse.mxcsr |= raised;
    // Windows reports an unmasked MIN/MAX #XM as STATUS_FLOAT_INVALID_OPERATION
    // for both invalid and denormal conditions (native sse_minmax_probe).
    if (((raised & MXCSR_IE) && !(ctx.cpu.sse.mxcsr & MXCSR_IM)) ||
        ((raised & MXCSR_DE) && !(ctx.cpu.sse.mxcsr & MXCSR_DM))) {
        ctx.exception = CpuException::FloatingPointInvalidOperation;
        // Native Windows #XM carries {0, MXCSR}, measured on both ABIs.
        ctx.exception_num_params = 2;
        ctx.exception_error = 0;
        ctx.exception_addr = ctx.cpu.sse.mxcsr;
        return ExecResult::Exception;
    }
    ctx.cpu.sse.xmm[dst] = result[0];
    if (wide) ctx.cpu.avx.ymm_high[dst] = result[1];
    else if (vex) ctx.cpu.avx.ymm_high[dst].clear();
    return ExecResult::Continue;
}

ExecResult exec_sse_arith(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    switch (instr.opcode) {
    case Opcode::MINPS_XMM_XMMM128: case Opcode::MAXPS_XMM_XMMM128:
    case Opcode::MINPD_XMM_XMMM128: case Opcode::MAXPD_XMM_XMMM128:
    case Opcode::MINSS_XMM_XMMM32: case Opcode::MAXSS_XMM_XMMM32:
    case Opcode::MINSD_XMM_XMMM64: case Opcode::MAXSD_XMM_XMMM64:
        return exec_sse_minmax(ctx,instr);
    default: break;
    }
    // The SSE half of the same contract as exec_fpu: MXCSR.RC governs SSE
    // arithmetic, and it is independent of x87's FCW.RC. Installing it here is
    // what makes DIVSD/DIVSS/ADDSD/... actually honour the guest's rounding
    // mode instead of inheriting whatever x87 last asked for.
    math::apply_sse_rounding(ctx.cpu.sse.mxcsr);
    // VEX encodings carry no 16-byte alignment requirement; `ctx.vex_origin`
    // is how that survives `avx_sse_bridge` clearing `prefixes.vex`.
    const bool aligned_form = !instr.prefixes.vex && !ctx.vex_origin;
    const auto& dst_op = instr.operands[0];
    const auto& src_op = instr.operands[1];

    switch (instr.opcode) {

    // ========================================================================
    // ADDSS / ADDSD / ADDPS / ADDPD
    // ========================================================================
    case Opcode::ADDSS_XMM_XMMM32: {
        auto src = read_ss(ctx, src_op);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        { auto& d = get_xmm(ctx, dst_op.reg);
          d.f32_[0] = sse_add(d.f32_[0], src.value()); }
        return ExecResult::Continue;
    }
    case Opcode::ADDSD_XMM_XMMM64: {
        auto src = read_sd(ctx, src_op);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        { auto& d = get_xmm(ctx, dst_op.reg);
          d.f64_[0] = sse_add(d.f64_[0], src.value()); }
        return ExecResult::Continue;
    }
    case Opcode::ADDPS_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 4; ++i)
            dst.f32_[i] = sse_add(dst.f32_[i], src.value().f32_[i]);
        return ExecResult::Continue;
    }
    case Opcode::ADDPD_XMM_XMMM128: {
        auto src = read_pd(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 2; ++i)
            dst.f64_[i] = sse_add(dst.f64_[i], src.value().f64_[i]);
        return ExecResult::Continue;
    }

    // ========================================================================
    // SUBSS / SUBSD / SUBPS / SUBPD
    // ========================================================================
    case Opcode::SUBSS_XMM_XMMM32: {
        auto src = read_ss(ctx, src_op);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        { auto& d = get_xmm(ctx, dst_op.reg);
          d.f32_[0] = sse_sub(d.f32_[0], src.value()); }
        return ExecResult::Continue;
    }
    case Opcode::SUBSD_XMM_XMMM64: {
        auto src = read_sd(ctx, src_op);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        { auto& d = get_xmm(ctx, dst_op.reg);
          d.f64_[0] = sse_sub(d.f64_[0], src.value()); }
        return ExecResult::Continue;
    }
    case Opcode::SUBPS_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 4; ++i)
            dst.f32_[i] = sse_sub(dst.f32_[i], src.value().f32_[i]);
        return ExecResult::Continue;
    }
    case Opcode::SUBPD_XMM_XMMM128: {
        auto src = read_pd(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 2; ++i)
            dst.f64_[i] = sse_sub(dst.f64_[i], src.value().f64_[i]);
        return ExecResult::Continue;
    }

    // ========================================================================
    // MULSS / MULSD / MULPS / MULPD
    // ========================================================================
    case Opcode::MULSS_XMM_XMMM32: {
        auto src = read_ss(ctx, src_op);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        { auto& d = get_xmm(ctx, dst_op.reg);
          d.f32_[0] = sse_mul(d.f32_[0], src.value()); }
        return ExecResult::Continue;
    }
    case Opcode::MULSD_XMM_XMMM64: {
        auto src = read_sd(ctx, src_op);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        { auto& d = get_xmm(ctx, dst_op.reg);
          d.f64_[0] = sse_mul(d.f64_[0], src.value()); }
        return ExecResult::Continue;
    }
    case Opcode::MULPS_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 4; ++i)
            dst.f32_[i] = sse_mul(dst.f32_[i], src.value().f32_[i]);
        return ExecResult::Continue;
    }
    case Opcode::MULPD_XMM_XMMM128: {
        auto src = read_pd(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 2; ++i)
            dst.f64_[i] = sse_mul(dst.f64_[i], src.value().f64_[i]);
        return ExecResult::Continue;
    }

    // ========================================================================
    // DIVSS / DIVSD / DIVPS / DIVPD
    // ========================================================================
    case Opcode::DIVSS_XMM_XMMM32: {
        auto src = read_ss(ctx, src_op);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        { auto& d = get_xmm(ctx, dst_op.reg);
          d.f32_[0] = sse_div(d.f32_[0], src.value(), ctx.cpu.sse.mxcsr); }
        return ExecResult::Continue;
    }
    case Opcode::DIVSD_XMM_XMMM64: {
        auto src = read_sd(ctx, src_op);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        { auto& d = get_xmm(ctx, dst_op.reg);
          d.f64_[0] = sse_div(d.f64_[0], src.value(), ctx.cpu.sse.mxcsr); }
        return ExecResult::Continue;
    }
    case Opcode::DIVPS_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 4; ++i)
            dst.f32_[i] = sse_div(dst.f32_[i], src.value().f32_[i], ctx.cpu.sse.mxcsr);
        return ExecResult::Continue;
    }
    case Opcode::DIVPD_XMM_XMMM128: {
        auto src = read_pd(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 2; ++i)
            dst.f64_[i] = sse_div(dst.f64_[i], src.value().f64_[i], ctx.cpu.sse.mxcsr);
        return ExecResult::Continue;
    }

    // ========================================================================
    // SQRTSS / SQRTSD / SQRTPS / SQRTPD
    // x86: sqrt of a negative non-NaN input returns the "Floating-point
    // Indefinite" QNaN which has the sign bit set (0xffc00000 / 0xfff800...0).
    // std::sqrt may return a positive QNaN, so force the sign bit when the
    // input was negative.
    // ========================================================================
    case Opcode::SQRTSS_XMM_XMMM32: {
        auto src = read_ss(ctx, src_op);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        f32 v = src.value();
        f32 r = std::sqrt(v);
        if (std::isnan(r) && !std::isnan(v) && std::signbit(v)) {
            u32 bits = 0xffc00000u;
            std::memcpy(&r, &bits, 4);
        }
        get_xmm(ctx, dst_op.reg).f32_[0] = r;
        return ExecResult::Continue;
    }
    case Opcode::SQRTSD_XMM_XMMM64: {
        auto src = read_sd(ctx, src_op);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        f64 v = src.value();
        f64 r = std::sqrt(v);
        if (std::isnan(r) && !std::isnan(v) && std::signbit(v)) {
            u64 bits = 0xfff8000000000000ull;
            std::memcpy(&r, &bits, 8);
        }
        get_xmm(ctx, dst_op.reg).f64_[0] = r;
        return ExecResult::Continue;
    }
    case Opcode::SQRTPS_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 4; ++i) {
            f32 v = src.value().f32_[i];
            f32 r = std::sqrt(v);
            if (std::isnan(r) && !std::isnan(v) && std::signbit(v)) {
                u32 bits = 0xffc00000u;
                std::memcpy(&r, &bits, 4);
            }
            dst.f32_[i] = r;
        }
        return ExecResult::Continue;
    }
    case Opcode::SQRTPD_XMM_XMMM128: {
        auto src = read_pd(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 2; ++i) {
            f64 v = src.value().f64_[i];
            f64 r = std::sqrt(v);
            if (std::isnan(r) && !std::isnan(v) && std::signbit(v)) {
                u64 bits = 0xfff8000000000000ull;
                std::memcpy(&r, &bits, 8);
            }
            dst.f64_[i] = r;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // MAXSS / MAXSD / MAXPS / MAXPD
    // Intel SDM: if either operand is NaN, or if operands are equal (incl.
    // +0/-0 case), the second source operand is returned.
    // ========================================================================

    // ========================================================================
    // MINSS / MINSD / MINPS / MINPD
    // ========================================================================

    // ========================================================================
    // SSE3: ADDSUBPD / ADDSUBPS / HADDPD / HADDPS / HSUBPD / HSUBPS
    // ========================================================================
    case Opcode::ADDSUBPD_XMM_XMMM128: {
        auto src = read_pd(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        // Each lane is an ordinary SSE add/subtract, so it takes the x86 NaN
        // rules (Intel SDM Vol. 1 §4.8.3.4): plain C++ operators would give
        // ARM's NaN selection and ARM's sign-clear default NaN.
        dst.f64_[0] = sse_sub(dst.f64_[0], src.value().f64_[0]);
        dst.f64_[1] = sse_add(dst.f64_[1], src.value().f64_[1]);
        return ExecResult::Continue;
    }
    case Opcode::ADDSUBPS_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        dst.f32_[0] = sse_sub(dst.f32_[0], src.value().f32_[0]);
        dst.f32_[1] = sse_add(dst.f32_[1], src.value().f32_[1]);
        dst.f32_[2] = sse_sub(dst.f32_[2], src.value().f32_[2]);
        dst.f32_[3] = sse_add(dst.f32_[3], src.value().f32_[3]);
        return ExecResult::Continue;
    }
    case Opcode::HADDPD_XMM_XMMM128: {
        auto src = read_pd(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        // Intel SDM Vol. 2 HADDPD: each lane is an ordinary SSE addition of an
        // adjacent pair (the lower element is the first operand), so it
        // takes the x86 NaN rules and invalid-operation signalling of Vol. 1
        // Table 4-7 -- plain C++ operators gave ARM's NaN selection, and the
        // optimiser may swap their operands.
        f64 d0 = dst.f64_[0], d1 = dst.f64_[1];
        f64 s0 = src.value().f64_[0], s1 = src.value().f64_[1];
        dst.f64_[0] = sse_add(d0, d1);
        dst.f64_[1] = sse_add(s0, s1);
        return ExecResult::Continue;
    }
    case Opcode::HADDPS_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        f32 d0 = dst.f32_[0], d1 = dst.f32_[1], d2 = dst.f32_[2], d3 = dst.f32_[3];
        f32 s0 = src.value().f32_[0], s1 = src.value().f32_[1];
        f32 s2 = src.value().f32_[2], s3 = src.value().f32_[3];
        dst.f32_[0] = sse_add(d0, d1);
        dst.f32_[1] = sse_add(d2, d3);
        dst.f32_[2] = sse_add(s0, s1);
        dst.f32_[3] = sse_add(s2, s3);
        return ExecResult::Continue;
    }
    case Opcode::HSUBPD_XMM_XMMM128: {
        auto src = read_pd(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        // Intel SDM Vol. 2 HSUBPD: lower element minus upper, x86 NaN rules.
        f64 d0 = dst.f64_[0], d1 = dst.f64_[1];
        f64 s0 = src.value().f64_[0], s1 = src.value().f64_[1];
        dst.f64_[0] = sse_sub(d0, d1);
        dst.f64_[1] = sse_sub(s0, s1);
        return ExecResult::Continue;
    }
    case Opcode::HSUBPS_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        f32 d0 = dst.f32_[0], d1 = dst.f32_[1], d2 = dst.f32_[2], d3 = dst.f32_[3];
        f32 s0 = src.value().f32_[0], s1 = src.value().f32_[1];
        f32 s2 = src.value().f32_[2], s3 = src.value().f32_[3];
        dst.f32_[0] = sse_sub(d0, d1);
        dst.f32_[1] = sse_sub(d2, d3);
        dst.f32_[2] = sse_sub(s0, s1);
        dst.f32_[3] = sse_sub(s2, s3);
        return ExecResult::Continue;
    }

    // ========================================================================
    // COMISS / COMISD
    // ========================================================================
    case Opcode::COMISS_XMM_XMMM32: {
        auto src = read_ss(ctx, src_op);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        f32 a = get_xmm(ctx, dst_op.reg).f32_[0];
        set_comiss_flags(ctx, a, src.value(), /*signaling=*/true);
        return ExecResult::Continue;
    }
    case Opcode::COMISD_XMM_XMMM64: {
        auto src = read_sd(ctx, src_op);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        f64 a = get_xmm(ctx, dst_op.reg).f64_[0];
        set_comisd_flags(ctx, a, src.value(), /*signaling=*/true);
        return ExecResult::Continue;
    }

    // ========================================================================
    // UCOMISS / UCOMISD  (same flag behavior, just no #IA for QNaN)
    // ========================================================================
    case Opcode::UCOMISS_XMM_XMMM32: {
        auto src = read_ss(ctx, src_op);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        f32 a = get_xmm(ctx, dst_op.reg).f32_[0];
        set_comiss_flags(ctx, a, src.value(), /*signaling=*/false);
        return ExecResult::Continue;
    }
    case Opcode::UCOMISD_XMM_XMMM64: {
        auto src = read_sd(ctx, src_op);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        f64 a = get_xmm(ctx, dst_op.reg).f64_[0];
        set_comisd_flags(ctx, a, src.value(), /*signaling=*/false);
        return ExecResult::Continue;
    }

    // ========================================================================
    // RCPSS / RCPPS  (reciprocal approximation)
    // Intel SDM: max relative error 1.5 * 2^-12. Exact reciprocal is within
    // tolerance; oracle comparisons for these ops use tolerance matching.
    // ========================================================================
    case Opcode::RCPSS_XMM_XMMM32: {
        auto src = read_ss(ctx, src_op);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        get_xmm(ctx, dst_op.reg).f32_[0] = 1.0f / src.value();
        return ExecResult::Continue;
    }
    case Opcode::RCPPS_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 4; ++i) dst.f32_[i] = 1.0f / src.value().f32_[i];
        return ExecResult::Continue;
    }

    // ========================================================================
    // RSQRTSS / RSQRTPS  (reciprocal square root approximation)
    // ========================================================================
    case Opcode::RSQRTSS_XMM_XMMM32: {
        auto src = read_ss(ctx, src_op);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        get_xmm(ctx, dst_op.reg).f32_[0] = 1.0f / std::sqrt(src.value());
        return ExecResult::Continue;
    }
    case Opcode::RSQRTPS_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) { ctx.exception = CpuException::SimdFloatingPoint; return ExecResult::Exception; }
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 4; ++i) dst.f32_[i] = 1.0f / std::sqrt(src.value().f32_[i]);
        return ExecResult::Continue;
    }

    default:
        ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }
}

} // namespace vitreo::cpu::ops
