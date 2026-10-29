// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_op_float.cpp
/// @brief Floating-point arithmetic opcodes.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

bool DxbcTranslator::translate_float_operation(u32 /*opcode_token*/,
                                               Sm4Opcode opcode,
                                               u32 /*sample_controls*/) {
    switch (opcode) {
    case Sm4Opcode::MOV: {
        Sm4Operand dst, src;
        if (!m_decoder.decode_operand(dst)) return fail("MOV: bad dst");
        if (!m_decoder.decode_operand(src)) return fail("MOV: bad src");
        u32 value = 0;
        if (!load_vec4_modified(src, value)) return false;
        // A raw mov keeps every bit (3.1.3.2); a negate/abs modifier makes
        // it a float operation, which flushes a denormal (WARP: mov -denorm
        // -> -0).
        if (src.modifier != 0u) value = flush_denormals(value);
        if (!store_vec4(dst, value)) return false;
        break;
    }

    case Sm4Opcode::ADD: {
        Sm4Operand dst, a, b;
        if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(a) ||
            !m_decoder.decode_operand(b)) {
            return fail("ADD: bad operands");
        }
        u32 va = 0, vb = 0;
        if (!load_vec4_modified(a, va) || !load_vec4_modified(b, vb)) {
            return false;
        }
        u32 sum = m_builder.emit_fadd(m_type_vec4, va, vb);
        // 3.1.3.2: "x +/- 0.0f must always result in x (except denorm
        // flushed)"; Metal folds the add, so the flush is explicit.
        if (immediate_has_magnitude(a, 0.0f, dst.write_mask) ||
            immediate_has_magnitude(b, 0.0f, dst.write_mask)) {
            sum = flush_denormals(sum);
        }
        if (!store_vec4(dst, sum)) return false;
        break;
    }

    case Sm4Opcode::MUL: {
        Sm4Operand dst, a, b;
        if (!m_decoder.decode_operand(dst)) return fail("MUL: bad dst");
        if (!m_decoder.decode_operand(a)) return fail("MUL: bad src0");
        if (!m_decoder.decode_operand(b)) return fail("MUL: bad src1");
        u32 va = 0, vb = 0;
        if (!load_vec4_modified(a, va) || !load_vec4_modified(b, vb)) {
            return false;
        }
        u32 product = m_builder.emit_fmul(m_type_vec4, va, vb);
        // 3.1.3.2: x*1.0f "is not detected as just a mov ... a denorm flush
        // would happen" (WARP: mul |denorm|, l(1.0) -> 0); Metal folds the
        // multiply into a move, so the flush is explicit.
        if (immediate_has_magnitude(a, 1.0f, dst.write_mask) ||
            immediate_has_magnitude(b, 1.0f, dst.write_mask)) {
            product = flush_denormals(product);
        }
        if (!store_vec4(dst, product)) return false;
        break;
    }

    case Sm4Opcode::DIV: {
        // Learn div (sm4): component-wise src0 / src1.
        Sm4Operand dst, a, b;
        if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(a) ||
            !m_decoder.decode_operand(b)) {
            return fail("DIV: bad operands");
        }
        u32 va = 0, vb = 0;
        if (!load_vec4_modified(a, va) || !load_vec4_modified(b, vb)) {
            return false;
        }
        u32 quotient = m_builder.emit_fdiv(m_type_vec4, va, vb);
        // 3.1.3.2: "x/1.0f must always result in x (except denorm flushed)".
        if (immediate_has_magnitude(b, 1.0f, dst.write_mask)) {
            quotient = flush_denormals(quotient);
        }
        if (!store_vec4(dst, quotient)) return false;
        break;
    }

    case Sm4Opcode::RCP: {
        // Microsoft Learn rcp (sm5): component-wise 1.0f / src with the same
        // division semantics as div — 1/0 is +/-INF, not an error — plus the
        // optional NEG/ABS source modifier.
        // Destination saturation is applied by store_vec4.
        Sm4Operand dst, src;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(src)) {
            return fail("RCP: bad operands");
        }
        if (src.modifier > 3u) {
            return fail("RCP: unsupported source modifier");
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
        const u32 one = m_builder.emit_constant_float(m_type_float, 1.0f);
        const u32 ones = m_builder.emit_composite_construct(
            m_type_vec4, {one, one, one, one});
        if (!store_vec4(dst, m_builder.emit_fdiv(m_type_vec4, ones, value))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::RSQ: {
        // Microsoft Learn rsq (sm4): component-wise 1/sqrt(src), including
        // signed-zero and infinity behavior, with optional NEG/ABS source
        // modifier. Destination saturation is applied by store_vec4.
        Sm4Operand dst, src;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(src)) {
            return fail("RSQ: bad operands");
        }
        if (src.modifier > 3u) {
            return fail("RSQ: unsupported source modifier");
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
        const u32 result = m_builder.emit_ext_inst(
            m_type_vec4, m_glsl_std450, kGlslStd450InverseSqrt, {value});
        if (!store_vec4(dst, result)) return false;
        break;
    }

    case Sm4Opcode::SQRT: {
        // Microsoft Learn sqrt (sm4): component-wise square root with 1 ulp
        // precision and optional NEG/ABS source modifier.
        // Destination saturation is applied by store_vec4.
        Sm4Operand dst, src;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(src)) {
            return fail("SQRT: bad operands");
        }
        if (src.modifier > 3u) {
            return fail("SQRT: unsupported source modifier");
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
        // Metal's sqrt is off by up to 1.5 ulp in every math mode (Windows
        // a09a accuracy set: sqrt(65504) -> 0x437FEFFE, WARP 0x437FEFFF).
        // One Newton step on the residual, computed exactly by fma, brings a
        // positive normal input to the correctly rounded root; zero,
        // denormals (flushed to zero), negatives, INF and NaN keep Metal's
        // result, whose special values are already the spec's.
        const u32 root = m_builder.emit_ext_inst(
            m_type_vec4, m_glsl_std450, kGlslStd450Sqrt, {value});
        const u32 residual = m_builder.emit_ext_inst(
            m_type_vec4, m_glsl_std450, kGlslStd450Fma,
            {m_builder.emit_fnegate(m_type_vec4, root), root, value});
        const u32 half_reciprocal = m_builder.emit_fmul(
            m_type_vec4,
            m_builder.emit_ext_inst(m_type_vec4, m_glsl_std450,
                                    kGlslStd450InverseSqrt, {value}),
            make_vec4_imm(0.5f, 0.5f, 0.5f, 0.5f));
        const u32 corrected = m_builder.emit_ext_inst(
            m_type_vec4, m_glsl_std450, kGlslStd450Fma,
            {residual, half_reciprocal, root});
        // bits - 0x00800000 < 0x7F000000: a positive, finite, normal input.
        const u32 bits = m_builder.emit_bitcast(m_type_uvec4, value);
        const u32 positive_normal = m_builder.emit_integer_compare(
            SpvOp::OpULessThan, m_type_bvec4,
            m_builder.emit_isub(m_type_uvec4, bits,
                                make_uvec4_imm(0x00800000u, 0x00800000u,
                                               0x00800000u, 0x00800000u)),
            make_uvec4_imm(0x7F000000u, 0x7F000000u, 0x7F000000u,
                           0x7F000000u));
        const u32 result = m_builder.emit_select(m_type_vec4, positive_normal,
                                                 corrected, root);
        if (!store_vec4(dst, result)) return false;
        break;
    }

    case Sm4Opcode::ROUND_NE:
    case Sm4Opcode::ROUND_NI:
    case Sm4Opcode::ROUND_PI:
    case Sm4Opcode::ROUND_Z: {
        // Microsoft Learn round_* (sm4): component-wise integral-float
        // rounding. NE is nearest-even, NI is floor, PI is ceil, and Z is
        // truncation toward zero.
        // Destination saturation is applied by store_vec4.
        Sm4Operand dst, src;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(src)) {
            return fail("ROUND: bad operands");
        }
        if (src.modifier > 3u) {
            return fail("ROUND: unsupported source modifier");
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
        const u32 instruction =
            opcode == Sm4Opcode::ROUND_NE ? kGlslStd450RoundEven :
            opcode == Sm4Opcode::ROUND_NI ? kGlslStd450Floor :
            opcode == Sm4Opcode::ROUND_PI ? kGlslStd450Ceil :
                                            kGlslStd450Trunc;
        const u32 result = m_builder.emit_ext_inst(
            m_type_vec4, m_glsl_std450, instruction, {value});
        if (!store_vec4(dst, result)) return false;
        break;
    }

    case Sm4Opcode::EXP:
    case Sm4Opcode::LOG: {
        // Microsoft Learn exp/log (sm4): component-wise base-2 exponent and
        // logarithm, with optional NEG/ABS source modifier.
        // Destination saturation is applied by store_vec4.
        Sm4Operand dst, src;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(src)) {
            return fail("EXP/LOG: bad operands");
        }
        if (src.modifier > 3u) {
            return fail("EXP/LOG: unsupported source modifier");
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
        const u32 instruction = opcode == Sm4Opcode::EXP
            ? kGlslStd450Exp2 : kGlslStd450Log2;
        const u32 result = m_builder.emit_ext_inst(
            m_type_vec4, m_glsl_std450, instruction, {value});
        if (!store_vec4(dst, result)) return false;
        break;
    }

    case Sm4Opcode::DERIV_RTX:
    case Sm4Opcode::DERIV_RTX_COARSE:
    case Sm4Opcode::DERIV_RTX_FINE:
    case Sm4Opcode::DERIV_RTY:
    case Sm4Opcode::DERIV_RTY_COARSE:
    case Sm4Opcode::DERIV_RTY_FINE: {
        // Microsoft Learn deriv_rtx/rty (sm4) and the sm5 coarse/fine forms:
        // per-component screen-space derivative after source
        // swizzle/modifier, available only in pixel shaders. Coarse uses one
        // derivative pair for the 2x2 quad; fine uses the neighbor texel.
        // Destination saturation is applied by store_vec4.
        if (m_container.stage != ShaderStage::Fragment) {
            return fail("DERIV: derivatives require a pixel shader");
        }
        const bool derivative_control =
            opcode == Sm4Opcode::DERIV_RTX_COARSE ||
            opcode == Sm4Opcode::DERIV_RTY_COARSE ||
            opcode == Sm4Opcode::DERIV_RTX_FINE ||
            opcode == Sm4Opcode::DERIV_RTY_FINE;
        if (derivative_control && !m_derivative_control_capability_emitted) {
            m_builder.emit_capability(SpvCapability::DerivativeControl);
            m_derivative_control_capability_emitted = true;
        }
        Sm4Operand dst, src;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(src)) {
            return fail("DERIV: bad operands");
        }
        if (src.modifier > 3u) {
            return fail("DERIV: unsupported source modifier");
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
        const SpvOp derivative = opcode == Sm4Opcode::DERIV_RTX_COARSE
            ? SpvOp::OpDPdxCoarse
            : opcode == Sm4Opcode::DERIV_RTY_COARSE
                ? SpvOp::OpDPdyCoarse
                : opcode == Sm4Opcode::DERIV_RTX_FINE
                    ? SpvOp::OpDPdxFine
                    : opcode == Sm4Opcode::DERIV_RTY_FINE
                        ? SpvOp::OpDPdyFine
                        : opcode == Sm4Opcode::DERIV_RTX
                            ? SpvOp::OpDPdx : SpvOp::OpDPdy;
        const bool coarse = opcode == Sm4Opcode::DERIV_RTX_COARSE ||
                            opcode == Sm4Opcode::DERIV_RTY_COARSE;
        u32 result = 0;
        if (coarse) {
            // D3D11.3 22.5.2/3: "Only a single x,y derivative pair is
            // computed for each 2x2 stamp" -- every pixel of the quad sees
            // the same value (WARP: the top row's x delta, the left
            // column's y delta). MSL's dfdx/dfdy are per pixel, so the
            // quad's first lane's fine derivative is broadcast to the quad.
            // (fxc compiles SM5 ddx()/ddy() to these coarse forms.)
            m_builder.emit_capability(SpvCapability::GroupNonUniform);
            m_builder.emit_capability(SpvCapability::GroupNonUniformQuad);
            const u32 fine = m_builder.emit_derivative(
                opcode == Sm4Opcode::DERIV_RTX_COARSE ? SpvOp::OpDPdxFine
                                                      : SpvOp::OpDPdyFine,
                m_type_vec4, value);
            result = m_builder.emit_quad_broadcast(
                m_type_vec4,
                m_builder.emit_constant_int(m_type_uint, 3u), // Subgroup
                fine, m_const_u0);
        } else {
            result = m_builder.emit_derivative(derivative, m_type_vec4, value);
        }
        if (!store_vec4(dst, result)) return false;
        break;
    }

    case Sm4Opcode::SINCOS: {
        // Microsoft Learn sincos (sm4): component-wise sine to destination 0
        // and cosine to destination 1, with either destination optionally
        // NULL.
        // Destination saturation (both results) is applied by store_vec4.
        Sm4Operand dst_sin, dst_cos, src;
        if (!m_decoder.decode_operand(dst_sin) ||
            !m_decoder.decode_operand(dst_cos) ||
            !m_decoder.decode_operand(src)) {
            return fail("SINCOS: bad operands");
        }
        if (src.modifier > 3u) {
            return fail("SINCOS: unsupported source modifier");
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
        // D3D11.3 22.10.20: sin/cos of +-INF or NaN is NaN, and a denormal
        // input is flushed (3.1.3.2), so sin(denorm) = +-0. Metal returns 0
        // for the non-finite inputs in every math mode and passes a
        // denormal through sin unflushed (measured on an M4 Pro).
        const u32 non_finite = non_finite_lanes(value);
        const u32 flushed = flush_denormals(value);
        const u32 nan4 = m_builder.emit_bitcast(
            m_type_vec4, make_uvec4_imm(0xFFC00000u, 0xFFC00000u, 0xFFC00000u,
                                        0xFFC00000u));
        // Both results are computed before either store: dst_sin may alias
        // the source.
        u32 sine = 0;
        u32 cosine = 0;
        if (dst_sin.type != Sm4OperandType::Null) {
            sine = m_builder.emit_select(
                m_type_vec4, non_finite, nan4,
                m_builder.emit_ext_inst(m_type_vec4, m_glsl_std450,
                                        kGlslStd450Sin, {flushed}));
        }
        if (dst_cos.type != Sm4OperandType::Null) {
            cosine = m_builder.emit_select(
                m_type_vec4, non_finite, nan4,
                m_builder.emit_ext_inst(m_type_vec4, m_glsl_std450,
                                        kGlslStd450Cos, {flushed}));
        }
        if (sine != 0 && !store_vec4(dst_sin, sine)) return false;
        if (cosine != 0 && !store_vec4(dst_cos, cosine)) return false;
        break;
    }

    case Sm4Opcode::DP2:
    case Sm4Opcode::DP3:
    case Sm4Opcode::DP4: {
        // Learn dp2/dp3/dp4: scalar dot over 2/3/4 components, replicated
        // to every enabled destination component.
        Sm4Operand dst, a, b;
        if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(a) ||
            !m_decoder.decode_operand(b)) {
            return fail("DP: bad operands");
        }
        u32 va = 0, vb = 0;
        if (!load_vec4_modified(a, va) || !load_vec4_modified(b, vb)) {
            return false;
        }

        const u32 comps =
            opcode == Sm4Opcode::DP2 ? 2u :
            opcode == Sm4Opcode::DP3 ? 3u : 4u;
        const u32 vec_ty =
            comps == 2 ? m_type_vec2 : comps == 3 ? m_type_vec3 : m_type_vec4;
        std::vector<u32> a_lanes(comps);
        std::vector<u32> b_lanes(comps);
        for (u32 i = 0; i < comps; ++i) {
            a_lanes[i] = m_builder.emit_composite_extract(
                m_type_float, va, {i});
            b_lanes[i] = m_builder.emit_composite_extract(
                m_type_float, vb, {i});
        }
        const u32 a_vec =
            m_builder.emit_composite_construct(vec_ty, a_lanes);
        const u32 b_vec =
            m_builder.emit_composite_construct(vec_ty, b_lanes);
        const u32 scalar = m_builder.emit_dot(m_type_float, a_vec, b_vec);
        const u32 splatted = m_builder.emit_composite_construct(
            m_type_vec4, {scalar, scalar, scalar, scalar});
        if (!store_vec4(dst, splatted)) return false;
        break;
    }

    case Sm4Opcode::MAD: {
        // Learn: mad dest, src0, src1, src2 → dest = src0 * src1 + src2
        Sm4Operand dst, a, b, c;
        if (!m_decoder.decode_operand(dst)) return fail("MAD: bad dst");
        if (!m_decoder.decode_operand(a)) return fail("MAD: bad src0");
        if (!m_decoder.decode_operand(b)) return fail("MAD: bad src1");
        if (!m_decoder.decode_operand(c)) return fail("MAD: bad src2");
        u32 va = 0, vb = 0, vc = 0;
        if (!load_vec4_modified(a, va) || !load_vec4_modified(b, vb) ||
            !load_vec4_modified(c, vc)) {
            return false;
        }
        const u32 prod = m_builder.emit_fmul(m_type_vec4, va, vb);
        if (!store_vec4(dst, m_builder.emit_fadd(m_type_vec4, prod, vc))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::MAX: {
        // Microsoft Learn, max (sm4 - asm): component-wise maximum. A
        // single NaN selects the other operand; two NaNs produce a NaN.
        Sm4Operand dst, a, b;
        if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(a) ||
            !m_decoder.decode_operand(b)) {
            return fail("MAX: bad operands");
        }
        u32 va = 0, vb = 0;
        if (!load_vec4_modified(a, va) || !load_vec4_modified(b, vb)) {
            return false;
        }

        const u32 a_is_nan = m_builder.emit_is_nan(m_type_bvec4, va);
        const u32 b_is_nan = m_builder.emit_is_nan(m_type_bvec4, vb);
        const u32 a_ge_b =
            m_builder.emit_ford_ge(m_type_bvec4, va, vb);
        const u32 ordered_max =
            m_builder.emit_select(m_type_vec4, a_ge_b, va, vb);
        const u32 b_nan_max =
            m_builder.emit_select(m_type_vec4, b_is_nan, va, ordered_max);
        const u32 result =
            m_builder.emit_select(m_type_vec4, a_is_nan, vb, b_nan_max);
        if (!store_vec4(dst, result)) return false;
        break;
    }

    case Sm4Opcode::MIN: {
        // Microsoft Learn, min (sm4 - asm): component-wise minimum. A
        // single NaN selects the other operand; two NaNs produce a NaN.
        Sm4Operand dst, a, b;
        if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(a) ||
            !m_decoder.decode_operand(b)) {
            return fail("MIN: bad operands");
        }
        u32 va = 0, vb = 0;
        if (!load_vec4_modified(a, va) || !load_vec4_modified(b, vb)) {
            return false;
        }

        const u32 a_is_nan = m_builder.emit_is_nan(m_type_bvec4, va);
        const u32 b_is_nan = m_builder.emit_is_nan(m_type_bvec4, vb);
        const u32 a_lt_b =
            m_builder.emit_ford_lt(m_type_bvec4, va, vb);
        const u32 ordered_min =
            m_builder.emit_select(m_type_vec4, a_lt_b, va, vb);
        const u32 b_nan_min =
            m_builder.emit_select(m_type_vec4, b_is_nan, va, ordered_min);
        const u32 result =
            m_builder.emit_select(m_type_vec4, a_is_nan, vb, b_nan_min);
        if (!store_vec4(dst, result)) return false;
        break;
    }

    case Sm4Opcode::FRC: {
        // Learn frc (sm4): dest = src0 - round_ni(src0) [= floor(src0)].
        // GLSL.std.450 Fract carries that contract directly.
        // Destination saturation is applied by store_vec4.
        Sm4Operand dst, src;
        if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(src)) {
            return fail("FRC: bad operands");
        }
        if (src.modifier > 3u) {
            return fail("FRC: unsupported source modifier");
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
        // D3D11.3 22.10.7: the result lies in [0, 1) and frc(+-INF) is NaN.
        // Metal's fract returns 1.0 for a tiny negative (x - floor(x) rounds
        // up) and 0 for +-INF, so the difference is clamped to the largest
        // float below 1.0 (Windows: frc(-FLT_MIN) = 0x3F7FFFFF) and the
        // non-finite lanes -- where the clamp would also hide a NaN -- are
        // NaN explicitly.
        const u32 difference = m_builder.emit_fsub(
            m_type_vec4, value,
            m_builder.emit_ext_inst(m_type_vec4, m_glsl_std450,
                                    kGlslStd450Floor, {value}));
        const u32 below_one = m_builder.emit_bitcast(
            m_type_vec4, make_uvec4_imm(0x3F7FFFFFu, 0x3F7FFFFFu, 0x3F7FFFFFu,
                                        0x3F7FFFFFu));
        const u32 clamped = m_builder.emit_ext_inst(
            m_type_vec4, m_glsl_std450, kGlslStd450FMin,
            {difference, below_one});
        const u32 nan4 = m_builder.emit_bitcast(
            m_type_vec4, make_uvec4_imm(0xFFC00000u, 0xFFC00000u, 0xFFC00000u,
                                        0xFFC00000u));
        const u32 result = m_builder.emit_select(
            m_type_vec4, non_finite_lanes(value), nan4, clamped);
        if (!store_vec4(dst, result)) return false;
        break;
    }

    default:
        return fail("Unsupported opcode: " +
                    std::to_string(static_cast<u32>(opcode)));
    }

    return true;
}

} // namespace vitreo::graphics::dxbc_spirv_detail
