// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_instructions.cpp
/// @brief The instruction loop and the opcode dispatch.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

// Translate instructions from the decoder's position until the program's
// top-level `ret` or @p end_token (a DWORD index into the bytecode). A hull
// shader translates each phase this way, into its own function.
bool DxbcTranslator::translate_instructions(usize end_token) {
    m_stopped_at_ret = false;
    while (m_decoder.has_tokens(1) && m_decoder.position() < end_token) {
        u32 opcode_token = m_decoder.read_token();
        Sm4Opcode opcode = m_decoder.decode_opcode(opcode_token);
        const u32 instruction_length =
            m_decoder.decode_instruction_length(opcode_token);
        const usize tokens_before = m_decoder.position();

        if (opcode == Sm4Opcode::LABEL) {
            return fail(m_in_subroutine
                            ? "CALL: subroutine is missing RET before the "
                              "next LABEL"
                            : "LABEL: reached before the program's RET");
        }

        if (opcode == Sm4Opcode::RET) {
            // The program's final `ret` ends translation; the function's
            // OpReturn is emitted once, after the loop.
            if (m_if_stack.empty() && m_loop_stack.empty() &&
                m_switch_stack.empty()) {
                m_stopped_at_ret = true;
                break;
            }
            if (m_in_subroutine) {
                // A `ret` nested in a subroutine's flow control returns to
                // the caller (22.7.15's own example); main's epilogue is
                // not this function's to run.
                m_builder.emit_return();
                m_builder.emit_label_id(m_builder.allocate_id());
                continue;
            }
            // A `ret` inside structured control flow is an early-out, and
            // instructions follow it — stopping here abandoned the rest of the
            // program and left the if/loop stacks unbalanced ("IF: missing
            // ENDIF"). Terminate this block with OpReturn and open a fresh
            // block so the enclosing ENDIF/ENDLOOP still has somewhere to
            // branch from; that block is unreachable, which SPIR-V allows.
            m_emitted_early_return = true;
            if (!emit_system_value_epilogue()) return false;
            m_builder.emit_return();
            m_builder.emit_label_id(m_builder.allocate_id());
            continue;
        }

        // CUSTOMDATA (53): length is the next DWORD (full block size), not
        // OpcodeToken0[30:24] — Microsoft D3D10 Tokenized Program Format.
        if (static_cast<u32>(opcode) == 53u) {
            if (!m_decoder.has_tokens(1)) {
                return fail("CUSTOMDATA truncated");
            }
            const u32 block_length = m_decoder.read_token();
            if (block_length < 2) {
                return fail("CUSTOMDATA invalid length");
            }
            m_decoder.skip_tokens(block_length - 2);
            continue;
        }

        // Skip declaration opcodes; signatures drive I/O variables.
        const u32 opc = static_cast<u32>(opcode);
        // 161 is dcl_resource_raw, 162 dcl_resource_structured: both are
        // collected by their own prescan before the body is translated.
        // 158 is dcl_uav_structured, 160 dcl_tgsm_structured: like 159/161/
        // 162 they are collected by their own prescan.
        // 143 dcl_stream and 206 dcl_gsinstances are collected by
        // scan_geometry_declarations.
        // 113-116 (hull phases) and 147-154 (tessellation declarations) are
        // collected by scan_tessellation_declarations.
        // 144-146 (dcl_function_body / _table / dcl_interface) are
        // collected by scan_class_linkage.
        if ((opc >= 88 && opc <= 106) || opc == 155u || opc == 156u ||
            opc == 157u || opc == 158u || opc == 159u || opc == 160u ||
            opc == 161u || opc == 162u || opc == 143u || opc == 206u ||
            (opc >= 113u && opc <= 116u) || (opc >= 144u && opc <= 154u)) {
            if (instruction_length > 1) {
                m_decoder.skip_tokens(instruction_length - 1);
            }
            continue;
        }

        // Opcode extensions precede operands. Resource-dimension and return-
        // type extensions merely repeat RDEF data; sample-control extensions
        // carry integer coordinate offsets and require explicit translation.
        u32 sample_controls = 0;
        u32 resource_dimension = 0;
        u32 extension = opcode_token;
        while ((extension & 0x80000000u) != 0) {
            if (!m_decoder.has_tokens(1)) {
                return fail("truncated extended opcode");
            }
            extension = m_decoder.read_token();
            if ((extension & 0x3Fu) == 1u) {
                sample_controls = extension;
            } else if ((extension & 0x3Fu) == 2u) {
                resource_dimension = (extension >> 6) & 0x1Fu;
            }
        }

        if (!translate_uniformly_indexed(
                opcode_token, sample_controls, resource_dimension,
                tokens_before - 1u + instruction_length)) {
            return false;
        }

        // Consume any unread tokens for this instruction (length includes
        // the opcode token already read).
        if (instruction_length > 1) {
            const usize consumed =
                1 + (m_decoder.position() - tokens_before);
            if (consumed < instruction_length) {
                m_decoder.skip_tokens(instruction_length - consumed);
            }
        }
    }

    if (!m_if_stack.empty()) {
        return fail("IF: missing ENDIF");
    }
    if (!m_loop_stack.empty()) {
        return fail("LOOP: missing ENDLOOP");
    }
    if (!m_switch_stack.empty()) {
        return fail("SWITCH: missing ENDSWITCH");
    }

    return true;
}

bool DxbcTranslator::begin_pending_switch_body(Sm4Opcode opcode) {
    if (m_switch_stack.empty() || opcode == Sm4Opcode::CASE ||
        opcode == Sm4Opcode::DEFAULT || opcode == Sm4Opcode::ENDSWITCH) {
        return true;
    }
    SwitchFrame& frame = m_switch_stack.back();
    if (frame.pending_cases.empty() && !frame.pending_default) return true;
    const u32 body = m_builder.allocate_id();
    for (u32 literal : frame.pending_cases) {
        frame.targets.emplace_back(literal, body);
    }
    if (frame.pending_default) frame.default_block = body;
    frame.pending_cases.clear();
    frame.pending_default = false;
    m_builder.emit_label_id(body);
    frame.active_body = true;
    m_block_terminated = false;
    return true;
}

bool DxbcTranslator::translate_instruction(u32 opcode_token,
                                           u32 sample_controls) {
    Sm4Opcode opcode = m_decoder.decode_opcode(opcode_token);

    // Learn: OpcodeToken0 bit 13 is the SATURATE instruction modifier. It is
    // reserved-zero on integer instructions, so recording it unconditionally
    // only ever affects float destinations.
    m_saturate_destination = (opcode_token & 0x00002000u) != 0u;
    // OpcodeToken0 bits 22:19: the precise mask (D3D11_SB_OPCODE_PRECISE).
    // fxc emits it per component; SPIR-V decorates the whole result, which
    // is the stricter reading and the one the invariance contract needs.
    m_builder.set_no_contraction(((opcode_token >> 19) & 0xFu) != 0u);

    if (!begin_pending_switch_body(opcode)) return false;

    if (m_block_terminated && opcode != Sm4Opcode::ELSE &&
        opcode != Sm4Opcode::ENDIF && opcode != Sm4Opcode::ENDLOOP &&
        opcode != Sm4Opcode::CASE && opcode != Sm4Opcode::DEFAULT &&
        opcode != Sm4Opcode::ENDSWITCH) {
        return fail("control flow: instruction follows terminated block");
    }

    // Shader model 5.1 descriptor arrays: every per-resource table maps a
    // range ID to its variable, and each handler loads the descriptor from
    // it. For this instruction, the entry of each array operand points at
    // the element the operand names instead -- the same pointer type a
    // single register's variable has -- and is restored afterwards. No
    // instruction names two elements of one t#/s#/u# range.
    struct ElementBinding {
        u32* entry;
        u32 original;
    };
    std::vector<ElementBinding> element_bindings;
    const std::vector<Sm4Operand> array_operands =
        std::move(m_pending_array_operands);
    m_pending_array_operands.clear();
    std::vector<u32> sampler_records;
    for (const Sm4Operand& operand : array_operands) {
        const DxbcRegisterClass cls = register_class_of(operand.type);
        const ResourceRange* range = find_range(cls, operand.index0);
        const u32 element = array_element_pointer(operand, *range);
        if (element == 0) return false;
        if (cls == DxbcRegisterClass::Sampler && m_sampler_state_var != 0) {
            // Its sampler_state_buffer record: binding + element.
            const u32 index = array_element_index(operand, *range);
            if (index == 0) return false;
            m_sampler_record_index[operand.index0] = m_builder.emit_iadd(
                m_type_uint,
                m_builder.emit_constant_int(m_type_uint,
                                            range->resolved.binding),
                index);
            sampler_records.push_back(operand.index0);
        }
        const auto bind = [&](std::map<u32, u32>& table, u32 value) {
            const auto found = table.find(operand.index0);
            if (found == table.end()) return;
            element_bindings.push_back({&found->second, found->second});
            found->second = value;
        };
        if (cls == DxbcRegisterClass::ShaderResource) {
            bind(m_texture_vars, element);
            bind(m_typed_buffer_srv_vars, element);
            bind(m_structured_srv_vars, element);
            bind(m_raw_srv_vars, element);
        } else if (cls == DxbcRegisterClass::Sampler) {
            bind(m_sampler_vars, element);
        } else {
            bind(m_uav_vars, element);
            bind(m_structured_uav_vars, element);
            bind(m_raw_uav_vars, element);
            // Its counter, when the program moves one: the same element of
            // the counter array.
            const auto counter_type =
                m_uav_counter_element_types.find(operand.index0);
            if (counter_type != m_uav_counter_element_types.end()) {
                const u32 index = array_element_index(operand, *range);
                if (index == 0) return false;
                bind(m_uav_counter_vars,
                     m_builder.emit_access_chain(
                         counter_type->second,
                         m_uav_counter_vars.at(operand.index0), {index}));
            }
        }
    }
    const bool translated = translate_gated_operation(
        opcode_token, opcode, sample_controls);
    for (auto it = element_bindings.rbegin(); it != element_bindings.rend();
         ++it) {
        *it->entry = it->original;
    }
    for (const u32 key : sampler_records) m_sampler_record_index.erase(key);
    return translated;
}

bool DxbcTranslator::translate_gated_operation(u32 opcode_token,
                                               Sm4Opcode opcode,
                                               u32 sample_controls) {
    // A u# store or a result-less atomic runs in the pixel's own invocation
    // only (m_uav_leader_var). The imm_atomic_* forms return a value every
    // invocation needs, so their handlers gate the operand instead.
    const u32 op = static_cast<u32>(opcode);
    const bool gated = m_uav_leader_var != 0 &&
        (opcode == Sm4Opcode::STORE_UAV_TYPED ||
         opcode == Sm4Opcode::STORE_RAW ||
         opcode == Sm4Opcode::STORE_STRUCTURED ||
         (op >= static_cast<u32>(Sm4Opcode::ATOMIC_AND) &&
          op <= static_cast<u32>(Sm4Opcode::ATOMIC_UMIN)));
    if (!gated) return translate_operation(opcode_token, opcode, sample_controls);
    const u32 run_block = m_builder.allocate_id();
    const u32 merge_block = m_builder.allocate_id();
    // Loaded before the merge: OpSelectionMerge must directly precede its
    // OpBranchConditional.
    const u32 leader = load_uav_leader();
    m_builder.emit_selection_merge(merge_block);
    m_builder.emit_branch_conditional(leader, run_block, merge_block);
    m_builder.emit_label_id(run_block);
    if (!translate_operation(opcode_token, opcode, sample_controls)) {
        return false;
    }
    m_builder.emit_branch(merge_block);
    m_builder.emit_label_id(merge_block);
    return true;
}

bool DxbcTranslator::translate_operation(u32 opcode_token, Sm4Opcode opcode,
                                         u32 sample_controls) {
    switch (opcode) {
    case Sm4Opcode::EMIT:
    case Sm4Opcode::CUT:
    case Sm4Opcode::EMITTHENCUT:
    case Sm4Opcode::EMIT_STREAM:
    case Sm4Opcode::CUT_STREAM:
    case Sm4Opcode::EMITTHENCUT_STREAM:
    case Sm4Opcode::INTERFACE_CALL:
    case Sm4Opcode::CALL:
    case Sm4Opcode::CALLC:
    case Sm4Opcode::SWITCH:
    case Sm4Opcode::CASE:
    case Sm4Opcode::DEFAULT:
    case Sm4Opcode::ENDSWITCH:
    case Sm4Opcode::LOOP:
    case Sm4Opcode::ENDLOOP:
    case Sm4Opcode::BREAK:
    case Sm4Opcode::CONTINUE:
    case Sm4Opcode::BREAKC:
    case Sm4Opcode::CONTINUEC:
    case Sm4Opcode::IF:
    case Sm4Opcode::ELSE:
    case Sm4Opcode::ENDIF:
    case Sm4Opcode::RETC:
    case Sm4Opcode::DISCARD:
        return translate_flow_control_operation(opcode_token, opcode, sample_controls);

    case Sm4Opcode::MOV:
    case Sm4Opcode::ADD:
    case Sm4Opcode::MUL:
    case Sm4Opcode::DIV:
    case Sm4Opcode::RCP:
    case Sm4Opcode::RSQ:
    case Sm4Opcode::SQRT:
    case Sm4Opcode::ROUND_NE:
    case Sm4Opcode::ROUND_NI:
    case Sm4Opcode::ROUND_PI:
    case Sm4Opcode::ROUND_Z:
    case Sm4Opcode::EXP:
    case Sm4Opcode::LOG:
    case Sm4Opcode::DERIV_RTX:
    case Sm4Opcode::DERIV_RTX_COARSE:
    case Sm4Opcode::DERIV_RTX_FINE:
    case Sm4Opcode::DERIV_RTY:
    case Sm4Opcode::DERIV_RTY_COARSE:
    case Sm4Opcode::DERIV_RTY_FINE:
    case Sm4Opcode::SINCOS:
    case Sm4Opcode::DP2:
    case Sm4Opcode::DP3:
    case Sm4Opcode::DP4:
    case Sm4Opcode::MAD:
    case Sm4Opcode::MAX:
    case Sm4Opcode::MIN:
    case Sm4Opcode::FRC:
        return translate_float_operation(opcode_token, opcode, sample_controls);

    case Sm4Opcode::IADD:
    case Sm4Opcode::INEG:
    case Sm4Opcode::UDIV:
    case Sm4Opcode::UADDC:
    case Sm4Opcode::USUBB:
    case Sm4Opcode::IMAD:
    case Sm4Opcode::IMUL:
    case Sm4Opcode::UMAD:
    case Sm4Opcode::MSAD:
    case Sm4Opcode::IMIN:
    case Sm4Opcode::IMAX:
    case Sm4Opcode::UMUL:
    case Sm4Opcode::UMIN:
    case Sm4Opcode::UMAX:
        return translate_integer_operation(opcode_token, opcode, sample_controls);

    case Sm4Opcode::USHR:
    case Sm4Opcode::ISHR:
    case Sm4Opcode::ISHL:
    case Sm4Opcode::COUNTBITS:
    case Sm4Opcode::FIRSTBIT_HI:
    case Sm4Opcode::FIRSTBIT_LO:
    case Sm4Opcode::FIRSTBIT_SHI:
    case Sm4Opcode::UBFE:
    case Sm4Opcode::IBFE:
    case Sm4Opcode::BFI:
    case Sm4Opcode::BFREV:
    case Sm4Opcode::AND:
    case Sm4Opcode::OR:
    case Sm4Opcode::XOR:
    case Sm4Opcode::NOT:
        return translate_bitwise_operation(opcode_token, opcode, sample_controls);

    case Sm4Opcode::F32TOF16:
    case Sm4Opcode::F16TOF32:
    case Sm4Opcode::NE:
    case Sm4Opcode::IEQ:
    case Sm4Opcode::INE:
    case Sm4Opcode::ILT:
    case Sm4Opcode::IGE:
    case Sm4Opcode::ULT:
    case Sm4Opcode::UGE:
    case Sm4Opcode::EQ:
    case Sm4Opcode::LT:
    case Sm4Opcode::GE:
    case Sm4Opcode::FTOI:
    case Sm4Opcode::FTOU:
    case Sm4Opcode::ITOF:
    case Sm4Opcode::UTOF:
    case Sm4Opcode::MOVC:
    case Sm4Opcode::SWAPC:
        return translate_compare_convert_operation(opcode_token, opcode, sample_controls);

    case Sm4Opcode::RESINFO:
    case Sm4Opcode::BUFINFO:
    case Sm4Opcode::SAMPLEINFO:
    case Sm4Opcode::SAMPLEPOS:
    case Sm4Opcode::LOD:
    case Sm4Opcode::EVAL_SNAPPED:
    case Sm4Opcode::EVAL_SAMPLE_INDEX:
    case Sm4Opcode::EVAL_CENTROID:
        return translate_resource_query_operation(opcode_token, opcode, sample_controls);

    case Sm4Opcode::LD:
    case Sm4Opcode::LD_MS:
    case Sm4Opcode::LD_UAV_TYPED:
    case Sm4Opcode::STORE_UAV_TYPED:
        return translate_typed_resource_operation(opcode_token, opcode, sample_controls);

    case Sm4Opcode::STORE_RAW:
    case Sm4Opcode::LD_RAW:
    case Sm4Opcode::SYNC:
    case Sm4Opcode::LD_STRUCTURED:
    case Sm4Opcode::STORE_STRUCTURED:
        return translate_buffer_operation(opcode_token, opcode, sample_controls);

    case Sm4Opcode::IMM_ATOMIC_ALLOC:
    case Sm4Opcode::IMM_ATOMIC_CONSUME:
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
    case Sm4Opcode::IMM_ATOMIC_IMIN:
        return translate_atomic_operation(opcode_token, opcode, sample_controls);

    case Sm4Opcode::SAMPLE:
    case Sm4Opcode::SAMPLE_C:
    case Sm4Opcode::SAMPLE_C_LZ:
    case Sm4Opcode::SAMPLE_L:
    case Sm4Opcode::SAMPLE_B:
    case Sm4Opcode::SAMPLE_D:
    case Sm4Opcode::GATHER4:
    case Sm4Opcode::GATHER4_C:
    case Sm4Opcode::GATHER4_PO:
    case Sm4Opcode::GATHER4_PO_C:
        return translate_sample_operation(opcode_token, opcode, sample_controls);

    case Sm4Opcode::NOP:
        break;

    case Sm4Opcode::RET:
        break;

    case Sm4Opcode::DADD:
    case Sm4Opcode::DMAX:
    case Sm4Opcode::DMIN:
    case Sm4Opcode::DMUL:
    case Sm4Opcode::DEQ:
    case Sm4Opcode::DGE:
    case Sm4Opcode::DLT:
    case Sm4Opcode::DNE:
    case Sm4Opcode::DMOV:
    case Sm4Opcode::DMOVC:
    case Sm4Opcode::DTOF:
    case Sm4Opcode::FTOD:
    case Sm4Opcode::DDIV:
    case Sm4Opcode::DFMA:
    case Sm4Opcode::DRCP:
    case Sm4Opcode::DTOI:
    case Sm4Opcode::DTOU:
    case Sm4Opcode::ITOD:
    case Sm4Opcode::UTOD:
        if (!translate_double(opcode)) return false;
        break;

    default:
        return fail("Unsupported opcode: " +
                    std::to_string(static_cast<u32>(opcode)));
    }

    return true;
}

} // namespace vitreo::graphics::dxbc_spirv_detail
