// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file sse_float_semantics.hpp
/// @brief x86 NaN semantics for SSE binary floating-point arithmetic.
///
/// Shared by the interpreter (`interpreter/ops/sse_arith.cpp`) and the JIT's
/// SSE helper (`jit/jit_helpers.cpp`). The interpreter is the CPU semantic
/// reference and the JIT must match it, so this contract lives in one place
/// rather than being implemented twice — a second copy is a second chance to
/// get the operand ordering wrong.
///
/// Intel SDM Vol. 1 §4.8.3.4 ("Operating on NaNs"), Table 4-7, with exceptions
/// masked (MXCSR default 0x1F80):
///
///   src1 (destination) is NaN -> src1, quietened only if it is an SNaN
///   else src2 (source)  is NaN -> src2, quietened only if it is an SNaN
///   else the op generates a NaN (0/0, inf/inf, inf-inf, 0*inf)
///                              -> the "Indefinite" QNaN, whose SIGN IS SET
///   else                       -> the arithmetic result
///
/// Two things here cannot be delegated to the host:
///
/// 1. **Operand order beats NaN kind.** A QNaN destination wins over an SNaN
///    source. An implementation that checks SNaN first still passes every
///    single-NaN test and diverges only when both operands are NaN of
///    different kinds.
/// 2. **Plain C++ operators are not safe.** Writing `dst *= src` lets the
///    optimiser emit ARM64 `fmul` with the operands in either order, since
///    multiplication is commutative to the optimiser — but ARM's NaN selection
///    is order-dependent, so a reassociated multiply silently returns the
///    wrong NaN. ARM's default NaN also has its sign clear where x86's
///    Indefinite QNaN has it set.
///
/// Both showed up on MULSS/DIVSS, not in a unit test.

#pragma once

#include <vitreo/types.hpp>

#include <bit>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace vitreo::cpu {

template <typename F> inline bool sse_is_snan(F v) noexcept;

template <> inline bool sse_is_snan<f32>(f32 v) noexcept {
    u32 b;
    std::memcpy(&b, &v, sizeof(b));
    // Exponent all ones, mantissa non-zero, mantissa MSB clear.
    return (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu) != 0
        && (b & 0x00400000u) == 0;
}

template <> inline bool sse_is_snan<f64>(f64 v) noexcept {
    u64 b;
    std::memcpy(&b, &v, sizeof(b));
    return (b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull
        && (b & 0x000FFFFFFFFFFFFFull) != 0
        && (b & 0x0008000000000000ull) == 0;
}

/// Quieten an SNaN by setting the mantissa MSB, preserving sign and payload.
inline f32 sse_quieten(f32 v) noexcept {
    u32 b;
    std::memcpy(&b, &v, sizeof(b));
    b |= 0x00400000u;
    f32 out;
    std::memcpy(&out, &b, sizeof(out));
    return out;
}

inline f64 sse_quieten(f64 v) noexcept {
    u64 b;
    std::memcpy(&b, &v, sizeof(b));
    b |= 0x0008000000000000ull;
    f64 out;
    std::memcpy(&out, &b, sizeof(out));
    return out;
}

/// x86 "Floating-Point Indefinite": the default QNaN with the sign bit set.
template <typename F> inline F sse_indefinite_qnan() noexcept;

template <> inline f32 sse_indefinite_qnan<f32>() noexcept {
    const u32 b = 0xFFC00000u;
    f32 out;
    std::memcpy(&out, &b, sizeof(out));
    return out;
}

template <> inline f64 sse_indefinite_qnan<f64>() noexcept {
    const u64 b = 0xFFF8000000000000ull;
    f64 out;
    std::memcpy(&out, &b, sizeof(out));
    return out;
}

/// MXCSR.IE (bit 0): the invalid-operation sticky flag.
inline constexpr u32 kSseMxcsrIE = 1u << 0;

/// Signal SSE invalid operation (#IA, masked) through the HOST sticky
/// accumulator, the channel both engines use for IE on SSE arithmetic and
/// compares: the JIT's native FADD/FMUL/FCMPE raise FPSR.IOC as a side
/// effect, and math::harvest_sse_exceptions folds it into MXCSR.IE wherever
/// the guest observes MXCSR (STMXCSR, FXSAVE/XSAVE, _statusfp). Writing IE
/// into CpuState.sse.mxcsr instead would put the same flag in a different
/// place from the JIT's, which every raw-MXCSR parity test then reports.
///
/// It is raised explicitly because the side effect of a host operation is not
/// enough: an SNaN SRC2 behind a QNaN SRC1 is never operated on, and a quiet
/// host compare (std::isnan) raises nothing for a QNaN.
inline void sse_raise_invalid() noexcept {
    (void)std::feraiseexcept(FE_INVALID);
}

/// Intel SDM Vol. 2 CMPPD/CMPPS Table 3-1, "Signals #IA on QNAN", rows
/// 00H-1FH (EQ_OQ, LT_OS, LE_OS, UNORD_Q, NEQ_UQ, NLT_US, NLE_US, ORD_Q,
/// EQ_UQ, NGE_US, NGT_US, FALSE_OQ, NEQ_OQ, GE_OS, GT_OS, TRUE_UQ, EQ_OS,
/// LT_OQ, LE_OQ, UNORD_S, NEQ_US, NLT_UQ, NLE_UQ, ORD_S, EQ_US, NGE_UQ,
/// NGT_UQ, FALSE_OS, NEQ_OS, GE_OQ, GT_OQ, TRUE_US): 10H-1FH are 00H-0FH with
/// the signalling flipped. Every predicate signals for an SNaN operand
/// ("Invalid if SNaN operand and invalid if QNaN and predicate as listed in
/// Table 3-1"). The scalar forms CMPSS/CMPSD define the same 32 predicates,
/// over element 0 only. Shared by the interpreter (sse_cmp.cpp) and tier 1
/// (emit_fp_cmp_predicate).
inline constexpr bool kSseCmpSignalsOnQnan[32] = {
    false, true,  true,  false, false, true,  true,  false,
    false, true,  true,  false, false, true,  true,  false,
    true,  false, false, true,  true,  false, false, true,
    true,  false, false, true,  true,  false, false, true,
};

/// Intel SDM Vol. 1 §4.8.3.4 / §4.9.1.2 / §11.5.2.1 (#IA): an SNaN source
/// operand of an SSE arithmetic instruction signals invalid operation,
/// WHICHEVER operand it is -- including when SRC1 is a QNaN and so decides
/// the result.
template <typename F> inline void sse_signal_snan(F a, F b) noexcept {
    if (sse_is_snan(a) || sse_is_snan(b)) sse_raise_invalid();
}

/// Apply one binary arithmetic op with x86 NaN-propagation semantics.
/// `a` is src1/destination, `b` is src2/source. Order matters — see above.
/// Invalid operation is signalled for an SNaN operand and for an invalid
/// operation (the op itself producing a NaN: inf - inf, 0 * inf), which
/// returns the indefinite.
template <typename F, typename Op>
inline F sse_binop(F a, F b, Op op) noexcept {
    sse_signal_snan(a, b);
    if (std::isnan(a)) return sse_is_snan(a) ? sse_quieten(a) : a;
    if (std::isnan(b)) return sse_is_snan(b) ? sse_quieten(b) : b;
    const F r = op(a, b);
    if (std::isnan(r)) {
        sse_raise_invalid();
        return sse_indefinite_qnan<F>();
    }
    return r;
}

template <typename F> inline F sse_add(F a, F b) noexcept {
    return sse_binop(a, b, [](F x, F y) { return x + y; });
}
template <typename F> inline F sse_sub(F a, F b) noexcept {
    return sse_binop(a, b, [](F x, F y) { return x - y; });
}
template <typename F> inline F sse_mul(F a, F b) noexcept {
    return sse_binop(a, b, [](F x, F y) { return x * y; });
}

/// Intel SDM Vol. 1 §11.5.2.6: sticky PE when the quotient is not exact.
/// Residual `a - r*b` via FMA is 0 iff the rounded divide was exact.
/// Finite / ±0 sets ZE (Vol. 1 §4.9.1.3); 0/0 and inf/inf are invalid
/// operations and yield the indefinite; an SNaN operand is invalid too.
/// Invalid is signalled through sse_raise_invalid, as for sse_binop.
template <typename F> inline F sse_div(F a, F b, u32& mxcsr) noexcept {
    sse_signal_snan(a, b);
    if (std::isnan(a)) return sse_is_snan(a) ? sse_quieten(a) : a;
    if (std::isnan(b)) return sse_is_snan(b) ? sse_quieten(b) : b;
    // Classified on the bits, so the host's flush mode cannot change it.
    // MXCSR.DAZ (bit 6) makes a denormal dividend a zero (Intel SDM Vol. 1
    // §10.2.3.4): denormal / 0 is 0 / 0, invalid and not divide-by-zero. A
    // denormal DIVISOR under DAZ divides by zero in the host FDIV itself,
    // which raises the flag the MXCSR harvest reports.
    {
        using Bits = std::conditional_t<sizeof(F) == 4, u32, u64>;
        constexpr Bits magnitude = ~std::bit_cast<Bits>(F(-0.0));
        constexpr Bits infinity =
            std::bit_cast<Bits>(std::numeric_limits<F>::infinity());
        constexpr Bits normal = std::bit_cast<Bits>(std::numeric_limits<F>::min());
        const Bits am = std::bit_cast<Bits>(a) & magnitude;
        const Bits bm = std::bit_cast<Bits>(b) & magnitude;
        const bool a_zero = am == 0 || ((mxcsr & (1u << 6)) != 0 && am < normal);
        if (bm == 0 && am < infinity && !a_zero) {
            mxcsr |= (1u << 2);  // MXCSR_ZE
        }
    }
    const F r = a / b;
    if (std::isnan(r)) {
        sse_raise_invalid();
        return sse_indefinite_qnan<F>();
    }
    if (std::isfinite(a) && std::isfinite(b) && b != F(0) && std::isfinite(r) &&
        std::fma(-r, b, a) != F(0)) {
        mxcsr |= (1u << 5);  // MXCSR_PE
    }
    return r;
}

template <typename F> inline F sse_div(F a, F b) noexcept {
    u32 unused = 0;
    return sse_div(a, b, unused);
}

/// Intel SDM Vol. 2 MINPS/MINSS/MINPD/MINSD: if either lane is NaN, or dest
/// is not strictly less than src, return src. ARM FMIN / std::fmin return
/// NaN when either input is NaN and would poison an AABB walk.
template <typename F> inline F sse_min(F dest, F src) noexcept {
    return (std::isnan(dest) || std::isnan(src) || !(dest < src)) ? src : dest;
}

/// Intel SDM Vol. 2 MAXPS/MAXSS/MAXPD/MAXSD: NaN or dest not strictly greater
/// → src. Same contract as MIN* with the compare flipped.
template <typename F> inline F sse_max(F dest, F src) noexcept {
    return (std::isnan(dest) || std::isnan(src) || !(dest > src)) ? src : dest;
}

/// MXCSR.DAZ applied to an input that an op returns by SELECTION rather than
/// arithmetic. The host FPU's input flush (FPCR.FIZ/FZ, installed from MXCSR
/// by math::apply_sse_rounding) reaches a comparison but not the raw bits a
/// select hands back, and silicon returns a DAZ-flushed denormal as signed
/// zero (VEX sweep: MINPS 0x80000001 -> 0x80000000). MIN/MAX never
/// underflow, so FTZ alone leaves them unchanged.
template <typename F> inline F sse_daz_in(F v, u32 mxcsr) noexcept {
    if ((mxcsr & 0x0040u) != 0 && std::fpclassify(v) == FP_SUBNORMAL) {
        return std::copysign(F{0}, v);
    }
    return v;
}

/// MIN*/MAX* with MXCSR.DAZ honoured, and Intel SDM Vol. 2 MINPS/MAXPS's
/// "Invalid (including QNaN Source Operand)": any NaN operand sets IE. This
/// one is written into MXCSR directly, the channel the interpreter's own
/// MIN/MAX (exec_sse_minmax) uses: they select with integer compares, so no
/// host FP operation signals for them.
template <typename F> inline F sse_min(F dest, F src, u32& mxcsr) noexcept {
    if (std::isnan(dest) || std::isnan(src)) mxcsr |= kSseMxcsrIE;
    return sse_min(sse_daz_in(dest, mxcsr), sse_daz_in(src, mxcsr));
}
template <typename F> inline F sse_max(F dest, F src, u32& mxcsr) noexcept {
    if (std::isnan(dest) || std::isnan(src)) mxcsr |= kSseMxcsrIE;
    return sse_max(sse_daz_in(dest, mxcsr), sse_daz_in(src, mxcsr));
}

/// Intel SDM Vol. 2 CVT*2SI / CVT*2DQ: Precision when a finite in-range
/// conversion is inexact. Invalid (NaN / overflow → indefinite) is not PE.
template <typename F, typename I>
inline void sse_cvt_int_pe(F src, I dst, u32& mxcsr) noexcept {
    if (std::isfinite(src) && static_cast<F>(dst) != src) {
        mxcsr |= (1u << 5);  // MXCSR_PE
    }
}

/// Intel SDM Vol. 2 CVTSD2SS / CVTPD2PS: PE when the narrow store is inexact.
template <typename Wide, typename Narrow>
inline void sse_cvt_narrow_pe(Wide src, Narrow dst, u32& mxcsr) noexcept {
    if (std::isfinite(src) && static_cast<Wide>(dst) != src) {
        mxcsr |= (1u << 5);
    }
}

/// Intel SDM Vol. 2 CVTSI2SS / CVTDQ2PS: PE when the integer is not exact in f32.
template <typename I>
inline void sse_cvt_int_to_f32_pe(I src, f32 dst, u32& mxcsr) noexcept {
    if (static_cast<I>(dst) != src) {
        mxcsr |= (1u << 5);
    }
}

}  // namespace vitreo::cpu
