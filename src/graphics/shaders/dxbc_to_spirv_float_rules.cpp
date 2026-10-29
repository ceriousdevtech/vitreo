// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_float_rules.cpp
/// @brief Immediate and float-rule helpers.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

u32 DxbcTranslator::make_vec4_imm(float x, float y, float z, float w) {
    return m_builder.emit_constant_composite(
        m_type_vec4,
        {m_builder.emit_constant_float(m_type_float, x),
         m_builder.emit_constant_float(m_type_float, y),
         m_builder.emit_constant_float(m_type_float, z),
         m_builder.emit_constant_float(m_type_float, w)});
}

u32 DxbcTranslator::make_uvec4_imm(u32 x, u32 y, u32 z, u32 w) {
    return m_builder.emit_constant_composite(
        m_type_uvec4,
        {m_builder.emit_constant_int(m_type_uint, x),
         m_builder.emit_constant_int(m_type_uint, y),
         m_builder.emit_constant_int(m_type_uint, z),
         m_builder.emit_constant_int(m_type_uint, w)});
}

// D3D11.3 3.1.3.1: a comparison with a NaN operand is false, except `ne`,
// which is true. Metal's Relaxed math mode -- chosen for every shader fxc
// compiled with refactoringAllowed, i.e. its default -- compiles a float
// compare as if no operand were NaN: `x != x` (fxc's isnan(x)) folds to
// false, `x == x` and `x >= x` to true. isnan() itself is honoured there,
// for loaded and computed values alike (measured on an M4 Pro, Relaxed and
// Safe), so every NaN-observing lane of a compare is decided by it instead.
// An immediate without a NaN needs no test.
u32 DxbcTranslator::source_nan_lanes(const Sm4Operand& operand, u32 value) {
    if (operand.is_immediate || operand.type == Sm4OperandType::Immediate32) {
        bool any_nan = false;
        for (const float component : operand.immediate) {
            any_nan = any_nan || std::isnan(component);
        }
        if (!any_nan) return 0;
    }
    return m_builder.emit_is_nan(m_type_bvec4, value);
}

u32 DxbcTranslator::float_compare_result(Sm4Opcode opcode,
                                         const Sm4Operand& a, u32 va,
                                         const Sm4Operand& b, u32 vb) {
    u32 compare = 0;
    if (opcode == Sm4Opcode::NE) {
        compare = m_builder.emit_funord_ne(m_type_bvec4, va, vb);
    } else if (opcode == Sm4Opcode::EQ) {
        compare = m_builder.emit_ford_eq(m_type_bvec4, va, vb);
    } else if (opcode == Sm4Opcode::LT) {
        compare = m_builder.emit_ford_lt(m_type_bvec4, va, vb);
    } else {
        compare = m_builder.emit_ford_ge(m_type_bvec4, va, vb);
    }
    const u32 a_nan = source_nan_lanes(a, va);
    const u32 b_nan = source_nan_lanes(b, vb);
    u32 any_nan = a_nan != 0 ? a_nan : b_nan;
    if (a_nan != 0 && b_nan != 0) {
        any_nan = m_builder.emit_logical(SpvOp::OpLogicalOr, m_type_bvec4,
                                         a_nan, b_nan);
    }
    if (any_nan != 0) {
        compare = opcode == Sm4Opcode::NE
            ? m_builder.emit_logical(SpvOp::OpLogicalOr, m_type_bvec4,
                                     compare, any_nan)
            : m_builder.emit_logical(
                  SpvOp::OpLogicalAnd, m_type_bvec4, compare,
                  m_builder.emit_logical_not(m_type_bvec4, any_nan));
    }
    return m_builder.emit_select(m_type_uvec4, compare, m_const_u_ffff4,
                                 m_const_u_zero4);
}

// D3D11.3 3.1.3.2: denormals flush to sign-preserved zero at the input and
// output of every float operation; only raw mov/movc/swapc leave bits alone.
// Apple GPUs flush in arithmetic, but a negate/abs modifier or an x*1.0 that
// Metal folds away is a pure sign-bit operation that keeps the denormal.
u32 DxbcTranslator::flush_denormals(u32 value) {
    const u32 bits = m_builder.emit_bitcast(m_type_uvec4, value);
    const u32 exponent = m_builder.emit_bitwise_and(
        m_type_uvec4, bits,
        make_uvec4_imm(0x7F800000u, 0x7F800000u, 0x7F800000u, 0x7F800000u));
    const u32 denormal_or_zero = m_builder.emit_integer_compare(
        SpvOp::OpIEqual, m_type_bvec4, exponent, m_const_u_zero4);
    const u32 sign = m_builder.emit_bitwise_and(
        m_type_uvec4, bits,
        make_uvec4_imm(0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u));
    return m_builder.emit_bitcast(
        m_type_vec4,
        m_builder.emit_select(m_type_uvec4, denormal_or_zero, sign, bits));
}

// Whether @p operand is an immediate with a written lane of +-@p magnitude:
// the identity operands (x*1, x/1, x+0) Metal folds into a move. Unwritten
// lanes (fxc pads immediates with 0) do not count.
bool DxbcTranslator::immediate_has_magnitude(const Sm4Operand& operand,
                                             float magnitude,
                                             u32 write_mask) {
    if (!operand.is_immediate &&
        operand.type != Sm4OperandType::Immediate32) {
        return false;
    }
    const u32 mask = write_mask == 0u ? 0xFu : write_mask;
    for (u32 i = 0; i < 4u; ++i) {
        if ((mask & (1u << i)) != 0u &&
            std::fabs(operand.immediate[i]) == magnitude) {
            return true;
        }
    }
    return false;
}

// Lanes whose exponent is all ones: +-INF or NaN.
u32 DxbcTranslator::non_finite_lanes(u32 value) {
    const u32 exponent_mask =
        make_uvec4_imm(0x7F800000u, 0x7F800000u, 0x7F800000u, 0x7F800000u);
    const u32 exponent = m_builder.emit_bitwise_and(
        m_type_uvec4, m_builder.emit_bitcast(m_type_uvec4, value),
        exponent_mask);
    return m_builder.emit_integer_compare(SpvOp::OpIEqual, m_type_bvec4,
                                          exponent, exponent_mask);
}

// f32tof16 (22.13.2) under D3D11.3 3.2.2's conversion rules, on the bits:
// round toward zero; a finite value beyond the half range becomes the
// largest half (0x7BFF), "NOT including signed infinity"; INF stays INF,
// NaN stays NaN; a result below the half denormal range (and every float32
// denormal, flushed on input) is a sign-preserved zero. GLSL PackHalf2x16
// rounds to nearest and overflows to INF, which is what this replaces.
// Windows (WARP): NaN -> 0x7FFF with the sign, 65520 -> 0x7BFF,
// 0.99999994 -> 0x3BFF, 1e-7 -> 0x0001.
u32 DxbcTranslator::f32_to_f16_bits(u32 value) {
    const auto splat = [&](u32 v) { return make_uvec4_imm(v, v, v, v); };
    const u32 bits = m_builder.emit_bitcast(m_type_uvec4, value);
    const u32 sign = m_builder.emit_bitwise_and(
        m_type_uvec4,
        m_builder.emit_shift_right_logical(m_type_uvec4, bits, splat(16u)),
        splat(0x8000u));
    const u32 magnitude =
        m_builder.emit_bitwise_and(m_type_uvec4, bits, splat(0x7FFFFFFFu));
    const auto at_least = [&](u32 threshold) {
        return m_builder.emit_integer_compare(SpvOp::OpUGreaterThanEqual,
                                              m_type_bvec4, magnitude,
                                              splat(threshold));
    };
    // Half denormal: magnitude in [2^-24, 2^-14). The value in units of
    // 2^-24 is the implicit-1 mantissa shifted right by 126 - exponent
    // (14..23 in this range; masked so other lanes stay defined).
    const u32 exponent =
        m_builder.emit_shift_right_logical(m_type_uvec4, magnitude,
                                           splat(23u));
    const u32 shift = m_builder.emit_bitwise_and(
        m_type_uvec4, m_builder.emit_isub(m_type_uvec4, splat(126u), exponent),
        splat(31u));
    const u32 mantissa = m_builder.emit_bitwise_or(
        m_type_uvec4,
        m_builder.emit_bitwise_and(m_type_uvec4, magnitude,
                                   splat(0x007FFFFFu)),
        splat(0x00800000u));
    const u32 half_denormal =
        m_builder.emit_shift_right_logical(m_type_uvec4, mantissa, shift);
    // Half normal: rebias the exponent (127 -> 15) and drop 13 mantissa bits.
    const u32 half_normal = m_builder.emit_isub(
        m_type_uvec4,
        m_builder.emit_shift_right_logical(m_type_uvec4, magnitude,
                                           splat(13u)),
        splat(0x1C000u));
    u32 result = m_const_u_zero4;
    result = m_builder.emit_select(m_type_uvec4, at_least(0x33800000u),
                                   half_denormal, result);
    result = m_builder.emit_select(m_type_uvec4, at_least(0x38800000u),
                                   half_normal, result);
    result = m_builder.emit_select(m_type_uvec4, at_least(0x47800000u),
                                   splat(0x7BFFu), result);
    result = m_builder.emit_select(m_type_uvec4, at_least(0x7F800000u),
                                   splat(0x7C00u), result);
    result = m_builder.emit_select(
        m_type_uvec4,
        m_builder.emit_integer_compare(SpvOp::OpUGreaterThan, m_type_bvec4,
                                       magnitude, splat(0x7F800000u)),
        splat(0x7FFFu), result);
    return m_builder.emit_bitwise_or(m_type_uvec4, result, sign);
}

} // namespace vitreo::graphics::dxbc_spirv_detail
