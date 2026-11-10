// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_vertex_pull.cpp
/// @brief Vertex pulling: the vertex shader fetches its own inputs.
///
/// D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT (and D3D11's) is 32 input
/// slots, while Metal's vertex-stage buffer table leaves only the indices
/// above the binding windows for vertex streams. A layout reading more slots
/// than that is fetched here instead: every slot is one entry of the pull
/// table ({address lo, address hi, stride, size}, VERTEX_PULL_TABLE_*), and
/// a prologue reads and converts each input-layout element into its v#
/// register before the body runs.
///
/// Fetch index (Learn, D3D12_INPUT_ELEMENT_DESC; D3D11.3 8.3.1 pseudocode):
/// per-vertex data at the vertex id the input assembler generates (the
/// non-indexed vertex number from StartVertexLocation, or the index value
/// plus BaseVertexLocation -- Metal's [[vertex_id]]); per-instance data at
/// StartInstanceLocation + instance / InstanceDataStepRate, a rate of 0
/// never advancing. A stride of 0 reads the element at the view's start for
/// every vertex and instance, which the same address arithmetic gives.
///
/// Conversions are D3D11.3 3.2 "Data Conversion": UNORM n divides by
/// 2^n - 1, SNORM by 2^(n-1) - 1 with the most negative code clamping to -1,
/// FLOAT16 / FLOAT11 / FLOAT10 widen exactly, integers widen (sign-extended
/// for SINT). A channel the format lacks is the default (0, 0, 0, 1): 1.0 for
/// a float format, integer 1 for an integer one. An element not wholly
/// inside the slot's size reads 0 in every channel it has (measured,
/// tools/oracle_probe/d3d12_a51_ia_indirect_probe slots32).

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

u32 DxbcTranslator::vertex_pull_element(const VertexPullElement& element) {
    auto& b = m_builder;
    const auto cu = [&](u32 v) { return b.emit_constant_int(m_type_uint, v); };
    const auto cf = [&](float v) {
        return b.emit_constant_float(m_type_float, v);
    };
    if (m_type_ptr_physical_uint == 0) {
        b.use_physical_storage_buffer();
        m_type_ptr_physical_uint = b.emit_type_pointer(
            SpvStorageClass::PhysicalStorageBuffer, m_type_uint);
        b.emit_decorate(m_type_ptr_physical_uint, SpvDecoration::ArrayStride,
                        4u);
    }
    if (m_type_add_carry == 0) {
        m_type_add_carry = b.emit_type_struct({m_type_uint, m_type_uint});
    }
    const auto table_word = [&](u32 word) {
        return b.emit_load(
            m_type_uint,
            b.emit_access_chain(
                m_type_ptr_storage_buffer_uint, m_vertex_pull_table,
                {m_const_u0,
                 cu(element.slot * VERTEX_PULL_TABLE_ENTRY_WORDS + word)}));
    };
    // (lo, hi) + 32-bit value.
    struct U64 {
        u32 lo;
        u32 hi;
    };
    const auto add64 = [&](U64 a, u32 value) -> U64 {
        const u32 sum = b.allocate_id();
        b.set_section(SpirvBuilder::Section::Functions);
        b.emit(SpvOp::OpIAddCarry, {m_type_add_carry, sum, a.lo, value});
        const u32 lo = b.emit_composite_extract(m_type_uint, sum, {0});
        const u32 carry = b.emit_composite_extract(m_type_uint, sum, {1});
        return {lo, b.emit_iadd(m_type_uint, a.hi, carry)};
    };

    // The fetch index.
    u32 index = 0;
    if (!element.per_instance) {
        index = b.emit_load(m_type_uint,
                            builtin_input(SpvBuiltIn::VertexIndex,
                                          "vitreo_pull_vertex_index"));
    } else {
        const auto header_word = [&](u32 word) {
            return b.emit_load(
                m_type_uint,
                b.emit_access_chain(
                    m_type_ptr_storage_buffer_uint, m_vertex_pull_table,
                    {m_const_u0,
                     cu(VERTEX_PULL_TABLE_HEADER *
                            VERTEX_PULL_TABLE_ENTRY_WORDS +
                        word)}));
        };
        const u32 draw_base = b.emit_load(
            m_type_uint,
            builtin_input(SpvBuiltIn::BaseInstance, "vitreo_base_instance"));
        // StartInstanceLocation: the draw's base instance, or the one a
        // per-instance capture names (VERTEX_PULL_TABLE_HEADER).
        const u32 base = b.emit_select(
            m_type_uint,
            b.emit_inotequal(m_type_bool, header_word(0), m_const_u0),
            header_word(1), draw_base);
        if (element.step_rate == 0) {
            index = base;
        } else {
            const u32 instance = b.emit_iadd(
                m_type_uint,
                b.emit_isub(
                    m_type_uint,
                    b.emit_load(m_type_uint,
                                builtin_input(SpvBuiltIn::InstanceIndex,
                                              "vitreo_pull_instance_index")),
                    draw_base),
                header_word(2));
            index = b.emit_iadd(
                m_type_uint, base,
                element.step_rate == 1
                    ? instance
                    : b.emit_udiv(m_type_uint, instance, cu(element.step_rate)));
        }
    }
    const u32 address_lo = table_word(0);
    const u32 address_hi = table_word(1);
    const u32 stride = table_word(2);
    const u32 size = table_word(3);
    // Byte of the element from the slot's address, 64-bit.
    const u32 product = b.emit_umul_extended(m_type_add_carry, index, stride);
    U64 rel{b.emit_composite_extract(m_type_uint, product, {0}),
            b.emit_composite_extract(m_type_uint, product, {1})};
    rel = add64(rel, cu(element.offset));
    // rel.hi of the sum can only grow past 0 by the carry; a wrap of the
    // 32-bit product's own high word into the add is impossible (< 2^32 *
    // 2^32 fits in 64 bits, and the offset is < 2^32).
    const U64 address = [&] {
        const u32 sum = b.allocate_id();
        b.set_section(SpirvBuilder::Section::Functions);
        b.emit(SpvOp::OpIAddCarry, {m_type_add_carry, sum, address_lo, rel.lo});
        const u32 lo = b.emit_composite_extract(m_type_uint, sum, {0});
        const u32 carry = b.emit_composite_extract(m_type_uint, sum, {1});
        return U64{lo, b.emit_iadd(m_type_uint,
                                   b.emit_iadd(m_type_uint, address_hi, rel.hi),
                                   carry)};
    }();
    const u32 rel_small = b.emit_integer_compare(SpvOp::OpIEqual, m_type_bool,
                                                 rel.hi, m_const_u0);

    // D3D12 (measured, d3d12_a51 slots32 on WARP and an RTX 4080): an
    // element that does not lie wholly inside the view's SizeInBytes reads
    // 0 in every channel it has -- one ending half-way through the view
    // too -- and the format's defaults in the channels it lacks.
    const u32 element_bytes =
        (element.kind == VertexPullKind::Float || element.kind == VertexPullKind::Half ||
         element.kind == VertexPullKind::UInt || element.kind == VertexPullKind::SInt ||
         element.kind == VertexPullKind::UNorm || element.kind == VertexPullKind::SNorm)
            ? element.components * (element.bits / 8u)
        : (element.kind == VertexPullKind::UNormB5G6R5 ||
           element.kind == VertexPullKind::UNormB5G5R5A1 ||
           element.kind == VertexPullKind::UNormB4G4R4A4)
            ? 2u
            : 4u;
    // rel.lo + element_bytes <= size, without overflow.
    const u32 inside = b.emit_logical(
        SpvOp::OpLogicalAnd, m_type_bool,
        b.emit_logical(
            SpvOp::OpLogicalAnd, m_type_bool, rel_small,
            b.emit_integer_compare(SpvOp::OpUGreaterThanEqual, m_type_bool,
                                   size, cu(element_bytes))),
        b.emit_integer_compare(SpvOp::OpULessThanEqual, m_type_bool, rel.lo,
                               b.emit_isub(m_type_uint, size,
                                           cu(element_bytes))));
    // The `bytes` (1..4) little-endian bytes at element byte `delta`, or 0
    // when the element is outside the view.
    const auto load_word = [&](U64 at) {
        const u32 aligned = b.emit_bitwise_and(m_type_uint, at.lo, cu(~3u));
        const u32 pointer = b.emit_bitcast(
            m_type_ptr_physical_uint,
            b.emit_composite_construct(m_type_uvec2, {aligned, at.hi}));
        return b.emit_load_aligned(m_type_uint, pointer, 4u);
    };
    const auto read = [&](u32 delta, u32 bytes) -> u32 {
        const u32 read_block = b.allocate_id();
        const u32 outside_block = b.allocate_id();
        const u32 merge = b.allocate_id();
        const u32 from = b.allocate_id();
        b.emit_selection_merge(merge);
        b.emit_branch_conditional(inside, read_block, outside_block);
        b.emit_label_id(read_block);
        const U64 first = delta == 0 ? address : add64(address, cu(delta));
        const U64 last = bytes == 1 ? first : add64(first, cu(bytes - 1u));
        const u32 shift = b.emit_imul(
            m_type_uint, b.emit_bitwise_and(m_type_uint, first.lo, cu(3u)),
            cu(8u));
        const u32 w0 = load_word(first);
        const u32 w1 = load_word(last);
        const u32 aligned_start = b.emit_integer_compare(
            SpvOp::OpIEqual, m_type_bool, shift, m_const_u0);
        const u32 low = b.emit_shift_right_logical(m_type_uint, w0, shift);
        const u32 high_shift = b.emit_select(
            m_type_uint, aligned_start, m_const_u0,
            b.emit_isub(m_type_uint, cu(32u), shift));
        const u32 high = b.emit_select(
            m_type_uint, aligned_start, m_const_u0,
            b.emit_shift_left_logical(m_type_uint, w1, high_shift));
        u32 value = b.emit_bitwise_or(m_type_uint, low, high);
        if (bytes < 4) {
            value = b.emit_bitwise_and(m_type_uint, value,
                                       cu((1u << (bytes * 8u)) - 1u));
        }
        b.emit_branch(from);
        b.emit_label_id(from);
        b.emit_branch(merge);
        b.emit_label_id(outside_block);
        b.emit_branch(merge);
        b.emit_label_id(merge);
        return b.emit_phi(m_type_uint,
                          {{value, from}, {m_const_u0, outside_block}});
    };

    const auto as_bits = [&](u32 f) { return b.emit_bitcast(m_type_uint, f); };
    // x / d rounded to nearest: the reciprocal product, then one fma
    // correction of its residual. A fast-math divide was 1 ulp off the
    // RTX 4080 and WARP for UNORM8 (d3d12_a51 slots32), whose fetches give
    // the correctly rounded quotient.
    const auto quotient = [&](u32 x, float d) {
        const u32 r = cf(1.0f / d);
        const u32 dd = cf(d);
        const u32 q = b.emit_fmul(m_type_float, x, r);
        const u32 residual = b.emit_ext_inst(
            m_type_float, m_glsl_std450, kGlslStd450Fma,
            {b.emit_fnegate(m_type_float, q), dd, x});
        return b.emit_ext_inst(m_type_float, m_glsl_std450, kGlslStd450Fma,
                               {residual, r, q});
    };
    const auto unorm = [&](u32 v, u32 bits) {
        return as_bits(quotient(b.emit_convert_u_to_f(m_type_float, v),
                                static_cast<float>((1ull << bits) - 1ull)));
    };
    const auto sext = [&](u32 v, u32 bits) {
        if (bits >= 32) return v;
        // The constant first: emitting one switches the builder's section.
        const u32 count = cu(bits);
        const u32 id = b.allocate_id();
        b.set_section(SpirvBuilder::Section::Functions);
        b.emit(SpvOp::OpBitFieldSExtract,
               {m_type_uint, id, v, m_const_u0, count});
        return id;
    };
    const auto snorm = [&](u32 v, u32 bits) {
        const u32 s = b.emit_bitcast(m_type_int, sext(v, bits));
        const u32 f = quotient(b.emit_convert_s_to_f(m_type_float, s),
                               static_cast<float>((1ull << (bits - 1u)) - 1ull));
        return as_bits(b.emit_ext_inst(m_type_float, m_glsl_std450,
                                       kGlslStd450FMax, {f, cf(-1.0f)}));
    };
    const auto field = [&](u32 v, u32 offset, u32 bits) {
        return b.emit_bitwise_and(
            m_type_uint,
            offset == 0 ? v
                        : b.emit_shift_right_logical(m_type_uint, v, cu(offset)),
            cu((1u << bits) - 1u));
    };
    // FLOAT11 (6-bit mantissa) / FLOAT10 (5-bit) to FLOAT32 bits: no sign,
    // 5-bit exponent biased 15 (D3D11.3 3.1.6).
    const auto small_float = [&](u32 v, u32 mantissa_bits) {
        const u32 m = b.emit_bitwise_and(m_type_uint, v,
                                         cu((1u << mantissa_bits) - 1u));
        const u32 e = b.emit_shift_right_logical(m_type_uint, v,
                                                 cu(mantissa_bits));
        const u32 normal = b.emit_bitwise_or(
            m_type_uint,
            b.emit_shift_left_logical(m_type_uint,
                                      b.emit_iadd(m_type_uint, e, cu(112u)),
                                      cu(23u)),
            b.emit_shift_left_logical(m_type_uint, m, cu(23u - mantissa_bits)));
        // A denormal is m * 2^(-14 - mantissa_bits).
        const u32 denormal = as_bits(b.emit_fmul(
            m_type_float, b.emit_convert_u_to_f(m_type_float, m),
            cf(1.0f / static_cast<float>(1u << (14u + mantissa_bits)))));
        const u32 special = b.emit_bitwise_or(
            m_type_uint, cu(0x7F800000u),
            b.emit_shift_left_logical(m_type_uint, m, cu(23u - mantissa_bits)));
        const u32 zero_e = b.emit_integer_compare(SpvOp::OpIEqual, m_type_bool,
                                                  e, m_const_u0);
        const u32 max_e = b.emit_integer_compare(SpvOp::OpIEqual, m_type_bool,
                                                 e, cu(31u));
        return b.emit_select(m_type_uint, zero_e, denormal,
                             b.emit_select(m_type_uint, max_e, special, normal));
    };

    const bool float_format =
        element.kind == VertexPullKind::Float ||
        element.kind == VertexPullKind::Half ||
        element.kind == VertexPullKind::UNorm ||
        element.kind == VertexPullKind::SNorm ||
        element.kind == VertexPullKind::UNorm1010102 ||
        element.kind == VertexPullKind::Float111110 ||
        element.kind == VertexPullKind::UNormBGRA8 ||
        element.kind == VertexPullKind::UNormB5G6R5 ||
        element.kind == VertexPullKind::UNormB5G5R5A1 ||
        element.kind == VertexPullKind::UNormB4G4R4A4 ||
        element.kind == VertexPullKind::UNormBGRX8;
    std::array<u32, 4> lanes{m_const_u0, m_const_u0, m_const_u0,
                             float_format ? cu(0x3F800000u) : cu(1u)};
    switch (element.kind) {
    case VertexPullKind::UNorm1010102:
    case VertexPullKind::UInt1010102: {
        const u32 v = read(0, 4);
        for (u32 c = 0; c < 4; ++c) {
            const u32 bits = c == 3 ? 2u : 10u;
            const u32 x = field(v, c * 10u, bits);
            lanes[c] = element.kind == VertexPullKind::UInt1010102
                           ? x
                           : unorm(x, bits);
        }
        break;
    }
    case VertexPullKind::Float111110: {
        const u32 v = read(0, 4);
        lanes[0] = small_float(field(v, 0, 11), 6);
        lanes[1] = small_float(field(v, 11, 11), 6);
        lanes[2] = small_float(field(v, 22, 10), 5);
        break;
    }
    case VertexPullKind::UNormBGRA8: {
        const u32 v = read(0, 4);
        lanes[2] = unorm(field(v, 0, 8), 8);
        lanes[1] = unorm(field(v, 8, 8), 8);
        lanes[0] = unorm(field(v, 16, 8), 8);
        lanes[3] = unorm(field(v, 24, 8), 8);
        break;
    }
    case VertexPullKind::UNormBGRX8: {
        const u32 v = read(0, 4);
        lanes[2] = unorm(field(v, 0, 8), 8);
        lanes[1] = unorm(field(v, 8, 8), 8);
        lanes[0] = unorm(field(v, 16, 8), 8);
        break;
    }
    case VertexPullKind::UNormB5G6R5:
    case VertexPullKind::UNormB5G5R5A1:
    case VertexPullKind::UNormB4G4R4A4: {
        // {offset, width} of R, G, B, A in the 16-bit element; width 0 is
        // a channel the format lacks (alpha 1).
        struct Field {
            u32 at, bits;
        };
        const std::array<Field, 4> fields =
            element.kind == VertexPullKind::UNormB5G6R5
                ? std::array<Field, 4>{{{11, 5}, {5, 6}, {0, 5}, {0, 0}}}
            : element.kind == VertexPullKind::UNormB5G5R5A1
                ? std::array<Field, 4>{{{10, 5}, {5, 5}, {0, 5}, {15, 1}}}
                : std::array<Field, 4>{{{8, 4}, {4, 4}, {0, 4}, {12, 4}}};
        const u32 v = read(0, 2);
        for (u32 c = 0; c < 4; ++c) {
            if (fields[c].bits != 0) {
                lanes[c] = unorm(field(v, fields[c].at, fields[c].bits),
                                 fields[c].bits);
            }
        }
        break;
    }
    default: {
        const u32 bits = element.bits;
        const u32 bytes = bits / 8u;
        if (bytes == 0 || element.components == 0 || element.components > 4) {
            return 0;
        }
        for (u32 c = 0; c < element.components; ++c) {
            const u32 x = read(c * bytes, bytes);
            switch (element.kind) {
            case VertexPullKind::Float: lanes[c] = x; break;
            case VertexPullKind::Half:
                lanes[c] = as_bits(b.emit_composite_extract(
                    m_type_float,
                    b.emit_ext_inst(m_type_vec2, m_glsl_std450,
                                    kGlslStd450UnpackHalf2x16, {x}),
                    {0}));
                break;
            case VertexPullKind::UInt: lanes[c] = x; break;
            case VertexPullKind::SInt: lanes[c] = sext(x, bits); break;
            case VertexPullKind::UNorm: lanes[c] = unorm(x, bits); break;
            case VertexPullKind::SNorm: lanes[c] = snorm(x, bits); break;
            default: return 0;
            }
        }
        break;
    }
    }
    return b.emit_composite_construct(m_type_uvec4,
                                      {lanes[0], lanes[1], lanes[2], lanes[3]});
}

bool DxbcTranslator::emit_vertex_pull_prologue() {
    if (m_pulled_vertex_inputs.empty()) return true;
    // Every global the prologue touches is in the entry point's interface,
    // which is written at the end of run() (m_entry_interface).
    const auto add_interface = [&](u32 var) {
        if (std::find(m_entry_interface.begin(), m_entry_interface.end(),
                      var) == m_entry_interface.end()) {
            m_entry_interface.push_back(var);
        }
    };
    if (m_vertex_pull_table == 0) {
        m_vertex_pull_table = declare_expansion_buffer(
            m_type_uint, 4u, "vitreo_vertex_pull", VERTEX_PULL_TABLE_BINDING,
            /*read_only=*/true);
    }
    add_interface(m_vertex_pull_table);
    bool instanced = false;
    for (const auto& pulled : m_pulled_vertex_inputs) {
        if (pulled.reg < m_options.vertex_pull.size() &&
            m_options.vertex_pull[pulled.reg].per_instance) {
            instanced = true;
        }
    }
    if (instanced) {
        m_builder.emit_capability(SpvCapability::DrawParameters);
        m_builder.emit_extension("SPV_KHR_shader_draw_parameters");
    }
    for (const auto& pulled : m_pulled_vertex_inputs) {
        u32 value = 0;
        if (pulled.reg < m_options.vertex_pull.size() &&
            m_options.vertex_pull[pulled.reg].kind != VertexPullKind::None) {
            value = vertex_pull_element(m_options.vertex_pull[pulled.reg]);
            if (value == 0) {
                return fail("vertex pulling: v" + std::to_string(pulled.reg) +
                            " has a format the pull cannot convert");
            }
        } else {
            // A register no element feeds reads 0 (nothing is fetched).
            value = m_builder.emit_composite_construct(
                m_type_uvec4, {m_const_u0, m_const_u0, m_const_u0, m_const_u0});
        }
        if (pulled.type != m_type_uvec4) {
            value = m_builder.emit_bitcast(pulled.type, value);
        }
        m_builder.emit_store(pulled.target, value);
    }
    for (const u32 builtin :
         {static_cast<u32>(SpvBuiltIn::VertexIndex),
          static_cast<u32>(SpvBuiltIn::InstanceIndex),
          static_cast<u32>(SpvBuiltIn::BaseInstance)}) {
        const auto found = m_builtin_inputs.find(builtin);
        if (found != m_builtin_inputs.end()) add_interface(found->second);
    }
    return true;
}

}  // namespace vitreo::graphics::dxbc_spirv_detail
