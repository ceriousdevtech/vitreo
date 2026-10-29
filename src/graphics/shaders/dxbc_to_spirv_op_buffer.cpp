// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_op_buffer.cpp
/// @brief Raw and structured buffer opcodes, and sync.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

bool DxbcTranslator::translate_buffer_operation(u32 opcode_token,
                                                Sm4Opcode opcode,
                                                u32 /*sample_controls*/) {
    switch (opcode) {
    case Sm4Opcode::STORE_RAW: {
        // Microsoft store_raw: g#.x/.xy/.xyz/.xyzw selects a contiguous
        // sequence of 32-bit words beginning at an aligned byte offset.
        Sm4Operand memory, byte_offset, source;
        if (!m_decoder.decode_operand(memory) ||
            !m_decoder.decode_operand(byte_offset) ||
            !m_decoder.decode_operand(source)) {
            return fail("STORE_RAW: bad operands");
        }
        if (m_result.stage != ShaderStage::Compute &&
            memory.type == Sm4OperandType::ThreadGroupSharedMemory) {
            return fail("STORE_RAW: groupshared memory outside compute");
        }
        // store_raw writes groupshared memory (g#) or an RWByteAddressBuffer
        // UAV (u#). Both are word arrays; they differ only in storage class
        // and, for the buffer, an enclosing Block member.
        const bool raw_uav_target =
            memory.type == Sm4OperandType::UnorderedAccessView;
        if ((memory.type != Sm4OperandType::ThreadGroupSharedMemory &&
             !raw_uav_target) ||
            memory.modifier != 0u || byte_offset.modifier != 0u ||
            source.modifier != 0u ||
            (memory.write_mask != 0x1u && memory.write_mask != 0x3u &&
             memory.write_mask != 0x7u && memory.write_mask != 0xFu)) {
            return fail("STORE_RAW: unsupported operand shape");
        }
        u32 raw_target_var = 0;
        u32 raw_pointer_type = 0;
        bool raw_indexed_through_member = false;
        if (raw_uav_target) {
            const auto found = m_raw_uav_vars.find(memory.index0);
            if (found == m_raw_uav_vars.end()) {
                return fail("STORE_RAW: undeclared raw u" +
                            std::to_string(memory.index0));
            }
            raw_target_var = found->second;
            raw_pointer_type = m_type_ptr_storage_buffer_uint;
            raw_indexed_through_member = true;
        } else {
            const auto found = m_tgsm_vars.find(memory.index0);
            if (found == m_tgsm_vars.end()) {
                return fail("STORE_RAW: undeclared g" +
                            std::to_string(memory.index0));
            }
            raw_target_var = found->second;
            raw_pointer_type = m_type_ptr_workgroup_uint;
        }
        u32 offset4 = 0, value4 = 0;
        if (!load_vec4(byte_offset, offset4) ||
            !load_vec4(source, value4)) {
            return false;
        }
        const u32 offset_bits = m_builder.emit_bitcast(m_type_uvec4, offset4);
        const u32 byte_index = m_builder.emit_composite_extract(
            m_type_uint, offset_bits, {0u});
        const u32 shift = m_builder.emit_constant_int(m_type_uint, 2u);
        const u32 base_word = m_builder.emit_shift_right_logical(
            m_type_uint, byte_index, shift);
        const u32 value_bits = m_builder.emit_bitcast(m_type_uvec4, value4);
        for (u32 lane = 0; lane < 4u; ++lane) {
            if ((memory.write_mask & (1u << lane)) == 0u) continue;
            const u32 word = lane == 0u ? base_word : m_builder.emit_iadd(
                m_type_uint, base_word,
                m_builder.emit_constant_int(m_type_uint, lane));
            const u32 value = m_builder.emit_composite_extract(
                m_type_uint, value_bits, {lane});
            if (guards_buffer(memory.type)) {
                // Learn store_raw: a word past the view is dropped.
                guarded_buffer_word_store(
                    raw_target_var, raw_pointer_type, word, value,
                    buffer_word_in_bounds(true, memory.index0, word));
                continue;
            }
            const u32 member = m_builder.emit_constant_int(m_type_uint, 0u);
            const u32 pointer =
                raw_indexed_through_member
                    ? m_builder.emit_access_chain(raw_pointer_type,
                                                  raw_target_var,
                                                  {member, word})
                    : m_builder.emit_access_chain(raw_pointer_type,
                                                  raw_target_var, {word});
            m_builder.emit_store(pointer, value);
        }
        break;
    }

    case Sm4Opcode::LD_RAW: {
        // Microsoft ld_raw reads only destination-selected components. This
        // matters for g#: unused components must not create extra OOB reads,
        // because an OOB groupshared read is explicitly undefined.
        Sm4Operand destination, byte_offset, memory;
        if (!m_decoder.decode_operand(destination) ||
            !m_decoder.decode_operand(byte_offset) ||
            !m_decoder.decode_operand(memory)) {
            return fail("LD_RAW: bad operands");
        }
        if (destination.modifier != 0u || byte_offset.modifier != 0u ||
            memory.modifier != 0u) {
            return fail("LD_RAW: unsupported source modifier");
        }
        // The same opcode reads groupshared memory (g#, compute only) and a
        // ByteAddressBuffer SRV (t#, any stage). Both are word arrays; they
        // differ only in SPIR-V storage class.
        u32 found_var = 0;
        u32 pointer_type = 0;
        bool indexed_through_member = false;
        if (memory.type == Sm4OperandType::ThreadGroupSharedMemory) {
            if (m_result.stage != ShaderStage::Compute) {
                return fail("LD_RAW: groupshared memory outside compute");
            }
            const auto found = m_tgsm_vars.find(memory.index0);
            if (found == m_tgsm_vars.end()) {
                return fail("LD_RAW: undeclared g" +
                            std::to_string(memory.index0));
            }
            found_var = found->second;
            pointer_type = m_type_ptr_workgroup_uint;
        } else if (memory.type == Sm4OperandType::UnorderedAccessView) {
            const auto found = m_raw_uav_vars.find(memory.index0);
            if (found == m_raw_uav_vars.end()) {
                return fail("LD_RAW: undeclared raw u" +
                            std::to_string(memory.index0));
            }
            found_var = found->second;
            pointer_type = m_type_ptr_storage_buffer_uint;
            indexed_through_member = true;
        } else if (memory.type == Sm4OperandType::Resource) {
            const auto found = m_raw_srv_vars.find(memory.index0);
            if (found == m_raw_srv_vars.end()) {
                return fail("LD_RAW: undeclared raw t" +
                            std::to_string(memory.index0));
            }
            found_var = found->second;
            pointer_type = m_type_ptr_storage_buffer_uint;
            indexed_through_member = true;
        } else {
            return fail("LD_RAW: unsupported memory operand");
        }
        u32 offset4 = 0;
        if (!load_vec4(byte_offset, offset4)) return false;
        const u32 offset_bits = m_builder.emit_bitcast(m_type_uvec4, offset4);
        const u32 byte_index = m_builder.emit_composite_extract(
            m_type_uint, offset_bits, {0u});
        const u32 shift = m_builder.emit_constant_int(m_type_uint, 2u);
        const u32 base_word = m_builder.emit_shift_right_logical(
            m_type_uint, byte_index, shift);
        std::vector<u32> lanes(4u, m_const_u0);
        for (u32 lane = 0; lane < 4u; ++lane) {
            if ((destination.write_mask & (1u << lane)) == 0u) continue;
            const u32 source_lane =
                (memory.swizzle >> (lane * 2u)) & 0x3u;
            const u32 word = source_lane == 0u ? base_word
                : m_builder.emit_iadd(
                      m_type_uint, base_word,
                      m_builder.emit_constant_int(m_type_uint, source_lane));
            if (guards_buffer(memory.type)) {
                // Learn ld_raw: a word past the view reads as 0.
                lanes[lane] = guarded_buffer_word_load(
                    found_var, pointer_type, word,
                    buffer_word_in_bounds(
                        memory.type == Sm4OperandType::UnorderedAccessView,
                        memory.index0, word));
                continue;
            }
            // A storage buffer wraps its word array in a Block struct, so the
            // chain needs the member index first; groupshared memory is the
            // bare array.
            const u32 pointer = indexed_through_member
                ? m_builder.emit_access_chain(
                      pointer_type, found_var,
                      {m_builder.emit_constant_int(m_type_uint, 0u), word})
                : m_builder.emit_access_chain(pointer_type, found_var, {word});
            lanes[lane] = m_builder.emit_load(m_type_uint, pointer);
        }
        const u32 bits = m_builder.emit_composite_construct(
            m_type_uvec4, lanes);
        if (!store_vec4(destination,
                        m_builder.emit_bitcast(m_type_vec4, bits))) {
            return false;
        }
        break;
    }

    case Sm4Opcode::SYNC: {
        // Learn (sync, sm5 asm): sync[_uglobal|_ugroup][_g][_t]. Each flag is
        // independent (d3d11TokenizedProgramFormat.hpp D3D11_SB_SYNC_*):
        //   _t        0x0800  every thread of the group reaches this point
        //   _g        0x1000  groupshared-memory fence
        //   _ugroup   0x2000  UAV-memory fence visible within the group
        //   _uglobal  0x4000  UAV-memory fence visible to the whole device
        // HLSL: GroupMemoryBarrier = sync_g, ...WithGroupSync = sync_g_t,
        // DeviceMemoryBarrier = sync_uglobal[_g] (sync_ugroup when the UAV is
        // not globallycoherent), All/DeviceMemoryBarrierWithGroupSync add _t.
        //
        // A barrier inside control flow is legal and ordinary: the canonical
        // groupshared reduction places one in a loop body, once per halving
        // step. For _t both HLSL and SPIR-V require only that the control
        // flow reaching it be *uniform* across the group — the shader
        // author's obligation, already met to run on Windows.
        constexpr u32 kSyncThreads = 0x0800u;
        constexpr u32 kSyncGroupShared = 0x1000u;
        constexpr u32 kSyncUavGroup = 0x2000u;
        constexpr u32 kSyncUavGlobal = 0x4000u;
        constexpr u32 kSyncFlagsMask = 0x7800u;
        const u32 flags = opcode_token & kSyncFlagsMask;
        if (flags == 0u ||
            (flags & (kSyncUavGroup | kSyncUavGlobal)) ==
                (kSyncUavGroup | kSyncUavGlobal)) {
            return fail("SYNC: invalid flag combination (none, or both "
                        "_ugroup and _uglobal)");
        }
        // Learn (sync, sm5 asm): a pixel shader may use sync_uglobal alone
        // (measured with fxc: DeviceMemoryBarrier() in ps_5_0 is 0x010040be,
        // flag bit 14). SPIRV-Cross 1.3.283 drops every OpMemoryBarrier
        // outside compute and tessellation-control (CompilerMSL::emit_barrier),
        // so there is no fence to emit; refusing is honest where a silent
        // no-op would not be.
        if (m_result.stage == ShaderStage::Fragment &&
            flags == kSyncUavGlobal) {
            return fail("SYNC: sync_uglobal in a pixel shader is not "
                        "implemented (no fragment-stage memory fence in the "
                        "SPIR-V -> MSL path)");
        }
        if (m_result.stage != ShaderStage::Compute) {
            return fail("SYNC: only a compute shader may use _t, _g or "
                        "_ugroup");
        }
        constexpr u32 kScopeDevice = 1u;
        constexpr u32 kScopeWorkgroup = 2u;
        constexpr u32 kSemanticsAcquireRelease = 0x008u;
        constexpr u32 kSemanticsUniformMemory = 0x040u;
        constexpr u32 kSemanticsWorkgroupMemory = 0x100u;
        constexpr u32 kSemanticsImageMemory = 0x800u;
        u32 semantics = 0u;
        if ((flags & kSyncGroupShared) != 0u) {
            semantics |= kSemanticsWorkgroupMemory;
        }
        if ((flags & (kSyncUavGroup | kSyncUavGlobal)) != 0u) {
            // A UAV is a buffer (Uniform/StorageBuffer) or a storage image.
            semantics |= kSemanticsUniformMemory | kSemanticsImageMemory;
        }
        if (semantics != 0u) semantics |= kSemanticsAcquireRelease;
        const u32 memory_scope = m_builder.emit_constant_int(
            m_type_uint,
            (flags & kSyncUavGlobal) != 0u ? kScopeDevice : kScopeWorkgroup);
        const u32 semantics_id =
            m_builder.emit_constant_int(m_type_uint, semantics);
        if ((flags & kSyncThreads) != 0u) {
            m_builder.emit_control_barrier(
                m_builder.emit_constant_int(m_type_uint, kScopeWorkgroup),
                memory_scope, semantics_id);
            break;
        }
        // A fence without _t does not rendezvous, so HLSL allows it in
        // divergent control flow. spirv_to_msl lowers OpMemoryBarrier to the
        // MSL 3.2 atomic_thread_fence. Where that is unavailable SPIRV-Cross
        // falls back to threadgroup_barrier -- a rendezvous -- which is a
        // strictly stronger equivalent only where every thread of the group
        // reaches it: the top level of the entry function before any early
        // return. Anywhere else it could deadlock the group.
        const bool divergent =
            !m_if_stack.empty() || !m_loop_stack.empty() ||
            !m_switch_stack.empty() || !m_call_stack.empty() ||
            m_in_subroutine || m_emitted_early_return;
        if (divergent && !msl_memory_fence_available()) {
            return fail("SYNC: a memory fence without _t in possibly "
                        "divergent control flow needs MSL 3.2 "
                        "atomic_thread_fence (macOS 15)");
        }
        m_builder.emit_memory_barrier(memory_scope, semantics_id);
        break;
    }

    case Sm4Opcode::LD_STRUCTURED: {
        // Microsoft ld_structured (sm5): dest[.mask], srcAddress (structure
        // index), srcByteOffset (offset within the structure), srcResource
        // [.swizzle]. The four consecutive 32-bit words at
        // index * stride + byteOffset are read into the destination, then the
        // resource swizzle selects among them — the element type is opaque to
        // the ISA, which is why the buffer is modelled as an array of words.
        Sm4Operand destination, element, offset, resource;
        if (!m_decoder.decode_operand(destination) ||
            !m_decoder.decode_operand(element) ||
            !m_decoder.decode_operand(offset) ||
            !m_decoder.decode_operand(resource)) {
            return fail("LD_STRUCTURED: bad operands");
        }
        if (destination.modifier != 0u || element.modifier != 0u ||
            offset.modifier != 0u || resource.modifier != 0u) {
            return fail("LD_STRUCTURED: unsupported source modifier");
        }
        // The same opcode reads a StructuredBuffer SRV (t#), an
        // RWStructuredBuffer UAV (u#) and structured groupshared memory (g#).
        // All three are word arrays; they differ only in SPIR-V storage class
        // and, for the two buffers, an enclosing Block member.
        u32 resource_var = 0;
        u32 stride = 0;
        u32 word_pointer_type = 0;
        bool indexed_through_member = false;
        if (!resolve_structured_operand(resource, "LD_STRUCTURED",
                                        resource_var, stride,
                                        word_pointer_type,
                                        indexed_through_member)) {
            return false;
        }
        u32 element_bits = 0;
        u32 offset_bits = 0;
        if (!load_vec4(element, element_bits)) return false;
        if (!load_vec4(offset, offset_bits)) return false;
        const u32 element_index = m_builder.emit_composite_extract(
            m_type_uint, m_builder.emit_bitcast(m_type_uvec4, element_bits),
            {0u});
        const u32 byte_offset = m_builder.emit_composite_extract(
            m_type_uint, m_builder.emit_bitcast(m_type_uvec4, offset_bits),
            {0u});
        const u32 stride_id = m_builder.emit_constant_int(m_type_uint, stride);
        const u32 base_bytes = m_builder.emit_iadd(
            m_type_uint,
            m_builder.emit_imul(m_type_uint, element_index, stride_id),
            byte_offset);
        // Word index: byte addresses in a structured buffer are 4-byte
        // aligned, so this is a shift, not a rounding division.
        const u32 base_word = m_builder.emit_shift_right_logical(
            m_type_uint, base_bytes,
            m_builder.emit_constant_int(m_type_uint, 2u));
        const u32 member = m_builder.emit_constant_int(m_type_uint, 0u);
        std::vector<u32> words;
        words.reserve(4);
        for (u32 i = 0; i < 4; ++i) {
            const u32 word_index = m_builder.emit_iadd(
                m_type_uint, base_word,
                m_builder.emit_constant_int(m_type_uint, i));
            if (guards_buffer(resource.type)) {
                // Learn ld_structured: a word past the view reads as 0.
                words.push_back(guarded_buffer_word_load(
                    resource_var, word_pointer_type, word_index,
                    buffer_word_in_bounds(
                        resource.type == Sm4OperandType::UnorderedAccessView,
                        resource.index0, word_index)));
                continue;
            }
            const u32 chain = indexed_through_member
                ? m_builder.emit_access_chain(word_pointer_type, resource_var,
                                              {member, word_index})
                : m_builder.emit_access_chain(word_pointer_type, resource_var,
                                              {word_index});
            words.push_back(m_builder.emit_load(m_type_uint, chain));
        }
        const u32 packed =
            m_builder.emit_composite_construct(m_type_uvec4, words);
        const u32 value = apply_swizzle(
            m_builder.emit_bitcast(m_type_vec4, packed), resource.swizzle);
        if (!store_vec4(destination, value)) return false;
        break;
    }

    case Sm4Opcode::STORE_STRUCTURED: {
        // Microsoft store_structured (sm5): dstResource[.writeMask],
        // dstAddress (structure index), dstByteOffset, src[.swizzle]. Only
        // the components the write mask selects are stored, each into the
        // consecutive word at index * stride + byteOffset. A structured store
        // targets a UAV or groupshared memory; an SRV is read-only.
        Sm4Operand resource, element, offset, source;
        if (!m_decoder.decode_operand(resource) ||
            !m_decoder.decode_operand(element) ||
            !m_decoder.decode_operand(offset) ||
            !m_decoder.decode_operand(source)) {
            return fail("STORE_STRUCTURED: bad operands");
        }
        if (resource.modifier != 0u || element.modifier != 0u ||
            offset.modifier != 0u || source.modifier != 0u) {
            return fail("STORE_STRUCTURED: unsupported source modifier");
        }
        if (resource.type == Sm4OperandType::Resource) {
            return fail("STORE_STRUCTURED: t# resources are read-only");
        }
        u32 resource_var = 0;
        u32 stride = 0;
        u32 word_pointer_type = 0;
        bool indexed_through_member = false;
        if (!resolve_structured_operand(resource, "STORE_STRUCTURED",
                                        resource_var, stride,
                                        word_pointer_type,
                                        indexed_through_member)) {
            return false;
        }
        u32 element_bits = 0, offset_bits = 0, value_bits = 0;
        if (!load_vec4(element, element_bits)) return false;
        if (!load_vec4(offset, offset_bits)) return false;
        if (!load_vec4(source, value_bits)) return false;
        const u32 element_index = m_builder.emit_composite_extract(
            m_type_uint, m_builder.emit_bitcast(m_type_uvec4, element_bits),
            {0u});
        const u32 byte_offset = m_builder.emit_composite_extract(
            m_type_uint, m_builder.emit_bitcast(m_type_uvec4, offset_bits),
            {0u});
        const u32 stride_id = m_builder.emit_constant_int(m_type_uint, stride);
        const u32 base_bytes = m_builder.emit_iadd(
            m_type_uint,
            m_builder.emit_imul(m_type_uint, element_index, stride_id),
            byte_offset);
        const u32 base_word = m_builder.emit_shift_right_logical(
            m_type_uint, base_bytes,
            m_builder.emit_constant_int(m_type_uint, 2u));
        const u32 member = m_builder.emit_constant_int(m_type_uint, 0u);
        const u32 values = m_builder.emit_bitcast(m_type_uvec4, value_bits);
        for (u32 lane = 0; lane < 4u; ++lane) {
            if ((resource.write_mask & (1u << lane)) == 0u) continue;
            const u32 word_index = lane == 0u
                ? base_word
                : m_builder.emit_iadd(
                      m_type_uint, base_word,
                      m_builder.emit_constant_int(m_type_uint, lane));
            const u32 lane_value =
                m_builder.emit_composite_extract(m_type_uint, values, {lane});
            if (guards_buffer(resource.type)) {
                // Learn store_structured: a word past the view is dropped.
                guarded_buffer_word_store(
                    resource_var, word_pointer_type, word_index, lane_value,
                    buffer_word_in_bounds(true, resource.index0, word_index));
                continue;
            }
            const u32 chain = indexed_through_member
                ? m_builder.emit_access_chain(word_pointer_type, resource_var,
                                              {member, word_index})
                : m_builder.emit_access_chain(word_pointer_type, resource_var,
                                              {word_index});
            m_builder.emit_store(chain, lane_value);
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
