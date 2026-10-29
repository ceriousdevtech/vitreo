// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_operands.cpp
/// @brief Operand load and store.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

u32 DxbcTranslator::apply_swizzle(u32 vec_id, u32 swizzle) {
    // Swizzle byte: two bits per component selecting source lane.
    std::vector<u32> lanes(4);
    for (u32 i = 0; i < 4; ++i) {
        lanes[i] = (swizzle >> (i * 2)) & 0x3u;
    }
    // Identity .xyzw (0xE4) → no shuffle.
    if (lanes[0] == 0 && lanes[1] == 1 && lanes[2] == 2 && lanes[3] == 3) {
        return vec_id;
    }
    return m_builder.emit_vector_shuffle(m_type_vec4, vec_id, vec_id, lanes);
}

bool DxbcTranslator::load_vec4(const Sm4Operand& src, u32& out_id) {
    if (src.is_immediate || src.type == Sm4OperandType::Immediate32) {
        out_id = make_vec4_imm(src.immediate[0], src.immediate[1],
                               src.immediate[2], src.immediate[3]);
        return true;
    }

    u32 loaded = 0;
    if (src.type == Sm4OperandType::Temp) {
        loaded = m_builder.emit_load(m_type_vec4, get_temp_variable(src.index0));
    } else if (src.type == Sm4OperandType::IndexableTemp) {
        u32 pointer = 0;
        if (!indexable_temp_pointer(src, pointer)) return false;
        loaded = m_builder.emit_load(m_type_vec4, pointer);
    } else if (src.type == Sm4OperandType::OutputControlPoint) {
        // vocp[cp][register]: a hull shader's fork/join phases reading what
        // its control-point phase wrote.
        const auto var = m_hs_ocp_vars.find(src.index1);
        if (!hs_as_compute() || var == m_hs_ocp_vars.end()) {
            return fail("vocp: no output control point o" +
                        std::to_string(src.index1));
        }
        u32 cp = m_builder.emit_constant_int(m_type_uint, src.index0);
        if (src.index0_relative) {
            if (src.relative_type != Sm4OperandType::Temp) {
                return fail("vocp: unsupported relative index source");
            }
            const u32 bits = m_builder.emit_bitcast(
                m_type_uvec4,
                m_builder.emit_load(m_type_vec4,
                                    get_temp_variable(src.relative_index0)));
            cp = m_builder.emit_iadd(
                m_type_uint, cp,
                m_builder.emit_composite_extract(m_type_uint, bits,
                                                 {src.relative_component}));
            const u32 last = m_builder.emit_constant_int(
                m_type_uint, std::max(m_hs_out_cp, 1u) - 1u);
            cp = m_builder.emit_select(
                m_type_uint,
                m_builder.emit_integer_compare(SpvOp::OpULessThan, m_type_bool,
                                               cp, last),
                cp, last);
        }
        const u32 type = m_hs_ocp_types.at(src.index1);
        loaded = m_builder.emit_load(
            type, m_builder.emit_access_chain(
                      m_builder.emit_type_pointer(SpvStorageClass::Private, type),
                      var->second, {cp}));
        if (type != m_type_vec4) loaded = m_builder.emit_bitcast(m_type_vec4, loaded);
    } else if (src.type == Sm4OperandType::InputPatchConstant) {
        // vpc#: the patch constants (hull join phases; domain inputs).
        const auto var = m_tess_pc_vars.find(src.index0);
        if (var == m_tess_pc_vars.end() || src.index0_relative) {
            return fail("vpc: patch constant " + std::to_string(src.index0) +
                        " is undeclared or dynamically indexed");
        }
        const u32 type = m_tess_pc_types.at(src.index0);
        loaded = m_builder.emit_load(type, var->second);
        if (type != m_type_vec4) loaded = m_builder.emit_bitcast(m_type_vec4, loaded);
    } else if (src.type == Sm4OperandType::InputDomainPoint) {
        if (m_ds_tess_coord == 0) return fail("vDomain outside a domain shader");
        const u32 uvw = m_builder.emit_load(m_type_vec3, m_ds_tess_coord);
        loaded = m_builder.emit_composite_construct(
            m_type_vec4,
            {m_builder.emit_composite_extract(m_type_float, uvw, {0u}),
             m_builder.emit_composite_extract(m_type_float, uvw, {1u}),
             m_builder.emit_composite_extract(m_type_float, uvw, {2u}),
             m_builder.emit_constant_float(m_type_float, 0.0f)});
    } else if (src.type == Sm4OperandType::OutputControlPointId ||
               src.type == Sm4OperandType::InputForkInstanceId ||
               src.type == Sm4OperandType::InputJoinInstanceId) {
        const u32 var = src.type == Sm4OperandType::OutputControlPointId
                            ? m_hs_cp_id_var
                        : src.type == Sm4OperandType::InputForkInstanceId
                            ? m_hs_fork_id_var
                            : m_hs_join_id_var;
        if (var == 0) return fail("hull-shader phase id outside a hull shader");
        const u32 value = m_builder.emit_load(m_type_uint, var);
        loaded = m_builder.emit_bitcast(
            m_type_vec4, m_builder.emit_composite_construct(
                             m_type_uvec4, {value, value, value, value}));
    } else if (src.type == Sm4OperandType::Input ||
               src.type == Sm4OperandType::InputControlPoint) {
        // A geometry shader's input operand is v[vertex][register], so the
        // register is index1. That is a property of the BYTECODE, so it holds
        // for a lowered geometry shader too, whose result stage is Compute.
        const u32 input_register =
            arrayed_inputs() ? src.index1 : src.index0;
        // A scalar system-value builtin is not a vec4 varying: load the
        // 32-bit value and place it in the register lane the signature
        // declared, exactly as the input assembler would.
        if (const auto builtins = m_input_scalar_builtins.find(input_register);
            builtins != m_input_scalar_builtins.end()) {
            // Start from the register's ordinary varying lanes, if it has
            // any, and place each 32-bit builtin in the lane the signature
            // declared, exactly as the input assembler / rasterizer would.
            std::array<u32, 4> lanes{m_const_u0, m_const_u0, m_const_u0,
                                     m_const_u0};
            if (const auto varying = m_input_vars.find(input_register);
                varying != m_input_vars.end() && !arrayed_inputs()) {
                const u32 type = m_input_value_types.at(input_register);
                u32 bits = m_builder.emit_load(type, varying->second);
                if (type != m_type_uvec4) {
                    bits = m_builder.emit_bitcast(m_type_uvec4, bits);
                }
                for (u32 i = 0; i < 4; ++i) {
                    lanes[i] =
                        m_builder.emit_composite_extract(m_type_uint, bits, {i});
                }
            }
            for (const auto& builtin : builtins->second) {
                if (builtin.boolean) {
                    const u32 predicate =
                        m_builder.emit_load(m_type_bool, builtin.var);
                    lanes[builtin.component] = m_builder.emit_select(
                        m_type_uint, predicate,
                        m_builder.emit_constant_int(m_type_uint, 0xffffffffu),
                        m_const_u0);
                } else {
                    u32 value = m_builder.emit_load(m_type_uint, builtin.var);
                    if (builtin.minus_var != 0) {
                        value = m_builder.emit_isub(
                            m_type_uint, value,
                            m_builder.emit_load(m_type_uint, builtin.minus_var));
                    }
                    if (builtin.divisor > 1u) {
                        value = m_builder.emit_udiv(
                            m_type_uint, value,
                            m_builder.emit_constant_int(m_type_uint,
                                                        builtin.divisor));
                    }
                    if (builtin.capture_instance_bias) {
                        const u32 member =
                            m_builder.emit_constant_int(m_type_uint, 3u);
                        value = m_builder.emit_iadd(
                            m_type_uint, value,
                            m_builder.emit_load(
                                m_type_uint,
                                m_builder.emit_access_chain(
                                    m_type_ptr_push_uint, push_block_var(),
                                    {member})));
                    }
                    lanes[builtin.component] = value;
                }
            }
            const u32 packed = m_builder.emit_composite_construct(
                m_type_uvec4, {lanes[0], lanes[1], lanes[2], lanes[3]});
            // SM4/5 registers are untyped 128-bit containers, so the integer
            // enters the float4 register representation by bitcast.
            out_id = apply_swizzle(
                m_builder.emit_bitcast(m_type_vec4, packed), src.swizzle);
            return true;
        }
        auto it = m_input_vars.find(input_register);
        if (it == m_input_vars.end()) {
            return fail("load: missing input r" +
                        std::to_string(input_register));
        }
        const auto value_type_it = m_input_value_types.find(input_register);
        const auto pointer_type_it =
            m_input_pointer_types.find(input_register);
        if (value_type_it == m_input_value_types.end() ||
            pointer_type_it == m_input_pointer_types.end()) {
            return fail("load: missing input type r" +
                        std::to_string(input_register));
        }
        const u32 input_value_type = value_type_it->second;
        if (arrayed_inputs()) {
            // GS input operands are v[vertex][register] (a hull shader's
            // control-point phase: v[cp][register]). Their first immediate
            // index must fit the declared width.
            if (src.index0 >= input_array_width()) {
                return fail("geometry input vertex index is out of range");
            }
            if (src.index1_relative) {
                return fail("geometry input with a dynamic register index");
            }
            u32 vertex = m_builder.emit_constant_int(m_type_uint, src.index0);
            // The vertex index is DYNAMIC whenever the shader loops over its
            // inputs -- `for (i...) stream.Append(input[i])` compiles to
            // v[r0.x + 0][...] -- which is how geometry shaders are usually
            // written. Reading only the immediate part fetched vertex 0 for
            // every iteration: three identical vertices, a degenerate
            // triangle, and nothing drawn.
            if (src.index0_relative) {
                if (src.relative_type != Sm4OperandType::Temp) {
                    return fail("geometry input: unsupported relative vertex "
                                "index source");
                }
                const u32 relative_bits = m_builder.emit_bitcast(
                    m_type_uvec4,
                    m_builder.emit_load(
                        m_type_vec4, get_temp_variable(src.relative_index0)));
                const u32 relative = m_builder.emit_composite_extract(
                    m_type_uint, relative_bits, {src.relative_component});
                vertex = m_builder.emit_iadd(m_type_uint, vertex, relative);
                // Out-of-range input indices are undefined in D3D; reading a
                // Private array out of bounds is undefined on the GPU too,
                // and can read another thread's state. Clamp to the last
                // declared vertex.
                const u32 last = m_builder.emit_constant_int(
                    m_type_uint, input_array_width() - 1u);
                vertex = m_builder.emit_select(
                    m_type_uint,
                    m_builder.emit_integer_compare(SpvOp::OpULessThan,
                                                   m_type_bool, vertex, last),
                    vertex, last);
            }
            const u32 chain = m_builder.emit_access_chain(
                pointer_type_it->second, it->second, {vertex});
            loaded = m_builder.emit_load(input_value_type, chain);
        } else {
            loaded = m_builder.emit_load(input_value_type, it->second);
            if (m_result.stage == ShaderStage::Fragment &&
                input_register == m_frag_coord_register) {
                // D3D's pixel-shader SV_Position.w is the interpolated clip
                // w; FragCoord.w (SPIR-V, Metal [[position]]) is its
                // reciprocal. Measured on Windows with
                // tools/oracle_probe/ps_interpolation.hlsl: 1.590 where Metal
                // hands the shader 0.629.
                const u32 w = m_builder.emit_composite_extract(
                    m_type_float, loaded, {3u});
                const u32 clip_w = m_builder.emit_fdiv(
                    m_type_float,
                    m_builder.emit_constant_float(m_type_float, 1.0f), w);
                loaded = m_builder.emit_composite_insert(m_type_vec4, clip_w,
                                                         loaded, {3u});
                if (m_frag_coord_at_pixel_centre) {
                    for (u32 axis = 0; axis < 2u; ++axis) {
                        const u32 centre = m_builder.emit_fadd(
                            m_type_float,
                            m_builder.emit_ext_inst(
                                m_type_float, m_glsl_std450,
                                kGlslStd450Floor,
                                {m_builder.emit_composite_extract(
                                    m_type_float, loaded, {axis})}),
                            m_builder.emit_constant_float(m_type_float,
                                                          0.5f));
                        loaded = m_builder.emit_composite_insert(
                            m_type_vec4, centre, loaded, {axis});
                    }
                }
            }
        }
        // SM4/5 registers are untyped 128-bit containers. Integer interface
        // values therefore enter the existing float4 register representation
        // by bitcast, not numeric conversion.
        if (input_value_type != m_type_vec4) {
            loaded = m_builder.emit_bitcast(m_type_vec4, loaded);
        }
    } else if (src.type == Sm4OperandType::ConstantBuffer &&
               src.index0_relative) {
        // D3D11.3 7.11 uniform indexing, cb[r1.x + n][...]: how a class
        // method reads its instance's data (this[i].x names the buffer,
        // 22.7.20). Every buffer the index can name is read at the element
        // and the one named kept; a slot outside the API's 14 (or one no
        // candidate names) reads 0 (7.11.2: as if nothing were bound).
        if (src.index0_relative_type != Sm4OperandType::Temp) {
            return fail("load: unsupported relative constant buffer source");
        }
        const u32 selector = relative_index_value(
            src.index0_relative_type, src.index0_relative_register,
            src.index0_relative_component, src.index0);
        loaded = make_vec4_imm(0.0f, 0.0f, 0.0f, 0.0f);
        for (const u32 slot : uniform_index_candidates(
                 Sm4OperandType::ConstantBuffer, src.index0, 0u)) {
            Sm4Operand fixed = src;
            fixed.index0 = slot;
            fixed.index0_relative = false;
            fixed.swizzle = 0xE4u;
            u32 element = 0;
            if (!load_vec4(fixed, element)) return false;
            loaded = m_builder.emit_select(
                m_type_vec4,
                m_builder.emit_integer_compare(
                    SpvOp::OpIEqual, m_type_bool, selector,
                    m_builder.emit_constant_int(m_type_uint, slot)),
                element, loaded);
        }
    } else if (src.type == Sm4OperandType::ThisPointer) {
        if (!load_this_pointer(src, loaded)) return false;
    } else if (src.type == Sm4OperandType::ConstantBuffer) {
        auto it = m_cbuffer_vars.find(src.index0);
        if (it == m_cbuffer_vars.end()) {
            return fail("load: missing cb" + std::to_string(src.index0));
        }
        // A 5.1 constant-buffer array: the register selects the buffer
        // (CB1[r0.x + 1][0]); the element indexes into it as below.
        u32 block = it->second;
        if (src.has_register_index) {
            const ResourceRange* range =
                find_range(DxbcRegisterClass::ConstantBuffer, src.index0);
            if (range != nullptr && range->is_array) {
                block = array_element_pointer(src, *range);
                if (block == 0) return false;
            }
        }
        // Uniform block: member 0 is float4 array; index1 selects vector.
        const u32 member =
            m_builder.emit_constant_int(m_type_uint, 0);
        u32 elem = m_builder.emit_constant_int(m_type_uint, src.index1);
        if (src.index1_relative) {
            if (src.relative_type != Sm4OperandType::Temp) {
                return fail("load: unsupported relative cbuffer index source");
            }
            const u32 relative_vec = m_builder.emit_load(
                m_type_vec4, get_temp_variable(src.relative_index0));
            const u32 relative_bits = m_builder.emit_bitcast(
                m_type_uvec4, relative_vec);
            const u32 relative_index = m_builder.emit_composite_extract(
                m_type_uint, relative_bits, {src.relative_component});
            elem = m_builder.emit_iadd(m_type_uint, elem, relative_index);
        }
        if (m_options.guard_constant_buffers &&
            src.index0 < kConstantBufferLimitSlots) {
            // D3D11.3 7.5: an element past what is bound at the slot -- the
            // *SetConstantBuffers1 window within the buffer, nothing for an
            // empty slot -- reads 0 in all components. The load itself moves
            // to element 0, so no address past the bound buffer is read (a
            // literal index past a small buffer would otherwise read beyond
            // its Metal allocation).
            const u32 limit_pointer = m_builder.emit_access_chain(
                m_type_ptr_push_uint, push_block_var(),
                {m_builder.emit_constant_int(m_type_uint,
                                             kPushMemberConstantBufferLimits),
                 m_builder.emit_constant_int(m_type_uint, src.index0)});
            const u32 entry = m_builder.emit_load(m_type_uint, limit_pointer);
            const u32 known = m_builder.emit_integer_compare(
                SpvOp::OpINotEqual, m_type_bool,
                m_builder.emit_bitwise_and(
                    m_type_uint, entry,
                    m_builder.emit_constant_int(m_type_uint,
                                                kConstantBufferLimitKnown)),
                m_const_u0);
            const u32 limit = m_builder.emit_select(
                m_type_uint, known,
                m_builder.emit_bitwise_and(
                    m_type_uint, entry,
                    m_builder.emit_constant_int(m_type_uint,
                                                ~kConstantBufferLimitKnown)),
                m_builder.emit_constant_int(m_type_uint, 0xFFFFFFFFu));
            const u32 in_bounds = m_builder.emit_integer_compare(
                SpvOp::OpULessThan, m_type_bool, elem, limit);
            const u32 safe_elem =
                m_builder.emit_select(m_type_uint, in_bounds, elem, m_const_u0);
            const u32 chain = m_builder.emit_access_chain(
                m_type_ptr_uniform_vec4, block, {member, safe_elem});
            loaded = m_builder.emit_select(
                m_type_vec4, in_bounds, m_builder.emit_load(m_type_vec4, chain),
                make_vec4_imm(0.0f, 0.0f, 0.0f, 0.0f));
        } else {
            const u32 chain = m_builder.emit_access_chain(
                m_type_ptr_uniform_vec4, block, {member, elem});
            loaded = m_builder.emit_load(m_type_vec4, chain);
        }
    } else if (src.type == Sm4OperandType::ImmediateConstantBuffer) {
        if (m_immediate_constants.empty()) {
            return fail("load: immediate constant buffer is undeclared");
        }
        if (!src.index0_relative) {
            if (src.index0 >= m_immediate_constants.size()) {
                return fail("load: immediate constant buffer index out of range");
            }
            const auto& value = m_immediate_constants[src.index0];
            loaded = make_vec4_imm(value[0], value[1], value[2], value[3]);
        } else {
            if (src.relative_type != Sm4OperandType::Temp) {
                return fail("load: unsupported relative immediate constant "
                            "buffer index source");
            }
            const u32 relative_vec = m_builder.emit_load(
                m_type_vec4, get_temp_variable(src.relative_index0));
            const u32 relative_bits = m_builder.emit_bitcast(
                m_type_uvec4, relative_vec);
            u32 index = m_builder.emit_composite_extract(
                m_type_uint, relative_bits, {src.relative_component});
            if (src.index0 != 0u) {
                index = m_builder.emit_iadd(
                    m_type_uint, index,
                    m_builder.emit_constant_int(m_type_uint, src.index0));
            }
            loaded = make_vec4_imm(0.0f, 0.0f, 0.0f, 0.0f);
            for (u32 i = 0; i < m_immediate_constants.size(); ++i) {
                const auto& value = m_immediate_constants[i];
                const u32 candidate =
                    make_vec4_imm(value[0], value[1], value[2], value[3]);
                const u32 equal = m_builder.emit_integer_compare(
                    SpvOp::OpIEqual, m_type_bool, index,
                    m_builder.emit_constant_int(m_type_uint, i));
                loaded = m_builder.emit_select(
                    m_type_vec4, equal, candidate, loaded);
            }
        }
    } else if (src.type == Sm4OperandType::Output) {
        auto it = m_output_vars.find(src.index0);
        if (it == m_output_vars.end()) {
            return fail("load: missing output o" + std::to_string(src.index0));
        }
        const auto type = m_output_value_types.find(src.index0);
        if (type == m_output_value_types.end()) {
            return fail("load: missing output type o" +
                        std::to_string(src.index0));
        }
        loaded = m_builder.emit_load(type->second, it->second);
        if (type->second != m_type_vec4) {
            loaded = m_builder.emit_bitcast(m_type_vec4, loaded);
        }
    } else if (src.type == Sm4OperandType::InputThreadId) {
        if (m_result.stage != ShaderStage::Compute ||
            m_dispatch_thread_id == 0u) {
            return fail("SV_DispatchThreadID used outside compute shader");
        }
        const u32 invocation = m_builder.emit_load(
            m_type_uvec3, m_dispatch_thread_id);
        const u32 x = m_builder.emit_composite_extract(
            m_type_uint, invocation, {0u});
        const u32 y = m_builder.emit_composite_extract(
            m_type_uint, invocation, {1u});
        const u32 z = m_builder.emit_composite_extract(
            m_type_uint, invocation, {2u});
        const u32 bits = m_builder.emit_composite_construct(
            m_type_uvec4, {x, y, z, m_const_u0});
        loaded = m_builder.emit_bitcast(m_type_vec4, bits);
    } else if (src.type == Sm4OperandType::InputThreadGroupId ||
               src.type == Sm4OperandType::InputThreadIdInGroup) {
        if (m_result.stage != ShaderStage::Compute) {
            return fail("compute group system value used outside compute shader");
        }
        const u32 variable =
            src.type == Sm4OperandType::InputThreadGroupId
                ? m_group_id : m_group_thread_id;
        if (variable == 0u) {
            return fail("compute group system value is undeclared");
        }
        const u32 value = m_builder.emit_load(m_type_uvec3, variable);
        const u32 x = m_builder.emit_composite_extract(
            m_type_uint, value, {0u});
        const u32 y = m_builder.emit_composite_extract(
            m_type_uint, value, {1u});
        const u32 z = m_builder.emit_composite_extract(
            m_type_uint, value, {2u});
        const u32 bits = m_builder.emit_composite_construct(
            m_type_uvec4, {x, y, z, m_const_u0});
        loaded = m_builder.emit_bitcast(m_type_vec4, bits);
    } else if (src.type == Sm4OperandType::InputPrimitiveID ||
               src.type == Sm4OperandType::InputGSInstanceID) {
        // vPrim / vGSInstanceID of a geometry shader lowered to compute: the
        // invocation's input primitive (counted from the draw's start, per
        // instance of an instanced draw) and its [instance(n)] index.
        u32 value = 0;
        if ((ds_lowered() || ds_as_compute()) &&
            src.type == Sm4OperandType::InputPrimitiveID) {
            // A domain shader's vPrim is its patch.
            value = m_builder.emit_bitcast(
                m_type_uint, m_builder.emit_load(m_type_int, m_ds_patch_id));
        } else if (gs_as_compute() ||
                   (hs_as_compute() &&
                    src.type == Sm4OperandType::InputPrimitiveID)) {
            value = m_builder.emit_load(
                m_type_uint, src.type == Sm4OperandType::InputPrimitiveID
                                 ? m_gs_primitive_var
                                 : m_gs_instance_var);
        } else {
            return fail(src.type == Sm4OperandType::InputPrimitiveID
                            ? "vPrim outside a lowered geometry, hull or "
                              "domain shader"
                            : "vGSInstanceID outside a lowered geometry "
                              "shader");
        }
        loaded = m_builder.emit_bitcast(
            m_type_vec4, m_builder.emit_composite_construct(
                             m_type_uvec4, {value, value, value, value}));
    } else if (src.type == Sm4OperandType::InputCoverageMask) {
        if (m_sample_mask_input == 0u) {
            return fail("vCoverage used but not declared");
        }
        u32 mask = m_builder.emit_load(
            m_type_uint,
            m_builder.emit_access_chain(
                m_builder.emit_type_pointer(SpvStorageClass::Input,
                                            m_type_uint),
                m_sample_mask_input, {m_const_u0}));
        if (m_options.fixed_function_push) {
            // D3D11.3 3.5.6.2: with a forced sample count "Pixel Shader input
            // coverage ... sees the primitive's coverage of the
            // ForcedSampleCount sample pattern". A forced single sample on a
            // multisampled target is rasterized with every Metal sample at
            // the pixel centre, which covers all of them or none: keep the
            // bits the rasterizer's pattern has (push member 5, the
            // rasterizer sample count; 0 = not described, no mask).
            const u32 count = m_builder.emit_load(
                m_type_uint,
                m_builder.emit_access_chain(
                    m_type_ptr_push_uint, push_block_var(),
                    {m_builder.emit_constant_int(
                        m_type_uint, kPushMemberRasterizerSampleCount)}));
            const u32 pattern = m_builder.emit_isub(
                m_type_uint,
                m_builder.emit_shift_left_logical(
                    m_type_uint, m_builder.emit_constant_int(m_type_uint, 1u),
                    count),
                m_builder.emit_constant_int(m_type_uint, 1u));
            const u32 whole = m_builder.emit_logical(
                SpvOp::OpLogicalOr, m_type_bool,
                m_builder.emit_integer_compare(SpvOp::OpIEqual, m_type_bool,
                                               count, m_const_u0),
                m_builder.emit_integer_compare(
                    SpvOp::OpUGreaterThanEqual, m_type_bool, count,
                    m_builder.emit_constant_int(m_type_uint, 32u)));
            mask = m_builder.emit_bitwise_and(
                m_type_uint, mask,
                m_builder.emit_select(m_type_uint, whole, m_const_u_ffff,
                                      pattern));
        }
        loaded = m_builder.emit_bitcast(
            m_type_vec4, m_builder.emit_composite_construct(
                             m_type_uvec4, {mask, mask, mask, mask}));
    } else if (src.type ==
               Sm4OperandType::InputThreadIdInGroupFlattened) {
        if (m_result.stage != ShaderStage::Compute || m_group_index == 0u) {
            return fail("SV_GroupIndex used outside compute shader");
        }
        const u32 index = m_builder.emit_load(m_type_uint, m_group_index);
        const u32 bits = m_builder.emit_composite_construct(
            m_type_uvec4, {index, m_const_u0, m_const_u0, m_const_u0});
        loaded = m_builder.emit_bitcast(m_type_vec4, bits);
    } else {
        return fail("load: unsupported operand type " +
                    std::to_string(static_cast<u32>(src.type)));
    }

    out_id = apply_swizzle(loaded, src.swizzle);
    return true;
}

/// Microsoft Learn "Instruction Return Type and Modifiers": a source operand
/// may carry the extended operand modifier NEG (1), ABS (2) or ABSNEG (3).
/// Both are *floating-point* modifiers applied after the swizzle and before
/// the instruction executes, so they belong to every float-typed source.
/// Dropping them silently turns `add r0, r0, -r1` into an addition — a wrong
/// result with no diagnostic, which is what broke SDF text rendering.
bool DxbcTranslator::load_vec4_modified(const Sm4Operand& src, u32& out_id) {
    if (src.modifier > 3u) {
        return fail("source: unsupported operand modifier " +
                    std::to_string(src.modifier));
    }
    u32 value = 0;
    if (!load_vec4(src, value)) return false;
    if (src.modifier == 2u || src.modifier == 3u) {
        value = m_builder.emit_ext_inst(
            m_type_vec4, m_glsl_std450, kGlslStd450FAbs, {value});
    }
    if (src.modifier == 1u || src.modifier == 3u) {
        value = m_builder.emit_fnegate(m_type_vec4, value);
    }
    out_id = value;
    return true;
}

bool DxbcTranslator::indexable_temp_pointer(const Sm4Operand& operand,
                                            u32& pointer) {
    const auto variable = m_indexable_temp_vars.find(operand.index0);
    if (variable == m_indexable_temp_vars.end()) {
        return fail("indexable temporary x" +
                    std::to_string(operand.index0) + " is undeclared");
    }
    u32 index = m_builder.emit_constant_int(m_type_uint, operand.index1);
    if (operand.index1_relative) {
        if (operand.relative_type != Sm4OperandType::Temp) {
            return fail("indexable temporary uses unsupported relative index");
        }
        const u32 relative = m_builder.emit_load(
            m_type_vec4, get_temp_variable(operand.relative_index0));
        const u32 relative_bits =
            m_builder.emit_bitcast(m_type_uvec4, relative);
        index = m_builder.emit_composite_extract(
            m_type_uint, relative_bits, {operand.relative_component});
        if (operand.index1 != 0u) {
            index = m_builder.emit_iadd(
                m_type_uint, index,
                m_builder.emit_constant_int(m_type_uint, operand.index1));
        }
    }
    pointer = m_builder.emit_access_chain(
        temp_pointer_type(), variable->second, {index});
    return true;
}

bool DxbcTranslator::store_vec4(const Sm4Operand& dst, u32 value_id) {
    // Learn: the SATURATE instruction modifier clamps the result to [0,1]
    // before the write mask is applied. D3D defines saturate(NaN) as 0, so
    // the low clamp must be an ordered compare that rejects NaN first.
    if (m_saturate_destination) {
        const u32 zero = m_builder.emit_constant_float(m_type_float, 0.0f);
        const u32 one = m_builder.emit_constant_float(m_type_float, 1.0f);
        const u32 zeros = m_builder.emit_composite_construct(
            m_type_vec4, {zero, zero, zero, zero});
        const u32 ones = m_builder.emit_composite_construct(
            m_type_vec4, {one, one, one, one});
        const u32 above_zero =
            m_builder.emit_ford_ge(m_type_bvec4, value_id, zeros);
        const u32 low_clamped =
            m_builder.emit_select(m_type_vec4, above_zero, value_id, zeros);
        const u32 below_one =
            m_builder.emit_ford_lt(m_type_bvec4, low_clamped, ones);
        value_id =
            m_builder.emit_select(m_type_vec4, below_one, low_clamped, ones);
    }
    if (dst.type == Sm4OperandType::OutputCoverageMask ||
        dst.type == Sm4OperandType::OutputStencilRef) {
        const bool mask = dst.type == Sm4OperandType::OutputCoverageMask;
        const u32 target = mask ? m_sample_mask_output : m_stencil_output;
        if (target == 0) {
            return fail(mask ? "store: oMask is undeclared"
                             : "store: oStencilRef is undeclared");
        }
        const u32 bits = m_builder.emit_composite_extract(
            m_type_uint, m_builder.emit_bitcast(m_type_uvec4, value_id),
            {0u});
        if (mask) {
            m_builder.emit_store(
                m_builder.emit_access_chain(
                    m_builder.emit_type_pointer(SpvStorageClass::Output,
                                                m_type_uint),
                    m_sample_mask_output, {m_const_u0}),
                bits);
        } else {
            m_builder.emit_store(m_stencil_output,
                                 m_builder.emit_bitcast(m_type_int, bits));
        }
        return true;
    }
    if (dst.type == Sm4OperandType::OutputDepth ||
        dst.type == Sm4OperandType::OutputDepthGreaterEqual ||
        dst.type == Sm4OperandType::OutputDepthLessEqual) {
        if (m_depth_output == 0) {
            return fail("store: SV_Depth output is undeclared");
        }
        const u32 depth =
            m_builder.emit_composite_extract(m_type_float, value_id, {0u});
        m_builder.emit_store(m_depth_output, depth);
        return true;
    }
    u32* var_id = nullptr;
    u32 temp_storage = 0;
    u32 storage_type = m_type_vec4;
    if (dst.type == Sm4OperandType::Temp) {
        temp_storage = get_temp_variable(dst.index0);
        var_id = &temp_storage;
    } else if (dst.type == Sm4OperandType::IndexableTemp) {
        if (!indexable_temp_pointer(dst, temp_storage)) return false;
        var_id = &temp_storage;
    } else if (dst.type == Sm4OperandType::Output && dst.index0_relative) {
        // o[r + n]: a fork phase writes its instance's register as
        // o[vForkInstanceID.x + n] (fxc, e.g. the three SV_TessFactor edges
        // of `float e[3]`). The registers are separate variables, so every
        // candidate from n up takes the value where the index names it and
        // keeps its own elsewhere. Writing o[n] alone left the other edges
        // at 0 and culled every patch (d3d11_a07_ia_probe `D patchlist`).
        Sm4Operand relative{};
        relative.type = dst.relative_type;
        relative.index0 = dst.relative_index0;
        u32 relative_value = 0;
        if (!load_vec4(relative, relative_value)) {
            return fail("store: unreadable relative output index");
        }
        u32 index = m_builder.emit_composite_extract(
            m_type_uint, m_builder.emit_bitcast(m_type_uvec4, relative_value),
            {dst.relative_component});
        if (dst.index0 != 0u) {
            index = m_builder.emit_iadd(
                m_type_uint, index,
                m_builder.emit_constant_int(m_type_uint, dst.index0));
        }
        const u32 mask = dst.write_mask == 0 ? 0xFu : dst.write_mask;
        bool any = false;
        for (const auto& [reg, var] : m_output_vars) {
            if (reg < dst.index0) continue;
            any = true;
            const u32 type = m_output_value_types.at(reg);
            u32 existing = m_builder.emit_load(type, var);
            if (type != m_type_vec4) {
                existing = m_builder.emit_bitcast(m_type_vec4, existing);
            }
            std::vector<u32> comps(4);
            for (u32 i = 0; i < 4; ++i) {
                comps[i] = m_builder.emit_composite_extract(
                    m_type_float, (mask & (1u << i)) != 0 ? value_id : existing,
                    {i});
            }
            const u32 selected = m_builder.emit_select(
                m_type_vec4,
                m_builder.emit_integer_compare(
                    SpvOp::OpIEqual, m_type_bool, index,
                    m_builder.emit_constant_int(m_type_uint, reg)),
                m_builder.emit_composite_construct(m_type_vec4, comps),
                existing);
            m_builder.emit_store(
                var, type == m_type_vec4 ? selected
                                         : m_builder.emit_bitcast(type, selected));
        }
        if (!any) {
            return fail("store: no output register at or above o" +
                        std::to_string(dst.index0));
        }
        return true;
    } else if (dst.type == Sm4OperandType::Output) {
        auto it = m_output_vars.find(dst.index0);
        if (it == m_output_vars.end()) {
            return fail("store: missing output o" + std::to_string(dst.index0));
        }
        var_id = &it->second;
        const auto type = m_output_value_types.find(dst.index0);
        if (type == m_output_value_types.end()) {
            return fail("store: missing output type o" +
                        std::to_string(dst.index0));
        }
        storage_type = type->second;
    } else {
        return fail("store: unsupported dest type " +
                    std::to_string(static_cast<u32>(dst.type)));
    }

    const u32 mask = dst.write_mask == 0 ? 0xFu : dst.write_mask;
    if (mask == 0xFu) {
        const u32 stored = storage_type == m_type_vec4
            ? value_id : m_builder.emit_bitcast(storage_type, value_id);
        m_builder.emit_store(*var_id, stored);
        return true;
    }

    // Merge write-masked components into the existing register value.
    u32 existing = m_builder.emit_load(storage_type, *var_id);
    if (storage_type != m_type_vec4) {
        existing = m_builder.emit_bitcast(m_type_vec4, existing);
    }
    std::vector<u32> comps(4);
    for (u32 i = 0; i < 4; ++i) {
        if ((mask & (1u << i)) != 0) {
            comps[i] = m_builder.emit_composite_extract(m_type_float, value_id, {i});
        } else {
            comps[i] = m_builder.emit_composite_extract(m_type_float, existing, {i});
        }
    }
    const u32 merged =
        m_builder.emit_composite_construct(m_type_vec4, comps);
    const u32 stored = storage_type == m_type_vec4
        ? merged : m_builder.emit_bitcast(storage_type, merged);
    m_builder.emit_store(*var_id, stored);
    return true;
}

} // namespace vitreo::graphics::dxbc_spirv_detail
