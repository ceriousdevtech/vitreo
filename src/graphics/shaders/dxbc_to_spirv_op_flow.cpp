// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_op_flow.cpp
/// @brief Flow-control opcodes: emit/cut, calls, switch, loops, branches, discard.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

bool DxbcTranslator::translate_flow_control_operation(u32 opcode_token,
                                                      Sm4Opcode opcode,
                                                      u32 sample_controls) {
    switch (opcode) {
    case Sm4Opcode::EMIT:
    case Sm4Opcode::CUT:
    case Sm4Opcode::EMITTHENCUT:
    case Sm4Opcode::EMIT_STREAM:
    case Sm4Opcode::CUT_STREAM:
    case Sm4Opcode::EMITTHENCUT_STREAM: {
        if (m_container.stage != ShaderStage::Geometry) {
            return fail("geometry stream instruction used outside geometry shader");
        }
        // SM4 has one implicit stream. EMIT snapshots every declared output;
        // CUT completes the current point/strip primitive. The SM5 forms name
        // the stream (22.8.2-22.8.6), whose region the vertex goes to.
        u32 stream_index = 0;
        if (opcode == Sm4Opcode::EMIT_STREAM ||
            opcode == Sm4Opcode::CUT_STREAM ||
            opcode == Sm4Opcode::EMITTHENCUT_STREAM) {
            Sm4Operand stream;
            if (!m_decoder.decode_operand(stream) ||
                stream.type != Sm4OperandType::Stream) {
                return fail("stream instruction: expected a stream operand");
            }
            stream_index = stream.index0;
            if (stream_index >= 4u ||
                (m_gs_stream_mask & (1u << stream_index)) == 0u) {
                return fail("stream instruction on m" +
                            std::to_string(stream_index) +
                            ", which the shader does not declare");
            }
        } else if ((m_gs_stream_mask & 1u) == 0u) {
            return fail("emit/cut without a stream in a shader that declares "
                        "no stream 0");
        }
        const bool emits = opcode == Sm4Opcode::EMIT ||
                           opcode == Sm4Opcode::EMITTHENCUT ||
                           opcode == Sm4Opcode::EMIT_STREAM ||
                           opcode == Sm4Opcode::EMITTHENCUT_STREAM;
        const bool cuts = opcode == Sm4Opcode::CUT ||
                          opcode == Sm4Opcode::EMITTHENCUT ||
                          opcode == Sm4Opcode::CUT_STREAM ||
                          opcode == Sm4Opcode::EMITTHENCUT_STREAM;
        if (gs_as_compute()) {
            // Lowered to compute: there is no geometry stage to emit into, so
            // the vertex is appended to the expansion buffer instead.
            if (emits) {
                if (!emit_geometry_vertex(cuts, stream_index)) return false;
            } else if (cuts) {
                emit_geometry_cut();
            }
            break;
        }
        if (emits) m_builder.emit_vertex();
        if (cuts) m_builder.emit_end_primitive();
        break;
    }

    case Sm4Opcode::INTERFACE_CALL:
        if (sample_controls != 0u) {
            return fail("FCALL: unsupported opcode modifier");
        }
        return emit_interface_call(opcode_token);

    case Sm4Opcode::CALL: {
        if ((opcode_token & 0x00002000u) != 0u || sample_controls != 0u) {
            return fail("CALL: unsupported opcode modifier");
        }
        Sm4Operand label;
        if (!m_decoder.decode_operand(label) ||
            label.type != Sm4OperandType::Label || label.modifier != 0u) {
            return fail("CALL: expected an unmodified label operand");
        }
        if (!hs_as_compute()) {
            if (!emit_subroutine_call(label.index0)) return false;
            break;
        }
        // A hull-shader phase keeps its registers function-local, so a
        // subroutine is inlined there, and only at the phase's top level.
        if (!m_if_stack.empty() || !m_loop_stack.empty() ||
            !m_switch_stack.empty()) {
            return fail("CALL: invocation inside structured flow of a hull "
                        "shader phase is not implemented");
        }
        if (!translate_subroutine(label.index0)) return false;
        break;
    }

    case Sm4Opcode::CALLC: {
        // D3D11.3 22.7.11: callc_z/_nz tests one component at the bit level
        // (OpcodeToken0 bit 18 selects _nz), then calls like `call`.
        if ((opcode_token & 0x00002000u) != 0u || sample_controls != 0u) {
            return fail("CALLC: unsupported opcode modifier");
        }
        Sm4Operand condition, label;
        if (!m_decoder.decode_operand(condition) ||
            !m_decoder.decode_operand(label) ||
            label.type != Sm4OperandType::Label || label.modifier != 0u) {
            return fail("CALLC: bad operands");
        }
        if (condition.modifier != 0u) {
            return fail("CALLC: source modifiers are not supported");
        }
        if (hs_as_compute()) {
            return fail("CALLC: conditional subroutine call in a hull shader "
                        "phase is not implemented");
        }
        u32 value = 0;
        if (!load_vec4(condition, value)) return false;
        const u32 lane = m_builder.emit_composite_extract(
            m_type_uint, m_builder.emit_bitcast(m_type_uvec4, value), {0u});
        const bool test_nonzero = (opcode_token & 0x00040000u) != 0u;
        const u32 taken = test_nonzero
            ? m_builder.emit_inotequal(m_type_bool, lane, m_const_u0)
            : m_builder.emit_integer_compare(SpvOp::OpIEqual, m_type_bool,
                                             lane, m_const_u0);
        const u32 call_block = m_builder.allocate_id();
        const u32 merge_block = m_builder.allocate_id();
        m_builder.emit_selection_merge(merge_block);
        m_builder.emit_branch_conditional(taken, call_block, merge_block);
        m_builder.emit_label_id(call_block);
        if (!emit_subroutine_call(label.index0)) return false;
        m_builder.emit_branch(merge_block);
        m_builder.emit_label_id(merge_block);
        break;
    }

    case Sm4Opcode::SWITCH: {
        if (m_if_stack.size() + m_loop_stack.size() + m_switch_stack.size() >=
            64u) {
            return fail("SWITCH: flow-control nesting exceeds 64");
        }
        if ((opcode_token & 0x00002000u) != 0u || sample_controls != 0u) {
            return fail("SWITCH: unsupported opcode modifier");
        }
        Sm4Operand source;
        if (!m_decoder.decode_operand(source)) {
            return fail("SWITCH: bad operand");
        }
        if (source.modifier != 0u) {
            return fail("SWITCH: source modifiers are not supported");
        }
        if (!source.is_scalar()) {
            const u32 selected = source.swizzle & 0x3u;
            if (source.swizzle != selected * 0x55u) {
                return fail("SWITCH: source must select one component");
            }
        }
        u32 value = 0;
        if (!load_vec4(source, value)) return false;
        value = m_builder.emit_bitcast(m_type_uvec4, value);
        const u32 selector = m_builder.emit_composite_extract(
            m_type_uint, value, {0u});
        const u32 dispatch = m_builder.allocate_id();
        const u32 merge = m_builder.allocate_id();
        m_builder.emit_branch(dispatch);
        m_block_terminated = true;
        SwitchFrame frame{};
        frame.selector = selector;
        frame.dispatch_block = dispatch;
        frame.merge_block = merge;
        m_switch_stack.push_back(std::move(frame));
        m_breakable_stack.push_back({BreakableKind::Switch, merge});
        break;
    }

    case Sm4Opcode::CASE: {
        if ((opcode_token & 0x00002000u) != 0u || sample_controls != 0u) {
            return fail("CASE: unsupported opcode modifier");
        }
        if (m_switch_stack.empty() || m_breakable_stack.empty() ||
            m_breakable_stack.back().kind != BreakableKind::Switch) {
            return fail("CASE: instruction is not inside the active switch");
        }
        SwitchFrame& frame = m_switch_stack.back();
        if (frame.active_body && !m_block_terminated) {
            return fail("CASE: code fallthrough is not permitted");
        }
        if (frame.active_body) frame.active_body = false;
        Sm4Operand literal;
        if (!m_decoder.decode_operand(literal) || !literal.is_immediate ||
            !literal.is_scalar() || literal.modifier != 0u) {
            return fail("CASE: expected a scalar 32-bit immediate");
        }
        u32 bits = 0;
        std::memcpy(&bits, &literal.immediate[0], sizeof(bits));
        if (!frame.case_values.insert(bits).second) {
            return fail("CASE: duplicate immediate value");
        }
        frame.pending_cases.push_back(bits);
        m_block_terminated = true;
        break;
    }

    case Sm4Opcode::DEFAULT: {
        if ((opcode_token & 0x00002000u) != 0u || sample_controls != 0u) {
            return fail("DEFAULT: unsupported opcode modifier");
        }
        if (m_switch_stack.empty() || m_breakable_stack.empty() ||
            m_breakable_stack.back().kind != BreakableKind::Switch) {
            return fail("DEFAULT: instruction is not inside the active switch");
        }
        SwitchFrame& frame = m_switch_stack.back();
        if (frame.has_default) return fail("DEFAULT: duplicate instruction");
        if (frame.active_body && !m_block_terminated) {
            return fail("DEFAULT: code fallthrough is not permitted");
        }
        if (frame.active_body) frame.active_body = false;
        frame.has_default = true;
        frame.pending_default = true;
        m_block_terminated = true;
        break;
    }

    case Sm4Opcode::ENDSWITCH: {
        if ((opcode_token & 0x00002000u) != 0u || sample_controls != 0u) {
            return fail("ENDSWITCH: unsupported opcode modifier");
        }
        if (m_switch_stack.empty() || m_breakable_stack.empty() ||
            m_breakable_stack.back().kind != BreakableKind::Switch) {
            return fail("ENDSWITCH: unmatched instruction");
        }
        SwitchFrame frame = std::move(m_switch_stack.back());
        m_switch_stack.pop_back();
        m_breakable_stack.pop_back();
        if (frame.active_body && !m_block_terminated) {
            m_builder.emit_branch(frame.merge_block);
        }
        for (u32 literal : frame.pending_cases) {
            frame.targets.emplace_back(literal, frame.merge_block);
        }
        if (frame.pending_default || !frame.has_default) {
            frame.default_block = frame.merge_block;
        }
        m_builder.emit_label_id(frame.dispatch_block);
        m_builder.emit_selection_merge(frame.merge_block);
        m_builder.emit_switch(
            frame.selector, frame.default_block, frame.targets);
        m_builder.emit_label_id(frame.merge_block);
        m_block_terminated = false;
        break;
    }

    case Sm4Opcode::LOOP: {
        if (m_if_stack.size() + m_loop_stack.size() + m_switch_stack.size() >=
            64u) {
            return fail("LOOP: flow-control nesting exceeds 64");
        }
        if ((opcode_token & 0x00002000u) != 0u || sample_controls != 0u) {
            return fail("LOOP: unsupported opcode modifier");
        }
        const u32 header_block = m_builder.allocate_id();
        const u32 body_block = m_builder.allocate_id();
        const u32 continue_block = m_builder.allocate_id();
        const u32 merge_block = m_builder.allocate_id();
        // Iteration guard (Timeout Detection and Recovery stand-in). On
        // Windows, GPU work that runs past ~2 s is stopped by TDR and the app
        // sees DXGI_ERROR_DEVICE_REMOVED; macOS has nothing equivalent for
        // us, and a shader looping on a runaway count pins the GPU until the
        // whole Mac freezes. Every loop therefore counts its iterations per
        // invocation and breaks out past a cap no real shader approaches
        // (VITREO_SHADER_LOOP_CAP, default 2^14; 0 disables). A tripped guard
        // shows up as a slow command buffer in gpu_watch instead of a hung
        // machine.
        const u32 cap = shader_loop_iteration_cap();
        u32 guard_var = 0;
        if (cap != 0) {
            guard_var = m_builder.emit_variable(
                m_builder.emit_type_pointer(SpvStorageClass::Private,
                                            m_type_uint),
                SpvStorageClass::Private);
            m_loop_guard_vars.push_back(guard_var);
            m_builder.emit_store(guard_var,
                                 m_builder.emit_constant_int(m_type_uint, 0u));
        }
        m_builder.emit_branch(header_block);
        m_builder.emit_label_id(header_block);
        m_builder.emit_loop_merge(merge_block, continue_block);
        m_builder.emit_branch(body_block);
        m_builder.emit_label_id(body_block);
        if (guard_var != 0) {
            const u32 count = m_builder.emit_iadd(
                m_type_uint, m_builder.emit_load(m_type_uint, guard_var),
                m_builder.emit_constant_int(m_type_uint, 1u));
            m_builder.emit_store(guard_var, count);
            const u32 exhausted = m_builder.emit_integer_compare(
                SpvOp::OpUGreaterThan, m_type_bool, count,
                m_builder.emit_constant_int(m_type_uint, cap));
            const u32 bail_block = m_builder.allocate_id();
            const u32 run_block = m_builder.allocate_id();
            m_builder.emit_selection_merge(run_block);
            m_builder.emit_branch_conditional(exhausted, bail_block, run_block);
            m_builder.emit_label_id(bail_block);
            m_builder.emit_branch(merge_block);
            m_builder.emit_label_id(run_block);
        }
        m_loop_stack.push_back(
            {header_block, continue_block, merge_block});
        m_breakable_stack.push_back({BreakableKind::Loop, merge_block});
        break;
    }

    case Sm4Opcode::ENDLOOP: {
        if (m_loop_stack.empty()) {
            return fail("ENDLOOP: unmatched instruction");
        }
        if ((opcode_token & 0x00002000u) != 0u || sample_controls != 0u) {
            return fail("ENDLOOP: unsupported opcode modifier");
        }
        const LoopFrame frame = m_loop_stack.back();
        m_loop_stack.pop_back();
        if (m_breakable_stack.empty() ||
            m_breakable_stack.back().kind != BreakableKind::Loop) {
            return fail("ENDLOOP: mismatched nested switch");
        }
        m_breakable_stack.pop_back();
        if (!m_block_terminated) {
            m_builder.emit_branch(frame.continue_block);
        }
        m_builder.emit_label_id(frame.continue_block);
        m_builder.emit_branch(frame.header_block);
        m_builder.emit_label_id(frame.merge_block);
        m_block_terminated = false;
        break;
    }

    case Sm4Opcode::BREAK:
    case Sm4Opcode::CONTINUE: {
        if (opcode == Sm4Opcode::CONTINUE && m_loop_stack.empty()) {
            return fail("CONTINUE: instruction is not inside a loop");
        }
        if (opcode == Sm4Opcode::BREAK && m_breakable_stack.empty()) {
            return fail(opcode == Sm4Opcode::BREAK
                ? "BREAK: instruction is not inside a supported loop"
                : "CONTINUE: instruction is not inside a loop");
        }
        if ((opcode_token & 0x00002000u) != 0u || sample_controls != 0u) {
            return fail("loop control: unsupported opcode modifier");
        }
        const u32 target = opcode == Sm4Opcode::BREAK
            ? m_breakable_stack.back().merge_block
            : m_loop_stack.back().continue_block;
        m_builder.emit_branch(target);
        m_block_terminated = true;
        break;
    }

    case Sm4Opcode::BREAKC:
    case Sm4Opcode::CONTINUEC: {
        if (opcode == Sm4Opcode::CONTINUEC && m_loop_stack.empty()) {
            return fail("CONTINUEC: instruction is not inside a loop");
        }
        if (opcode == Sm4Opcode::BREAKC && m_breakable_stack.empty()) {
            return fail(opcode == Sm4Opcode::BREAKC
                ? "BREAKC: instruction is not inside a supported loop"
                : "CONTINUEC: instruction is not inside a loop");
        }
        if ((opcode_token & 0x00002000u) != 0u || sample_controls != 0u) {
            return fail("conditional loop control: unsupported modifier");
        }
        Sm4Operand source;
        if (!m_decoder.decode_operand(source)) {
            return fail("conditional loop control: bad operand");
        }
        if (source.modifier != 0u) {
            return fail("conditional loop control: source modifier");
        }
        if (!source.is_scalar()) {
            const u32 selected = source.swizzle & 0x3u;
            if (source.swizzle != selected * 0x55u) {
                return fail("conditional loop control: scalar required");
            }
        }
        u32 value = 0;
        if (!load_vec4(source, value)) return false;
        value = m_builder.emit_bitcast(m_type_uvec4, value);
        const u32 lane = m_builder.emit_composite_extract(
            m_type_uint, value, {0u});
        const u32 zero = m_builder.emit_constant_int(m_type_uint, 0u);
        const bool test_nonzero = (opcode_token & 0x00040000u) != 0u;
        const u32 condition = test_nonzero
            ? m_builder.emit_inotequal(m_type_bool, lane, zero)
            : m_builder.emit_integer_compare(
                  SpvOp::OpIEqual, m_type_bool, lane, zero);
        const u32 transfer_target = opcode == Sm4Opcode::BREAKC
            ? m_breakable_stack.back().merge_block
            : m_loop_stack.back().continue_block;
        const u32 transfer_block = m_builder.allocate_id();
        const u32 fallthrough_block = m_builder.allocate_id();
        m_builder.emit_selection_merge(fallthrough_block);
        m_builder.emit_branch_conditional(
            condition, transfer_block, fallthrough_block);
        m_builder.emit_label_id(transfer_block);
        m_builder.emit_branch(transfer_target);
        m_builder.emit_label_id(fallthrough_block);
        break;
    }

    case Sm4Opcode::IF: {
        // Microsoft Learn if (sm4): a selected 32-bit lane is tested at the
        // bit level. OpcodeToken0 bit 18 selects _nz; clear selects _z.
        if (m_if_stack.size() + m_loop_stack.size() + m_switch_stack.size() >=
            64u) {
            return fail("IF: nesting exceeds the SM4 limit of 64");
        }
        if ((opcode_token & 0x00002000u) != 0u || sample_controls != 0u) {
            return fail("IF: unsupported opcode modifier");
        }
        Sm4Operand source;
        if (!m_decoder.decode_operand(source)) return fail("IF: bad operand");
        if (source.modifier != 0u) {
            return fail("IF: source modifiers are not supported");
        }
        if (!source.is_scalar()) {
            const u32 selected = source.swizzle & 0x3u;
            if (source.swizzle != selected * 0x55u) {
                return fail("IF: source must select one component");
            }
        }
        u32 value = 0;
        if (!load_vec4(source, value)) return false;
        value = m_builder.emit_bitcast(m_type_uvec4, value);
        const u32 lane = m_builder.emit_composite_extract(
            m_type_uint, value, {0u});
        const u32 zero = m_builder.emit_constant_int(m_type_uint, 0u);
        const bool test_nonzero = (opcode_token & 0x00040000u) != 0u;
        const u32 condition = test_nonzero
            ? m_builder.emit_inotequal(m_type_bool, lane, zero)
            : m_builder.emit_integer_compare(
                  SpvOp::OpIEqual, m_type_bool, lane, zero);
        const u32 then_block = m_builder.allocate_id();
        const u32 else_block = m_builder.allocate_id();
        const u32 merge_block = m_builder.allocate_id();
        m_builder.emit_selection_merge(merge_block);
        m_builder.emit_branch_conditional(condition, then_block, else_block);
        m_builder.emit_label_id(then_block);
        m_if_stack.push_back({else_block, merge_block, false});
        break;
    }

    case Sm4Opcode::ELSE: {
        if (m_if_stack.empty()) return fail("ELSE: unmatched instruction");
        IfFrame& frame = m_if_stack.back();
        if (frame.has_else) return fail("ELSE: duplicate instruction");
        if (!m_block_terminated) m_builder.emit_branch(frame.merge_block);
        m_builder.emit_label_id(frame.else_block);
        frame.has_else = true;
        m_block_terminated = false;
        break;
    }

    case Sm4Opcode::ENDIF: {
        if (m_if_stack.empty()) return fail("ENDIF: unmatched instruction");
        const IfFrame frame = m_if_stack.back();
        m_if_stack.pop_back();
        if (!m_block_terminated) m_builder.emit_branch(frame.merge_block);
        if (!frame.has_else) {
            m_builder.emit_label_id(frame.else_block);
            m_builder.emit_branch(frame.merge_block);
        }
        m_builder.emit_label_id(frame.merge_block);
        m_block_terminated = false;
        break;
    }

    case Sm4Opcode::RETC: {
        // Microsoft Learn retc (sm4): conditionally return when the selected
        // lane is zero/nonzero at the bit level -- from main, or from the
        // subroutine function it appears in (22.7.17).
        if (!m_call_stack.empty()) {
            return fail("RETC: conditional return from a subroutine inlined "
                        "into a hull shader phase is not implemented");
        }
        if ((opcode_token & 0x00002000u) != 0u || sample_controls != 0u) {
            return fail("RETC: unsupported opcode modifier");
        }
        Sm4Operand source;
        if (!m_decoder.decode_operand(source)) {
            return fail("RETC: bad operand");
        }
        if (source.modifier != 0u) {
            return fail("RETC: source modifiers are not supported");
        }
        if (!source.is_scalar()) {
            const u32 selected = source.swizzle & 0x3u;
            if (source.swizzle != selected * 0x55u) {
                return fail("RETC: source must select one component");
            }
        }
        // The contract forbids RETC as the last instruction; a following RET
        // (or other instruction) must remain in the token stream.
        if (!m_decoder.has_tokens(1)) {
            return fail("RETC: cannot be the final instruction");
        }
        u32 value = 0;
        if (!load_vec4(source, value)) return false;
        value = m_builder.emit_bitcast(m_type_uvec4, value);
        const u32 lane = m_builder.emit_composite_extract(
            m_type_uint, value, {0u});
        const u32 zero = m_builder.emit_constant_int(m_type_uint, 0u);
        const bool test_nonzero = (opcode_token & 0x00040000u) != 0u;
        const u32 condition = test_nonzero
            ? m_builder.emit_inotequal(m_type_bool, lane, zero)
            : m_builder.emit_integer_compare(
                  SpvOp::OpIEqual, m_type_bool, lane, zero);
        const u32 return_block = m_builder.allocate_id();
        const u32 continue_block = m_builder.allocate_id();
        m_builder.emit_selection_merge(continue_block);
        m_builder.emit_branch_conditional(
            condition, return_block, continue_block);
        m_builder.emit_label_id(return_block);
        if (!m_in_subroutine) {
            m_emitted_early_return = true;
            if (!emit_system_value_epilogue()) return false;
        }
        m_builder.emit_return();
        m_builder.emit_label_id(continue_block);
        break;
    }

    case Sm4Opcode::DISCARD: {
        // D3D11.3 sections 16.13 and 22.5.1: keep executing for quad
        // derivatives, commit earlier UAV accesses, and suppress subsequent
        // UAV writes and framebuffer outputs. Demotion preserves derivatives
        // and suppresses side effects; its MSL discard_fragment lowering
        // guarantees ordering of the earlier resource writes. OpKill would
        // terminate execution and lose the derivative contract.
        if (m_result.stage != ShaderStage::Fragment) {
            return fail("DISCARD: only pixel shaders are supported");
        }
        Sm4Operand source;
        if (!m_decoder.decode_operand(source)) {
            return fail("DISCARD: bad operand");
        }
        if (source.modifier != 0u) {
            return fail("DISCARD: unsupported source modifier");
        }
        u32 value = 0;
        if (!load_vec4(source, value)) return false;
        value = m_builder.emit_bitcast(m_type_uvec4, value);
        const u32 selected = m_builder.emit_composite_extract(
            m_type_uint, value, {0u});
        const u32 zero = m_builder.emit_constant_int(m_type_uint, 0u);
        const bool discard_nonzero = (opcode_token & 0x00040000u) != 0u;
        const u32 discard_condition = discard_nonzero
            ? m_builder.emit_inotequal(m_type_bool, selected, zero)
            : m_builder.emit_integer_compare(
                  SpvOp::OpIEqual, m_type_bool, selected, zero);
        const u32 kill_block = m_builder.allocate_id();
        const u32 merge_block = m_builder.allocate_id();
        m_builder.emit_selection_merge(merge_block);
        m_builder.emit_branch_conditional(
            discard_condition, kill_block, merge_block);
        m_builder.emit_label_id(kill_block);
        m_builder.emit_extension("SPV_EXT_demote_to_helper_invocation");
        m_builder.emit_capability(SpvCapability::DemoteToHelperInvocationEXT);
        m_builder.set_section(SpirvBuilder::Section::Functions);
        m_builder.emit(SpvOp::OpDemoteToHelperInvocationEXT);
        m_builder.emit_branch(merge_block);
        m_builder.emit_label_id(merge_block);
        break;
    }

    default:
        return fail("Unsupported opcode: " +
                    std::to_string(static_cast<u32>(opcode)));
    }

    return true;
}

} // namespace vitreo::graphics::dxbc_spirv_detail
