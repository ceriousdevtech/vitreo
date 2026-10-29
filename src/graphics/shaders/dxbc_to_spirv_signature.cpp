// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_signature.cpp
/// @brief Stage I/O variables: the input and output signatures and stage extras.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

bool DxbcTranslator::emit_variables() {
    const auto semantic_is = [](const std::string& semantic,
                                const char* expected) {
        usize i = 0;
        for (; i < semantic.size() && expected[i] != '\0'; ++i) {
            const unsigned char lhs =
                static_cast<unsigned char>(semantic[i]);
            const unsigned char rhs =
                static_cast<unsigned char>(expected[i]);
            if (std::toupper(lhs) != std::toupper(rhs)) return false;
        }
        return i == semantic.size() && expected[i] == '\0';
    };
    // The fragment stage always has the block (LOD biases); another stage
    // needs it only for border colours.
    // vCoverage: the rasterizer's sample mask, a one-element uint array.
    if (m_result.stage == ShaderStage::Fragment &&
        declares_input_operand(Sm4OperandType::InputCoverageMask)) {
        const u32 array_ty = m_builder.emit_type_array(
            m_type_uint, m_builder.emit_constant_int(m_type_uint, 1u));
        m_sample_mask_input = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::Input, array_ty),
            SpvStorageClass::Input);
        m_builder.emit_name(m_sample_mask_input, "SV_Coverage");
        m_builder.emit_decorate(m_sample_mask_input, SpvDecoration::BuiltIn,
                                static_cast<u32>(SpvBuiltIn::SampleMask));
        m_builder.emit_decorate_string(m_sample_mask_input,
                                       SpvDecoration::UserSemantic,
                                       kRasterCoverageSemantic);
    }
    // The once-per-pixel gate for u# side effects (m_uav_leader_var). It
    // reads the raw rasterizer coverage -- the vCoverage variable when the
    // shader declares one; its reader adjusts only the loaded value.
    // A PSInvocations-counting variant writes memory too, so it takes the
    // same gate: on a multisampled target it counts each pixel at its
    // lowest covered sample.
    if (m_result.stage == ShaderStage::Fragment &&
        (dxbc_writes_memory(m_container) ||
         m_options.count_pixel_invocations) &&
        !dxbc_pixel_runs_per_sample(m_container)) {
        if (m_sample_mask_input == 0) {
            const u32 array_ty = m_builder.emit_type_array(
                m_type_uint, m_builder.emit_constant_int(m_type_uint, 1u));
            m_sample_mask_input = m_builder.emit_variable(
                m_builder.emit_type_pointer(SpvStorageClass::Input, array_ty),
                SpvStorageClass::Input);
            m_builder.emit_name(m_sample_mask_input, "vitreo_coverage");
            m_builder.emit_decorate(m_sample_mask_input,
                                    SpvDecoration::BuiltIn,
                                    static_cast<u32>(SpvBuiltIn::SampleMask));
            m_builder.emit_decorate_string(m_sample_mask_input,
                                           SpvDecoration::UserSemantic,
                                           kRasterCoverageSemantic);
        }
        m_builder.emit_capability(SpvCapability::SampleRateShading);
        m_uav_leader_sample_id = m_builder.emit_variable(
            m_type_ptr_input_uint, SpvStorageClass::Input);
        m_builder.emit_name(m_uav_leader_sample_id, "vitreo_sample_id");
        m_builder.emit_decorate(m_uav_leader_sample_id, SpvDecoration::BuiltIn,
                                static_cast<u32>(SpvBuiltIn::SampleId));
        // Reading SampleId gives the shader SPIR-V's sample-rate semantics,
        // under which SPIRV-Cross moves FragCoord to the sample's position:
        // SV_Position is put back at the pixel centre on load
        // (m_frag_coord_at_pixel_centre). The coverage input keeps the
        // pixel's raster coverage (kRasterCoverageSemantic).
        m_uav_leader_var = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::Private, m_type_bool),
            SpvStorageClass::Private);
        m_builder.emit_name(m_uav_leader_var, "vitreo_uav_leader");
    }
    // The entry point's interface list is written before the instructions
    // are translated, so the block must exist now for every shader that will
    // read it: fragment LOD biases, border twins, and the buffer bounds guards
    // (any raw/structured t#/u#, any cb#, and a Texture3D u#'s W window --
    // volume_uav_window).
    const auto declares_volume_uav = [&] {
        for (const auto& [slot, declaration] : scan_typed_uavs()) {
            (void)slot;
            if (declaration.first == 5u) return true;  // Texture3D
        }
        return false;
    };
    // A typed buffer view's element bias (typed_buffer_bias): any Buffer<T>
    // t# (resource dimension 1) or RWBuffer<T> u# with a size-table entry.
    const auto declares_typed_buffer = [&] {
        if (!m_options.typed_buffer_bias) return false;
        const auto resources = scan_shader_resources();
        for (const auto& [slot, dimension] : std::get<0>(resources)) {
            if (dimension == 1u && slot < kGuardSizeSrvSlots) return true;
        }
        for (const auto& [slot, declaration] : scan_typed_uavs()) {
            if (declaration.first == 1u && slot < kGuardSizeUavSlots) {
                return true;
            }
        }
        return false;
    };
    if (m_result.stage == ShaderStage::Fragment ||
        m_options.border_twin_samplers != 0 || vs_capture() ||
        (m_options.guard_constant_buffers &&
         !scan_constant_buffers().empty()) ||
        (sampler_bias_bound() &&
         !std::get<2>(scan_shader_resources()).empty()) ||
        (m_options.guard_buffer_bounds && !gs_as_compute() &&
         !hs_as_compute() &&
         (!scan_structured_srvs().empty() || !scan_raw_srvs().empty() ||
          !scan_structured_uavs().empty() || !scan_raw_uavs().empty() ||
          declares_volume_uav())) ||
        declares_typed_buffer()) {
        (void)push_block_var();
    }
    if (m_result.stage == ShaderStage::Compute) {
        m_dispatch_thread_id = m_builder.emit_variable(
            m_type_ptr_input_uvec3, SpvStorageClass::Input);
        m_builder.emit_name(m_dispatch_thread_id, "SV_DispatchThreadID");
        m_builder.emit_decorate(
            m_dispatch_thread_id, SpvDecoration::BuiltIn,
            static_cast<u32>(SpvBuiltIn::GlobalInvocationId));
        m_group_id = m_builder.emit_variable(
            m_type_ptr_input_uvec3, SpvStorageClass::Input);
        m_builder.emit_name(m_group_id, "SV_GroupID");
        m_builder.emit_decorate(
            m_group_id, SpvDecoration::BuiltIn,
            static_cast<u32>(SpvBuiltIn::WorkgroupId));
        m_group_thread_id = m_builder.emit_variable(
            m_type_ptr_input_uvec3, SpvStorageClass::Input);
        m_builder.emit_name(m_group_thread_id, "SV_GroupThreadID");
        m_builder.emit_decorate(
            m_group_thread_id, SpvDecoration::BuiltIn,
            static_cast<u32>(SpvBuiltIn::LocalInvocationId));
        m_group_index = m_builder.emit_variable(
            m_type_ptr_input_uint, SpvStorageClass::Input);
        m_builder.emit_name(m_group_index, "SV_GroupIndex");
        m_builder.emit_decorate(
            m_group_index, SpvDecoration::BuiltIn,
            static_cast<u32>(SpvBuiltIn::LocalInvocationIndex));
    }
    // Create input variables based on signature. D3D signature component
    // types describe the shader interface, not an arithmetic conversion:
    // UINT/SINT inputs enter the register file with their original 32-bit
    // lane bits. Preserve that interface type so the Metal vertex descriptor
    // can feed integer formats without first converting them to float.
    const std::map<u32, u32> input_interpolation =
        m_result.stage == ShaderStage::Fragment ? scan_input_interpolation()
                                                : std::map<u32, u32>{};
    for (const auto& input : m_container.input_signature) {
        // Microsoft Learn: SV_Position read by a pixel shader is the
        // rasterizer-provided pixel center in screen space (with the 0.5
        // offset). It is not a user varying produced by the vertex shader.
        const bool fragment_position =
            m_result.stage == ShaderStage::Fragment &&
            (input.system_value_type == 1 ||
             semantic_is(input.semantic_name, "SV_Position"));
        // A geometry, hull or domain shader receives SV_Position as ordinary
        // per-vertex (per-control-point) data.
        const bool geometry_position =
            arrayed_inputs() &&
            (input.system_value_type == 1 ||
             semantic_is(input.semantic_name, "SV_Position"));
        // Microsoft Learn (Semantics): SV_VertexID and SV_InstanceID are
        // generated by the input assembler, not fed from a vertex buffer, so
        // they are SPIR-V *builtins* — a scalar 32-bit integer each — not
        // location-bound varyings. D3D_NAME_VERTEX_ID is 6 and
        // D3D_NAME_INSTANCE_ID is 8. Instanced draws declare SV_InstanceID, so
        // rejecting it failed every instancing vertex shader and left the
        // whole frame on the error material.
        const bool vertex_id_input =
            m_result.stage == ShaderStage::Vertex &&
            (input.system_value_type == 6u ||
             semantic_is(input.semantic_name, "SV_VertexID"));
        const bool instance_id_input =
            m_result.stage == ShaderStage::Vertex &&
            (input.system_value_type == 8u ||
             semantic_is(input.semantic_name, "SV_InstanceID"));
        const bool front_face_input =
            m_result.stage == ShaderStage::Fragment &&
            (input.system_value_type == 9u ||
             semantic_is(input.semantic_name, "SV_IsFrontFace"));
        if (front_face_input) {
            const u32 var_id = m_builder.emit_variable(
                m_type_ptr_input_bool, SpvStorageClass::Input);
            m_builder.emit_name(var_id, "SV_IsFrontFace");
            m_builder.emit_decorate(
                var_id, SpvDecoration::BuiltIn,
                static_cast<u32>(SpvBuiltIn::FrontFacing));
            u32 component = 0;
            for (u32 i = 0; i < 4; ++i) {
                if ((input.component_mask & (1u << i)) != 0u) {
                    component = i;
                    break;
                }
            }
            m_input_scalar_builtins[input.register_num].push_back(
                {var_id, component, true});
            continue;
        }
        if (vertex_id_input || instance_id_input) {
            const u32 var_id = m_builder.emit_variable(
                m_type_ptr_input_uint, SpvStorageClass::Input);
            m_builder.emit_name(var_id, vertex_id_input ? "SV_VertexID"
                                                        : "SV_InstanceID");
            m_builder.emit_decorate(
                var_id, SpvDecoration::BuiltIn,
                static_cast<u32>(vertex_id_input ? SpvBuiltIn::VertexIndex
                                                 : SpvBuiltIn::InstanceIndex));
            if (vertex_id_input) m_vertex_id_input = var_id;
            m_builtin_inputs[static_cast<u32>(
                vertex_id_input ? SpvBuiltIn::VertexIndex
                                : SpvBuiltIn::InstanceIndex)] = var_id;
            // D3D11 functional spec / Learn (DrawInstanced,
            // StartInstanceLocation): SV_InstanceID counts from 0 in every
            // draw; StartInstanceLocation only offsets per-instance vertex
            // fetches. SPIR-V InstanceIndex and Metal [[instance_id]] both
            // INCLUDE the base instance, so the D3D value is
            // InstanceIndex - BaseInstance. GPU-driven instancing draws with a
            // non-zero StartInstanceLocation per mesh batch; without the
            // subtraction each draw read another batch's instance data.
            // SV_VertexID likewise (D3D11.3 8.16): a non-indexed draw counts
            // from 0 whatever StartVertexLocation is, and an indexed draw
            // reads the index value itself -- BaseVertexLocation offsets only
            // the fetch (Windows: Draw(3, start 5) -> 0 1 2, DrawIndexed
            // base 10 -> the raw indices). Metal's [[vertex_id]] includes
            // vertexStart / baseVertex, and [[base_vertex]] is that same
            // base, so the D3D value is VertexIndex - BaseVertex. (The
            // capture variants key their records by the raw [[vertex_id]],
            // m_capture_vertex_index, not by this value.)
            u32 minus_var = 0;
            m_builder.emit_capability(SpvCapability::DrawParameters);
            m_builder.emit_extension("SPV_KHR_shader_draw_parameters");
            minus_var = m_builder.emit_variable(m_type_ptr_input_uint,
                                                SpvStorageClass::Input);
            m_builder.emit_name(minus_var, instance_id_input
                                               ? "vitreo_base_instance"
                                               : "vitreo_base_vertex");
            m_builder.emit_decorate(
                minus_var, SpvDecoration::BuiltIn,
                static_cast<u32>(instance_id_input ? SpvBuiltIn::BaseInstance
                                                   : SpvBuiltIn::BaseVertex));
            m_builtin_inputs[static_cast<u32>(
                instance_id_input ? SpvBuiltIn::BaseInstance
                                  : SpvBuiltIn::BaseVertex)] = minus_var;
            // The signature's component mask says which lane of the register
            // carries the value; everything else in that register is
            // undefined and reads as zero.
            u32 component = 0;
            for (u32 i = 0; i < 4; ++i) {
                if ((input.component_mask & (1u << i)) != 0u) {
                    component = i;
                    break;
                }
            }
            m_input_scalar_builtins[input.register_num].push_back(
                {var_id, component, false, minus_var,
                 instance_id_input && vs_capture()});
            continue;
        }
        // Pixel-shader inputs the rasterizer supplies (D3D_NAME_*):
        // RENDER_TARGET_ARRAY_INDEX 4, VIEWPORT_ARRAY_INDEX 5, PRIMITIVE_ID 7,
        // SAMPLE_INDEX 10. Scalar builtins, like SV_VertexID for a VS; a
        // stage that did not write the index reads 0, in both APIs.
        const u32 isv = input.system_value_type;
        if (m_result.stage == ShaderStage::Fragment &&
            (isv == 4u || isv == 5u || isv == 7u || isv == 10u)) {
            const SpvBuiltIn builtin = isv == 4u ? SpvBuiltIn::Layer
                : isv == 5u ? SpvBuiltIn::ViewportIndex
                : isv == 7u ? SpvBuiltIn::PrimitiveId
                            : SpvBuiltIn::SampleId;
            m_builder.emit_capability(isv == 10u
                                          ? SpvCapability::SampleRateShading
                                      : isv == 5u ? SpvCapability::MultiViewport
                                                  : SpvCapability::Geometry);
            const u32 var_id = m_builder.emit_variable(
                m_type_ptr_input_uint, SpvStorageClass::Input);
            m_builder.emit_name(var_id, input.semantic_name);
            m_builder.emit_decorate(var_id, SpvDecoration::BuiltIn,
                                    static_cast<u32>(builtin));
            u32 component = 0;
            while (component < 3 &&
                   (input.component_mask & (1u << component)) == 0u) {
                ++component;
            }
            ScalarBuiltin scalar{var_id, component, false};
            if (isv == 7u) {
                scalar.divisor = m_options.line_quad_primitive_divisor;
            }
            m_input_scalar_builtins[input.register_num].push_back(scalar);
            continue;
        }
        // A geometry shader's SV_PrimitiveID is system-generated: ISGN lists
        // it with no register (0xFFFFFFFF, measured on fxc gs_5_0 output) and
        // the body reads it through the vPrim operand, not a v# register.
        // Asks the CONTAINER: a lowered geometry shader reports Compute.
        if (arrayed_inputs() && isv == 7u &&
            input.register_num == 0xFFFFFFFFu) {
            continue;
        }
        // SV_ClipDistance / SV_CullDistance read by a pixel shader: the
        // stage before writes that register as an ordinary varying as well
        // as the builtin, so it arrives here as one.
        const bool distance_input = m_result.stage == ShaderStage::Fragment &&
                                    (isv == 2u || isv == 3u);
        if (input.system_value_type != 0u && !distance_input &&
            !fragment_position && !geometry_position) {
            return fail("unsupported input system value " +
                        std::to_string(input.system_value_type) +
                        " at register " +
                        std::to_string(input.register_num));
        }
        // Packed elements share one register, and so one variable (see the
        // matching rule for outputs).
        if (m_input_vars.contains(input.register_num)) continue;
        u32 value_type = m_type_vec4;
        u8 component_type = input.component_type;
        if (m_result.stage == ShaderStage::Vertex &&
            input.register_num <
                m_options.vertex_input_component_types.size() &&
            m_options.vertex_input_component_types[input.register_num] != 0u) {
            component_type =
                m_options.vertex_input_component_types[input.register_num];
        }
        if (!fragment_position && !geometry_position) {
            if (component_type == 1u) {
                value_type = m_type_uvec4;
            } else if (component_type == 2u) {
                value_type = m_type_ivec4;
            }
        }
        // A lowered geometry shader keeps the same v[vertex][register] ARRAY
        // shape but in Private storage, filled by a prologue from the
        // captured vertex-stage buffer. That leaves the whole existing load
        // path -- the access chain on the vertex index, the bitcasts, the
        // swizzles -- untouched; a compute shader cannot carry
        // location-bound Input variables at all.
        if (m_result.stage == ShaderStage::Vertex && !private_inputs() &&
            input.register_num < 32 &&
            (m_options.vertex_input_unpack_uint1010102 &
             (1u << input.register_num)) != 0) {
            // R10G10B10A2_UINT: the stage-in is the normalized fetch, and the
            // register the body reads is a Private copy the prologue fills
            // (emit_vertex_input_unpack_prologue).
            const u32 source = m_builder.emit_variable(
                m_builder.emit_type_pointer(SpvStorageClass::Input,
                                            m_type_vec4),
                SpvStorageClass::Input);
            m_builder.emit_name(source, input.semantic_name +
                                            std::to_string(input.semantic_index) +
                                            "_packed");
            m_builder.emit_decorate(source, SpvDecoration::Location,
                                    input.register_num);
            const u32 unpacked = m_builder.emit_variable(
                m_builder.emit_type_pointer(SpvStorageClass::Private,
                                            value_type),
                SpvStorageClass::Private);
            m_builder.emit_name(unpacked, input.semantic_name +
                                              std::to_string(
                                                  input.semantic_index));
            m_unpacked_vertex_inputs.push_back({source, unpacked, value_type});
            m_input_vars[input.register_num] = unpacked;
            m_input_value_types[input.register_num] = value_type;
            m_input_pointer_types[input.register_num] =
                m_builder.emit_type_pointer(SpvStorageClass::Private,
                                            value_type);
            continue;
        }
        const SpvStorageClass input_class = private_inputs()
            ? SpvStorageClass::Private
            : SpvStorageClass::Input;
        const u32 element_pointer_type = m_builder.emit_type_pointer(
            input_class, value_type);
        u32 variable_type = value_type;
        if (arrayed_inputs()) {
            const u32 primitive_width = m_builder.emit_constant_int(
                m_type_uint, input_array_width());
            variable_type = m_builder.emit_type_array(
                value_type, primitive_width);
        }
        const u32 variable_pointer_type = m_builder.emit_type_pointer(
            input_class, variable_type);
        const u32 var_id = m_builder.emit_variable(
            variable_pointer_type, input_class);

        m_builder.emit_name(var_id, input.semantic_name +
                           std::to_string(input.semantic_index));
        if (private_inputs()) {
            // A lowered geometry shader's inputs are Private variables that
            // the prologue fills from the captured vertex-stage buffer. They
            // take NO interface decoration: a surviving BuiltIn Position makes
            // SPIRV-Cross build a stage-out struct for a compute kernel, which
            // Metal then rejects with "use of undeclared identifier 'out'".
        } else if (fragment_position) {
            m_builder.emit_decorate(
                var_id, SpvDecoration::BuiltIn,
                static_cast<u32>(SpvBuiltIn::FragCoord));
            m_frag_coord_register = input.register_num;
        } else if (geometry_position) {
            m_builder.emit_decorate(
                var_id, SpvDecoration::BuiltIn,
                static_cast<u32>(SpvBuiltIn::Position));
        } else {
            m_builder.emit_decorate(
                var_id, SpvDecoration::Location, input.register_num);
            // SPIR-V requires integer fragment inputs to use flat
            // interpolation. Decorating the integer interface consistently
            // also lets SPIRV-Cross emit matching Metal stage attributes.
            const bool integer_input = value_type == m_type_uvec4 ||
                                       value_type == m_type_ivec4;
            if (m_result.stage != ShaderStage::Vertex && integer_input) {
                m_builder.emit_decorate(var_id, SpvDecoration::Flat);
            }
            // D3D11.3 16.4: each pixel-shader input is interpolated as its
            // dcl_input_ps declares -- constant (nointerpolation), linear,
            // noperspective, centroid and sample in combination. Metal
            // expresses all eight as fragment input attributes.
            if (m_result.stage == ShaderStage::Fragment && !integer_input) {
                const auto mode = input_interpolation.find(input.register_num);
                const u32 m = mode != input_interpolation.end() ? mode->second
                                                                : 2u;
                if (m == 1u) {
                    m_builder.emit_decorate(var_id, SpvDecoration::Flat);
                }
                if (m == 4u || m == 5u || m == 7u) {
                    m_builder.emit_decorate(var_id,
                                            SpvDecoration::NoPerspective);
                }
                if (m == 3u || m == 5u) {
                    m_builder.emit_decorate(var_id, SpvDecoration::Centroid);
                }
                if (m == 6u || m == 7u) {
                    m_builder.emit_capability(
                        SpvCapability::SampleRateShading);
                    m_builder.emit_decorate(var_id, SpvDecoration::Sample);
                }
            }
        }

        m_input_vars[input.register_num] = var_id;
        m_input_value_types[input.register_num] = value_type;
        m_input_pointer_types[input.register_num] = element_pointer_type;
    }
    // D3D11.3 16.3/16.4: a pixel shader runs per sample when it reads
    // SV_SampleIndex or declares any input `sample`, and then SV_Position is
    // still the pixel centre unless it is itself declared `sample` (Windows
    // [ps_persample]: fraction 0.5 0.5 at every sample). Metal's [[position]]
    // in a sample-rate fragment function is the sample's position.
    if (m_result.stage == ShaderStage::Fragment &&
        m_frag_coord_register != ~0u) {
        bool per_sample = false;
        for (const auto& [reg, mode] : input_interpolation) {
            if ((mode == 6u || mode == 7u) && reg != m_frag_coord_register) {
                per_sample = true;
            }
        }
        for (const auto& input : m_container.input_signature) {
            if (input.system_value_type == 10u) per_sample = true;
        }
        const auto position_mode =
            input_interpolation.find(m_frag_coord_register);
        const bool position_at_sample =
            position_mode != input_interpolation.end() &&
            (position_mode->second == 6u || position_mode->second == 7u);
        m_frag_coord_at_pixel_centre =
            (per_sample && !position_at_sample) || m_uav_leader_var != 0;
    }

    // SV_ClipDistance components as {semantic index, register, component}:
    // the clip array is ordered by semantic index, then component.
    std::vector<std::tuple<u32, u32, u32>> clip_elements;
    bool layer_output = false;
    bool viewport_output = false;
    // Create output variables based on signature
    for (const auto& output : m_container.output_signature) {
        // The rasterizer position is the output whose system value is
        // D3D_NAME_POSITION. A semantic spelled POSITION<n> is an ordinary
        // user output: fxc gives it SysValue NONE, and even a level-9 shader
        // must write SV_Position (tools/oracle_probe/vs_position_semantics).
        const bool position_semantic =
            output.system_value_type == 1 ||
            semantic_is(output.semantic_name, "SV_Position");
        // A lowered geometry shader reports stage Compute, so ask the
        // CONTAINER what the source stage was. Getting this wrong loses
        // SV_Position, which is the one output the draw pass cannot do without.
        const bool position = position_semantic &&
            (m_result.stage == ShaderStage::Vertex ||
             m_container.stage == ShaderStage::Geometry ||
             m_container.stage == ShaderStage::Domain);
        const bool fragment_target =
            m_result.stage == ShaderStage::Fragment &&
            output.system_value_type == 64u; // D3D_NAME_TARGET
        // oDepth, oDepthGE, oDepthLE, oMask and oStencilRef are named, not
        // numbered: fxc writes them with system value 0 and register
        // 0xFFFFFFFF (measured, tools/oracle_probe/system_value_shaders.hlsl),
        // so the semantic name identifies them. One FragDepth, with the
        // conservative bound as an execution mode.
        const u32 sv = output.system_value_type;
        const bool depth_ge =
            semantic_is(output.semantic_name, "SV_DepthGreaterEqual");
        const bool depth_le =
            semantic_is(output.semantic_name, "SV_DepthLessEqual");
        const bool fragment_depth =
            m_result.stage == ShaderStage::Fragment &&
            (semantic_is(output.semantic_name, "SV_Depth") || sv == 65u ||
             depth_ge || depth_le);
        if (fragment_depth) {
            const u32 ptr_type = m_builder.emit_type_pointer(
                SpvStorageClass::Output, m_type_float);
            m_depth_output = m_builder.emit_variable(
                ptr_type, SpvStorageClass::Output);
            m_builder.emit_name(m_depth_output, "SV_Depth");
            m_builder.emit_decorate(
                m_depth_output, SpvDecoration::BuiltIn,
                static_cast<u32>(SpvBuiltIn::FragDepth));
            m_depth_greater = depth_ge;
            m_depth_less = depth_le;
            continue;
        }
        // SV_Coverage: the pixel's sample mask.
        if (m_result.stage == ShaderStage::Fragment &&
            semantic_is(output.semantic_name, "SV_Coverage")) {
            const u32 array_ty = m_builder.emit_type_array(
                m_type_uint, m_builder.emit_constant_int(m_type_uint, 1u));
            m_sample_mask_output = m_builder.emit_variable(
                m_builder.emit_type_pointer(SpvStorageClass::Output, array_ty),
                SpvStorageClass::Output);
            m_builder.emit_name(m_sample_mask_output, "SV_Coverage");
            m_builder.emit_decorate(m_sample_mask_output,
                                    SpvDecoration::BuiltIn,
                                    static_cast<u32>(SpvBuiltIn::SampleMask));
            continue;
        }
        // SV_StencilRef: the per-pixel stencil reference.
        if (m_result.stage == ShaderStage::Fragment &&
            semantic_is(output.semantic_name, "SV_StencilRef")) {
            m_builder.emit_capability(SpvCapability::StencilExportEXT);
            m_builder.emit_extension("SPV_EXT_shader_stencil_export");
            m_stencil_output = m_builder.emit_variable(
                m_builder.emit_type_pointer(SpvStorageClass::Output,
                                            m_type_int),
                SpvStorageClass::Output);
            m_builder.emit_name(m_stencil_output, "SV_StencilRef");
            m_builder.emit_decorate(
                m_stencil_output, SpvDecoration::BuiltIn,
                static_cast<u32>(SpvBuiltIn::FragStencilRefEXT));
            continue;
        }
        // Rasterizer system values from a vertex shader (D3D_NAME_*): the
        // register is still declared below as an ordinary varying, and the
        // epilogue also copies these components into their builtins. A
        // buffer-bound stage (a lowered geometry shader, a capture vertex
        // shader) keeps them in its record; its consumer decides.
        // A domain shader feeds the rasterizer exactly as a vertex shader
        // does; a lowered hull shader's control points are records.
        const bool rasterized_vertex =
            (m_result.stage == ShaderStage::Vertex && !vs_capture()) ||
            ds_lowered();
        const bool record_stage = gs_as_compute() || vs_capture() ||
                                  hs_as_compute() || ds_as_compute();
        bool delivered = false;
        if (sv == 2u || sv == 3u || sv == 4u || sv == 5u) {
            if (record_stage) {
                delivered = true;
            } else if (rasterized_vertex && sv == 2u) {
                for (u32 c = 0; c < 4; ++c) {
                    if ((output.component_mask & (1u << c)) != 0u) {
                        clip_elements.push_back(
                            {output.semantic_index, output.register_num, c});
                    }
                }
                delivered = true;
            } else if (rasterized_vertex && (sv == 4u || sv == 5u)) {
                u32 component = 0;
                while (component < 3 &&
                       (output.component_mask & (1u << component)) == 0u) {
                    ++component;
                }
                if (sv == 4u) {
                    m_layer_source = {output.register_num, component};
                    layer_output = true;
                } else {
                    m_viewport_source = {output.register_num, component};
                    viewport_output = true;
                }
                delivered = true;
            } else if (rasterized_vertex && sv == 3u) {
                return fail("SV_CullDistance: Metal has no cull distances");
            }
        }
        if (hs_as_compute()) delivered = true;  // control-point data
        if (sv != 0u && !position && !fragment_target && !delivered) {
            return fail("unsupported output system value " +
                        std::to_string(output.system_value_type) +
                        " at register " +
                        std::to_string(output.register_num));
        }
        // fxc packs several elements into one register (layer in o2.x and
        // viewport in o2.y; two float2 texcoords in one vec4). The register
        // is ONE variable: a second would orphan the first and, in a lowered
        // geometry shader, count the register twice in the record.
        if (m_output_vars.contains(output.register_num)) continue;
        u32 type_id = m_type_vec4;
        if (!position) {
            if (output.component_type == 1u) {
                type_id = m_type_uvec4;
            } else if (output.component_type == 2u) {
                type_id = m_type_ivec4;
            }
        }
        // A geometry shader lowered to compute stages its o# registers in
        // PRIVATE variables and flushes them to the expansion buffer on
        // `emit`. Keeping them as variables is what lets the whole existing
        // store/load path -- write masks, component merges, bitcasts -- work
        // untouched: only the storage class changes, and Private carries no
        // Location/BuiltIn decoration and takes no part in the entry-point
        // interface. See .docs/design-geometry-shader-emulation.md.
        // A capture vertex shader has the same need as a lowered geometry
        // shader: its outputs go to a buffer, not down the pipeline. Metal
        // additionally REQUIRES a rasterization-disabled vertex function to
        // return void, which it only does when the entry point declares no
        // Output variables at all -- so Private is not merely tidier here, it
        // is what makes the pipeline buildable.
        const SpvStorageClass output_class =
            (gs_as_compute() || vs_capture() || hs_as_compute() ||
             ds_as_compute())
            ? SpvStorageClass::Private
            : SpvStorageClass::Output;
        u32 ptr_type = m_builder.emit_type_pointer(output_class, type_id);
        u32 var_id = m_builder.emit_variable(ptr_type, output_class);

        m_builder.emit_name(var_id, output.semantic_name +
                           std::to_string(output.semantic_index));

        // D3D_NAME_POSITION == 1. Signature semantic spelling is
        // case-insensitive and FXC commonly serializes it as SV_POSITION.
        // Prefer the structured system-value field, retaining POSITION for
        // backwards-compatible signatures produced by older compilers.
        if (gs_as_compute() || vs_capture() || hs_as_compute() ||
            ds_as_compute()) {
            // Private variables take no interface decoration. Record the
            // register order so `emit` writes the buffer in signature order,
            // which is the record layout the host and any later stream-out
            // consumer agree on.
            if (position) m_position_output = var_id;
            if (gs_as_compute()) {
                m_gs_emit_registers.push_back(output.register_num);
            }
        } else if (position) {
            m_builder.emit_decorate(var_id, SpvDecoration::BuiltIn,
                                   static_cast<u32>(SpvBuiltIn::Position));
            // Learn `precise` / D3D11.3 22.19.2: a precise position is
            // computed without refactoring, which is what lets a depth
            // prepass and a later pass over the same geometry (different
            // shaders) agree bit for bit. Metal's mechanism for that is an
            // [[invariant]] position compiled with preserveInvariance, which
            // the D3D11 frontend requests for these shaders.
            if (math_contract_of(m_container).uses_precise) {
                m_builder.emit_decorate(var_id, SpvDecoration::Invariant);
            }
            m_position_output = var_id;
        } else {
            m_builder.emit_decorate(var_id, SpvDecoration::Location,
                                   output.register_num);
            if (m_result.stage != ShaderStage::Fragment &&
                (type_id == m_type_uvec4 || type_id == m_type_ivec4)) {
                m_builder.emit_decorate(var_id, SpvDecoration::Flat);
            }
        }

        m_output_vars[output.register_num] = var_id;
        m_output_value_types[output.register_num] = type_id;
    }
    if (m_result.stage == ShaderStage::Fragment &&
        m_options.fixed_function_push && m_options.apply_sample_mask &&
        m_sample_mask_output == 0) {
        // The blend state's SampleMask needs a coverage output even when the
        // shader writes no oMask; the epilogue stores the mask into it.
        const u32 array_ty = m_builder.emit_type_array(
            m_type_uint, m_builder.emit_constant_int(m_type_uint, 1u));
        m_sample_mask_output = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::Output, array_ty),
            SpvStorageClass::Output);
        m_builder.emit_name(m_sample_mask_output, "vitreo_sample_mask");
        m_builder.emit_decorate(m_sample_mask_output, SpvDecoration::BuiltIn,
                                static_cast<u32>(SpvBuiltIn::SampleMask));
        m_sample_mask_from_state_only = true;
    }
    if (m_result.stage == ShaderStage::Fragment &&
        m_options.line_coverage_alpha) {
        // The alpha-antialiased line as (n.x, n.y, c) in window pixels, n
        // its unit normal: the pixel's distance from it is |n.p + c|, exact
        // at the pixel centre (the epilogue turns it into o0.a's coverage).
        // Flat: every corner of the line's rectangle carries the same one.
        m_line_distance_input = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::Input, m_type_vec3),
            SpvStorageClass::Input);
        m_builder.emit_name(m_line_distance_input, "vitreo_line_equation");
        m_builder.emit_decorate(m_line_distance_input, SpvDecoration::Location,
                                kLineDistanceLocation);
        m_builder.emit_decorate(m_line_distance_input, SpvDecoration::Flat);
        if (const auto position = m_input_vars.find(m_frag_coord_register);
            m_frag_coord_register != ~0u && position != m_input_vars.end()) {
            m_line_frag_coord = position->second;
        } else {
            m_line_frag_coord = m_builder.emit_variable(
                m_builder.emit_type_pointer(SpvStorageClass::Input,
                                            m_type_vec4),
                SpvStorageClass::Input);
            m_builder.emit_name(m_line_frag_coord, "vitreo_line_position");
            m_builder.emit_decorate(m_line_frag_coord, SpvDecoration::BuiltIn,
                                    static_cast<u32>(SpvBuiltIn::FragCoord));
            m_line_frag_coord_declared = m_line_frag_coord;
        }
    }
    if (!clip_elements.empty()) {
        std::sort(clip_elements.begin(), clip_elements.end(),
                  [](const auto& a, const auto& b) {
                      return std::get<0>(a) != std::get<0>(b)
                                 ? std::get<0>(a) < std::get<0>(b)
                                 : std::get<2>(a) < std::get<2>(b);
                  });
        // D3D11_CLIP_OR_CULL_DISTANCE_COUNT is 8.
        if (clip_elements.size() > 8) {
            return fail("SV_ClipDistance: more than 8 components");
        }
        for (const auto& [index, reg, component] : clip_elements) {
            m_clip_sources.push_back({reg, component});
        }
        m_builder.emit_capability(SpvCapability::ClipDistance);
        const u32 array_ty = m_builder.emit_type_array(
            m_type_float,
            m_builder.emit_constant_int(
                m_type_uint, static_cast<u32>(m_clip_sources.size())));
        m_clip_distance_output = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::Output, array_ty),
            SpvStorageClass::Output);
        m_builder.emit_name(m_clip_distance_output, "SV_ClipDistance");
        m_builder.emit_decorate(m_clip_distance_output, SpvDecoration::BuiltIn,
                                static_cast<u32>(SpvBuiltIn::ClipDistance));
    }
    if (layer_output) {
        m_builder.emit_capability(SpvCapability::ShaderLayer);
        m_layer_output = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::Output, m_type_uint),
            SpvStorageClass::Output);
        m_builder.emit_name(m_layer_output, "SV_RenderTargetArrayIndex");
        m_builder.emit_decorate(m_layer_output, SpvDecoration::BuiltIn,
                                static_cast<u32>(SpvBuiltIn::Layer));
    }
    if (viewport_output) {
        m_builder.emit_capability(SpvCapability::ShaderViewportIndex);
        m_viewport_output = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::Output, m_type_uint),
            SpvStorageClass::Output);
        m_builder.emit_name(m_viewport_output, "SV_ViewportArrayIndex");
        m_builder.emit_decorate(m_viewport_output, SpvDecoration::BuiltIn,
                                static_cast<u32>(SpvBuiltIn::ViewportIndex));
    }
    if (gs_as_compute()) {
        std::sort(m_gs_emit_registers.begin(), m_gs_emit_registers.end());
        m_result.gs_output_registers =
            static_cast<u32>(m_gs_emit_registers.size());
        for (const auto& [reg, _] : m_input_vars) {
            m_gs_input_registers.push_back(reg);
        }
        if (!emit_geometry_expansion_buffers()) return false;
    }
    if ((vs_capture() || vs_capture_rasterized()) &&
        !emit_vertex_capture_declarations()) {
        return false;
    }
    if (m_result.stage == ShaderStage::Fragment &&
        m_options.count_pixel_invocations &&
        !emit_pixel_statistics_declarations()) {
        return false;
    }
    if ((hs_as_compute() || ds_lowered() || ds_as_compute()) &&
        !emit_tessellation_declarations()) {
        return false;
    }
    return emit_resource_variables();
}

} // namespace vitreo::graphics::dxbc_spirv_detail
