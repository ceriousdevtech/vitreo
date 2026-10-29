// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_tessellation.cpp
/// @brief Hull and domain shader lowering.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

// Temps (r#) and indexable temps (x#) of the function being emitted, declared
// at its entry as SPIR-V requires; scanned from @p start_token to the next
// `ret` (the program, or one hull-shader phase).
void DxbcTranslator::declare_function_temps(usize start_token) {
    m_temp_vars.clear();
    m_indexable_temp_vars.clear();
    const u32 temp_count = scan_temp_count(start_token);
    for (u32 i = 0; i < temp_count; ++i) {
        (void)get_temp_variable(i);
    }
    for (const auto& [slot, count] : scan_indexable_temps(start_token)) {
        const u32 length =
            m_builder.emit_constant_int(m_type_uint, count);
        const u32 array_type =
            m_builder.emit_type_array(m_type_vec4, length);
        const u32 pointer_type = m_builder.emit_type_pointer(
            temp_storage_class(), array_type);
        const u32 variable = m_builder.emit_variable(
            pointer_type, temp_storage_class());
        m_builder.emit_name(variable, "x" + std::to_string(slot));
        m_indexable_temp_vars[slot] = variable;
    }
}

// One SPIR-V function per hull-shader phase: its own temps, its `ret` a plain
// OpReturn, its o# the control-point staging registers (control-point phase)
// or the patch constants (fork / join phases).
bool DxbcTranslator::emit_hull_phase_functions() {
    if (!scan_subroutine_labels()) return false;
    for (usize i = 0; i < m_hs_phases.size(); ++i) {
        const HsPhase& phase = m_hs_phases[i];
        const u32 function = m_builder.emit_function(m_type_void, m_func_type);
        m_builder.emit_name(function, "hs_phase" + std::to_string(i));
        (void)m_builder.emit_label();
        declare_function_temps(phase.begin);
        if (phase.kind == HsPhaseKind::ControlPoint) {
            m_output_vars = m_hs_cp_staging_vars;
            m_output_value_types = m_hs_cp_staging_types;
        } else {
            m_output_vars = m_tess_pc_vars;
            m_output_value_types = m_tess_pc_types;
        }
        m_decoder.set_position(phase.begin);
        if (!translate_instructions(phase.end)) return false;
        if (!m_if_stack.empty() || !m_loop_stack.empty() ||
            !m_switch_stack.empty()) {
            return fail("hull shader: a phase ends inside structured flow");
        }
        m_builder.emit_return();
        m_builder.emit_function_end();
        m_hs_phase_functions.push_back(function);
    }
    m_temp_vars.clear();
    m_indexable_temp_vars.clear();
    return true;
}

// main of the hull kernel: this thread's patch through every phase in order,
// then the results out (emit_hull_epilogue).
bool DxbcTranslator::emit_hull_program() {
    if (!emit_geometry_input_prologue()) return false;
    const auto copy_register = [&](u32 source_var, u32 source_type,
                                   bool source_arrayed, u32 source_index,
                                   u32 reg, u32 cp) {
        const auto destination = m_hs_ocp_vars.find(reg);
        if (destination == m_hs_ocp_vars.end()) return;
        const u32 dest_type = m_hs_ocp_types.at(reg);
        u32 value = m_builder.emit_load(
            source_type,
            source_arrayed
                ? m_builder.emit_access_chain(
                      m_builder.emit_type_pointer(SpvStorageClass::Private,
                                                  source_type),
                      source_var,
                      {m_builder.emit_constant_int(m_type_uint, source_index)})
                : source_var);
        if (source_type != dest_type) {
            value = m_builder.emit_bitcast(dest_type, value);
        }
        m_builder.emit_store(
            m_builder.emit_access_chain(
                m_builder.emit_type_pointer(SpvStorageClass::Private,
                                            dest_type),
                destination->second,
                {m_builder.emit_constant_int(m_type_uint, cp)}),
            value);
    };
    const bool has_cp_phase = std::any_of(
        m_hs_phases.begin(), m_hs_phases.end(),
        [](const HsPhase& p) { return p.kind == HsPhaseKind::ControlPoint; });
    if (!has_cp_phase) {
        // No control-point phase: the output control points are the input
        // ones, register for register (fxc elides a pass-through phase).
        for (u32 cp = 0; cp < m_hs_out_cp; ++cp) {
            for (const auto& [reg, var] : m_input_vars) {
                copy_register(var, m_input_value_types.at(reg), true, cp, reg,
                              cp);
            }
        }
    }
    for (usize i = 0; i < m_hs_phases.size(); ++i) {
        const HsPhase& phase = m_hs_phases[i];
        const u32 function = m_hs_phase_functions[i];
        const u32 count = phase.kind == HsPhaseKind::ControlPoint
                              ? m_hs_out_cp
                              : phase.instances;
        const u32 id_var = phase.kind == HsPhaseKind::ControlPoint
                               ? m_hs_cp_id_var
                           : phase.kind == HsPhaseKind::Fork ? m_hs_fork_id_var
                                                             : m_hs_join_id_var;
        for (u32 n = 0; n < count; ++n) {
            m_builder.emit_store(id_var,
                                 m_builder.emit_constant_int(m_type_uint, n));
            (void)m_builder.emit_function_call(m_type_void, function);
            if (phase.kind == HsPhaseKind::ControlPoint) {
                for (const auto& [reg, var] : m_hs_cp_staging_vars) {
                    copy_register(var, m_hs_cp_staging_types.at(reg), false, 0,
                                  reg, n);
                }
            }
        }
    }
    return emit_hull_epilogue();
}

// Output control points to binding 1 ([patch][cp][register], one slot per
// register number), patch constants to binding 4 ([patch][register]), and
// the tessellation factors to binding 5 in Metal's half-precision layout:
// MTLTriangleTessellationFactorsHalf {edge[3], inside} = 2 words,
// MTLQuadTessellationFactorsHalf {edge[4], inside[2]} = 3 words.
bool DxbcTranslator::emit_hull_epilogue() {
    const u32 member = m_const_u0;
    const u32 vec4_ptr =
        m_builder.emit_type_pointer(SpvStorageClass::StorageBuffer, m_type_vec4);
    const u32 uint_ptr =
        m_builder.emit_type_pointer(SpvStorageClass::StorageBuffer, m_type_uint);
    const u32 patch = m_gs_primitive_index;
    const u32 cp_stride = std::max(m_result.hs_cp_record_vec4s, 1u);
    const u32 pc_stride = std::max(m_result.hs_pc_record_vec4s, 1u);
    const auto as_vec4 = [&](u32 value, u32 type) {
        return type == m_type_vec4 ? value
                                   : m_builder.emit_bitcast(m_type_vec4, value);
    };
    for (const auto& [reg, var] : m_hs_ocp_vars) {
        const u32 type = m_hs_ocp_types.at(reg);
        for (u32 cp = 0; cp < m_hs_out_cp; ++cp) {
            const u32 value = m_builder.emit_load(
                type, m_builder.emit_access_chain(
                          m_builder.emit_type_pointer(SpvStorageClass::Private,
                                                      type),
                          var, {m_builder.emit_constant_int(m_type_uint, cp)}));
            const u32 slot = m_builder.emit_iadd(
                m_type_uint,
                m_builder.emit_imul(
                    m_type_uint, patch,
                    m_builder.emit_constant_int(m_type_uint,
                                                m_hs_out_cp * cp_stride)),
                m_builder.emit_constant_int(m_type_uint, cp * cp_stride + reg));
            m_builder.emit_store(
                m_builder.emit_access_chain(vec4_ptr, m_gs_out_var,
                                            {member, slot}),
                as_vec4(value, type));
        }
    }
    for (const auto& [reg, var] : m_tess_pc_vars) {
        const u32 type = m_tess_pc_types.at(reg);
        const u32 slot = m_builder.emit_iadd(
            m_type_uint,
            m_builder.emit_imul(m_type_uint, patch,
                                m_builder.emit_constant_int(m_type_uint,
                                                            pc_stride)),
            m_builder.emit_constant_int(m_type_uint, reg));
        m_builder.emit_store(
            m_builder.emit_access_chain(vec4_ptr, m_hs_pc_buffer,
                                        {member, slot}),
            as_vec4(m_builder.emit_load(type, var), type));
    }

    // Tessellation factors. D3D names each by edge (SV_TessFactor index i:
    // finalTriUeq0 / Veq0 / Weq0, finalQuadUeq0 / Veq0 / Ueq1 / Veq1;
    // isolines finalLineDensity (D3D_NAME 16) then finalLineDetail (15)),
    // and Metal takes them in the same order (pinned by the GPU tests).
    // Everything is clamped to the declared maxtessfactor, which D3D11.3
    // 10.13 leaves to the shader ("enforced by HLSL autogenerated TessFactor
    // clamping code as the last step"); NaN stays NaN for the tessellator.
    const bool quad = m_tess_domain == 3u;
    const bool isoline = m_tess_domain == 1u;
    const u32 max_factor = m_builder.emit_constant_float(
        m_type_float, std::clamp(m_hs_max_tess_factor, 1.0f, 64.0f));
    const auto raw_factor = [&](u32 sv, u32 index) -> u32 {
        for (const auto& e : m_container.patch_constant_signature) {
            if (e.system_value_type != sv) continue;
            // An isoline's two factors are one element each (index 0 and 1
            // of the SV_TessFactor array, but distinct system values).
            if (!isoline && e.semantic_index != index) continue;
            const auto var = m_tess_pc_vars.find(e.register_num);
            if (var == m_tess_pc_vars.end()) break;
            u32 component = 0;
            while (component < 3 && (e.component_mask & (1u << component)) == 0)
                ++component;
            const u32 type = m_tess_pc_types.at(e.register_num);
            const u32 v = as_vec4(m_builder.emit_load(type, var->second), type);
            const u32 f = m_builder.emit_composite_extract(m_type_float, v,
                                                           {component});
            return m_builder.emit_select(
                m_type_float, m_builder.emit_ford_lt(m_type_bool, max_factor, f),
                max_factor, f);
        }
        return m_builder.emit_constant_float(m_type_float, 0.0f);
    };
    if (m_options.domain_to_compute) {
        // The software tessellator takes the raw float32 factors
        // (kTessFactorWords per patch, in its own argument order) and
        // applies the D3D11 hardware rules itself: culling, range clamps,
        // integer rounding, fixed-point conversion (11.7.11).
        std::vector<u32> raw;
        if (isoline) {
            raw = {raw_factor(16u, 0u), raw_factor(15u, 1u)};
        } else if (quad) {
            raw = {raw_factor(11u, 0u), raw_factor(11u, 1u),
                   raw_factor(11u, 2u), raw_factor(11u, 3u),
                   raw_factor(12u, 0u), raw_factor(12u, 1u)};
        } else {
            raw = {raw_factor(13u, 0u), raw_factor(13u, 1u),
                   raw_factor(13u, 2u), raw_factor(14u, 0u)};
        }
        for (u32 w = 0; w < raw.size(); ++w) {
            const u32 slot = m_builder.emit_iadd(
                m_type_uint,
                m_builder.emit_imul(
                    m_type_uint, patch,
                    m_builder.emit_constant_int(m_type_uint, kTessFactorWords)),
                m_builder.emit_constant_int(m_type_uint, w));
            m_builder.emit_store(
                m_builder.emit_access_chain(uint_ptr, m_hs_factor_buffer,
                                            {member, slot}),
                m_builder.emit_bitcast(m_type_uint, raw[w]));
        }
        return true;
    }

    // Metal's tessellator: half-precision factors. An edge factor that is not
    // positive -- or NaN -- culls the patch in D3D; Metal culls a patch with
    // a zero edge factor, so both become 0. For integer and pow2 partitioning
    // the D3D11 hardware clamp to [1, 64] and round-up (11.7.11; "Pow2 maps to
    // integer in hardware", the reference tessellator's
    // HWIntegerPartitioning) are applied here in float32, so the halves are
    // exact integers and Metal's integer partitioning sees what D3D's does.
    const bool integer_rounding =
        m_tess_partitioning == 1u || m_tess_partitioning == 2u;
    const u32 edge_sv = quad ? 11u : 13u;
    const u32 inside_sv = quad ? 12u : 14u;
    const auto factor = [&](u32 sv, u32 index, bool edge) -> u32 {
        const u32 f = raw_factor(sv, index);
        const u32 zero = m_builder.emit_constant_float(m_type_float, 0.0f);
        const u32 one = m_builder.emit_constant_float(m_type_float, 1.0f);
        const u32 positive = m_builder.emit_ford_lt(m_type_bool, zero, f);
        u32 value = f;
        if (integer_rounding) {
            // positive -> >= its own value; the inside's NaN/<=0 -> 1 below.
            value = m_builder.emit_ext_inst(
                m_type_float, m_glsl_std450, kGlslStd450Ceil,
                {m_builder.emit_select(
                    m_type_float, m_builder.emit_ford_lt(m_type_bool, f, one),
                    one, f)});
        }
        return m_builder.emit_select(m_type_float, positive, value,
                                     edge ? zero : one);
    };
    std::vector<u32> halves;
    for (u32 i = 0; i < (quad ? 4u : 3u); ++i) {
        halves.push_back(factor(edge_sv, i, true));
    }
    for (u32 i = 0; i < (quad ? 2u : 1u); ++i) {
        halves.push_back(factor(inside_sv, i, false));
    }
    const u32 words = quad ? 3u : 2u;
    for (u32 w = 0; w < words; ++w) {
        const u32 pair = m_builder.emit_composite_construct(
            m_type_vec2, {halves[w * 2], halves[w * 2 + 1]});
        const u32 packed = m_builder.emit_ext_inst(
            m_type_uint, m_glsl_std450, kGlslStd450PackHalf2x16, {pair});
        const u32 slot = m_builder.emit_iadd(
            m_type_uint,
            m_builder.emit_imul(m_type_uint, patch,
                                m_builder.emit_constant_int(m_type_uint, words)),
            m_builder.emit_constant_int(m_type_uint, w));
        m_builder.emit_store(
            m_builder.emit_access_chain(uint_ptr, m_hs_factor_buffer,
                                        {member, slot}),
            packed);
    }
    return true;
}

// Domain: this patch's control points (vicp) and patch constants (vpc) out
// of the hull kernel's buffers. Record strides come from the control block
// ([5] control-point record, [6] patch-constant record, in vec4s), so one
// domain shader works behind any hull shader whose outputs it reads.
bool DxbcTranslator::emit_domain_input_prologue() {
    const u32 member = m_const_u0;
    const u32 vec4_ptr =
        m_builder.emit_type_pointer(SpvStorageClass::StorageBuffer, m_type_vec4);
    const u32 uint_ptr =
        m_builder.emit_type_pointer(SpvStorageClass::StorageBuffer, m_type_uint);
    const auto control = [&](u32 index) {
        return m_builder.emit_load(
            m_type_uint,
            m_builder.emit_access_chain(
                uint_ptr, m_gs_counter_var,
                {member, m_builder.emit_constant_int(m_type_uint, index)}));
    };
    const u32 cp_stride = control(5u);
    const u32 pc_stride = control(6u);
    // The software tessellator names the patch's slot in the hull output
    // apart from its SV_PrimitiveID (they differ across draw instances).
    const u32 patch = ds_as_compute()
        ? m_builder.emit_load(m_type_uint, m_ds_cp_patch)
        : m_builder.emit_bitcast(m_type_uint,
                                 m_builder.emit_load(m_type_int, m_ds_patch_id));
    const u32 cp_base = m_builder.emit_imul(
        m_type_uint, patch,
        m_builder.emit_imul(m_type_uint, cp_stride,
                            m_builder.emit_constant_int(m_type_uint,
                                                        m_ds_in_cp)));
    for (const auto& [reg, var] : m_input_vars) {
        const u32 type = m_input_value_types.at(reg);
        for (u32 cp = 0; cp < m_ds_in_cp; ++cp) {
            const u32 slot = m_builder.emit_iadd(
                m_type_uint, cp_base,
                m_builder.emit_iadd(
                    m_type_uint,
                    m_builder.emit_imul(
                        m_type_uint, cp_stride,
                        m_builder.emit_constant_int(m_type_uint, cp)),
                    m_builder.emit_constant_int(m_type_uint, reg)));
            u32 value = m_builder.emit_load(
                m_type_vec4, m_builder.emit_access_chain(
                                 vec4_ptr, m_gs_out_var, {member, slot}));
            if (type != m_type_vec4) value = m_builder.emit_bitcast(type, value);
            m_builder.emit_store(
                m_builder.emit_access_chain(
                    m_builder.emit_type_pointer(SpvStorageClass::Private, type),
                    var, {m_builder.emit_constant_int(m_type_uint, cp)}),
                value);
        }
    }
    const u32 pc_base = m_builder.emit_imul(m_type_uint, patch, pc_stride);
    for (const auto& [reg, var] : m_tess_pc_vars) {
        const u32 type = m_tess_pc_types.at(reg);
        u32 value = m_builder.emit_load(
            m_type_vec4,
            m_builder.emit_access_chain(
                vec4_ptr, m_hs_pc_buffer,
                {member, m_builder.emit_iadd(
                             m_type_uint, pc_base,
                             m_builder.emit_constant_int(m_type_uint, reg))}));
        if (type != m_type_vec4) value = m_builder.emit_bitcast(type, value);
        m_builder.emit_store(var, value);
    }
    return true;
}

// Domain on the software tessellator: this thread's domain point. The point
// buffer holds one uint4 per point -- u, v, w as float bits and the patch's
// SV_PrimitiveID -- and the control block's word 7 how many there are
// (threads past it return: the dispatch is sized for the most there can be).
bool DxbcTranslator::emit_domain_compute_prologue() {
    if (m_dispatch_thread_id == 0) {
        return fail("domain emulation: no dispatch thread id");
    }
    const u32 member = m_const_u0;
    const u32 uint_ptr =
        m_builder.emit_type_pointer(SpvStorageClass::StorageBuffer, m_type_uint);
    const u32 vec4_ptr =
        m_builder.emit_type_pointer(SpvStorageClass::StorageBuffer, m_type_vec4);
    const u32 point = m_builder.emit_composite_extract(
        m_type_uint, m_builder.emit_load(m_type_uvec3, m_dispatch_thread_id),
        {0u});
    const u32 count = m_builder.emit_load(
        m_type_uint,
        m_builder.emit_access_chain(
            uint_ptr, m_gs_counter_var,
            {member, m_builder.emit_constant_int(m_type_uint,
                                                 kDomainControlPointCount)}));
    const u32 exit_block = m_builder.allocate_id();
    const u32 run_block = m_builder.allocate_id();
    // The compare first: OpSelectionMerge must directly precede its
    // OpBranchConditional (spirv-val; SPIRV-Cross tolerated the reverse).
    const u32 past_end = m_builder.emit_integer_compare(
        SpvOp::OpUGreaterThanEqual, m_type_bool, point, count);
    m_builder.emit_selection_merge(run_block);
    m_builder.emit_branch_conditional(past_end, exit_block, run_block);
    m_builder.emit_label_id(exit_block);
    m_builder.emit_return();
    m_builder.emit_label_id(run_block);
    m_ds_point_index = point;
    const u32 record = m_builder.emit_bitcast(
        m_type_uvec4,
        m_builder.emit_load(m_type_vec4,
                            m_builder.emit_access_chain(vec4_ptr, m_gs_in_var,
                                                        {member, point})));
    // {u, v, the patch's index in the hull kernel's output, its
    // SV_PrimitiveID}. A tri domain's w is 1 - u - v: u and v are 16.16
    // fixed point, so the float subtraction is exact (the D3D11 reference
    // tessellator stores only u and v too).
    const u32 u = m_builder.emit_bitcast(
        m_type_float, m_builder.emit_composite_extract(m_type_uint, record, {0u}));
    const u32 v = m_builder.emit_bitcast(
        m_type_float, m_builder.emit_composite_extract(m_type_uint, record, {1u}));
    const u32 w = m_tess_domain == 2u
        ? m_builder.emit_fsub(
              m_type_float,
              m_builder.emit_fsub(m_type_float,
                                  m_builder.emit_constant_float(m_type_float, 1.0f),
                                  u),
              v)
        : m_builder.emit_constant_float(m_type_float, 0.0f);
    m_builder.emit_store(m_ds_tess_coord,
                         m_builder.emit_composite_construct(m_type_vec3,
                                                            {u, v, w}));
    m_builder.emit_store(
        m_ds_cp_patch,
        m_builder.emit_composite_extract(m_type_uint, record, {2u}));
    m_builder.emit_store(
        m_ds_patch_id,
        m_builder.emit_bitcast(
            m_type_int,
            m_builder.emit_composite_extract(m_type_uint, record, {3u})));
    return true;
}

// Every output register of this domain point into its record, ascending
// register order -- the layout of a vertex capture, which the pass-through,
// stream output and a geometry shader read.
bool DxbcTranslator::emit_domain_compute_epilogue() {
    if (m_emitted_early_return) {
        return fail("domain emulation: the shader returns early, so its "
                    "record would not be written for every point");
    }
    const u32 member = m_const_u0;
    const u32 vec4_ptr =
        m_builder.emit_type_pointer(SpvStorageClass::StorageBuffer, m_type_vec4);
    const u32 record_base = m_builder.emit_imul(
        m_type_uint, m_ds_point_index,
        m_builder.emit_constant_int(
            m_type_uint, static_cast<u32>(m_capture_registers.size())));
    for (usize i = 0; i < m_capture_registers.size(); ++i) {
        const u32 reg = m_capture_registers[i];
        const auto var = m_output_vars.find(reg);
        const auto value_type = m_output_value_types.find(reg);
        if (var == m_output_vars.end() ||
            value_type == m_output_value_types.end()) {
            return fail("domain emulation: output o" + std::to_string(reg) +
                        " has no variable");
        }
        const u32 loaded = m_builder.emit_load(value_type->second, var->second);
        const u32 raw = value_type->second == m_type_vec4
                            ? loaded
                            : m_builder.emit_bitcast(m_type_vec4, loaded);
        m_builder.emit_store(
            m_builder.emit_access_chain(
                vec4_ptr, m_capture_buffer,
                {member, m_builder.emit_iadd(
                             m_type_uint, record_base,
                             m_builder.emit_constant_int(
                                 m_type_uint, static_cast<u32>(i)))}),
            raw);
    }
    return true;
}

// Hull: the control-point staging registers (the o# its control-point phase
// writes), a Private array per register over the output control points, a
// Private vec4 per patch-constant register, the phase system values, and the
// kernel's buffers. Domain: its patch-constant inputs, TessCoord and
// PrimitiveId, and the hull kernel's output buffers it reads.
bool DxbcTranslator::emit_tessellation_declarations() {
    const auto component_type_of = [&](u8 component_type) {
        return component_type == 1u   ? m_type_uvec4
               : component_type == 2u ? m_type_ivec4
                                      : m_type_vec4;
    };
    for (const auto& element : m_container.patch_constant_signature) {
        if (element.register_num == 0xFFFFFFFFu ||
            m_tess_pc_vars.contains(element.register_num)) {
            continue;
        }
        const u32 type = component_type_of(element.component_type);
        const u32 var = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::Private, type),
            SpvStorageClass::Private);
        m_builder.emit_name(var, "pc" + std::to_string(element.register_num));
        m_tess_pc_vars[element.register_num] = var;
        m_tess_pc_types[element.register_num] = type;
    }
    const u32 private_uint =
        m_builder.emit_type_pointer(SpvStorageClass::Private, m_type_uint);
    for (const auto& [reg, _] : m_input_vars) m_gs_input_registers.push_back(reg);

    if (hs_as_compute()) {
        m_hs_cp_staging_vars = m_output_vars;
        m_hs_cp_staging_types = m_output_value_types;
        const u32 width = m_builder.emit_constant_int(
            m_type_uint, std::max(m_hs_out_cp, 1u));
        for (const auto& [reg, type] : m_hs_cp_staging_types) {
            const u32 var = m_builder.emit_variable(
                m_builder.emit_type_pointer(SpvStorageClass::Private,
                                            m_builder.emit_type_array(type,
                                                                      width)),
                SpvStorageClass::Private);
            m_builder.emit_name(var, "ocp" + std::to_string(reg));
            m_hs_ocp_vars[reg] = var;
            m_hs_ocp_types[reg] = type;
        }
        const auto sysval = [&](const char* name) {
            const u32 var =
                m_builder.emit_variable(private_uint, SpvStorageClass::Private);
            m_builder.emit_name(var, name);
            return var;
        };
        m_hs_cp_id_var = sysval("vOutputControlPointID");
        m_hs_fork_id_var = sysval("vForkInstanceID");
        m_hs_join_id_var = sysval("vJoinInstanceID");
        m_gs_primitive_var = sysval("vPrim");
        m_gs_in_var = declare_expansion_buffer(m_type_vec4, 16u, "vitreo_hs_in",
                                               0u, /*read_only=*/true);
        m_gs_out_var = declare_expansion_buffer(
            m_type_vec4, 16u, "vitreo_hs_cp_out", 1u, /*read_only=*/false);
        m_gs_counter_var = declare_expansion_buffer(
            m_type_uint, 4u, "vitreo_tess_control", 2u, /*read_only=*/true);
        m_gs_index_var = declare_expansion_buffer(
            m_type_uint, 4u, "vitreo_hs_index", 3u, /*read_only=*/true);
        m_hs_pc_buffer = declare_expansion_buffer(
            m_type_vec4, 16u, "vitreo_hs_pc_out", 4u, /*read_only=*/false);
        m_hs_factor_buffer = declare_expansion_buffer(
            m_type_uint, 4u, "vitreo_hs_tess_factors", 5u, /*read_only=*/false);
        return true;
    }

    // Domain.
    if (ds_as_compute()) {
        // The domain point and its patch come from the tessellator's point
        // buffer (binding 0), staged in Private variables by the prologue;
        // the outputs go to the record buffer (binding 3).
        m_ds_tess_coord = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::Private, m_type_vec3),
            SpvStorageClass::Private);
        m_builder.emit_name(m_ds_tess_coord, "vDomain");
        m_ds_patch_id = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::Private, m_type_int),
            SpvStorageClass::Private);
        m_builder.emit_name(m_ds_patch_id, "vPrim");
        m_ds_cp_patch = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::Private, m_type_uint),
            SpvStorageClass::Private);
        m_builder.emit_name(m_ds_cp_patch, "vitreo_ds_patch_slot");
        m_gs_in_var = declare_expansion_buffer(m_type_vec4, 16u,
                                               "vitreo_ds_points", 0u,
                                               /*read_only=*/true);
        m_capture_buffer = declare_expansion_buffer(
            m_type_vec4, 16u, "vitreo_ds_records", 3u, /*read_only=*/false);
        for (const auto& [reg, _] : m_output_vars) {
            m_capture_registers.push_back(reg);
        }
        m_result.capture_record_vec4s =
            static_cast<u32>(m_capture_registers.size());
        m_result.capture_registers = m_capture_registers;
    } else {
    m_ds_tess_coord = m_builder.emit_variable(
        m_builder.emit_type_pointer(SpvStorageClass::Input, m_type_vec3),
        SpvStorageClass::Input);
    m_builder.emit_name(m_ds_tess_coord, "vDomain");
    m_builder.emit_decorate(m_ds_tess_coord, SpvDecoration::BuiltIn,
                            static_cast<u32>(SpvBuiltIn::TessCoord));
    m_ds_patch_id = m_builder.emit_variable(
        m_builder.emit_type_pointer(SpvStorageClass::Input, m_type_int),
        SpvStorageClass::Input);
    m_builder.emit_name(m_ds_patch_id, "vPrim");
    m_builder.emit_decorate(m_ds_patch_id, SpvDecoration::BuiltIn,
                            static_cast<u32>(SpvBuiltIn::PrimitiveId));
    }
    m_gs_out_var = declare_expansion_buffer(m_type_vec4, 16u,
                                            "vitreo_hs_cp_out", 1u,
                                            /*read_only=*/true);
    m_gs_counter_var = declare_expansion_buffer(
        m_type_uint, 4u, "vitreo_tess_control", 2u, /*read_only=*/true);
    m_hs_pc_buffer = declare_expansion_buffer(m_type_vec4, 16u,
                                              "vitreo_hs_pc_out", 4u,
                                              /*read_only=*/true);
    return true;
}

} // namespace vitreo::graphics::dxbc_spirv_detail
