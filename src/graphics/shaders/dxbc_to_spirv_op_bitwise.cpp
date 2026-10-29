// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_op_bitwise.cpp
/// @brief Shift and bit-manipulation opcodes.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

bool DxbcTranslator::translate_bitwise_operation(u32 /*opcode_token*/,
                                                 Sm4Opcode opcode,
                                                 u32 /*sample_controls*/) {
    switch (opcode) {
    case Sm4Opcode::USHR: {
        // Microsoft Learn ushr (sm4): logical component-wise right shift,
        // inserting zeroes. Only the low five bits of the selected scalar
        // shift count are observed and that count applies to every lane.
        Sm4Operand dst, value, count;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(value) ||
            !m_decoder.decode_operand(count)) {
            return fail("USHR: bad operands");
        }
        if (value.modifier != 0u || count.modifier != 0u) {
            return fail("USHR: unsupported source modifier");
        }
        u32 value4 = 0, count4 = 0;
        if (!load_vec4(value, value4) || !load_vec4(count, count4)) {
            return false;
        }
        value4 = m_builder.emit_bitcast(m_type_uvec4, value4);
        count4 = m_builder.emit_bitcast(m_type_uvec4, count4);
        const u32 count_mask =
            m_builder.emit_constant_int(m_type_uint, 31u);
        const u32 count_mask4 = m_builder.emit_constant_composite(
            m_type_uvec4,
            {count_mask, count_mask, count_mask, count_mask});
        count4 = m_builder.emit_bitwise_and(
            m_type_uvec4, count4, count_mask4);
        const u32 shifted = m_builder.emit_shift_right_logical(
            m_type_uvec4, value4, count4);
        if (!store_vec4(
                dst, m_builder.emit_bitcast(m_type_vec4, shifted))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::ISHR:
    case Sm4Opcode::ISHL: {
        // Microsoft Learn ishr/ishl (sm4): a selected scalar count is
        // broadcast and reduced to its low five bits. ISHR sign-extends bit
        // 31; ISHL inserts zeroes and retains only the low 32 result bits.
        Sm4Operand dst, value, count;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(value) ||
            !m_decoder.decode_operand(count)) {
            return fail(opcode == Sm4Opcode::ISHR
                            ? "ISHR: bad operands"
                            : "ISHL: bad operands");
        }
        if (value.modifier != 0u || count.modifier != 0u) {
            return fail(opcode == Sm4Opcode::ISHR
                            ? "ISHR: unsupported source modifier"
                            : "ISHL: unsupported source modifier");
        }
        u32 value4 = 0, count4 = 0;
        if (!load_vec4(value, value4) || !load_vec4(count, count4)) {
            return false;
        }
        count4 = m_builder.emit_bitcast(m_type_uvec4, count4);
        const u32 count_mask =
            m_builder.emit_constant_int(m_type_uint, 31u);
        const u32 count_mask4 = m_builder.emit_constant_composite(
            m_type_uvec4,
            {count_mask, count_mask, count_mask, count_mask});
        count4 = m_builder.emit_bitwise_and(
            m_type_uvec4, count4, count_mask4);

        u32 shifted = 0;
        if (opcode == Sm4Opcode::ISHR) {
            value4 = m_builder.emit_bitcast(m_type_ivec4, value4);
            shifted = m_builder.emit_shift_right_arithmetic(
                m_type_ivec4, value4, count4);
        } else {
            value4 = m_builder.emit_bitcast(m_type_uvec4, value4);
            shifted = m_builder.emit_shift_left_logical(
                m_type_uvec4, value4, count4);
        }
        if (!store_vec4(
                dst, m_builder.emit_bitcast(m_type_vec4, shifted))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::COUNTBITS:
    case Sm4Opcode::FIRSTBIT_HI:
    case Sm4Opcode::FIRSTBIT_LO:
    case Sm4Opcode::FIRSTBIT_SHI: {
        // Microsoft Learn SM5 bit operations are component-wise. firstbit
        // returns 0xffffffff when no matching bit exists, matching GLSL.std.450
        // Find*'s -1 result exactly.
        Sm4Operand dst, src;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(src)) {
            return fail("bit scan/count: bad operands");
        }
        if (src.modifier != 0u) {
            return fail("bit scan/count: unsupported source modifier");
        }
        u32 value = 0;
        if (!load_vec4(src, value)) return false;
        u32 result = 0;
        if (opcode == Sm4Opcode::COUNTBITS) {
            value = m_builder.emit_bitcast(m_type_uvec4, value);
            result = m_builder.emit_unary_integer(
                SpvOp::OpBitCount, m_type_uvec4, value);
        } else {
            const bool signed_scan = opcode == Sm4Opcode::FIRSTBIT_SHI;
            value = m_builder.emit_bitcast(
                signed_scan ? m_type_ivec4 : m_type_uvec4, value);
            const u32 instruction =
                opcode == Sm4Opcode::FIRSTBIT_LO ? kGlslStd450FindILsb :
                signed_scan ? kGlslStd450FindSMsb :
                              kGlslStd450FindUMsb;
            result = m_builder.emit_ext_inst(
                m_type_ivec4, m_glsl_std450, instruction, {value});
            if (opcode != Sm4Opcode::FIRSTBIT_LO) {
                // D3D11.3 22.11.5: firstbit_hi/_shi count from the MSB
                // ("firstbit_hi on 0x10000000 returns 3"); Find*Msb count
                // from the LSB. Measured: fxc follows both with
                // `iadd r, -r, 31` to form HLSL firstbithigh(), and Windows
                // returns 7 for 0xEE (tools/oracle_probe/firstbit_cs.hlsl).
                // No match stays ~0.
                const u32 i31 = m_builder.emit_constant_int(m_type_int, 31);
                const u32 i31x4 = m_builder.emit_constant_composite(
                    m_type_ivec4, {i31, i31, i31, i31});
                const u32 none = m_builder.emit_integer_compare(
                    SpvOp::OpIEqual, m_type_bvec4,
                    m_builder.emit_bitcast(m_type_uvec4, result),
                    m_const_u_ffff4);
                const u32 from_msb =
                    m_builder.emit_isub(m_type_ivec4, i31x4, result);
                result = m_builder.emit_select(m_type_ivec4, none, result,
                                               from_msb);
            }
        }
        if (!store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, result))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::UBFE:
    case Sm4Opcode::IBFE: {
        // Microsoft Learn ubfe/ibfe (sm5): src0 and src1 contribute their low
        // five bits as width and offset. Width zero yields zero; when
        // width+offset reaches 32, extraction continues only to bit 31.
        Sm4Operand dst, width, offset, value;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(width) ||
            !m_decoder.decode_operand(offset) ||
            !m_decoder.decode_operand(value)) {
            return fail(opcode == Sm4Opcode::IBFE
                            ? "IBFE: bad operands" : "UBFE: bad operands");
        }
        if (width.modifier != 0u || offset.modifier != 0u ||
            value.modifier != 0u) {
            return fail(opcode == Sm4Opcode::IBFE
                            ? "IBFE: unsupported source modifier"
                            : "UBFE: unsupported source modifier");
        }
        u32 width4 = 0, offset4 = 0, value4 = 0;
        if (!load_vec4(width, width4) || !load_vec4(offset, offset4) ||
            !load_vec4(value, value4)) {
            return false;
        }
        const auto splat = [&](u32 scalar) {
            return m_builder.emit_constant_composite(
                m_type_uvec4, {scalar, scalar, scalar, scalar});
        };
        const u32 five_bits =
            splat(m_builder.emit_constant_int(m_type_uint, 31u));
        width4 = m_builder.emit_bitwise_and(
            m_type_uvec4, m_builder.emit_bitcast(m_type_uvec4, width4),
            five_bits);
        offset4 = m_builder.emit_bitwise_and(
            m_type_uvec4, m_builder.emit_bitcast(m_type_uvec4, offset4),
            five_bits);
        const u32 remaining = m_builder.emit_isub(
            m_type_uvec4,
            splat(m_builder.emit_constant_int(m_type_uint, 32u)), offset4);
        const u32 width_fits = m_builder.emit_integer_compare(
            SpvOp::OpULessThan, m_type_bvec4, width4, remaining);
        const u32 count = m_builder.emit_select(
            m_type_uvec4, width_fits, width4, remaining);
        const bool signed_extract = opcode == Sm4Opcode::IBFE;
        const u32 integer_type =
            signed_extract ? m_type_ivec4 : m_type_uvec4;
        value4 = m_builder.emit_bitcast(integer_type, value4);
        // Metal's extract_bits overload takes scalar offset/count even when
        // the base is a vector. DXBC supplies those values per component, so
        // lower each lane independently rather than asking SPIRV-Cross to
        // emit an unavailable vector-offset overload.
        std::vector<u32> lanes;
        lanes.reserve(4);
        for (u32 lane = 0; lane < 4u; ++lane) {
            const u32 base_lane = m_builder.emit_composite_extract(
                signed_extract ? m_type_int : m_type_uint, value4, {lane});
            const u32 offset_lane = m_builder.emit_composite_extract(
                m_type_uint, offset4, {lane});
            const u32 count_lane = m_builder.emit_composite_extract(
                m_type_uint, count, {lane});
            lanes.push_back(m_builder.emit_bit_field_extract(
                signed_extract ? SpvOp::OpBitFieldSExtract
                               : SpvOp::OpBitFieldUExtract,
                signed_extract ? m_type_int : m_type_uint,
                base_lane, offset_lane, count_lane));
        }
        const u32 result =
            m_builder.emit_composite_construct(integer_type, lanes);
        if (!store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, result))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::BFI: {
        // Microsoft Learn bfi (sm5): dest = ((src2 << src1) & bitmask) |
        // (src3 & ~bitmask), where
        //   bitmask = (((1 << src0) - 1) << src1)
        // and both the width (src0) and the offset (src1) are reduced to
        // their low five bits. This is per-component, and a width of 0 must
        // leave src3 untouched — which the mask expression already gives,
        // because (1 << 0) - 1 is zero.
        Sm4Operand dst, width, offset, insert, base;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(width) ||
            !m_decoder.decode_operand(offset) ||
            !m_decoder.decode_operand(insert) ||
            !m_decoder.decode_operand(base)) {
            return fail("BFI: bad operands");
        }
        if (width.modifier != 0u || offset.modifier != 0u ||
            insert.modifier != 0u || base.modifier != 0u) {
            return fail("BFI: unsupported source modifier");
        }
        u32 width4 = 0, offset4 = 0, insert4 = 0, base4 = 0;
        if (!load_vec4(width, width4) || !load_vec4(offset, offset4) ||
            !load_vec4(insert, insert4) || !load_vec4(base, base4)) {
            return false;
        }
        const auto splat = [&](u32 scalar) {
            return m_builder.emit_constant_composite(
                m_type_uvec4, {scalar, scalar, scalar, scalar});
        };
        const u32 five_bits =
            splat(m_builder.emit_constant_int(m_type_uint, 31u));
        width4 = m_builder.emit_bitwise_and(
            m_type_uvec4, m_builder.emit_bitcast(m_type_uvec4, width4),
            five_bits);
        offset4 = m_builder.emit_bitwise_and(
            m_type_uvec4, m_builder.emit_bitcast(m_type_uvec4, offset4),
            five_bits);
        insert4 = m_builder.emit_bitcast(m_type_uvec4, insert4);
        base4 = m_builder.emit_bitcast(m_type_uvec4, base4);
        // (1 << width) - 1, written as ~(~0 << width): identical for every
        // width 0..31, which is all the five-bit reduction can produce, and
        // it needs no integer subtract.
        const u32 all_ones =
            splat(m_builder.emit_constant_int(m_type_uint, 0xFFFFFFFFu));
        const u32 span = m_builder.emit_not(
            m_type_uvec4,
            m_builder.emit_shift_left_logical(m_type_uvec4, all_ones, width4));
        const u32 bitmask = m_builder.emit_shift_left_logical(
            m_type_uvec4, span, offset4);
        const u32 inserted = m_builder.emit_bitwise_and(
            m_type_uvec4,
            m_builder.emit_shift_left_logical(m_type_uvec4, insert4, offset4),
            bitmask);
        const u32 kept = m_builder.emit_bitwise_and(
            m_type_uvec4, base4,
            m_builder.emit_not(m_type_uvec4, bitmask));
        const u32 result = m_builder.emit_bitwise_or(
            m_type_uvec4, inserted, kept);
        if (!store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, result))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::BFREV: {
        // Microsoft bfrev (sm5): component-wise reversal of the 32 bits in
        // src0. Two operands (dest, src0); a bit operation, not a float one.
        Sm4Operand dst, src;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(src)) {
            return fail("BFREV: bad operands");
        }
        if (src.modifier != 0u) {
            return fail("BFREV: unsupported source modifier");
        }
        u32 value = 0;
        if (!load_vec4(src, value)) return false;
        value = m_builder.emit_bitcast(m_type_uvec4, value);
        const u32 reversed = m_builder.emit_unary_integer(
            SpvOp::OpBitReverse, m_type_uvec4, value);
        if (!store_vec4(dst, m_builder.emit_bitcast(m_type_vec4, reversed))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::AND:
    case Sm4Opcode::OR:
    case Sm4Opcode::XOR: {
        // Microsoft Learn and/or/xor (sm4): component-wise bitwise ops on
        // pairs of 32-bit values. Their source syntax has no modifiers.
        Sm4Operand dst, a, b;
        if (!m_decoder.decode_operand(dst) || !m_decoder.decode_operand(a) ||
            !m_decoder.decode_operand(b)) {
            return fail("BITWISE: bad operands");
        }
        if (a.modifier != 0u || b.modifier != 0u) {
            return fail("BITWISE: unsupported source modifier");
        }
        u32 va = 0, vb = 0;
        if (!load_vec4(a, va) || !load_vec4(b, vb)) return false;
        const u32 ua = m_builder.emit_bitcast(m_type_uvec4, va);
        const u32 ub = m_builder.emit_bitcast(m_type_uvec4, vb);
        const u32 bits = opcode == Sm4Opcode::AND
            ? m_builder.emit_bitwise_and(m_type_uvec4, ua, ub)
            : opcode == Sm4Opcode::OR
                ? m_builder.emit_bitwise_or(m_type_uvec4, ua, ub)
                : m_builder.emit_bitwise_xor(m_type_uvec4, ua, ub);
        const u32 as_float = m_builder.emit_bitcast(m_type_vec4, bits);
        if (!store_vec4(dst, as_float)) return false;
        break;
    }

    case Sm4Opcode::NOT: {
        // Microsoft Learn not (sm4): component-wise one's complement of
        // each 32-bit source value. The source syntax has no modifier.
        Sm4Operand dst, src;
        if (!m_decoder.decode_operand(dst) ||
            !m_decoder.decode_operand(src)) {
            return fail("NOT: bad operands");
        }
        if (src.modifier != 0u) {
            return fail("NOT: unsupported source modifier");
        }
        u32 value = 0;
        if (!load_vec4(src, value)) return false;
        value = m_builder.emit_bitcast(m_type_uvec4, value);
        value = m_builder.emit_not(m_type_uvec4, value);
        if (!store_vec4(dst,
                        m_builder.emit_bitcast(m_type_vec4, value))) {
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
