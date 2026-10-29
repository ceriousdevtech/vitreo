// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_function.cpp
/// @brief Main-function emission and the system-value epilogue.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

bool DxbcTranslator::emit_function() {
    // A hull shader's phases are functions of their own, emitted before main
    // (a function's instructions cannot interleave with another's).
    if (hs_as_compute() && !emit_hull_phase_functions()) return false;
    if (!hs_as_compute() && !emit_subroutine_functions()) return false;

    // Create main function
    m_main_func = m_builder.emit_function(m_type_void, m_func_type);
    m_builder.emit_name(m_main_func, "main");

    // Entry point (SPIR-V 1.4+: interface must list all static globals used,
    // including Uniform blocks — not only Input/Output).
    std::vector<u32> interface_vars;
    // A lowered geometry shader stages its interface in Private variables,
    // which take no part in the entry-point interface.
    if (!private_inputs()) {
        for (const auto& [_, var_id] : m_input_vars) {
            interface_vars.push_back(var_id);
        }
        for (const auto& unpacked : m_unpacked_vertex_inputs) {
            interface_vars.push_back(unpacked.source);
        }
    }
    for (const auto& [_, builtins] : m_input_scalar_builtins) {
        for (const auto& builtin : builtins) {
            interface_vars.push_back(builtin.var);
            if (builtin.minus_var != 0) interface_vars.push_back(builtin.minus_var);
        }
    }
    if (!gs_as_compute() && !vs_capture() && !hs_as_compute() &&
        !ds_as_compute()) {
        for (const auto& [_, var_id] : m_output_vars) {
            interface_vars.push_back(var_id);
        }
    }
    for (const u32 var : {m_hs_pc_buffer, m_hs_factor_buffer, m_ds_tess_coord,
                          m_ds_patch_id}) {
        if (var != 0) interface_vars.push_back(var);
    }
    if (m_depth_output != 0 && !gs_as_compute()) {
        interface_vars.push_back(m_depth_output);
    }
    for (const u32 var : {m_sample_mask_output, m_sample_mask_input,
                          m_line_distance_input, m_line_frag_coord_declared,
                          m_uav_leader_sample_id, m_uav_leader_var,
                          m_stencil_output, m_clip_distance_output,
                          m_layer_output, m_viewport_output}) {
        if (var != 0) interface_vars.push_back(var);
    }
    for (const auto& [_, var_id] : m_cbuffer_vars) {
        interface_vars.push_back(var_id);
    }
    for (const auto& [_, var_id] : m_texture_vars) {
        interface_vars.push_back(var_id);
    }
    for (const auto& [_, var_id] : m_logic_op_inputs) {
        interface_vars.push_back(var_id);
    }
    for (const auto& [_, var_id] : m_sampler_vars) {
        interface_vars.push_back(var_id);
    }
    for (const auto& [_, var_id] : m_border_twin_vars) {
        interface_vars.push_back(var_id);
    }
    for (const auto& [_, var_id] : m_uav_vars) {
        interface_vars.push_back(var_id);
    }
    for (const auto& [_, var_id] : m_structured_srv_vars) {
        interface_vars.push_back(var_id);
    }
    for (const auto& [_, var_id] : m_structured_uav_vars) {
        interface_vars.push_back(var_id);
    }
    for (const auto& [_, var_id] : m_raw_uav_vars) {
        interface_vars.push_back(var_id);
    }
    for (const auto& [_, var_id] : m_uav_counter_vars) {
        interface_vars.push_back(var_id);
    }
    for (const auto& [_, var_id] : m_typed_buffer_srv_vars) {
        interface_vars.push_back(var_id);
    }
    // The geometry-expansion buffers. Leaving them out does not just trip a
    // validation rule: SPIRV-Cross derives the kernel's buffer arguments from
    // the interface, so the generated MSL referenced vitreo_gs_in/out/counter
    // without declaring them and Metal rejected it.
    if (m_gs_in_var != 0) interface_vars.push_back(m_gs_in_var);
    if (m_gs_out_var != 0) interface_vars.push_back(m_gs_out_var);
    if (m_gs_counter_var != 0) interface_vars.push_back(m_gs_counter_var);
    if (m_gs_index_var != 0) interface_vars.push_back(m_gs_index_var);
    if (m_capture_buffer != 0) interface_vars.push_back(m_capture_buffer);
    for (const u32 var : {m_stats_record_var, m_stats_helper_var,
                          m_stats_lane_var}) {
        if (var != 0) interface_vars.push_back(var);
    }
    if (m_stats_sample_id_declared) interface_vars.push_back(m_stats_sample_id);
    for (const u32 var : m_capture_extra_inputs) {
        interface_vars.push_back(var);
    }
    if (m_capture_vertex_index != 0 &&
        m_capture_vertex_index != m_vertex_id_input) {
        interface_vars.push_back(m_capture_vertex_index);
    }
    for (const auto& [_, var_id] : m_raw_srv_vars) {
        interface_vars.push_back(var_id);
    }
    for (const auto& [_, var_id] : m_tgsm_vars) {
        interface_vars.push_back(var_id);
    }
    // The system-value epilogue reads the fixed-function members (depth
    // offset, SampleMask, viewport depth range) of the push block; it must be
    // in the interface before the body that the epilogue ends is emitted.
    if (fixed_function_epilogue()) (void)push_block_var();
    if (m_sampler_biases != 0) interface_vars.push_back(m_sampler_biases);
    if (m_sampler_state_var != 0) {
        interface_vars.push_back(m_sampler_state_var);
    }
    if (m_dispatch_thread_id != 0) {
        interface_vars.push_back(m_dispatch_thread_id);
    }
    if (m_group_id != 0) interface_vars.push_back(m_group_id);
    if (m_group_thread_id != 0) interface_vars.push_back(m_group_thread_id);
    if (m_group_index != 0) interface_vars.push_back(m_group_index);
    for (const u32 guard : m_loop_guard_vars) interface_vars.push_back(guard);

    // Hull and domain shaders run through the tessellation emulation (a
    // hull kernel, a post-tessellation vertex function or a domain kernel);
    // without DxbcTranslationOptions::emulate_tessellation there is no Metal
    // stage for them, and the model below would otherwise fall through to
    // Fragment.
    if (m_result.stage == ShaderStage::Hull ||
        (m_result.stage == ShaderStage::Domain && !ds_lowered())) {
        return fail("hull and domain shaders translate only with "
                    "emulate_tessellation");
    }
    SpvExecutionModel model = m_result.stage == ShaderStage::Vertex
        ? SpvExecutionModel::Vertex
        : ds_lowered()
            ? SpvExecutionModel::TessellationEvaluation
        : m_result.stage == ShaderStage::Geometry
            ? SpvExecutionModel::Geometry
            : m_result.stage == ShaderStage::Compute
                ? SpvExecutionModel::GLCompute
                : SpvExecutionModel::Fragment;
    // OpEntryPoint is written once every function body exists (run()): a
    // loop or a subroutine translated later adds Private variables, and
    // SPIR-V 1.4+ wants every global the entry point's call tree uses in the
    // interface.
    m_entry_model = model;
    m_entry_interface = std::move(interface_vars);

    if (m_result.stage == ShaderStage::Fragment) {
        m_builder.emit_execution_mode(m_main_func, SpvExecutionMode::OriginUpperLeft);
        // HLSL [earlydepthstencil] is dcl_globalFlags forceEarlyDepthStencil:
        // measured with fxc as OpcodeToken0 bit 13 (0x0100286a beside
        // refactoringAllowed's 0x0100086a). The depth/stencil test and write
        // then happen before the shader runs, so a pixel that fails them
        // never writes its UAVs. Without the flag, a shader with UAV side
        // effects is tested late -- Metal's default for a fragment function
        // that writes memory, and D3D's.
        constexpr u32 kForceEarlyDepthStencil = 1u << 13;
        if ((scan_global_flags() & kForceEarlyDepthStencil) != 0u) {
            if (m_depth_output != 0) {
                return fail("forceEarlyDepthStencil with an oDepth output is "
                            "not implemented (Metal refuses a depth output "
                            "under [[early_fragment_tests]])");
            }
            m_builder.emit_execution_mode(
                m_main_func, SpvExecutionMode::EarlyFragmentTests);
        }
        if (m_depth_output != 0) {
            m_builder.emit_execution_mode(
                m_main_func, SpvExecutionMode::DepthReplacing);
            if (m_depth_greater) {
                m_builder.emit_execution_mode(m_main_func,
                                              SpvExecutionMode::DepthGreater);
            } else if (m_depth_less) {
                m_builder.emit_execution_mode(m_main_func,
                                              SpvExecutionMode::DepthLess);
            }
        }
    } else if (m_result.stage == ShaderStage::Geometry) {
        const SpvExecutionMode input_mode = m_gs_input_primitive == 1u
            ? SpvExecutionMode::InputPoints
            : m_gs_input_primitive == 2u
                ? SpvExecutionMode::InputLines
            : m_gs_input_primitive == 4u
                ? SpvExecutionMode::InputLinesAdjacency
            : m_gs_input_primitive == 6u
                ? SpvExecutionMode::InputTrianglesAdjacency
                : SpvExecutionMode::Triangles;
        const SpvExecutionMode output_mode = m_gs_output_topology == 1u
            ? SpvExecutionMode::OutputPoints
            : m_gs_output_topology == 3u
                ? SpvExecutionMode::OutputLineStrip
                : SpvExecutionMode::OutputTriangleStrip;
        m_builder.emit_execution_mode(m_main_func, input_mode);
        m_builder.emit_execution_mode(m_main_func, output_mode);
        m_builder.emit_execution_mode(
            m_main_func, SpvExecutionMode::OutputVertices,
            m_gs_max_output_vertices);
    } else if (ds_lowered()) {
        // The tessellator's domain; spacing and winding are pipeline state
        // on Metal (from the hull shader), so these only complete the module.
        m_builder.emit_execution_mode(
            m_main_func, m_tess_domain == 3u ? SpvExecutionMode::Quads
                                             : SpvExecutionMode::Triangles);
        m_builder.emit_execution_mode(m_main_func,
                                      SpvExecutionMode::SpacingEqual);
        m_builder.emit_execution_mode(m_main_func,
                                      SpvExecutionMode::VertexOrderCw);
        // SPIRV-Cross writes this into Metal's [[patch(domain, N)]], which a
        // drawPatches call's numberOfPatchControlPoints must match.
        m_builder.emit_execution_mode(
            m_main_func, SpvExecutionMode::OutputVertices, m_ds_in_cp);
    } else if (m_result.stage == ShaderStage::Compute) {
        m_builder.emit_execution_mode(
            m_main_func, SpvExecutionMode::LocalSize,
            m_compute_group_size_x, m_compute_group_size_y,
            m_compute_group_size_z);
    }

    // Function body
    [[maybe_unused]] u32 entry_label = m_builder.emit_label();

    // SPIR-V requires Function OpVariables at the start of the entry block.
    // (With subroutines they are module-scope Private, declared already.)
    if (!m_temps_private) declare_function_temps(2);

    // A lowered geometry shader fills its Private input arrays from the
    // captured vertex-stage buffer before the translated body runs. This has
    // to follow the Function OpVariables, which SPIR-V requires at the top of
    // the entry block.
    if (gs_as_compute() && !emit_geometry_input_prologue()) return false;
    if (ds_lowered() && !emit_domain_input_prologue()) return false;
    if (ds_as_compute() && (!emit_domain_compute_prologue() ||
                            !emit_domain_input_prologue())) {
        return false;
    }
    // R10G10B10A2_UINT inputs: v = n / (2^bits - 1) as fetched, so
    // v * (1023, 1023, 1023, 3) + 0.5 truncates back to exactly n.
    for (const auto& unpacked : m_unpacked_vertex_inputs) {
        const u32 scale = m_builder.emit_constant_composite(
            m_type_vec4,
            {m_builder.emit_constant_float(m_type_float, 1023.0f),
             m_builder.emit_constant_float(m_type_float, 1023.0f),
             m_builder.emit_constant_float(m_type_float, 1023.0f),
             m_builder.emit_constant_float(m_type_float, 3.0f)});
        const u32 half = m_builder.emit_constant_composite(
            m_type_vec4,
            {m_builder.emit_constant_float(m_type_float, 0.5f),
             m_builder.emit_constant_float(m_type_float, 0.5f),
             m_builder.emit_constant_float(m_type_float, 0.5f),
             m_builder.emit_constant_float(m_type_float, 0.5f)});
        const u32 fetched = m_builder.emit_load(m_type_vec4, unpacked.source);
        u32 integers = m_builder.emit_convert_f_to_u(
            m_type_uvec4,
            m_builder.emit_fadd(m_type_vec4,
                                m_builder.emit_fmul(m_type_vec4, fetched,
                                                    scale),
                                half));
        // An untyped SM4/5 register keeps the integer bits whatever type
        // the signature gave it.
        if (unpacked.type != m_type_uvec4) {
            integers = m_builder.emit_bitcast(unpacked.type, integers);
        }
        m_builder.emit_store(unpacked.target, integers);
    }

    if (hs_as_compute()) {
        // The phases were translated into their own functions above; main
        // runs them for this patch and writes the results out.
        if (!emit_hull_program()) return false;
    } else {
        // Decode and translate instructions.
        // SHDR chunk body: version DWORD, then length DWORD, then opcodes.
        if (!scan_subroutine_labels()) return false;
        m_decoder.skip_tokens(2);

        if (m_uav_leader_var != 0) {
            // The pixel's invocation: the one at its lowest covered sample.
            const u32 sample =
                m_builder.emit_load(m_type_uint, m_uav_leader_sample_id);
            const u32 coverage = m_builder.emit_load(
                m_type_uint,
                m_builder.emit_access_chain(
                    m_builder.emit_type_pointer(SpvStorageClass::Input,
                                                m_type_uint),
                    m_sample_mask_input, {m_const_u0}));
            const u32 lowest = m_builder.emit_ext_inst(
                m_type_uint, m_glsl_std450, kGlslStd450FindILsb, {coverage});
            m_builder.emit_store(
                m_uav_leader_var,
                m_builder.emit_integer_compare(SpvOp::OpIEqual, m_type_bool,
                                               sample, lowest));
        }
        if (m_stats_record_var != 0) emit_pixel_statistics_prologue();

        if (!translate_instructions(m_container.bytecode.size() / 4u)) {
            return false;
        }
    }

    if ((vs_capture() || vs_capture_rasterized()) &&
        !emit_vertex_capture_epilogue()) {
        return false;
    }
    if (ds_as_compute() && !emit_domain_compute_epilogue()) return false;
    if (!ds_as_compute() && !emit_system_value_epilogue()) return false;

    m_builder.emit_return();
    m_builder.emit_function_end();

    // After every function that calls them (a call may precede the
    // callee's definition).
    return emit_double_helpers();
}

bool DxbcTranslator::emit_system_value_epilogue() {
    const auto push_member = [&](u32 type, u32 pointer_type, u32 member) {
        return m_builder.emit_load(
            type, m_builder.emit_access_chain(
                      pointer_type, push_block_var(),
                      {m_builder.emit_constant_int(m_type_uint, member)}));
    };
    if (!fixed_function_epilogue()) {
        // Nothing of the fixed-function state applies to this stage.
    } else if (m_result.stage != ShaderStage::Fragment &&
               m_position_output != 0) {
        // D3D11.3 15.10, UNORM depth: Bias = DepthBias * r, the same for
        // every z. A depth buffer stored as float makes the rasterizer use
        // the float rule (r scaled by the primitive's exponent), so the
        // frontend moves the constant part here: z_ndc + offset for every
        // vertex is z + offset after the viewport, before interpolation.
        // Guarded by offset != 0 so z is untouched (w may be inf) otherwise.
        const u32 offset = push_member(m_type_float, m_type_ptr_push_float,
                                        kPushMemberDepthOffset);
        const u32 position = m_builder.emit_load(m_type_vec4, m_position_output);
        const u32 z = m_builder.emit_composite_extract(m_type_float, position,
                                                       {2u});
        const u32 w = m_builder.emit_composite_extract(m_type_float, position,
                                                       {3u});
        const u32 biased = m_builder.emit_fadd(
            m_type_float, z, m_builder.emit_fmul(m_type_float, offset, w));
        const u32 active = m_builder.emit_funord_ne(
            m_type_bool, offset,
            m_builder.emit_constant_float(m_type_float, 0.0f));
        m_builder.emit_store(
            m_position_output,
            m_builder.emit_composite_insert(
                m_type_vec4,
                m_builder.emit_select(m_type_float, active, biased, z),
                position, {2u}));
    }
    if (fixed_function_epilogue() &&
        m_result.stage == ShaderStage::Fragment && m_depth_output != 0) {
        // D3D11.3 15.6: z is clamped to the viewport's [MinDepth, MaxDepth]
        // before the depth test whether it was interpolated or written by
        // the shader. Metal clamps a written depth only in its clamp
        // (DepthClipEnable FALSE) mode.
        const u32 depth = m_builder.emit_load(m_type_float, m_depth_output);
        const u32 lo = push_member(m_type_float, m_type_ptr_push_float,
                                    kPushMemberMinDepth);
        const u32 hi = push_member(m_type_float, m_type_ptr_push_float,
                                    kPushMemberMaxDepth);
        const u32 raised = m_builder.emit_select(
            m_type_float, m_builder.emit_ford_lt(m_type_bool, depth, lo), lo,
            depth);
        m_builder.emit_store(
            m_depth_output,
            m_builder.emit_select(
                m_type_float, m_builder.emit_ford_lt(m_type_bool, hi, raised),
                hi, raised));
    }
    if (fixed_function_epilogue() &&
        m_result.stage == ShaderStage::Fragment &&
        m_options.apply_sample_mask && m_sample_mask_output != 0) {
        const u32 pointer = m_builder.emit_access_chain(
            m_builder.emit_type_pointer(SpvStorageClass::Output, m_type_uint),
            m_sample_mask_output, {m_const_u0});
        u32 mask = push_member(m_type_uint, m_type_ptr_push_uint,
                               kPushMemberSampleMask);
        if (!m_sample_mask_from_state_only) {
            mask = m_builder.emit_bitwise_and(
                m_type_uint, mask, m_builder.emit_load(m_type_uint, pointer));
        }
        m_builder.emit_store(pointer, mask);
    }
    if (m_line_distance_input != 0) {
        // D3D11.3 3.4.4: an alpha-antialiased line's coverage "is multiplied
        // into the Pixel Shader output o0.a value before the Output Merger";
        // WARP's is 0.675 - 0.45 d, d the distance in pixels, clamped to
        // [0, 1] (DxbcTranslationOptions::line_coverage_alpha). A shader
        // with no float o0 has nothing to scale (undefined, 3.4.4).
        const auto output = m_output_vars.find(0u);
        if (output != m_output_vars.end() &&
            m_output_value_types.at(0u) == m_type_vec4) {
            const u32 line =
                m_builder.emit_load(m_type_vec3, m_line_distance_input);
            const u32 position =
                m_builder.emit_load(m_type_vec4, m_line_frag_coord);
            const auto lane = [&](u32 vector, u32 index) {
                return m_builder.emit_composite_extract(m_type_float, vector,
                                                        {index});
            };
            const u32 signed_distance = m_builder.emit_fadd(
                m_type_float,
                m_builder.emit_fadd(
                    m_type_float,
                    m_builder.emit_fmul(m_type_float, lane(line, 0),
                                        lane(position, 0)),
                    m_builder.emit_fmul(m_type_float, lane(line, 1),
                                        lane(position, 1))),
                lane(line, 2));
            const u32 distance = m_builder.emit_ext_inst(
                m_type_float, m_glsl_std450, kGlslStd450FAbs,
                {signed_distance});
            const u32 raw = m_builder.emit_fsub(
                m_type_float,
                m_builder.emit_constant_float(m_type_float, 0.675f),
                m_builder.emit_fmul(
                    m_type_float,
                    m_builder.emit_constant_float(m_type_float, 0.45f),
                    distance));
            const u32 coverage = m_builder.emit_ext_inst(
                m_type_float, m_glsl_std450, kGlslStd450FMin,
                {m_builder.emit_ext_inst(
                     m_type_float, m_glsl_std450, kGlslStd450FMax,
                     {raw, m_builder.emit_constant_float(m_type_float,
                                                         0.0f)}),
                 m_builder.emit_constant_float(m_type_float, 1.0f)});
            const u32 colour = m_builder.emit_load(m_type_vec4, output->second);
            const u32 alpha = m_builder.emit_fmul(
                m_type_float,
                m_builder.emit_composite_extract(m_type_float, colour, {3u}),
                coverage);
            m_builder.emit_store(output->second,
                                 m_builder.emit_composite_insert(
                                     m_type_vec4, alpha, colour, {3u}));
        }
    }
    for (const auto& [target, input] : m_logic_op_inputs) {
        // D3D11.3 17.7 logic op: result = op(source, destination) on the
        // bits the target's format holds (D3D11_LOGIC_OP order, as the
        // D3D11_LOGIC_OP reference lists it; the results measured on WARP,
        // d3d11_fl111_probe "logic r32", match: e.g. AND_REVERSE s & ~d).
        const auto output = m_output_vars.find(target);
        if (output == m_output_vars.end()) continue;
        const u32 type = m_output_value_types.at(target);
        u32 source = m_builder.emit_load(type, output->second);
        if (type != m_type_uvec4) {
            source = m_builder.emit_bitcast(m_type_uvec4, source);
        }
        const auto& bits = m_options.logic_op_component_masks[target];
        const u32 mask = make_uvec4_imm(bits[0], bits[1], bits[2], bits[3]);
        source = m_builder.emit_bitwise_and(m_type_uvec4, source, mask);
        const u32 origin = m_builder.emit_constant_composite(
            m_type_ivec2, {m_builder.emit_constant_int(m_type_int, 0u),
                           m_builder.emit_constant_int(m_type_int, 0u)});
        const u32 destination = m_builder.emit_image_read(
            m_type_uvec4,
            m_builder.emit_load(m_logic_op_image_type, input),
            origin);
        const auto invert = [&](u32 value) {
            return m_builder.emit_not(m_type_uvec4, value);
        };
        const auto op_and = [&](u32 a, u32 b) {
            return m_builder.emit_bitwise_and(m_type_uvec4, a, b);
        };
        const auto op_or = [&](u32 a, u32 b) {
            return m_builder.emit_bitwise_or(m_type_uvec4, a, b);
        };
        const u32 s = source;
        const u32 d = destination;
        u32 result = 0;
        switch (m_options.logic_op) {
        case 0: result = m_const_u_zero4; break;                   // CLEAR
        case 1: result = m_const_u_ffff4; break;                   // SET
        case 2: result = s; break;                                 // COPY
        case 3: result = invert(s); break;                         // COPY_INVERTED
        case 4: result = d; break;                                 // NOOP
        case 5: result = invert(d); break;                         // INVERT
        case 6: result = op_and(s, d); break;                      // AND
        case 7: result = invert(op_and(s, d)); break;              // NAND
        case 8: result = op_or(s, d); break;                       // OR
        case 9: result = invert(op_or(s, d)); break;               // NOR
        case 10:                                                   // XOR
            result = m_builder.emit_bitwise_xor(m_type_uvec4, s, d);
            break;
        case 11:                                                   // EQUIV
            result = invert(m_builder.emit_bitwise_xor(m_type_uvec4, s, d));
            break;
        case 12: result = op_and(s, invert(d)); break;             // AND_REVERSE
        case 13: result = op_and(invert(s), d); break;             // AND_INVERTED
        case 14: result = op_or(s, invert(d)); break;              // OR_REVERSE
        case 15: result = op_or(invert(s), d); break;              // OR_INVERTED
        default:
            return fail("logic op " + std::to_string(m_options.logic_op) +
                        " is not a D3D11_LOGIC_OP");
        }
        result = m_builder.emit_bitwise_and(m_type_uvec4, result, mask);
        m_builder.emit_store(output->second,
                             type != m_type_uvec4
                                 ? m_builder.emit_bitcast(type, result)
                                 : result);
    }
    const auto component_of = [&](u32 reg, u32 component) -> u32 {
        const u32 type = m_output_value_types.at(reg);
        u32 bits = m_builder.emit_load(type, m_output_vars.at(reg));
        if (type != m_type_uvec4) {
            bits = m_builder.emit_bitcast(m_type_uvec4, bits);
        }
        return m_builder.emit_composite_extract(m_type_uint, bits,
                                                {component});
    };
    if (m_clip_distance_output != 0) {
        const u32 pointer = m_builder.emit_type_pointer(
            SpvStorageClass::Output, m_type_float);
        for (usize i = 0; i < m_clip_sources.size(); ++i) {
            const auto [reg, component] = m_clip_sources[i];
            if (!m_output_vars.contains(reg)) {
                return fail("SV_ClipDistance: o" + std::to_string(reg) +
                            " has no variable");
            }
            m_builder.emit_store(
                m_builder.emit_access_chain(
                    pointer, m_clip_distance_output,
                    {m_builder.emit_constant_int(m_type_uint,
                                                 static_cast<u32>(i))}),
                m_builder.emit_bitcast(m_type_float,
                                       component_of(reg, component)));
        }
    }
    if (m_layer_output != 0) {
        const auto [reg, component] = m_layer_source;
        if (!m_output_vars.contains(reg)) {
            return fail("SV_RenderTargetArrayIndex: o" + std::to_string(reg) +
                        " has no variable");
        }
        m_builder.emit_store(m_layer_output, component_of(reg, component));
    }
    if (m_viewport_output != 0) {
        const auto [reg, component] = m_viewport_source;
        if (!m_output_vars.contains(reg)) {
            return fail("SV_ViewportArrayIndex: o" + std::to_string(reg) +
                        " has no variable");
        }
        m_builder.emit_store(m_viewport_output, component_of(reg, component));
    }
    return true;
}

} // namespace vitreo::graphics::dxbc_spirv_detail
