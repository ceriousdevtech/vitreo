// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_op_atomic.cpp
/// @brief UAV and TGSM atomic opcodes.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

bool DxbcTranslator::translate_atomic_operation(u32 /*opcode_token*/,
                                                Sm4Opcode opcode,
                                                u32 /*sample_controls*/) {
    switch (opcode) {
    case Sm4Opcode::IMM_ATOMIC_ALLOC:
    case Sm4Opcode::IMM_ATOMIC_CONSUME: {
        // Learn (imm_atomic_alloc / imm_atomic_consume, sm5 asm):
        //   imm_atomic_alloc   dest[.mask], dstUAV -- increment the hidden
        //                      counter, return the ORIGINAL value;
        //   imm_atomic_consume dest[.mask], dstUAV -- decrement it, return
        //                      the NEW value.
        // The count wraps either way. The counter was declared with the UAVs
        // (scan_uav_counters).
        Sm4Operand destination, uav;
        if (!m_decoder.decode_operand(destination) ||
            !m_decoder.decode_operand(uav)) {
            return fail("ATOMIC: bad counter operands");
        }
        const auto counter = m_uav_counter_vars.find(uav.index0);
        if (uav.type != Sm4OperandType::UnorderedAccessView ||
            counter == m_uav_counter_vars.end()) {
            return fail("ATOMIC: counter operand is not a declared u#");
        }
        const u32 pointer = m_builder.emit_access_chain(
            m_type_ptr_storage_buffer_uint, counter->second,
            {m_builder.emit_constant_int(m_type_uint, 0u)});
        const bool consume = opcode == Sm4Opcode::IMM_ATOMIC_CONSUME;
        const u32 one = m_builder.emit_constant_int(m_type_uint, 1u);
        // Only the pixel's own invocation moves the counter
        // (m_uav_leader_var); the others step it by 0, which reads it.
        const u32 step = m_uav_leader_var == 0
            ? one
            : m_builder.emit_select(m_type_uint, load_uav_leader(), one,
                                    m_builder.emit_constant_int(m_type_uint,
                                                                0u));
        // Device scope, Relaxed, as for the other SM5 atomics.
        u32 value = m_builder.emit_atomic_rmw(
            consume ? SpvOp::OpAtomicISub : SpvOp::OpAtomicIAdd, m_type_uint,
            pointer, m_builder.emit_constant_int(m_type_uint, 1u),
            m_builder.emit_constant_int(m_type_uint, 0u), step);
        // OpAtomicISub returns the original; consume reports the new value.
        if (consume) value = m_builder.emit_isub(m_type_uint, value, one);
        const u32 splat = m_builder.emit_composite_construct(
            m_type_uvec4, {value, value, value, value});
        if (!store_vec4(destination,
                        m_builder.emit_bitcast(m_type_vec4, splat))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::ATOMIC_AND:
    case Sm4Opcode::ATOMIC_OR:
    case Sm4Opcode::ATOMIC_XOR:
    case Sm4Opcode::ATOMIC_CMP_STORE:
    case Sm4Opcode::IMM_ATOMIC_CMP_EXCH:
    case Sm4Opcode::ATOMIC_IADD:
    case Sm4Opcode::ATOMIC_IMAX:
    case Sm4Opcode::ATOMIC_IMIN:
    case Sm4Opcode::ATOMIC_UMAX:
    case Sm4Opcode::ATOMIC_UMIN:
    case Sm4Opcode::IMM_ATOMIC_IADD:
    case Sm4Opcode::IMM_ATOMIC_AND:
    case Sm4Opcode::IMM_ATOMIC_OR:
    case Sm4Opcode::IMM_ATOMIC_XOR:
    case Sm4Opcode::IMM_ATOMIC_EXCH:
    case Sm4Opcode::IMM_ATOMIC_UMAX:
    case Sm4Opcode::IMM_ATOMIC_UMIN:
    case Sm4Opcode::IMM_ATOMIC_IMAX:
    case Sm4Opcode::IMM_ATOMIC_IMIN: {
        // Microsoft Learn (Interlocked* / atomic_*): the atomic_ forms are
        //   atomic_op dstMemory, dstAddress, src0
        // and the imm_atomic_ forms prepend a destination register that
        // receives the value *before* the operation. Both address a raw or
        // structured UAV, or groupshared memory, as 32-bit words.
        const bool returns_original =
            opcode == Sm4Opcode::IMM_ATOMIC_IADD ||
            opcode == Sm4Opcode::IMM_ATOMIC_AND ||
            opcode == Sm4Opcode::IMM_ATOMIC_OR ||
            opcode == Sm4Opcode::IMM_ATOMIC_XOR ||
            opcode == Sm4Opcode::IMM_ATOMIC_EXCH ||
            opcode == Sm4Opcode::IMM_ATOMIC_CMP_EXCH ||
            opcode == Sm4Opcode::IMM_ATOMIC_UMAX ||
            opcode == Sm4Opcode::IMM_ATOMIC_UMIN ||
            opcode == Sm4Opcode::IMM_ATOMIC_IMAX ||
            opcode == Sm4Opcode::IMM_ATOMIC_IMIN;
        // The two compare forms carry an extra source: src0 is the value the
        // memory is compared against and src1 the value written on a match.
        const bool compares = opcode == Sm4Opcode::ATOMIC_CMP_STORE ||
                              opcode == Sm4Opcode::IMM_ATOMIC_CMP_EXCH;
        Sm4Operand original, memory, address, source, source1;
        if (returns_original && !m_decoder.decode_operand(original)) {
            return fail("ATOMIC: bad destination operand");
        }
        if (!m_decoder.decode_operand(memory) ||
            !m_decoder.decode_operand(address) ||
            !m_decoder.decode_operand(source)) {
            return fail("ATOMIC: bad operands");
        }
        if (compares && !m_decoder.decode_operand(source1)) {
            return fail("ATOMIC: bad compare operands");
        }
        if (memory.modifier != 0u || address.modifier != 0u ||
            source.modifier != 0u || (compares && source1.modifier != 0u)) {
            return fail("ATOMIC: unsupported source modifier");
        }

        // Resolve the word array. A raw UAV and groupshared memory are byte
        // addressed; a structured UAV multiplies the element index by its
        // stride. Both end at a 32-bit word index.
        u32 target_var = 0;
        u32 pointer_type = 0;
        bool indexed_through_member = false;
        u32 stride = 0;
        // A typed UAV is a storage image, not a word array: the texel is
        // reached with OpImageTexelPointer rather than by scaling a byte
        // address, so it needs its own path. Learn (Interlocked*): atomics on
        // a typed UAV are legal for 32-bit integer views only.
        bool typed_uav = false;
        u32 typed_uav_image_var = 0;
        u32 typed_uav_scalar_type = 0;
        u32 typed_uav_pointer_type = 0;
        // A typed buffer view's raw-index bounds test (typed_buffer_bias):
        // a bool id, 0 when the access is not guarded here. Learn
        // (imm_atomic_*): past the view "nothing [is] written to memory" and
        // the returned value is undefined -- 0 here; D3D11.3 7.13: every
        // atomic behaves alike.
        u32 atomic_in_view = 0;
        if (memory.type == Sm4OperandType::UnorderedAccessView) {
            const auto raw = m_raw_uav_vars.find(memory.index0);
            const auto structured = m_structured_uav_vars.find(memory.index0);
            const auto typed = m_uav_vars.find(memory.index0);
            if (raw != m_raw_uav_vars.end()) {
                target_var = raw->second;
            } else if (structured != m_structured_uav_vars.end()) {
                target_var = structured->second;
                stride = m_structured_uav_strides[memory.index0];
            } else if (typed != m_uav_vars.end()) {
                const auto return_type = m_uav_return_types.find(memory.index0);
                const u32 kind =
                    return_type == m_uav_return_types.end() ? 5u
                                                            : return_type->second;
                if (kind != 3u && kind != 4u) {
                    return fail("ATOMIC: typed UAV u" +
                                std::to_string(memory.index0) +
                                " is not an integer view; D3D allows typed "
                                "atomics on R32_UINT/R32_SINT only");
                }
                typed_uav = true;
                typed_uav_image_var = typed->second;
                typed_uav_scalar_type =
                    kind == 4u ? m_type_uint : m_type_int;
                typed_uav_pointer_type = kind == 4u ? m_type_ptr_image_uint
                                                    : m_type_ptr_image_int;
                m_result.uses_typed_uav_atomics = true;
            } else {
                return fail("ATOMIC: undeclared u" +
                            std::to_string(memory.index0));
            }
            pointer_type = m_type_ptr_storage_buffer_uint;
            indexed_through_member = true;
        } else if (memory.type == Sm4OperandType::ThreadGroupSharedMemory) {
            if (m_result.stage != ShaderStage::Compute) {
                return fail("ATOMIC: groupshared memory outside compute");
            }
            const auto found = m_tgsm_vars.find(memory.index0);
            if (found == m_tgsm_vars.end()) {
                return fail("ATOMIC: undeclared g" +
                            std::to_string(memory.index0));
            }
            target_var = found->second;
            const auto stride_it = m_tgsm_strides.find(memory.index0);
            if (stride_it != m_tgsm_strides.end()) stride = stride_it->second;
            pointer_type = m_type_ptr_workgroup_uint;
        } else {
            return fail("ATOMIC: target is not a u# or g# operand");
        }

        u32 address_bits = 0, value_bits = 0, compare_bits = 0;
        if (!load_vec4(address, address_bits)) return false;
        if (typed_uav && m_uav_1d.contains(memory.index0)) {
            address_bits = widen_1d_address(
                address_bits, m_array_uav_slots.contains(memory.index0),
                false);
        }
        if (compares) {
            if (!load_vec4(source, compare_bits)) return false;
            if (!load_vec4(source1, value_bits)) return false;
        } else if (!load_vec4(source, value_bits)) {
            return false;
        }
        const u32 address_uvec4 =
            m_builder.emit_bitcast(m_type_uvec4, address_bits);
        // The scalar type the atomic operates in. A word array is always
        // uint; a typed UAV's pointee is the image's own sampled type, and a
        // SPIR-V atomic's result type must match the pointer it is given.
        const u32 atomic_type = typed_uav ? typed_uav_scalar_type : m_type_uint;
        u32 pointer = 0;
        if (typed_uav) {
            // The coordinate shape is the same one ld_uav_typed /
            // store_uav_typed use: one element index for a Dim=Buffer UAV, an
            // (x, y) texel for a Texture2D, and (x, y, slice-or-z) for an
            // arrayed or volume one, where the array layer is part of the
            // coordinate.
            const bool buffer_uav = m_buffer_uav_slots.contains(memory.index0);
            const bool three_component_uav =
                m_volume_uav_slots.contains(memory.index0) ||
                m_array_uav_slots.contains(memory.index0);
            u32 coordinate =
                buffer_uav
                    ? m_builder.emit_composite_extract(m_type_uint,
                                                       address_uvec4, {0u})
                : three_component_uav
                    ? m_builder.emit_vector_shuffle(m_type_uvec3,
                                                    address_uvec4,
                                                    address_uvec4,
                                                    {0u, 1u, 2u})
                    : m_builder.emit_vector_shuffle(m_type_uvec2,
                                                    address_uvec4,
                                                    address_uvec4, {0u, 1u});
            if (m_volume_uav_slots.contains(memory.index0)) {
                // A W-slice window: z from its first slice; past its size
                // the address leaves the texture, as any out-of-range
                // typed-UAV atomic does.
                u32 outside = 0;
                coordinate =
                    volume_uav_window(memory.index0, coordinate, outside);
                if (outside != 0) {
                    const u32 beyond =
                        m_builder.emit_constant_int(m_type_uint, 0xFFFFFFFFu);
                    coordinate = m_builder.emit_select(
                        m_type_uvec3,
                        m_builder.emit_composite_construct(
                            m_type_bvec3, {outside, outside, outside}),
                        m_builder.emit_composite_construct(
                            m_type_uvec3, {beyond, beyond, beyond}),
                        coordinate);
                }
            }
            if (buffer_uav) {
                // A buffer view's element bias (typed_buffer_bias): element
                // i is texel i + bias. An i at or past NumElements (tested on
                // i, so none wraps in front of the view) runs no atomic at
                // all: nothing is written and an imm_atomic returns 0 (see
                // atomic_in_view below).
                const u32 entry = typed_buffer_entry(true, memory.index0);
                if (entry != 0) {
                    const u32 bias = typed_buffer_bias(entry);
                    u32 image_type = 0, vector_type = 0;
                    typed_uav_types(memory.index0, true, image_type,
                                    vector_type);
                    const u32 count = typed_buffer_count(
                        m_builder.emit_load(image_type, typed_uav_image_var),
                        bias);
                    atomic_in_view = m_builder.emit_integer_compare(
                        SpvOp::OpULessThan, m_type_bool, coordinate, count);
                    // The texel pointer of an index past the view is never
                    // dereferenced; it names the view's first element.
                    coordinate = m_builder.emit_iadd(
                        m_type_uint,
                        m_builder.emit_select(m_type_uint, atomic_in_view,
                                              coordinate, m_const_u0),
                        bias);
                }
            }
            // Sample must be 0 for a non-multisampled image, and a typed UAV
            // is never multisampled.
            const u32 sample = m_builder.emit_constant_int(m_type_uint, 0u);
            pointer = m_builder.emit_image_texel_pointer(
                typed_uav_pointer_type, typed_uav_image_var, coordinate,
                sample);
        } else {
            u32 byte_index = m_builder.emit_composite_extract(
                m_type_uint, address_uvec4, {0u});
            if (stride != 0u) {
                // A structured target addresses element * stride + byteOffset,
                // and the second address component carries that byte offset.
                const u32 offset = m_builder.emit_composite_extract(
                    m_type_uint, address_uvec4, {1u});
                byte_index = m_builder.emit_iadd(
                    m_type_uint,
                    m_builder.emit_imul(
                        m_type_uint, byte_index,
                        m_builder.emit_constant_int(m_type_uint, stride)),
                    offset);
            }
            const u32 word_index = m_builder.emit_shift_right_logical(
                m_type_uint, byte_index,
                m_builder.emit_constant_int(m_type_uint, 2u));
            const u32 member = m_builder.emit_constant_int(m_type_uint, 0u);
            pointer = indexed_through_member
                          ? m_builder.emit_access_chain(
                                pointer_type, target_var, {member, word_index})
                          : m_builder.emit_access_chain(pointer_type,
                                                        target_var,
                                                        {word_index});
        }
        u32 operand = m_builder.emit_composite_extract(
            m_type_uint, m_builder.emit_bitcast(m_type_uvec4, value_bits),
            {0u});
        // An imm_atomic_* outside the pixel's own invocation
        // (m_uav_leader_var) applies the operation's identity, so it returns
        // the location's value without changing it. exch has no identity and
        // compare-exchange keeps its compare value; both are handled below.
        const bool leader_gated = m_uav_leader_var != 0 && returns_original;
        if (leader_gated && !compares &&
            opcode != Sm4Opcode::IMM_ATOMIC_EXCH) {
            const u32 identity =
                opcode == Sm4Opcode::IMM_ATOMIC_AND ||
                        opcode == Sm4Opcode::IMM_ATOMIC_UMIN
                    ? 0xFFFFFFFFu
                : opcode == Sm4Opcode::IMM_ATOMIC_IMAX ? 0x80000000u
                : opcode == Sm4Opcode::IMM_ATOMIC_IMIN ? 0x7FFFFFFFu
                                                       : 0u;
            operand = m_builder.emit_select(
                m_type_uint, load_uav_leader(), operand,
                m_builder.emit_constant_int(m_type_uint, identity));
        }
        if (atomic_type != m_type_uint) {
            // D3D's atomic operand is 32 raw bits; reinterpreting is correct
            // where converting would not be.
            operand = m_builder.emit_bitcast(atomic_type, operand);
        }

        SpvOp atomic_op = SpvOp::OpAtomicIAdd;
        switch (opcode) {
        case Sm4Opcode::ATOMIC_AND:
        case Sm4Opcode::IMM_ATOMIC_AND: atomic_op = SpvOp::OpAtomicAnd; break;
        case Sm4Opcode::ATOMIC_OR:
        case Sm4Opcode::IMM_ATOMIC_OR: atomic_op = SpvOp::OpAtomicOr; break;
        case Sm4Opcode::ATOMIC_XOR:
        case Sm4Opcode::IMM_ATOMIC_XOR: atomic_op = SpvOp::OpAtomicXor; break;
        case Sm4Opcode::ATOMIC_IMAX:
        case Sm4Opcode::IMM_ATOMIC_IMAX: atomic_op = SpvOp::OpAtomicSMax; break;
        case Sm4Opcode::ATOMIC_IMIN:
        case Sm4Opcode::IMM_ATOMIC_IMIN: atomic_op = SpvOp::OpAtomicSMin; break;
        case Sm4Opcode::ATOMIC_UMAX:
        case Sm4Opcode::IMM_ATOMIC_UMAX: atomic_op = SpvOp::OpAtomicUMax; break;
        case Sm4Opcode::ATOMIC_UMIN:
        case Sm4Opcode::IMM_ATOMIC_UMIN: atomic_op = SpvOp::OpAtomicUMin; break;
        case Sm4Opcode::IMM_ATOMIC_EXCH:
            atomic_op = SpvOp::OpAtomicExchange;
            break;
        default: atomic_op = SpvOp::OpAtomicIAdd; break;
        }
        // Device scope, Relaxed semantics: D3D's atomics are globally atomic
        // but impose no ordering on other memory operations.
        const u32 scope = m_builder.emit_constant_int(m_type_uint, 1u);
        const u32 semantics = m_builder.emit_constant_int(m_type_uint, 0u);
        // An index past a typed buffer view runs no atomic (atomic_in_view).
        u32 guard_body_end = 0, guard_outside = 0, guard_merge = 0;
        if (atomic_in_view != 0) {
            const u32 body = m_builder.allocate_id();
            guard_body_end = m_builder.allocate_id();
            guard_outside = m_builder.allocate_id();
            guard_merge = m_builder.allocate_id();
            m_builder.emit_selection_merge(guard_merge);
            m_builder.emit_branch_conditional(atomic_in_view, body,
                                              guard_outside);
            m_builder.emit_label_id(body);
        }
        u32 previous = 0;
        if (compares) {
            u32 comparator = m_builder.emit_composite_extract(
                m_type_uint,
                m_builder.emit_bitcast(m_type_uvec4, compare_bits), {0u});
            if (atomic_type != m_type_uint) {
                comparator = m_builder.emit_bitcast(atomic_type, comparator);
            }
            // Writing the compare value back where it matched changes
            // nothing: a read for the other invocations.
            if (leader_gated) {
                operand = m_builder.emit_select(atomic_type, load_uav_leader(),
                                                operand, comparator);
            }
            previous = m_builder.emit_atomic_compare_exchange(
                atomic_type, pointer, scope, semantics, semantics, operand,
                comparator);
        } else if (leader_gated && opcode == Sm4Opcode::IMM_ATOMIC_EXCH) {
            // The other invocations read with a compare-exchange whose
            // compare and write values are equal.
            const u32 exchange_block = m_builder.allocate_id();
            const u32 read_block = m_builder.allocate_id();
            const u32 merge_block = m_builder.allocate_id();
            const u32 leader = load_uav_leader();
            m_builder.emit_selection_merge(merge_block);
            m_builder.emit_branch_conditional(leader, exchange_block,
                                              read_block);
            m_builder.emit_label_id(exchange_block);
            const u32 exchanged = m_builder.emit_atomic_rmw(
                atomic_op, atomic_type, pointer, scope, semantics, operand);
            m_builder.emit_branch(merge_block);
            m_builder.emit_label_id(read_block);
            const u32 zero = m_builder.emit_constant_int(atomic_type, 0u);
            const u32 read = m_builder.emit_atomic_compare_exchange(
                atomic_type, pointer, scope, semantics, semantics, zero, zero);
            m_builder.emit_branch(merge_block);
            m_builder.emit_label_id(merge_block);
            previous = m_builder.emit_phi(
                atomic_type, {{exchanged, exchange_block}, {read, read_block}});
        } else {
            previous = m_builder.emit_atomic_rmw(
                atomic_op, atomic_type, pointer, scope, semantics, operand);
        }
        if (atomic_in_view != 0) {
            // The atomic's own block may end in a nested merge: a block of
            // its own names the predecessor the phi needs.
            m_builder.emit_branch(guard_body_end);
            m_builder.emit_label_id(guard_body_end);
            m_builder.emit_branch(guard_merge);
            m_builder.emit_label_id(guard_outside);
            m_builder.emit_branch(guard_merge);
            m_builder.emit_label_id(guard_merge);
            previous = m_builder.emit_phi(
                atomic_type,
                {{previous, guard_body_end},
                 {m_builder.emit_constant_int(atomic_type, 0u),
                  guard_outside}});
        }
        if (atomic_type != m_type_uint) {
            previous = m_builder.emit_bitcast(m_type_uint, previous);
        }
        if (returns_original) {
            const u32 splat = m_builder.emit_composite_construct(
                m_type_uvec4, {previous, previous, previous, previous});
            if (!store_vec4(original,
                            m_builder.emit_bitcast(m_type_vec4, splat))) {
                return false;
            }
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
