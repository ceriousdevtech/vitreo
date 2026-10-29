// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_buffer_access.cpp
/// @brief Push-block and guarded buffer access.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

u32 DxbcTranslator::push_block_var() {
    if (m_sampler_biases == 0) {
        m_sampler_biases = m_builder.emit_variable(
            m_type_ptr_push_sampler_biases, SpvStorageClass::PushConstant);
        m_builder.emit_name(m_sampler_biases, "vitreo_sampler_lod_biases");
    }
    return m_sampler_biases;
}

u32 DxbcTranslator::typed_buffer_entry(bool uav, u32 slot) {
    if (!m_options.typed_buffer_bias ||
        slot >= (uav ? kGuardSizeUavSlots : kGuardSizeSrvSlots)) {
        return 0;
    }
    return m_builder.emit_load(
        m_type_uint,
        m_builder.emit_access_chain(
            m_type_ptr_push_uint, push_block_var(),
            {m_builder.emit_constant_int(m_type_uint, 2u),
             m_builder.emit_constant_int(
                 m_type_uint, (uav ? kGuardSizeSrvSlots : 0u) + slot)}));
}

u32 DxbcTranslator::typed_buffer_bias(u32 entry) {
    if (entry == 0) return 0;
    return m_builder.emit_bitwise_and(
        m_type_uint, entry,
        m_builder.emit_constant_int(m_type_uint, kTypedBufferBiasMask));
}

u32 DxbcTranslator::typed_buffer_out_of_view(u32 result_type, u32 return_type,
                                             u32 entry) {
    const bool integer = return_type == 4u || return_type == 3u;
    const u32 scalar_type = return_type == 4u ? m_type_uint
                          : return_type == 3u ? m_type_int
                                              : m_type_float;
    const u32 zero = integer ? m_builder.emit_constant_int(scalar_type, 0u)
                             : m_builder.emit_constant_float(m_type_float, 0.0f);
    const u32 one = integer ? m_builder.emit_constant_int(scalar_type, 1u)
                            : m_builder.emit_constant_float(m_type_float, 1.0f);
    // D3D11.3 19.1.3.3: a component the format lacks reads 0, alpha 1.
    const u32 alpha_missing = m_builder.emit_inotequal(
        m_type_bool,
        m_builder.emit_bitwise_and(
            m_type_uint, entry,
            m_builder.emit_constant_int(m_type_uint, kTypedBufferAlphaMissing)),
        m_const_u0);
    const u32 alpha = m_builder.emit_select(scalar_type, alpha_missing, one,
                                            zero);
    return m_builder.emit_composite_construct(result_type,
                                              {zero, zero, zero, alpha});
}

u32 DxbcTranslator::typed_buffer_count(u32 image, u32 bias) {
    m_builder.emit_capability(SpvCapability::ImageQuery);
    const u32 width = m_builder.emit_bitcast(
        m_type_uint, m_builder.emit_image_query_size(m_type_int, image));
    // The texture is bias + NumElements wide: the bias never exceeds it.
    return bias == 0 ? width : m_builder.emit_isub(m_type_uint, width, bias);
}

u32 DxbcTranslator::load_rgb32_buffer_element(u32 image, u32 result_type,
                                              u32 return_type, u32 element,
                                              u32 bias) {
    const bool integer = return_type == 4u || return_type == 3u;
    const u32 scalar_type = return_type == 4u ? m_type_uint
                          : return_type == 3u ? m_type_int
                                              : m_type_float;
    const u32 three = m_builder.emit_constant_int(m_type_uint, 3u);
    // The view holds texels / 3 elements, after the bias's texels. Past them
    // the three stored components read 0 and the missing alpha still reads
    // 1 (measured on Windows, tools/oracle_probe/
    // d3d11_a03_rgb32_buffer_probe: "thread 5 float 0 0 0 3f800000").
    // Nothing is fetched for an element past the view (the index is tested
    // before any bias is added, so none wraps in front of it).
    const u32 texels = typed_buffer_count(image, bias);
    const u32 in_bounds = m_builder.emit_integer_compare(
        SpvOp::OpULessThan, m_type_bool, element,
        m_builder.emit_udiv(m_type_uint, texels, three));
    const u32 zero = integer ? m_builder.emit_constant_int(scalar_type, 0u)
                             : m_builder.emit_constant_float(m_type_float, 0.0f);
    // D3D11.3 19.1.3.3: a component the format lacks reads 0, alpha 1.
    const u32 one = integer ? m_builder.emit_constant_int(scalar_type, 1u)
                            : m_builder.emit_constant_float(m_type_float, 1.0f);
    const u32 read_block = m_builder.allocate_id();
    const u32 outside_block = m_builder.allocate_id();
    const u32 merge_block = m_builder.allocate_id();
    m_builder.emit_selection_merge(merge_block);
    m_builder.emit_branch_conditional(in_bounds, read_block, outside_block);
    m_builder.emit_label_id(read_block);
    u32 first = m_builder.emit_imul(m_type_uint, element, three);
    if (bias != 0) first = m_builder.emit_iadd(m_type_uint, first, bias);
    std::array<u32, 4> components{};
    for (u32 i = 0; i < 3; ++i) {
        const u32 texel = m_builder.emit_iadd(
            m_type_uint, first, m_builder.emit_constant_int(m_type_uint, i));
        components[i] = m_builder.emit_composite_extract(
            scalar_type,
            m_builder.emit_image_fetch_plain(result_type, image, texel), {0});
    }
    components[3] = one;
    const u32 read = m_builder.emit_composite_construct(
        result_type, {components[0], components[1], components[2],
                      components[3]});
    m_builder.emit_branch(merge_block);
    m_builder.emit_label_id(outside_block);
    const u32 outside =
        m_builder.emit_composite_construct(result_type, {zero, zero, zero, one});
    m_builder.emit_branch(merge_block);
    m_builder.emit_label_id(merge_block);
    return m_builder.emit_phi(result_type,
                              {{read, read_block}, {outside, outside_block}});
}

bool DxbcTranslator::guards_buffer(Sm4OperandType type) const {
    // The geometry/hull kernels and the stream-out repack are dispatched by
    // paths that bind no push block, so an unset buffer 0 must not be read
    // as sizes there.
    return m_options.guard_buffer_bounds && !gs_as_compute() &&
           !hs_as_compute() &&
           (type == Sm4OperandType::Resource ||
            type == Sm4OperandType::UnorderedAccessView);
}

u32 DxbcTranslator::volume_uav_window(u32 slot, u32 coordinate,
                                      u32& outside) {
    outside = 0;
    if (!m_volume_uav_slots.contains(slot) ||
        !guards_buffer(Sm4OperandType::UnorderedAccessView) ||
        slot >= kGuardSizeUavSlots) {
        return coordinate;
    }
    const u32 window = m_builder.emit_load(
        m_type_uint,
        m_builder.emit_access_chain(
            m_type_ptr_push_uint, push_block_var(),
            {m_builder.emit_constant_int(m_type_uint, 2u),
             m_builder.emit_constant_int(m_type_uint,
                                         kGuardSizeSrvSlots + slot)}));
    const u32 first = m_builder.emit_bitwise_and(
        m_type_uint, window, m_builder.emit_constant_int(m_type_uint, 0xFFFFu));
    const u32 size = m_builder.emit_shift_right_logical(
        m_type_uint, window,
        m_builder.emit_constant_int(m_type_uint, kVolumeUavWindowSizeShift));
    const u32 z = m_builder.emit_composite_extract(m_type_uint, coordinate, {2u});
    // 0 = the whole mip: no window.
    outside = m_builder.emit_logical(
        SpvOp::OpLogicalAnd, m_type_bool,
        m_builder.emit_inotequal(m_type_bool, window, m_const_u0),
        m_builder.emit_integer_compare(SpvOp::OpUGreaterThanEqual,
                                       m_type_bool, z, size));
    return m_builder.emit_composite_insert(
        m_type_uvec3, m_builder.emit_iadd(m_type_uint, z, first), coordinate,
        {2u});
}

u32 DxbcTranslator::buffer_word_in_bounds(bool uav, u32 slot, u32 word) {
    // Every slot has its own entry: t9 is never checked against t1's view.
    // A slot past the table (u8+ from an 11.1 shader) has no entry: no check.
    if (slot >= (uav ? kGuardSizeUavSlots : kGuardSizeSrvSlots)) {
        return m_builder.emit_integer_compare(SpvOp::OpIEqual, m_type_bool,
                                              m_const_u0, m_const_u0);
    }
    const u32 index = m_builder.emit_constant_int(
        m_type_uint, (uav ? kGuardSizeSrvSlots : 0u) + slot);
    const u32 member = m_builder.emit_constant_int(m_type_uint, 2u);
    const u32 pointer = m_builder.emit_access_chain(
        m_type_ptr_push_uint, push_block_var(), {member, index});
    const u32 size_words = m_builder.emit_load(m_type_uint, pointer);
    // 0 = the frontend did not describe this slot: no check.
    const u32 unknown = m_builder.emit_integer_compare(
        SpvOp::OpIEqual, m_type_bool, size_words, m_const_u0);
    const u32 limit = m_builder.emit_select(
        m_type_uint, unknown,
        m_builder.emit_constant_int(m_type_uint, 0xFFFFFFFFu), size_words);
    return m_builder.emit_integer_compare(SpvOp::OpULessThan, m_type_bool,
                                          word, limit);
}

u32 DxbcTranslator::guarded_buffer_word_load(u32 variable, u32 pointer_type,
                                             u32 word, u32 in_bounds) {
    const u32 safe_word =
        m_builder.emit_select(m_type_uint, in_bounds, word, m_const_u0);
    const u32 member = m_builder.emit_constant_int(m_type_uint, 0u);
    const u32 pointer = m_builder.emit_access_chain(pointer_type, variable,
                                                    {member, safe_word});
    const u32 loaded = m_builder.emit_load(m_type_uint, pointer);
    return m_builder.emit_select(m_type_uint, in_bounds, loaded, m_const_u0);
}

void DxbcTranslator::guarded_buffer_word_store(u32 variable, u32 pointer_type,
                                               u32 word, u32 value,
                                               u32 in_bounds) {
    const u32 write_block = m_builder.allocate_id();
    const u32 merge_block = m_builder.allocate_id();
    m_builder.emit_selection_merge(merge_block);
    m_builder.emit_branch_conditional(in_bounds, write_block, merge_block);
    m_builder.emit_label_id(write_block);
    const u32 member = m_builder.emit_constant_int(m_type_uint, 0u);
    m_builder.emit_store(
        m_builder.emit_access_chain(pointer_type, variable, {member, word}),
        value);
    m_builder.emit_branch(merge_block);
    m_builder.emit_label_id(merge_block);
}

bool DxbcTranslator::resolve_structured_operand(
    const Sm4Operand& resource, const char* opcode_name,
    u32& variable, u32& stride, u32& word_pointer_type,
    bool& indexed_through_member) {
    const std::string prefix = std::string(opcode_name) + ": ";
    if (resource.type == Sm4OperandType::Resource) {
        const auto found = m_structured_srv_vars.find(resource.index0);
        if (found == m_structured_srv_vars.end()) {
            return fail(prefix + "undeclared structured t" +
                        std::to_string(resource.index0));
        }
        variable = found->second;
        stride = m_structured_srv_strides[resource.index0];
        word_pointer_type = m_type_ptr_storage_buffer_uint;
        indexed_through_member = true;
    } else if (resource.type == Sm4OperandType::UnorderedAccessView) {
        const auto found = m_structured_uav_vars.find(resource.index0);
        if (found == m_structured_uav_vars.end()) {
            return fail(prefix + "undeclared structured u" +
                        std::to_string(resource.index0));
        }
        variable = found->second;
        stride = m_structured_uav_strides[resource.index0];
        word_pointer_type = m_type_ptr_storage_buffer_uint;
        indexed_through_member = true;
    } else if (resource.type == Sm4OperandType::ThreadGroupSharedMemory) {
        if (m_result.stage != ShaderStage::Compute) {
            return fail(prefix + "groupshared memory outside compute");
        }
        const auto found = m_tgsm_vars.find(resource.index0);
        const auto stride_it = m_tgsm_strides.find(resource.index0);
        if (found == m_tgsm_vars.end() || stride_it == m_tgsm_strides.end()) {
            return fail(prefix + "undeclared structured g" +
                        std::to_string(resource.index0));
        }
        variable = found->second;
        stride = stride_it->second;
        word_pointer_type = m_type_ptr_workgroup_uint;
        indexed_through_member = false;
    } else {
        return fail(prefix + "resource is not a t#, u# or g# operand");
    }
    if (stride == 0u || (stride % 4u) != 0u) {
        return fail(prefix + "structure stride " + std::to_string(stride) +
                    " is not a word multiple");
    }
    return true;
}

} // namespace vitreo::graphics::dxbc_spirv_detail
