// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_pixel_stats.cpp
/// @brief PSInvocations counting.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

// PSInvocations counting (DxbcTranslationOptions::count_pixel_invocations):
// the statistics record and the builtins the prologue reads.
bool DxbcTranslator::emit_pixel_statistics_declarations() {
    m_builder.emit_capability(SpvCapability::GroupNonUniform);
    m_builder.emit_capability(SpvCapability::GroupNonUniformBallot);
    const u32 array_ty = m_builder.emit_type_runtime_array(m_type_uint);
    m_builder.emit_decorate(array_ty, SpvDecoration::ArrayStride, 4u);
    const u32 struct_ty = m_builder.emit_type_struct({array_ty});
    m_builder.emit_decorate(struct_ty, SpvDecoration::Block);
    m_builder.emit_member_decorate(struct_ty, 0, SpvDecoration::Offset, 0);
    m_stats_record_var = m_builder.emit_variable(
        m_builder.emit_type_pointer(SpvStorageClass::StorageBuffer, struct_ty),
        SpvStorageClass::StorageBuffer);
    m_builder.emit_name(m_stats_record_var, "vitreo_pipeline_statistics");
    m_builder.emit_decorate(m_stats_record_var, SpvDecoration::DescriptorSet,
                            SPIRV_STATISTICS_DESCRIPTOR_SET);
    m_builder.emit_decorate(m_stats_record_var, SpvDecoration::Binding, 0u);
    m_stats_helper_var = m_builder.emit_variable(m_type_ptr_input_bool,
                                                 SpvStorageClass::Input);
    m_builder.emit_name(m_stats_helper_var, "vitreo_helper_invocation");
    m_builder.emit_decorate(m_stats_helper_var, SpvDecoration::BuiltIn,
                            static_cast<u32>(SpvBuiltIn::HelperInvocation));
    m_stats_lane_var = m_builder.emit_variable(m_type_ptr_input_uint,
                                               SpvStorageClass::Input);
    m_builder.emit_name(m_stats_lane_var, "vitreo_subgroup_lane");
    m_builder.emit_decorate(
        m_stats_lane_var, SpvDecoration::BuiltIn,
        static_cast<u32>(SpvBuiltIn::SubgroupLocalInvocationId));
    if (m_uav_leader_var != 0) return true;
    // A shader that runs per sample itself (SV_SampleIndex, or a `sample`
    // input) has no leader gate: the pixel test reads its SampleId -- the
    // shader's own SV_SampleIndex variable when it has one, as Metal takes a
    // [[sample_id]] once -- and the raster coverage.
    for (const auto& input : m_container.input_signature) {
        if (input.system_value_type != 10u) continue;
        const auto found = m_input_scalar_builtins.find(input.register_num);
        if (found != m_input_scalar_builtins.end() && !found->second.empty()) {
            m_stats_sample_id = found->second.front().var;
        }
    }
    if (m_stats_sample_id == 0) {
        m_builder.emit_capability(SpvCapability::SampleRateShading);
        m_stats_sample_id = m_builder.emit_variable(m_type_ptr_input_uint,
                                                    SpvStorageClass::Input);
        m_builder.emit_name(m_stats_sample_id, "vitreo_stats_sample_id");
        m_builder.emit_decorate(m_stats_sample_id, SpvDecoration::BuiltIn,
                                static_cast<u32>(SpvBuiltIn::SampleId));
        m_stats_sample_id_declared = true;
    }
    if (m_sample_mask_input == 0) {
        const u32 coverage_ty = m_builder.emit_type_array(
            m_type_uint, m_builder.emit_constant_int(m_type_uint, 1u));
        m_sample_mask_input = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::Input, coverage_ty),
            SpvStorageClass::Input);
        m_builder.emit_name(m_sample_mask_input, "vitreo_coverage");
        m_builder.emit_decorate(m_sample_mask_input, SpvDecoration::BuiltIn,
                                static_cast<u32>(SpvBuiltIn::SampleMask));
        m_builder.emit_decorate_string(m_sample_mask_input,
                                       SpvDecoration::UserSemantic,
                                       kRasterCoverageSemantic);
    }
    return true;
}

// On entry, before anything can discard: one atomic per subgroup adds the
// subgroup's count to the record's PSInvocations. Single-sample (push member
// 5 <= 1): every lane of each 2x2 quad holding a non-helper lane -- a quad
// is four consecutive subgroup lanes (SPIR-V's quad group; on Apple GPUs
// the 2x2 stamp, measured: 4224 for a 64x64 two-triangle quad, 4 for a
// one-pixel triangle, 8 for two pixels in two stamps). Multisampled: the
// covered pixels -- the invocations at each pixel's lowest covered sample.
void DxbcTranslator::emit_pixel_statistics_prologue() {
    const u32 real = m_builder.emit_logical_not(
        m_type_bool, m_builder.emit_load(m_type_bool, m_stats_helper_var));
    const u32 samples = m_builder.emit_load(
        m_type_uint,
        m_builder.emit_access_chain(
            m_type_ptr_push_uint, push_block_var(),
            {m_builder.emit_constant_int(m_type_uint,
                                         kPushMemberRasterizerSampleCount)}));
    const u32 one = m_builder.emit_constant_int(m_type_uint, 1u);
    const u32 multisampled = m_builder.emit_integer_compare(
        SpvOp::OpUGreaterThan, m_type_bool, samples, one);
    u32 leader = 0;
    if (m_uav_leader_var != 0) {
        leader = load_uav_leader();
    } else {
        const u32 sample = m_builder.emit_load(m_type_uint, m_stats_sample_id);
        const u32 coverage = m_builder.emit_load(
            m_type_uint,
            m_builder.emit_access_chain(
                m_builder.emit_type_pointer(SpvStorageClass::Input,
                                            m_type_uint),
                m_sample_mask_input, {m_const_u0}));
        leader = m_builder.emit_integer_compare(
            SpvOp::OpIEqual, m_type_bool, sample,
            m_builder.emit_ext_inst(m_type_uint, m_glsl_std450,
                                    kGlslStd450FindILsb, {coverage}));
    }
    const u32 counted = m_builder.emit_select(
        m_type_bool, multisampled,
        m_builder.emit_logical(SpvOp::OpLogicalAnd, m_type_bool, real, leader),
        real);
    const u32 ballot = m_builder.emit_group_ballot(
        m_type_uvec4, m_builder.emit_constant_int(m_type_uint, 3u),  // Subgroup
        counted);
    const u32 lo = m_builder.emit_composite_extract(m_type_uint, ballot, {0u});
    const u32 hi = m_builder.emit_composite_extract(m_type_uint, ballot, {1u});
    const auto quads = [&](u32 bits) {
        u32 any = bits;
        for (u32 shift = 1; shift <= 3; ++shift) {
            any = m_builder.emit_bitwise_or(
                m_type_uint, any,
                m_builder.emit_shift_right_logical(
                    m_type_uint, bits,
                    m_builder.emit_constant_int(m_type_uint, shift)));
        }
        return m_builder.emit_unary_integer(
            SpvOp::OpBitCount, m_type_uint,
            m_builder.emit_bitwise_and(
                m_type_uint, any,
                m_builder.emit_constant_int(m_type_uint, 0x11111111u)));
    };
    const u32 quad_lanes = m_builder.emit_shift_left_logical(
        m_type_uint, m_builder.emit_iadd(m_type_uint, quads(lo), quads(hi)),
        m_builder.emit_constant_int(m_type_uint, 2u));
    const u32 pixels = m_builder.emit_iadd(
        m_type_uint,
        m_builder.emit_unary_integer(SpvOp::OpBitCount, m_type_uint, lo),
        m_builder.emit_unary_integer(SpvOp::OpBitCount, m_type_uint, hi));
    const u32 count =
        m_builder.emit_select(m_type_uint, multisampled, pixels, quad_lanes);
    const u32 first = m_builder.emit_select(
        m_type_uint,
        m_builder.emit_integer_compare(SpvOp::OpINotEqual, m_type_bool, lo,
                                       m_const_u0),
        m_builder.emit_ext_inst(m_type_uint, m_glsl_std450,
                                kGlslStd450FindILsb, {lo}),
        m_builder.emit_iadd(
            m_type_uint, m_builder.emit_constant_int(m_type_uint, 32u),
            m_builder.emit_ext_inst(m_type_uint, m_glsl_std450,
                                    kGlslStd450FindILsb, {hi})));
    const u32 lane = m_builder.emit_load(m_type_uint, m_stats_lane_var);
    const u32 adds = m_builder.emit_logical(
        SpvOp::OpLogicalAnd, m_type_bool, counted,
        m_builder.emit_integer_compare(SpvOp::OpIEqual, m_type_bool, lane,
                                       first));
    const u32 uint_ptr =
        m_builder.emit_type_pointer(SpvStorageClass::StorageBuffer, m_type_uint);
    const u32 device_scope = m_builder.emit_constant_int(m_type_uint, 1u);
    const u32 relaxed = m_const_u0;
    const u32 add_block = m_builder.allocate_id();
    const u32 done = m_builder.allocate_id();
    m_builder.emit_selection_merge(done);
    m_builder.emit_branch_conditional(adds, add_block, done);
    m_builder.emit_label_id(add_block);
    {
        const u32 low_ptr = m_builder.emit_access_chain(
            uint_ptr, m_stats_record_var,
            {m_const_u0, m_builder.emit_constant_int(
                             m_type_uint, 2u * STAT_PS_INVOCATIONS)});
        const u32 old = m_builder.emit_atomic_rmw(
            SpvOp::OpAtomicIAdd, m_type_uint, low_ptr, device_scope, relaxed,
            count);
        // The 64-bit sum's carry (pipeline_stats.hpp).
        const u32 wrapped = m_builder.emit_integer_compare(
            SpvOp::OpULessThan, m_type_bool,
            m_builder.emit_iadd(m_type_uint, old, count), old);
        const u32 carry_block = m_builder.allocate_id();
        const u32 carried = m_builder.allocate_id();
        m_builder.emit_selection_merge(carried);
        m_builder.emit_branch_conditional(wrapped, carry_block, carried);
        m_builder.emit_label_id(carry_block);
        const u32 high_ptr = m_builder.emit_access_chain(
            uint_ptr, m_stats_record_var,
            {m_const_u0, m_builder.emit_constant_int(
                             m_type_uint, 2u * STAT_PS_INVOCATIONS + 1u)});
        (void)m_builder.emit_atomic_rmw(SpvOp::OpAtomicIAdd, m_type_uint,
                                        high_ptr, device_scope, relaxed, one);
        m_builder.emit_branch(carried);
        m_builder.emit_label_id(carried);
    }
    m_builder.emit_branch(done);
    m_builder.emit_label_id(done);
}

} // namespace vitreo::graphics::dxbc_spirv_detail
