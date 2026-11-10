// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_typed_raw.cpp
/// @brief Typed buffer views read and written as raw memory.
///
/// Learn (D3D12_BUFFER_SRV / D3D12_BUFFER_UAV): a typed buffer view is
/// NumElements elements of its DXGI format from FirstElement, and ld,
/// ld_uav_typed and store_uav_typed convert each element with the format's
/// rules (D3D11.3 3.2 "Data Conversion"). The normal path lets the backend's
/// texture-buffer hardware do that conversion. A view the backend cannot
/// alias as a texture (Metal returns no texture for a placement sparse
/// buffer, at any offset) is marked kTypedBufferRaw in its side-buffer entry
/// and carries a device address and a description of its format instead
/// (dxbc_to_spirv.hpp). This file reads that memory through a
/// PhysicalStorageBuffer pointer and converts the channels itself.
///
/// Conversions (D3D11.3 3.2.3): UNORM n bits to float divides by 2^n - 1;
/// SNORM sign-extends, divides by 2^(n-1) - 1, and the most negative code
/// clamps to -1. Float to UNORM / SNORM clamps to [0, 1] / [-1, 1], NaN is 0,
/// and the product is rounded to the nearest code. UINT / SINT to fewer bits
/// clamp to the target range (3.2.3.13). FLOAT11 / FLOAT10 have no sign:
/// negative values store 0 (3.1.6), and float conversion rounds toward zero.
/// FLOAT16 uses the backend's half conversion, as the texture path does.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

namespace {
constexpr u32 kChannelMask = 0xFFFFu;
}  // namespace

u32 DxbcTranslator::typed_raw_word(bool uav, u32 slot, u32 word) {
    const u32 variable = uav ? m_uav_typed_bias_var : m_srv_typed_bias_var;
    if (variable == 0) return 0;
    const u32 index = descriptor_slot_index(uav, slot);
    if (index == 0) return 0;
    // A heap window element's record is in the heap's side buffer, at the
    // same word offsets (kDescriptorHeapWindowSideWords).
    if (heap_window_key(uav, slot)) return heap_window_side_word(index, word);
    u32 at = m_builder.emit_imul(
        m_type_uint, index,
        m_builder.emit_constant_int(m_type_uint, kTypedBufferEntryWords));
    if (word != 0) {
        at = m_builder.emit_iadd(m_type_uint, at,
                                 m_builder.emit_constant_int(m_type_uint, word));
    }
    return m_builder.emit_load(
        m_type_uint,
        m_builder.emit_access_chain(m_type_ptr_storage_buffer_uint, variable,
                                    {m_const_u0, at}));
}

DxbcTranslator::TypedRawSplit DxbcTranslator::typed_raw_begin(bool uav,
                                                              u32 slot) {
    TypedRawSplit split;
    const u32 entry = typed_raw_word(uav, slot, 0);
    if (entry == 0) return split;
    const u32 raw = m_builder.emit_inotequal(
        m_type_bool,
        m_builder.emit_bitwise_and(
            m_type_uint, entry,
            m_builder.emit_constant_int(m_type_uint, kTypedBufferRaw)),
        m_const_u0);
    split.active = true;
    const u32 raw_block = m_builder.allocate_id();
    split.texture_block = m_builder.allocate_id();
    split.merge = m_builder.allocate_id();
    m_builder.emit_selection_merge(split.merge);
    m_builder.emit_branch_conditional(raw, raw_block, split.texture_block);
    m_builder.emit_label_id(raw_block);
    return split;
}

void DxbcTranslator::typed_raw_else(TypedRawSplit& split, u32 raw_value) {
    if (!split.active) return;
    split.raw_value = raw_value;
    split.raw_end = m_builder.allocate_id();
    m_builder.emit_branch(split.raw_end);
    m_builder.emit_label_id(split.raw_end);
    m_builder.emit_branch(split.merge);
    m_builder.emit_label_id(split.texture_block);
}

u32 DxbcTranslator::typed_raw_end(TypedRawSplit& split, u32 result_type,
                                  u32 texture_value) {
    if (!split.active) return texture_value;
    split.texture_end = m_builder.allocate_id();
    m_builder.emit_branch(split.texture_end);
    m_builder.emit_label_id(split.texture_end);
    m_builder.emit_branch(split.merge);
    m_builder.emit_label_id(split.merge);
    if (result_type == 0) return 0;
    return m_builder.emit_phi(result_type,
                              {{split.raw_value, split.raw_end},
                               {texture_value, split.texture_end}});
}

u32 DxbcTranslator::typed_raw_count(bool uav, u32 slot) {
    return typed_raw_word(uav, slot, 3);
}

u32 DxbcTranslator::typed_raw_pointer(bool uav, u32 slot, u32 byte_lo,
                                      u32 byte_hi) {
    if (m_type_ptr_physical_uint == 0) {
        m_builder.use_physical_storage_buffer();
        m_type_ptr_physical_uint = m_builder.emit_type_pointer(
            SpvStorageClass::PhysicalStorageBuffer, m_type_uint);
        // OpPtrAccessChain needs the stride of the pointee (SPIR-V 2.16.2).
        m_builder.emit_decorate(m_type_ptr_physical_uint,
                                SpvDecoration::ArrayStride, 4u);
    }
    if (m_type_add_carry == 0) {
        m_type_add_carry = m_builder.emit_type_struct({m_type_uint, m_type_uint});
    }
    const u32 lo = typed_raw_word(uav, slot, 1);
    const u32 hi = typed_raw_word(uav, slot, 2);
    const u32 sum = m_builder.allocate_id();
    m_builder.emit(SpvOp::OpIAddCarry, {m_type_add_carry, sum, lo, byte_lo});
    const u32 sum_lo = m_builder.emit_composite_extract(m_type_uint, sum, {0});
    const u32 carry = m_builder.emit_composite_extract(m_type_uint, sum, {1});
    const u32 sum_hi = m_builder.emit_iadd(
        m_type_uint, m_builder.emit_iadd(m_type_uint, hi, byte_hi), carry);
    const u32 address =
        m_builder.emit_composite_construct(m_type_uvec2, {sum_lo, sum_hi});
    const u32 base = m_builder.emit_bitcast(m_type_ptr_physical_uint, address);
    // Element 0 of the pointer: the MSL backend makes that an lvalue
    // (`p[0]`), which its atomics take the address of; a bare bitcast
    // pointer is an rvalue there.
    const u32 element = m_builder.allocate_id();
    m_builder.emit(SpvOp::OpPtrAccessChain,
                   {m_type_ptr_physical_uint, element, base, m_const_u0});
    return element;
}

namespace {

/// Per-channel addressing shared by load and store: the 32-bit word that
/// holds the channel, the channel's shift in it, its width and kind, and
/// the all-ones value of its width.
struct RawChannel {
    u32 pointer{0};
    u32 shift{0};
    u32 width{0};
    u32 kind{0};
    u32 ones{0};
    u32 absent{0};
};

}  // namespace

u32 DxbcTranslator::typed_raw_load(bool uav, u32 slot, u32 element,
                                   u32 return_type, u32 result_type) {
    auto& b = m_builder;
    const auto cu = [&](u32 v) { return b.emit_constant_int(m_type_uint, v); };
    const auto cf = [&](float v) { return b.emit_constant_float(m_type_float, v); };
    const u32 entry = typed_raw_word(uav, slot, 0);
    const u32 in_view = b.emit_integer_compare(
        SpvOp::OpULessThan, m_type_bool, element, typed_raw_count(uav, slot));
    const u32 read_block = b.allocate_id();
    const u32 read_end = b.allocate_id();
    const u32 outside_block = b.allocate_id();
    const u32 merge = b.allocate_id();
    b.emit_selection_merge(merge);
    b.emit_branch_conditional(in_view, read_block, outside_block);
    b.emit_label_id(read_block);

    const bool integer = return_type == 4u || return_type == 3u;
    const u32 element_bytes = typed_raw_word(uav, slot, 4);
    if (m_type_add_carry == 0) {
        m_type_add_carry = b.emit_type_struct({m_type_uint, m_type_uint});
    }
    const u32 product = b.emit_umul_extended(m_type_add_carry, element,
                                             element_bytes);
    const u32 base_lo = b.emit_composite_extract(m_type_uint, product, {0});
    const u32 base_hi = b.emit_composite_extract(m_type_uint, product, {1});
    const std::array<u32, 2> channel_words{typed_raw_word(uav, slot, 5),
                                           typed_raw_word(uav, slot, 6)};
    std::array<u32, 4> components{};
    u32 shared_exponent = 0;
    for (u32 c = 0; c < 4; ++c) {
        const u32 packed = channel_words[c / 2];
        const u32 desc =
            (c & 1u) == 0
                ? b.emit_bitwise_and(m_type_uint, packed, cu(kChannelMask))
                : b.emit_shift_right_logical(m_type_uint, packed, cu(16));
        const u32 offset = b.emit_bitwise_and(m_type_uint, desc, cu(0x7F));
        const u32 width = b.emit_bitwise_and(
            m_type_uint, b.emit_shift_right_logical(m_type_uint, desc, cu(7)),
            cu(0x3F));
        const u32 kind = b.emit_bitwise_and(
            m_type_uint, b.emit_shift_right_logical(m_type_uint, desc, cu(13)),
            cu(7));
        // The byte holding the channel's first bit, then its aligned word.
        // A channel never crosses a word: elements of one or two bytes sit
        // inside one word, wider ones are multiples of four bytes.
        if (m_type_add_carry == 0) {
            m_type_add_carry = b.emit_type_struct({m_type_uint, m_type_uint});
        }
        const u32 byte = b.allocate_id();
        b.emit(SpvOp::OpIAddCarry,
               {m_type_add_carry, byte, base_lo,
                b.emit_shift_right_logical(m_type_uint, offset, cu(3))});
        const u32 byte_lo = b.emit_composite_extract(m_type_uint, byte, {0});
        const u32 byte_hi = b.emit_iadd(
            m_type_uint, base_hi,
            b.emit_composite_extract(m_type_uint, byte, {1}));
        const u32 shift = b.emit_iadd(
            m_type_uint,
            b.emit_shift_left_logical(
                m_type_uint, b.emit_bitwise_and(m_type_uint, byte_lo, cu(3)),
                cu(3)),
            b.emit_bitwise_and(m_type_uint, offset, cu(7)));
        const u32 pointer = typed_raw_pointer(
            uav, slot, b.emit_bitwise_and(m_type_uint, byte_lo, cu(~3u)),
            byte_hi);
        const u32 word = b.emit_load_aligned(m_type_uint, pointer, 4);
        const u32 absent = b.emit_integer_compare(SpvOp::OpIEqual, m_type_bool,
                                                  width, m_const_u0);
        const u32 width1 = b.emit_select(m_type_uint, absent, cu(1), width);
        const u32 ones = b.emit_shift_right_logical(
            m_type_uint, cu(~0u), b.emit_isub(m_type_uint, cu(32), width1));
        const u32 raw = b.emit_bit_field_extract(SpvOp::OpBitFieldUExtract,
                                                 m_type_uint, word, shift,
                                                 width1);
        const u32 sraw = b.emit_bit_field_extract(SpvOp::OpBitFieldSExtract,
                                                  m_type_uint, word, shift,
                                                  width1);
        // c / d rounded to the nearest float: the quotient from the
        // reciprocal, then one residual step with fused multiply-adds (the
        // device's division is not correctly rounded; the texture path's
        // conversion is). Exact for the integer codes and divisors here.
        const auto quotient = [&](u32 c, u32 d) {
            const u32 r = b.emit_fdiv(m_type_float, cf(1.0f), d);
            const u32 q = b.emit_fmul(m_type_float, c, r);
            const u32 e = b.emit_ext_inst(
                m_type_float, m_glsl_std450, kGlslStd450Fma,
                {b.emit_fnegate(m_type_float, q), d, c});
            return b.emit_ext_inst(m_type_float, m_glsl_std450, kGlslStd450Fma,
                                   {e, r, q});
        };
        // UNORM: code / (2^n - 1).
        const u32 unorm = b.emit_bitcast(
            m_type_uint,
            quotient(b.emit_convert_u_to_f(m_type_float, raw),
                     b.emit_convert_u_to_f(m_type_float, ones)));
        // SNORM: code / (2^(n-1) - 1), the most negative code clamped to -1.
        const u32 snorm_scale = b.emit_convert_u_to_f(
            m_type_float, b.emit_shift_right_logical(m_type_uint, ones, cu(1)));
        const u32 snorm = b.emit_bitcast(
            m_type_uint,
            b.emit_ext_inst(
                m_type_float, m_glsl_std450, kGlslStd450FMax,
                {quotient(b.emit_convert_s_to_f(
                              m_type_float, b.emit_bitcast(m_type_int, sraw)),
                          snorm_scale),
                 cf(-1.0f)}));
        const u32 half = b.emit_bitcast(
            m_type_uint,
            b.emit_composite_extract(
                m_type_float,
                b.emit_ext_inst(m_type_vec2, m_glsl_std450,
                                kGlslStd450UnpackHalf2x16, {raw}),
                {0}));
        // FLOAT11 / FLOAT10: 5 exponent bits (bias 15), 6 or 5 mantissa bits.
        const u32 mbits = b.emit_isub(m_type_uint, width1, cu(5));
        const u32 exponent = b.emit_shift_right_logical(m_type_uint, raw, mbits);
        const u32 mantissa = b.emit_bitwise_and(
            m_type_uint, raw,
            b.emit_isub(m_type_uint, b.emit_shift_left_logical(m_type_uint, cu(1), mbits),
                        cu(1)));
        const u32 mantissa_up = b.emit_shift_left_logical(
            m_type_uint, mantissa, b.emit_isub(m_type_uint, cu(23), mbits));
        const u32 normal = b.emit_bitwise_or(
            m_type_uint,
            b.emit_shift_left_logical(
                m_type_uint, b.emit_iadd(m_type_uint, exponent, cu(112)),
                cu(23)),
            mantissa_up);
        const u32 denormal = b.emit_bitcast(
            m_type_uint,
            b.emit_ext_inst(
                m_type_float, m_glsl_std450, 53u /* Ldexp */,
                {b.emit_convert_u_to_f(m_type_float, mantissa),
                 b.emit_snegate(m_type_int,
                                b.emit_bitcast(m_type_int,
                                               b.emit_iadd(m_type_uint, mbits,
                                                           cu(14))))}));
        const u32 special =
            b.emit_bitwise_or(m_type_uint, cu(0x7F800000u), mantissa_up);
        const u32 small_float = b.emit_select(
            m_type_uint,
            b.emit_integer_compare(SpvOp::OpIEqual, m_type_bool, exponent,
                                   m_const_u0),
            denormal,
            b.emit_select(m_type_uint,
                          b.emit_integer_compare(SpvOp::OpIEqual, m_type_bool,
                                                 exponent, cu(31)),
                          special, normal));
        const auto is_kind = [&](TypedRawKind k) {
            return b.emit_integer_compare(SpvOp::OpIEqual, m_type_bool, kind,
                                          cu(static_cast<u32>(k)));
        };
        u32 bits = small_float;
        bits = b.emit_select(m_type_uint, is_kind(TypedRawKind::Float16), half,
                             bits);
        bits = b.emit_select(m_type_uint, is_kind(TypedRawKind::Float), raw,
                             bits);
        bits = b.emit_select(m_type_uint, is_kind(TypedRawKind::Sint), sraw,
                             bits);
        bits = b.emit_select(m_type_uint, is_kind(TypedRawKind::Uint), raw,
                             bits);
        bits = b.emit_select(m_type_uint, is_kind(TypedRawKind::Snorm), snorm,
                             bits);
        bits = b.emit_select(m_type_uint, is_kind(TypedRawKind::Unorm), unorm,
                             bits);
        // R9G9B9E5_SHAREDEXP (kTypedRawSharedExponent): no sign, no implied
        // one; mantissa * 2^(e - 15 - 9) for the 5-bit exponent e in bits
        // 27-31 (Learn DXGI_FORMAT: a 5-bit biased (15) exponent shared by
        // three 9-bit mantissas).
        if (shared_exponent == 0) {
            shared_exponent = b.emit_inotequal(
                m_type_bool,
                b.emit_bitwise_and(m_type_uint, typed_raw_word(uav, slot, 7),
                                   cu(kTypedRawSharedExponent)),
                m_const_u0);
        }
        const u32 shared = b.emit_bitcast(
            m_type_uint,
            b.emit_ext_inst(
                m_type_float, m_glsl_std450, 53u /* Ldexp */,
                {b.emit_convert_u_to_f(m_type_float, raw),
                 b.emit_isub(
                     m_type_int,
                     b.emit_bitcast(
                         m_type_int,
                         b.emit_bitwise_and(
                             m_type_uint,
                             b.emit_shift_right_logical(m_type_uint, word,
                                                        cu(27)),
                             cu(31))),
                     b.emit_constant_int(m_type_int, 24u))}));
        bits = b.emit_select(m_type_uint, shared_exponent, shared, bits);
        // D3D11.3 19.1.3.3: a component the format lacks reads 0, alpha 1.
        const u32 missing =
            c == 3 ? (integer ? cu(1u) : cu(0x3F800000u)) : m_const_u0;
        components[c] = b.emit_select(m_type_uint, absent, missing, bits);
    }
    const u32 read = b.emit_bitcast(
        result_type,
        b.emit_composite_construct(m_type_uvec4,
                                   {components[0], components[1],
                                    components[2], components[3]}));
    b.emit_branch(read_end);
    b.emit_label_id(read_end);
    b.emit_branch(merge);
    b.emit_label_id(outside_block);
    const u32 outside = typed_buffer_out_of_view(result_type, return_type, entry);
    b.emit_branch(merge);
    b.emit_label_id(merge);
    const u32 value =
        b.emit_phi(result_type, {{read, read_end}, {outside, outside_block}});
    if (uav) return value;
    // Learn (D3D12_SHADER_COMPONENT_MAPPING): an SRV's
    // Shader4ComponentMapping picks each returned component from the
    // view's R, G, B, A (0-3) or forces 0 (4) or 1 (5), three bits per
    // component from bit 0. Word 7 holds it (the default mapping when the
    // view has none), applied to every value the view returns, as the
    // texture path's swizzle is.
    const u32 mapping = typed_raw_word(false, slot, 7);
    const u32 bits = b.emit_bitcast(m_type_uvec4, value);
    std::array<u32, 4> source{};
    for (u32 c = 0; c < 4; ++c) {
        source[c] = b.emit_composite_extract(m_type_uint, bits, {c});
    }
    const u32 one = integer ? cu(1u) : cu(0x3F800000u);
    std::array<u32, 4> mapped{};
    for (u32 c = 0; c < 4; ++c) {
        const u32 select = b.emit_bitwise_and(
            m_type_uint, b.emit_shift_right_logical(m_type_uint, mapping, cu(3 * c)),
            cu(7));
        const auto is = [&](u32 v) {
            return b.emit_integer_compare(SpvOp::OpIEqual, m_type_bool, select,
                                          cu(v));
        };
        u32 picked = m_const_u0;  // FORCE_VALUE_0 (4)
        picked = b.emit_select(m_type_uint, is(5u), one, picked);
        for (u32 from = 0; from < 4; ++from) {
            picked = b.emit_select(m_type_uint, is(from), source[from], picked);
        }
        mapped[c] = picked;
    }
    return b.emit_bitcast(
        result_type,
        b.emit_composite_construct(m_type_uvec4,
                                   {mapped[0], mapped[1], mapped[2], mapped[3]}));
}

u32 DxbcTranslator::typed_raw_atomic(u32 slot, u32 element, SpvOp op,
                                     bool compares, u32 operand,
                                     u32 comparator, bool leader_gated,
                                     u32 scope, u32 semantics) {
    auto& b = m_builder;
    // Learn (Interlocked*): an atomic on a typed UAV acts on its 32-bit
    // R32_UINT / R32_SINT element. An element at or past NumElements runs
    // no atomic: nothing is written and the result is 0, as on the
    // texture path (dxbc_to_spirv_op_atomic.cpp).
    const u32 in_view = b.emit_integer_compare(
        SpvOp::OpULessThan, m_type_bool, element, typed_raw_count(true, slot));
    const u32 body = b.allocate_id();
    const u32 body_end = b.allocate_id();
    const u32 outside = b.allocate_id();
    const u32 merge = b.allocate_id();
    b.emit_selection_merge(merge);
    b.emit_branch_conditional(in_view, body, outside);
    b.emit_label_id(body);
    if (m_type_add_carry == 0) {
        m_type_add_carry = b.emit_type_struct({m_type_uint, m_type_uint});
    }
    const u32 product = b.emit_umul_extended(m_type_add_carry, element,
                                             typed_raw_word(true, slot, 4));
    const u32 pointer = typed_raw_pointer(
        true, slot, b.emit_composite_extract(m_type_uint, product, {0}),
        b.emit_composite_extract(m_type_uint, product, {1}));
    u32 previous = 0;
    if (compares) {
        previous = b.emit_atomic_compare_exchange(m_type_uint, pointer, scope,
                                                  semantics, semantics,
                                                  operand, comparator);
    } else if (leader_gated && op == SpvOp::OpAtomicExchange) {
        const u32 exchange_block = b.allocate_id();
        const u32 read_block = b.allocate_id();
        const u32 exchanged_merge = b.allocate_id();
        b.emit_selection_merge(exchanged_merge);
        b.emit_branch_conditional(load_uav_leader(), exchange_block, read_block);
        b.emit_label_id(exchange_block);
        const u32 exchanged =
            b.emit_atomic_rmw(op, m_type_uint, pointer, scope, semantics, operand);
        b.emit_branch(exchanged_merge);
        b.emit_label_id(read_block);
        const u32 read = b.emit_atomic_compare_exchange(
            m_type_uint, pointer, scope, semantics, semantics, m_const_u0,
            m_const_u0);
        b.emit_branch(exchanged_merge);
        b.emit_label_id(exchanged_merge);
        previous = b.emit_phi(m_type_uint, {{exchanged, exchange_block},
                                            {read, read_block}});
    } else {
        previous =
            b.emit_atomic_rmw(op, m_type_uint, pointer, scope, semantics, operand);
    }
    b.emit_branch(body_end);
    b.emit_label_id(body_end);
    b.emit_branch(merge);
    b.emit_label_id(outside);
    b.emit_branch(merge);
    b.emit_label_id(merge);
    return b.emit_phi(m_type_uint,
                      {{previous, body_end}, {m_const_u0, outside}});
}

void DxbcTranslator::typed_raw_store(u32 slot, u32 element, u32 texel,
                                     u32 return_type) {
    (void)return_type;
    auto& b = m_builder;
    const auto cu = [&](u32 v) { return b.emit_constant_int(m_type_uint, v); };
    const auto cf = [&](float v) { return b.emit_constant_float(m_type_float, v); };
    // Learn store_uav_typed: an element past the view writes nothing.
    const u32 in_view = b.emit_integer_compare(
        SpvOp::OpULessThan, m_type_bool, element, typed_raw_count(true, slot));
    const u32 write_block = b.allocate_id();
    const u32 merge = b.allocate_id();
    b.emit_selection_merge(merge);
    b.emit_branch_conditional(in_view, write_block, merge);
    b.emit_label_id(write_block);

    const u32 element_bytes = typed_raw_word(true, slot, 4);
    if (m_type_add_carry == 0) {
        m_type_add_carry = b.emit_type_struct({m_type_uint, m_type_uint});
    }
    const u32 product = b.emit_umul_extended(m_type_add_carry, element,
                                             element_bytes);
    const u32 base_lo = b.emit_composite_extract(m_type_uint, product, {0});
    const u32 base_hi = b.emit_composite_extract(m_type_uint, product, {1});
    const std::array<u32, 2> channel_words{typed_raw_word(true, slot, 5),
                                           typed_raw_word(true, slot, 6)};
    if (m_type_add_carry == 0) {
        m_type_add_carry = b.emit_type_struct({m_type_uint, m_type_uint});
    }
    const u32 scope = cu(1u);      // Device
    const u32 semantics = cu(0u);  // Relaxed
    for (u32 c = 0; c < 4; ++c) {
        const u32 packed = channel_words[c / 2];
        const u32 desc =
            (c & 1u) == 0
                ? b.emit_bitwise_and(m_type_uint, packed, cu(kChannelMask))
                : b.emit_shift_right_logical(m_type_uint, packed, cu(16));
        const u32 offset = b.emit_bitwise_and(m_type_uint, desc, cu(0x7F));
        const u32 width = b.emit_bitwise_and(
            m_type_uint, b.emit_shift_right_logical(m_type_uint, desc, cu(7)),
            cu(0x3F));
        const u32 kind = b.emit_bitwise_and(
            m_type_uint, b.emit_shift_right_logical(m_type_uint, desc, cu(13)),
            cu(7));
        const u32 byte = b.allocate_id();
        b.emit(SpvOp::OpIAddCarry,
               {m_type_add_carry, byte, base_lo,
                b.emit_shift_right_logical(m_type_uint, offset, cu(3))});
        const u32 byte_lo = b.emit_composite_extract(m_type_uint, byte, {0});
        const u32 byte_hi = b.emit_iadd(
            m_type_uint, base_hi,
            b.emit_composite_extract(m_type_uint, byte, {1}));
        const u32 shift = b.emit_iadd(
            m_type_uint,
            b.emit_shift_left_logical(
                m_type_uint, b.emit_bitwise_and(m_type_uint, byte_lo, cu(3)),
                cu(3)),
            b.emit_bitwise_and(m_type_uint, offset, cu(7)));
        const u32 absent = b.emit_integer_compare(SpvOp::OpIEqual, m_type_bool,
                                                  width, m_const_u0);
        const u32 width1 = b.emit_select(m_type_uint, absent, cu(1), width);
        const u32 ones = b.emit_shift_right_logical(
            m_type_uint, cu(~0u), b.emit_isub(m_type_uint, cu(32), width1));
        const u32 value = b.emit_composite_extract(m_type_uint, texel, {c});
        const u32 f = b.emit_bitcast(m_type_float, value);
        const u32 nan = b.emit_is_nan(m_type_bool, f);
        const u32 f0 = b.emit_select(m_type_float, nan, cf(0.0f), f);
        // UNORM: clamp to [0, 1], scale, round to the nearest code.
        const u32 unorm = b.emit_convert_f_to_u(
            m_type_uint,
            b.emit_ext_inst(
                m_type_float, m_glsl_std450, kGlslStd450Floor,
                {b.emit_fadd(
                    m_type_float,
                    b.emit_fmul(m_type_float,
                                b.emit_ext_inst(m_type_float, m_glsl_std450,
                                                43u /* FClamp */,
                                                {f0, cf(0.0f), cf(1.0f)}),
                                b.emit_convert_u_to_f(m_type_float, ones)),
                    cf(0.5f))}));
        // SNORM: clamp to [-1, 1], scale by 2^(n-1) - 1, round to nearest.
        const u32 half_range =
            b.emit_shift_right_logical(m_type_uint, ones, cu(1));
        const u32 scaled = b.emit_fmul(
            m_type_float,
            b.emit_ext_inst(m_type_float, m_glsl_std450, 43u /* FClamp */,
                            {f0, cf(-1.0f), cf(1.0f)}),
            b.emit_convert_u_to_f(m_type_float, half_range));
        const u32 rounded = b.emit_ext_inst(
            m_type_float, m_glsl_std450, kGlslStd450Trunc,
            {b.emit_fadd(m_type_float, scaled,
                         b.emit_select(m_type_float,
                                       b.emit_ford_lt(m_type_bool, scaled,
                                                      cf(0.0f)),
                                       cf(-0.5f), cf(0.5f)))});
        const u32 snorm = b.emit_bitcast(
            m_type_uint, b.emit_convert_f_to_s(m_type_int, rounded));
        // UINT / SINT to fewer bits clamp (D3D11.3 3.2.3.13).
        const u32 uint_v = b.emit_ext_inst(m_type_uint, m_glsl_std450,
                                           38u /* UMin */, {value, ones});
        const u32 smax = b.emit_bitcast(m_type_int, half_range);
        const u32 smin = b.emit_snegate(
            m_type_int, b.emit_iadd(m_type_int, smax,
                                    b.emit_constant_int(m_type_int, 1u)));
        const u32 sint_v = b.emit_bitcast(
            m_type_uint,
            b.emit_ext_inst(m_type_int, m_glsl_std450, 45u /* SClamp */,
                            {b.emit_bitcast(m_type_int, value), smin, smax}));
        const u32 half = b.emit_ext_inst(
            m_type_uint, m_glsl_std450, kGlslStd450PackHalf2x16,
            {b.emit_composite_construct(m_type_vec2, {f, cf(0.0f)})});
        // FLOAT11 / FLOAT10: no sign (negative stores 0), round toward zero,
        // past the largest finite value the largest finite value; Inf and NaN
        // keep their encodings.
        const u32 mbits = b.emit_isub(m_type_uint, width1, cu(5));
        const u32 mmask = b.emit_isub(
            m_type_uint, b.emit_shift_left_logical(m_type_uint, cu(1), mbits),
            cu(1));
        const u32 positive = b.emit_ext_inst(m_type_float, m_glsl_std450,
                                             kGlslStd450FMax, {f, cf(0.0f)});
        const u32 pbits = b.emit_bitcast(m_type_uint, positive);
        const u32 biased = b.emit_bitcast(
            m_type_int, b.emit_shift_right_logical(m_type_uint, pbits, cu(23)));
        const u32 exponent = b.emit_isub(m_type_int, biased,
                                         b.emit_constant_int(m_type_int, 112u));
        const u32 normal = b.emit_bitwise_or(
            m_type_uint,
            b.emit_shift_left_logical(m_type_uint,
                                      b.emit_bitcast(m_type_uint, exponent),
                                      mbits),
            b.emit_shift_right_logical(
                m_type_uint, b.emit_bitwise_and(m_type_uint, pbits, cu(0x7FFFFF)),
                b.emit_isub(m_type_uint, cu(23), mbits)));
        const u32 denormal = b.emit_ext_inst(
            m_type_uint, m_glsl_std450, 38u /* UMin */,
            {b.emit_convert_f_to_u(
                 m_type_uint,
                 b.emit_ext_inst(m_type_float, m_glsl_std450, 53u /* Ldexp */,
                                 {positive,
                                  b.emit_bitcast(m_type_int,
                                                 b.emit_iadd(m_type_uint, mbits,
                                                             cu(14)))})),
             mmask});
        const u32 max_finite = b.emit_bitwise_or(
            m_type_uint, b.emit_shift_left_logical(m_type_uint, cu(30), mbits),
            mmask);
        const u32 infinity = b.emit_shift_left_logical(m_type_uint, cu(31), mbits);
        u32 small_float = b.emit_select(
            m_type_uint,
            b.emit_integer_compare(SpvOp::OpSLessThanEqual, m_type_bool,
                                   exponent,
                                   b.emit_constant_int(m_type_int, 0u)),
            denormal,
            b.emit_select(m_type_uint,
                          b.emit_integer_compare(
                              SpvOp::OpSGreaterThanEqual, m_type_bool, exponent,
                              b.emit_constant_int(m_type_int, 31u)),
                          max_finite, normal));
        small_float = b.emit_select(
            m_type_uint,
            b.emit_integer_compare(SpvOp::OpIEqual, m_type_bool, pbits,
                                   cu(0x7F800000u)),
            infinity, small_float);
        small_float = b.emit_select(
            m_type_uint, nan, b.emit_bitwise_or(m_type_uint, infinity, mmask),
            small_float);
        const auto is_kind = [&](TypedRawKind k) {
            return b.emit_integer_compare(SpvOp::OpIEqual, m_type_bool, kind,
                                          cu(static_cast<u32>(k)));
        };
        u32 code = small_float;
        code = b.emit_select(m_type_uint, is_kind(TypedRawKind::Float16), half,
                             code);
        code = b.emit_select(m_type_uint, is_kind(TypedRawKind::Float), value,
                             code);
        code = b.emit_select(m_type_uint, is_kind(TypedRawKind::Sint), sint_v,
                             code);
        code = b.emit_select(m_type_uint, is_kind(TypedRawKind::Uint), uint_v,
                             code);
        code = b.emit_select(m_type_uint, is_kind(TypedRawKind::Snorm), snorm,
                             code);
        code = b.emit_select(m_type_uint, is_kind(TypedRawKind::Unorm), unorm,
                             code);
        // Only this channel's bits change: the clear and the set are
        // atomics, so a neighbouring element of a narrow format that another
        // invocation stores into the same word keeps its own bits.
        const u32 mask = b.emit_select(
            m_type_uint, absent, m_const_u0,
            b.emit_shift_left_logical(m_type_uint, ones, shift));
        const u32 field = b.emit_bitwise_and(
            m_type_uint,
            b.emit_shift_left_logical(
                m_type_uint, b.emit_bitwise_and(m_type_uint, code, ones), shift),
            mask);
        const u32 pointer = typed_raw_pointer(
            true, slot, b.emit_bitwise_and(m_type_uint, byte_lo, cu(~3u)),
            byte_hi);
        (void)b.emit_atomic_rmw(SpvOp::OpAtomicAnd, m_type_uint, pointer, scope,
                                semantics,
                                b.emit_not(m_type_uint, mask));
        (void)b.emit_atomic_rmw(SpvOp::OpAtomicOr, m_type_uint, pointer, scope,
                                semantics, field);
    }
    b.emit_branch(merge);
    b.emit_label_id(merge);
}

}  // namespace vitreo::graphics::dxbc_spirv_detail
