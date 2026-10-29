// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_op_integer.cpp
/// @brief Integer arithmetic opcodes.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

bool DxbcTranslator::translate_integer_operation(u32 /*opcode_token*/,
                                                 Sm4Opcode opcode,
                                                 u32 /*sample_controls*/) {
    switch (opcode) {
    case Sm4Opcode::IADD: {
        // Microsoft Learn iadd (sm4): component-wise 32-bit addition with no
        // carry beyond each component. Optional source negate is two's
        // complement, so signedness does not affect the result bits.
        Sm4Operand dst, a, b;
        if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(a) ||
            !m_decoder.decode_operand(b)) {
            return fail("IADD: bad operands");
        }
        if (a.modifier > 1u || b.modifier > 1u) {
            return fail("IADD: unsupported source modifier");
        }
        u32 va = 0, vb = 0;
        if (!load_vec4(a, va) || !load_vec4(b, vb)) return false;
        va = m_builder.emit_bitcast(m_type_ivec4, va);
        vb = m_builder.emit_bitcast(m_type_ivec4, vb);
        if (a.modifier == 1u) {
            va = m_builder.emit_snegate(m_type_ivec4, va);
        }
        if (b.modifier == 1u) {
            vb = m_builder.emit_snegate(m_type_ivec4, vb);
        }
        const u32 sum = m_builder.emit_iadd(m_type_ivec4, va, vb);
        if (!store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, sum))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::INEG: {
        // Microsoft Learn ineg (sm4): component-wise two's complement of
        // each 32-bit source value. The source syntax has no modifier.
        Sm4Operand dst, src;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(src)) {
            return fail("INEG: bad operands");
        }
        if (src.modifier != 0u) {
            return fail("INEG: unsupported source modifier");
        }
        u32 value = 0;
        if (!load_vec4(src, value)) return false;
        value = m_builder.emit_bitcast(m_type_ivec4, value);
        value = m_builder.emit_snegate(m_type_ivec4, value);
        if (!store_vec4(dst,
                        m_builder.emit_bitcast(m_type_vec4, value))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::UDIV: {
        // Microsoft Learn udiv (sm4): component-wise unsigned quotient and
        // remainder. Either destination may be NULL. Divide by zero returns
        // 0xffffffff for both; sanitize before SPIR-V's undefined zero divide.
        Sm4Operand dst_quotient, dst_remainder, dividend, divisor;
        if (!m_decoder.decode_operand(dst_quotient) ||
            !m_decoder.decode_operand(dst_remainder) ||
            !m_decoder.decode_operand(dividend) ||
            !m_decoder.decode_operand(divisor)) {
            return fail("UDIV: bad operands");
        }
        if (dividend.modifier != 0u || divisor.modifier != 0u) {
            return fail("UDIV: unsupported source modifier");
        }
        u32 numerator = 0, denominator = 0;
        if (!load_vec4(dividend, numerator) ||
            !load_vec4(divisor, denominator)) {
            return false;
        }
        numerator = m_builder.emit_bitcast(m_type_uvec4, numerator);
        denominator = m_builder.emit_bitcast(m_type_uvec4, denominator);
        const u32 zero_divisor = m_builder.emit_integer_compare(
            SpvOp::OpIEqual, m_type_bvec4, denominator, m_const_u_zero4);
        const u32 uint_one = m_builder.emit_constant_int(m_type_uint, 1u);
        const u32 uint_one4 = m_builder.emit_constant_composite(
            m_type_uvec4, {uint_one, uint_one, uint_one, uint_one});
        const u32 safe_denominator = m_builder.emit_select(
            m_type_uvec4, zero_divisor, uint_one4, denominator);
        u32 quotient = m_builder.emit_udiv(
            m_type_uvec4, numerator, safe_denominator);
        u32 remainder = m_builder.emit_umod(
            m_type_uvec4, numerator, safe_denominator);
        quotient = m_builder.emit_select(
            m_type_uvec4, zero_divisor, m_const_u_ffff4, quotient);
        remainder = m_builder.emit_select(
            m_type_uvec4, zero_divisor, m_const_u_ffff4, remainder);
        if (dst_quotient.type != Sm4OperandType::Null &&
            !store_vec4(dst_quotient,
                        m_builder.emit_bitcast(m_type_vec4, quotient))) {
            return false;
        }
        if (dst_remainder.type != Sm4OperandType::Null &&
            !store_vec4(dst_remainder,
                        m_builder.emit_bitcast(m_type_vec4, remainder))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::UADDC:
    case Sm4Opcode::USUBB: {
        // D3D11.3 22.12.8 / 22.12.14: dest0 = low 32 bits of src0 + src1
        // (src0 - src1); dest1 = 1 where the add carried (the subtract
        // borrowed), else 0. Either destination may be NULL. Both results
        // are computed before either store, since dest0 may alias a source.
        Sm4Operand dst_value, dst_flag, a, b;
        if (!m_decoder.decode_operand(dst_value) ||
            !m_decoder.decode_operand(dst_flag) ||
            !m_decoder.decode_operand(a) || !m_decoder.decode_operand(b)) {
            return fail("UADDC/USUBB: bad operands");
        }
        if (a.modifier != 0u || b.modifier != 0u) {
            return fail("UADDC/USUBB: unsupported source modifier");
        }
        u32 va = 0, vb = 0;
        if (!load_vec4(a, va) || !load_vec4(b, vb)) return false;
        va = m_builder.emit_bitcast(m_type_uvec4, va);
        vb = m_builder.emit_bitcast(m_type_uvec4, vb);
        const bool add = opcode == Sm4Opcode::UADDC;
        const u32 value = add ? m_builder.emit_iadd(m_type_uvec4, va, vb)
                              : m_builder.emit_isub(m_type_uvec4, va, vb);
        // A carry out of an add leaves the sum below either addend; a
        // borrow happens exactly when src0 < src1.
        const u32 flagged = add
            ? m_builder.emit_integer_compare(SpvOp::OpULessThan, m_type_bvec4,
                                             value, va)
            : m_builder.emit_integer_compare(SpvOp::OpULessThan, m_type_bvec4,
                                             va, vb);
        const u32 flag = m_builder.emit_select(
            m_type_uvec4, flagged, make_uvec4_imm(1u, 1u, 1u, 1u),
            m_const_u_zero4);
        if (dst_value.type != Sm4OperandType::Null &&
            !store_vec4(dst_value, m_builder.emit_bitcast(m_type_vec4, value))) {
            return false;
        }
        if (dst_flag.type != Sm4OperandType::Null &&
            !store_vec4(dst_flag, m_builder.emit_bitcast(m_type_vec4, flag))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::IMAD: {
        // Microsoft Learn imad (sm4): signed component-wise multiplication,
        // retaining the low 32 product bits, followed by a wrapping iadd.
        Sm4Operand dst, a, b, c;
        if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(a) ||
            !m_decoder.decode_operand(b) || !m_decoder.decode_operand(c)) {
            return fail("IMAD: bad operands");
        }
        if (a.modifier > 1u || b.modifier > 1u || c.modifier > 1u) {
            return fail("IMAD: unsupported source modifier");
        }
        u32 va = 0, vb = 0, vc = 0;
        if (!load_vec4(a, va) || !load_vec4(b, vb) ||
            !load_vec4(c, vc)) {
            return false;
        }
        va = m_builder.emit_bitcast(m_type_ivec4, va);
        vb = m_builder.emit_bitcast(m_type_ivec4, vb);
        vc = m_builder.emit_bitcast(m_type_ivec4, vc);
        if (a.modifier == 1u) va = m_builder.emit_snegate(m_type_ivec4, va);
        if (b.modifier == 1u) vb = m_builder.emit_snegate(m_type_ivec4, vb);
        if (c.modifier == 1u) vc = m_builder.emit_snegate(m_type_ivec4, vc);
        const u32 product = m_builder.emit_imul(m_type_ivec4, va, vb);
        const u32 result = m_builder.emit_iadd(
            m_type_ivec4, product, vc);
        if (!store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, result))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::IMUL: {
        // Microsoft Learn imul (sm4): full signed 32x32 -> 64 multiply per
        // component. SPIR-V's extended result member 0 is low, member 1 high.
        Sm4Operand dst_hi, dst_lo, a, b;
        if (!m_decoder.decode_operand(dst_hi) ||
            !m_decoder.decode_operand(dst_lo) ||
            !m_decoder.decode_operand(a) || !m_decoder.decode_operand(b)) {
            return fail("IMUL: bad operands");
        }
        if (a.modifier > 1u || b.modifier > 1u) {
            return fail("IMUL: unsupported source modifier");
        }
        u32 va = 0, vb = 0;
        if (!load_vec4(a, va) || !load_vec4(b, vb)) return false;
        va = m_builder.emit_bitcast(m_type_ivec4, va);
        vb = m_builder.emit_bitcast(m_type_ivec4, vb);
        if (a.modifier == 1u) va = m_builder.emit_snegate(m_type_ivec4, va);
        if (b.modifier == 1u) vb = m_builder.emit_snegate(m_type_ivec4, vb);
        const u32 product = m_builder.emit_smul_extended(
            m_type_smul_result, va, vb);
        const u32 low = m_builder.emit_composite_extract(
            m_type_ivec4, product, {0u});
        const u32 high = m_builder.emit_composite_extract(
            m_type_ivec4, product, {1u});
        if (dst_lo.type != Sm4OperandType::Null &&
            !store_vec4(dst_lo, m_builder.emit_bitcast(m_type_vec4, low))) {
            return false;
        }
        if (dst_hi.type != Sm4OperandType::Null &&
            !store_vec4(dst_hi, m_builder.emit_bitcast(m_type_vec4, high))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::UMAD: {
        // Microsoft Learn umad (sm4): retain the low 32 bits of each
        // unsigned product, then perform a wrapping 32-bit add. The syntax
        // permits no source modifiers.
        Sm4Operand dst, a, b, c;
        if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(a) ||
            !m_decoder.decode_operand(b) || !m_decoder.decode_operand(c)) {
            return fail("UMAD: bad operands");
        }
        if (a.modifier != 0u || b.modifier != 0u || c.modifier != 0u) {
            return fail("UMAD: unsupported source modifier");
        }
        u32 va = 0, vb = 0, vc = 0;
        if (!load_vec4(a, va) || !load_vec4(b, vb) || !load_vec4(c, vc)) {
            return false;
        }
        va = m_builder.emit_bitcast(m_type_uvec4, va);
        vb = m_builder.emit_bitcast(m_type_uvec4, vb);
        vc = m_builder.emit_bitcast(m_type_uvec4, vc);
        const u32 product = m_builder.emit_imul(m_type_uvec4, va, vb);
        const u32 result = m_builder.emit_iadd(m_type_uvec4, product, vc);
        if (!store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, result))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::MSAD: {
        // D3D11.3 22.12.15: per component, dest = accum (src2) plus
        // |ref byte - src byte| over the four bytes of src0 ("ref") and
        // src1 ("src") whose ref byte is non-zero. The spec leaves overflow
        // undefined (saturate recommended, wrap allowed); WARP wraps
        // (d3d11_fl111_probe "msad ... accum=fffffff0": 0xfffffff1 + 0x80
        // gives 0x71), so the add wraps here too.
        Sm4Operand dst, ref, src, accum;
        if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(ref) ||
            !m_decoder.decode_operand(src) ||
            !m_decoder.decode_operand(accum)) {
            return fail("MSAD: bad operands");
        }
        if (ref.modifier != 0u || src.modifier != 0u ||
            accum.modifier != 0u) {
            return fail("MSAD: unsupported source modifier");
        }
        u32 vref = 0, vsrc = 0, vacc = 0;
        if (!load_vec4(ref, vref) || !load_vec4(src, vsrc) ||
            !load_vec4(accum, vacc)) {
            return false;
        }
        vref = m_builder.emit_bitcast(m_type_uvec4, vref);
        vsrc = m_builder.emit_bitcast(m_type_uvec4, vsrc);
        u32 sum = m_builder.emit_bitcast(m_type_uvec4, vacc);
        const u32 byte_mask = make_uvec4_imm(0xFFu, 0xFFu, 0xFFu, 0xFFu);
        for (u32 i = 0; i < 4; ++i) {
            const u32 shift = make_uvec4_imm(i * 8u, i * 8u, i * 8u, i * 8u);
            const u32 ref_byte = m_builder.emit_bitwise_and(
                m_type_uvec4,
                m_builder.emit_shift_right_logical(m_type_uvec4, vref, shift),
                byte_mask);
            const u32 src_byte = m_builder.emit_bitwise_and(
                m_type_uvec4,
                m_builder.emit_shift_right_logical(m_type_uvec4, vsrc, shift),
                byte_mask);
            const u32 ref_ge = m_builder.emit_integer_compare(
                SpvOp::OpUGreaterThanEqual, m_type_bvec4, ref_byte, src_byte);
            const u32 difference = m_builder.emit_select(
                m_type_uvec4, ref_ge,
                m_builder.emit_isub(m_type_uvec4, ref_byte, src_byte),
                m_builder.emit_isub(m_type_uvec4, src_byte, ref_byte));
            const u32 counted = m_builder.emit_integer_compare(
                SpvOp::OpINotEqual, m_type_bvec4, ref_byte, m_const_u_zero4);
            sum = m_builder.emit_iadd(
                m_type_uvec4, sum,
                m_builder.emit_select(m_type_uvec4, counted, difference,
                                      m_const_u_zero4));
        }
        if (!store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, sum))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::IMIN:
    case Sm4Opcode::IMAX: {
        // Microsoft Learn imin/imax (sm4): signed component-wise selection.
        // The only permitted source modifier is optional two's-complement
        // negate, applied before the comparison.
        Sm4Operand dst, a, b;
        if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(a) ||
            !m_decoder.decode_operand(b)) {
            return fail(opcode == Sm4Opcode::IMIN
                        ? "IMIN: bad operands" : "IMAX: bad operands");
        }
        if (a.modifier > 1u || b.modifier > 1u) {
            return fail(opcode == Sm4Opcode::IMIN
                        ? "IMIN: unsupported source modifier"
                        : "IMAX: unsupported source modifier");
        }
        u32 va = 0, vb = 0;
        if (!load_vec4(a, va) || !load_vec4(b, vb)) return false;
        va = m_builder.emit_bitcast(m_type_ivec4, va);
        vb = m_builder.emit_bitcast(m_type_ivec4, vb);
        if (a.modifier == 1u) va = m_builder.emit_snegate(m_type_ivec4, va);
        if (b.modifier == 1u) vb = m_builder.emit_snegate(m_type_ivec4, vb);
        const u32 a_less_b = m_builder.emit_integer_compare(
            SpvOp::OpSLessThan, m_type_bvec4, va, vb);
        const u32 selected = opcode == Sm4Opcode::IMIN
            ? m_builder.emit_select(m_type_ivec4, a_less_b, va, vb)
            : m_builder.emit_select(m_type_ivec4, a_less_b, vb, va);
        if (!store_vec4(dst,
                        m_builder.emit_bitcast(m_type_vec4, selected))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::UMUL: {
        // Microsoft Learn umul (sm4): full unsigned 32x32 -> 64 multiply per
        // component. SPIR-V's extended result members are low then high.
        // Either DXBC destination can independently be NULL.
        Sm4Operand dst_hi, dst_lo, a, b;
        if (!m_decoder.decode_operand(dst_hi) ||
            !m_decoder.decode_operand(dst_lo) ||
            !m_decoder.decode_operand(a) || !m_decoder.decode_operand(b)) {
            return fail("UMUL: bad operands");
        }
        if (a.modifier != 0u || b.modifier != 0u) {
            return fail("UMUL: unsupported source modifier");
        }
        u32 va = 0, vb = 0;
        if (!load_vec4(a, va) || !load_vec4(b, vb)) return false;
        va = m_builder.emit_bitcast(m_type_uvec4, va);
        vb = m_builder.emit_bitcast(m_type_uvec4, vb);
        const u32 product = m_builder.emit_umul_extended(
            m_type_umul_result, va, vb);
        const u32 low = m_builder.emit_composite_extract(
            m_type_uvec4, product, {0u});
        const u32 high = m_builder.emit_composite_extract(
            m_type_uvec4, product, {1u});
        if (dst_lo.type != Sm4OperandType::Null &&
            !store_vec4(dst_lo, m_builder.emit_bitcast(m_type_vec4, low))) {
            return false;
        }
        if (dst_hi.type != Sm4OperandType::Null &&
            !store_vec4(dst_hi, m_builder.emit_bitcast(m_type_vec4, high))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::UMIN:
    case Sm4Opcode::UMAX: {
        // Microsoft Learn umin/umax (sm4): component-wise unsigned compare
        // and selection. Their assembly syntax permits no source modifiers.
        Sm4Operand dst, a, b;
        if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(a) ||
            !m_decoder.decode_operand(b)) {
            return fail(opcode == Sm4Opcode::UMIN
                        ? "UMIN: bad operands" : "UMAX: bad operands");
        }
        if (a.modifier != 0u || b.modifier != 0u) {
            return fail(opcode == Sm4Opcode::UMIN
                        ? "UMIN: unsupported source modifier"
                        : "UMAX: unsupported source modifier");
        }
        u32 va = 0, vb = 0;
        if (!load_vec4(a, va) || !load_vec4(b, vb)) return false;
        va = m_builder.emit_bitcast(m_type_uvec4, va);
        vb = m_builder.emit_bitcast(m_type_uvec4, vb);
        const u32 a_less_b = m_builder.emit_integer_compare(
            SpvOp::OpULessThan, m_type_bvec4, va, vb);
        const u32 selected = opcode == Sm4Opcode::UMIN
            ? m_builder.emit_select(m_type_uvec4, a_less_b, va, vb)
            : m_builder.emit_select(m_type_uvec4, a_less_b, vb, va);
        if (!store_vec4(dst,
                        m_builder.emit_bitcast(m_type_vec4, selected))) {
            return false;
        }
        break;
    }

    default:
        return fail("Unsupported opcode: " +
                    std::to_string(static_cast<u32>(opcode)));
    }

    return true;
}

} // namespace vitreo::graphics::dxbc_spirv_detail
