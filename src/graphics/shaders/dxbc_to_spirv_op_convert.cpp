// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_op_convert.cpp
/// @brief Comparison, conversion and select opcodes.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

bool DxbcTranslator::translate_compare_convert_operation(u32 /*opcode_token*/,
                                                         Sm4Opcode opcode,
                                                         u32 /*sample_controls*/) {
    switch (opcode) {
    case Sm4Opcode::F32TOF16:
    case Sm4Opcode::F16TOF32: {
        // Microsoft Learn (f32tof16 / f16tof32): component-wise conversions
        // between a float32 and the IEEE-754 half stored in the LOW 16 bits of
        // a uint; the upper 16 bits of an f32tof16 result are zero, and
        // f16tof32 reads only the low half of its source.
        //
        // f32tof16 follows D3D11.3 3.2.2's conversion rules on the bits
        // (f32_to_f16_bits: round toward zero, overflow to the largest half).
        // f16tof32 widens exactly, so GLSL UnpackHalf2x16 (low half first)
        // is the primitive; it is scalar, hence the per-component loop.
        Sm4Operand dst, src;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(src)) {
            return fail("f32tof16/f16tof32: bad operands");
        }
        // D3D11.3 22.13.1-2 allow `[-]src`, and fxc emits `-` and `-|x|` for
        // f32tof16(-x) / f32tof16(-abs(x)): the float source is modified
        // before the conversion. For f16tof32 the modifier acts on the
        // float result, not as a 2's complement negate of the integer
        // source (Windows, a patched `f16tof32 r, -r` on WARP:
        // 0x0001 -> 0xB3800000, 0xFFFF -> 0x7FFFE000).
        if (src.modifier > 3u) {
            return fail("f32tof16/f16tof32: unsupported source modifier");
        }
        const bool to_half = opcode == Sm4Opcode::F32TOF16;
        u32 value = 0;
        if (to_half) {
            if (!load_vec4_modified(src, value)) return false;
            const u32 result = f32_to_f16_bits(value);
            if (!store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, result))) {
                return false;
            }
            break;
        }
        if (!load_vec4(src, value)) return false;
        const u32 source_vector = m_builder.emit_bitcast(m_type_uvec4, value);
        std::vector<u32> components;
        components.reserve(4);
        for (u32 i = 0; i < 4u; ++i) {
            const u32 scalar = m_builder.emit_composite_extract(
                m_type_uint, source_vector, {i});
            const u32 pair = m_builder.emit_ext_inst(
                m_type_vec2, m_glsl_std450, kGlslStd450UnpackHalf2x16,
                {scalar});
            components.push_back(
                m_builder.emit_composite_extract(m_type_float, pair, {0u}));
        }
        u32 result = m_builder.emit_composite_construct(m_type_vec4, components);
        if (src.modifier == 2u || src.modifier == 3u) {
            result = m_builder.emit_ext_inst(
                m_type_vec4, m_glsl_std450, kGlslStd450FAbs, {result});
        }
        if (src.modifier == 1u || src.modifier == 3u) {
            result = m_builder.emit_fnegate(m_type_vec4, result);
        }
        if (!store_vec4(dst, result)) return false;
        break;
    }

    case Sm4Opcode::NE: {
        // Learn ne (sm4): component-wise src0 != src1; true → 0xFFFFFFFF,
        // false → 0; comparison with NaN returns true. fxc's isnan(x) is
        // `ne r, x, x`, so the NaN lanes are decided explicitly
        // (float_compare_result).
        Sm4Operand dst, a, b;
        if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(a) ||
            !m_decoder.decode_operand(b)) {
            return fail("NE: bad operands");
        }
        u32 va = 0, vb = 0;
        if (!load_vec4_modified(a, va) || !load_vec4_modified(b, vb)) {
            return false;
        }
        const u32 bits = float_compare_result(opcode, a, va, b, vb);
        const u32 as_float = m_builder.emit_bitcast(m_type_vec4, bits);
        if (!store_vec4(dst, as_float)) return false;
        break;
    }

    case Sm4Opcode::IEQ:
    case Sm4Opcode::INE:
    case Sm4Opcode::ILT:
    case Sm4Opcode::IGE:
    case Sm4Opcode::ULT:
    case Sm4Opcode::UGE: {
        // Microsoft Learn integer comparisons: compare each signed/unsigned
        // 32-bit lane and materialize D3D booleans as all bits set or zero.
        // These opcode syntaxes do not permit source modifiers.
        Sm4Operand dst, a, b;
        if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(a) ||
            !m_decoder.decode_operand(b)) {
            return fail("INTEGER_CMP: bad operands");
        }
        if (a.modifier != 0u || b.modifier != 0u) {
            return fail("INTEGER_CMP: unsupported source modifier");
        }
        u32 va = 0, vb = 0;
        if (!load_vec4(a, va) || !load_vec4(b, vb)) return false;
        const bool signed_compare =
            opcode == Sm4Opcode::ILT || opcode == Sm4Opcode::IGE;
        const u32 operand_type = signed_compare ? m_type_ivec4 : m_type_uvec4;
        va = m_builder.emit_bitcast(operand_type, va);
        vb = m_builder.emit_bitcast(operand_type, vb);

        SpvOp comparison = SpvOp::OpIEqual;
        if (opcode == Sm4Opcode::INE) {
            comparison = SpvOp::OpINotEqual;
        } else if (opcode == Sm4Opcode::ILT) {
            comparison = SpvOp::OpSLessThan;
        } else if (opcode == Sm4Opcode::IGE) {
            comparison = SpvOp::OpSGreaterThanEqual;
        } else if (opcode == Sm4Opcode::ULT) {
            comparison = SpvOp::OpULessThan;
        } else if (opcode == Sm4Opcode::UGE) {
            comparison = SpvOp::OpUGreaterThanEqual;
        }
        const u32 predicate = m_builder.emit_integer_compare(
            comparison, m_type_bvec4, va, vb);
        const u32 bits = m_builder.emit_select(
            m_type_uvec4, predicate, m_const_u_ffff4, m_const_u_zero4);
        if (!store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, bits))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::EQ:
    case Sm4Opcode::LT:
    case Sm4Opcode::GE: {
        // Learn eq/lt/ge (sm4): ordered float compares; true → 0xFFFFFFFF,
        // false → 0; comparison with NaN returns false (fxc's !isnan(x) is
        // `eq r, x, x`; see float_compare_result).
        Sm4Operand dst, a, b;
        if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(a) ||
            !m_decoder.decode_operand(b)) {
            return fail("CMP: bad operands");
        }
        u32 va = 0, vb = 0;
        if (!load_vec4_modified(a, va) || !load_vec4_modified(b, vb)) {
            return false;
        }
        const u32 bits = float_compare_result(opcode, a, va, b, vb);
        const u32 as_float = m_builder.emit_bitcast(m_type_vec4, bits);
        if (!store_vec4(dst, as_float)) return false;
        break;
    }

    case Sm4Opcode::FTOI: {
        // Microsoft Learn ftoi (sm4): truncate each component toward zero,
        // clamp overflow to signed 32-bit range, and produce zero for NaN.
        // SPIR-V leaves invalid conversions unspecified, so sanitize those
        // lanes before OpConvertFToS and select their exact integer results.
        Sm4Operand dst, src;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(src)) {
            return fail("FTOI: bad operands");
        }
        u32 value = 0;
        if (!load_vec4_modified(src, value)) return false;

        const u32 min_float = make_vec4_imm(
            -2147483648.0f, -2147483648.0f,
            -2147483648.0f, -2147483648.0f);
        const u32 upper_float = make_vec4_imm(
            2147483648.0f, 2147483648.0f,
            2147483648.0f, 2147483648.0f);
        const u32 zero_float = make_vec4_imm(0.0f, 0.0f, 0.0f, 0.0f);
        const u32 is_nan = m_builder.emit_is_nan(m_type_bvec4, value);
        const u32 below_min =
            m_builder.emit_ford_lt(m_type_bvec4, value, min_float);
        const u32 at_or_above_max =
            m_builder.emit_ford_ge(m_type_bvec4, value, upper_float);

        u32 safe = m_builder.emit_select(
            m_type_vec4, is_nan, zero_float, value);
        safe = m_builder.emit_select(
            m_type_vec4, below_min, zero_float, safe);
        safe = m_builder.emit_select(
            m_type_vec4, at_or_above_max, zero_float, safe);
        const u32 converted =
            m_builder.emit_convert_f_to_s(m_type_ivec4, safe);

        const u32 int_zero = m_builder.emit_constant_int(m_type_int, 0u);
        const u32 int_min =
            m_builder.emit_constant_int(m_type_int, 0x80000000u);
        const u32 int_max =
            m_builder.emit_constant_int(m_type_int, 0x7FFFFFFFu);
        const u32 int_zero4 = m_builder.emit_constant_composite(
            m_type_ivec4, {int_zero, int_zero, int_zero, int_zero});
        const u32 int_min4 = m_builder.emit_constant_composite(
            m_type_ivec4, {int_min, int_min, int_min, int_min});
        const u32 int_max4 = m_builder.emit_constant_composite(
            m_type_ivec4, {int_max, int_max, int_max, int_max});
        u32 result = m_builder.emit_select(
            m_type_ivec4, below_min, int_min4, converted);
        result = m_builder.emit_select(
            m_type_ivec4, at_or_above_max, int_max4, result);
        result = m_builder.emit_select(
            m_type_ivec4, is_nan, int_zero4, result);
        if (!store_vec4(
                dst, m_builder.emit_bitcast(m_type_vec4, result))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::FTOU: {
        // Microsoft Learn ftou (sm4): apply optional negate/absolute source
        // modifiers, truncate each component toward zero, clamp to the
        // unsigned 32-bit range, and produce zero for NaN. SPIR-V leaves
        // invalid conversions unspecified, so sanitize and select bounds.
        Sm4Operand dst, src;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(src)) {
            return fail("FTOU: bad operands");
        }
        if (src.modifier > 3u) {
            return fail("FTOU: unsupported source modifier");
        }
        u32 value = 0;
        if (!load_vec4(src, value)) return false;
        if (src.modifier == 2u || src.modifier == 3u) {
            value = m_builder.emit_ext_inst(
                m_type_vec4, m_glsl_std450, kGlslStd450FAbs, {value});
        }
        if (src.modifier == 1u || src.modifier == 3u) {
            value = m_builder.emit_fnegate(m_type_vec4, value);
        }

        const u32 zero_float = make_vec4_imm(0.0f, 0.0f, 0.0f, 0.0f);
        const u32 upper_float = make_vec4_imm(
            4294967296.0f, 4294967296.0f,
            4294967296.0f, 4294967296.0f);
        const u32 is_nan = m_builder.emit_is_nan(m_type_bvec4, value);
        const u32 below_zero =
            m_builder.emit_ford_lt(m_type_bvec4, value, zero_float);
        const u32 at_or_above_max =
            m_builder.emit_ford_ge(m_type_bvec4, value, upper_float);

        u32 safe = m_builder.emit_select(
            m_type_vec4, is_nan, zero_float, value);
        safe = m_builder.emit_select(
            m_type_vec4, below_zero, zero_float, safe);
        safe = m_builder.emit_select(
            m_type_vec4, at_or_above_max, zero_float, safe);
        const u32 converted =
            m_builder.emit_convert_f_to_u(m_type_uvec4, safe);
        u32 result = m_builder.emit_select(
            m_type_uvec4, at_or_above_max, m_const_u_ffff4, converted);
        result = m_builder.emit_select(
            m_type_uvec4, below_zero, m_const_u_zero4, result);
        result = m_builder.emit_select(
            m_type_uvec4, is_nan, m_const_u_zero4, result);
        if (!store_vec4(
                dst, m_builder.emit_bitcast(m_type_vec4, result))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::ITOF: {
        // Microsoft Learn itof (sm4): interpret each source component as a
        // signed 32-bit integer and convert it numerically to float. DXBC
        // registers are float-backed here, so recover their integer bits
        // before the SPIR-V numeric conversion.
        Sm4Operand dst, src;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(src)) {
            return fail("ITOF: bad operands");
        }
        // D3D11.3 22.13.5: "Optional negate modifier on source operand takes
        // 2's complement before performing arithmetic operation." fxc emits
        // `itof r, -r` to turn an all-ones boolean into 1.0; dropping it gave
        // -1.0. No other modifier is defined.
        if (src.modifier > 1u) {
            return fail("ITOF: unsupported source modifier");
        }
        u32 value = 0;
        if (!load_vec4(src, value)) return false;
        u32 signed_value = m_builder.emit_bitcast(m_type_ivec4, value);
        if (src.modifier == 1u) {
            signed_value = m_builder.emit_snegate(m_type_ivec4, signed_value);
        }
        const u32 converted =
            m_builder.emit_convert_s_to_f(m_type_vec4, signed_value);
        if (!store_vec4(dst, converted)) return false;
        break;
    }

    case Sm4Opcode::UTOF: {
        // Microsoft Learn utof (sm4): interpret each source component as an
        // unsigned 32-bit integer and convert it per component to float,
        // rounding non-exact values to nearest even. DXBC registers remain
        // float-backed, so recover their unsigned bits before conversion.
        Sm4Operand dst, src;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(src)) {
            return fail("UTOF: bad operands");
        }
        // D3D11.3 22.13.6 defines no source modifier for utof.
        if (src.modifier != 0u) {
            return fail("UTOF: unsupported source modifier");
        }
        u32 value = 0;
        if (!load_vec4(src, value)) return false;
        const u32 unsigned_value =
            m_builder.emit_bitcast(m_type_uvec4, value);
        const u32 converted =
            m_builder.emit_convert_u_to_f(m_type_vec4, unsigned_value);
        if (!store_vec4(dst, converted)) return false;
        break;
    }

    case Sm4Opcode::MOVC: {
        // Learn movc: dest = (src0 != 0) ? src1 : src2 (per component).
        Sm4Operand dst, pred, a, b;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(pred) ||
            !m_decoder.decode_operand(a) || !m_decoder.decode_operand(b)) {
            return fail("MOVC: bad operands");
        }
        // src1/src2 are ordinary float sources and carry operand modifiers:
        // a compute shader flips the twiddle angle with `movc r.w, cond, r.w, -r.w`,
        // and loading them unmodified made both arms identical (the inverse
        // transform ran forward; measured against Windows with the game's
        // own bytecode, tools/oracle_probe/d3d11_dxbc_compute_probe.c).
        u32 vp = 0, va = 0, vb = 0;
        if (!load_vec4(pred, vp) || !load_vec4_modified(a, va) ||
            !load_vec4_modified(b, vb)) {
            return false;
        }
        // As for mov: only an unmodified source is moved bit for bit.
        if (a.modifier != 0u) va = flush_denormals(va);
        if (b.modifier != 0u) vb = flush_denormals(vb);
        const u32 pred_u = m_builder.emit_bitcast(m_type_uvec4, vp);
        const u32 nonzero = m_builder.emit_inotequal(
            m_type_bvec4, pred_u, m_const_u_zero4);
        const u32 selected =
            m_builder.emit_select(m_type_vec4, nonzero, va, vb);
        if (!store_vec4(dst, selected)) return false;
        break;
    }

    case Sm4Opcode::SWAPC: {
        Sm4Operand dst0, dst1, pred, src1, src2;
        if (!m_decoder.decode_operand(dst0) ||
            !m_decoder.decode_operand(dst1) ||
            !m_decoder.decode_operand(pred) ||
            !m_decoder.decode_operand(src1) ||
            !m_decoder.decode_operand(src2)) {
            return fail("SWAPC: bad operands");
        }
        if (pred.modifier != 0u) {
            return fail("SWAPC: predicate modifier is unsupported");
        }
        u32 condition_bits = 0, a = 0, b = 0;
        if (!load_vec4(pred, condition_bits) ||
            !load_vec4_modified(src1, a) ||
            !load_vec4_modified(src2, b)) {
            return false;
        }
        // As for mov: only an unmodified source is moved bit for bit.
        if (src1.modifier != 0u) a = flush_denormals(a);
        if (src2.modifier != 0u) b = flush_denormals(b);
        const u32 predicate = m_builder.emit_inotequal(
            m_type_bvec4,
            m_builder.emit_bitcast(m_type_uvec4, condition_bits),
            m_const_u_zero4);
        const u32 first =
            m_builder.emit_select(m_type_vec4, predicate, b, a);
        const u32 second =
            m_builder.emit_select(m_type_vec4, predicate, a, b);
        if (!store_vec4(dst0, first) || !store_vec4(dst1, second)) {
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
