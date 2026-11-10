// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_subroutines.cpp
/// @brief label/call subroutines.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

bool DxbcTranslator::scan_subroutine_labels() {
    m_subroutine_labels.clear();
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    if (!scan.has_tokens(2)) return fail("CALL: truncated shader stream");
    scan.skip_tokens(2);
    while (scan.has_tokens(1)) {
        const usize instruction_start = scan.position();
        const u32 token = scan.read_token();
        const Sm4Opcode opcode = scan.decode_opcode(token);
        if (static_cast<u32>(opcode) == 53u) {
            if (!scan.has_tokens(1)) return fail("CUSTOMDATA truncated");
            const u32 block_length = scan.read_token();
            if (block_length < 2u) return fail("CUSTOMDATA invalid length");
            scan.set_position(instruction_start + block_length);
            continue;
        }
        const u32 length = scan.decode_instruction_length(token);
        if (length == 0u) return fail("CALL: invalid instruction length");
        if (opcode == Sm4Opcode::LABEL) {
            Sm4Operand label;
            if (!scan.decode_operand(label) ||
                (label.type != Sm4OperandType::Label &&
                 label.type != Sm4OperandType::FunctionBody) ||
                label.index0 >= kFunctionBodyKey) {
                return fail("LABEL: invalid operand");
            }
            // `label fb#` opens a function body (22.3.49), `label l#` a
            // subroutine: two numberings, one map.
            const u32 key = label.type == Sm4OperandType::FunctionBody
                ? (label.index0 | kFunctionBodyKey)
                : label.index0;
            if (m_subroutine_labels.contains(key)) {
                return fail("LABEL: duplicate label");
            }
            m_subroutine_labels[key] = instruction_start + length;
        }
        scan.set_position(instruction_start + length);
    }
    return true;
}

bool DxbcTranslator::translate_subroutine(u32 label) {
    const auto found = m_subroutine_labels.find(label);
    if (found == m_subroutine_labels.end()) return fail("CALL: unknown label");
    if (m_call_stack.size() >= 32u) {
        return fail("CALL: subroutine depth exceeds 32");
    }
    if (std::find(m_call_stack.begin(), m_call_stack.end(), label) !=
        m_call_stack.end()) {
        return fail("CALL: recursion is not permitted");
    }
    // Does a `ret`/`retc` sit inside a loop or switch of this body? Then a
    // return breaks out of several constructs and needs a flag.
    bool needs_flag = false;
    {
        DxbcDecoder scan(m_container.bytecode.data(),
                         m_container.bytecode.size());
        scan.set_position(found->second);
        std::vector<Sm4Opcode> open;
        u32 breakable = 0;
        while (scan.has_tokens(1)) {
            const usize start = scan.position();
            const u32 token = scan.read_token();
            const Sm4Opcode opcode = scan.decode_opcode(token);
            if (static_cast<u32>(opcode) == 53u) {
                if (!scan.has_tokens(1)) break;
                const u32 block_length = scan.read_token();
                if (block_length < 2u) break;
                scan.set_position(start + block_length);
                continue;
            }
            const u32 length = scan.decode_instruction_length(token);
            if (length == 0u || opcode == Sm4Opcode::LABEL) break;
            if (opcode == Sm4Opcode::IF || opcode == Sm4Opcode::LOOP ||
                opcode == Sm4Opcode::SWITCH) {
                open.push_back(opcode);
                if (opcode != Sm4Opcode::IF) ++breakable;
            } else if ((opcode == Sm4Opcode::ENDIF ||
                        opcode == Sm4Opcode::ENDLOOP ||
                        opcode == Sm4Opcode::ENDSWITCH) && !open.empty()) {
                if (open.back() != Sm4Opcode::IF) --breakable;
                open.pop_back();
            } else if (opcode == Sm4Opcode::RET && open.empty()) {
                break;
            } else if ((opcode == Sm4Opcode::RET ||
                        opcode == Sm4Opcode::RETC) && breakable != 0u) {
                needs_flag = true;
            }
            scan.set_position(start + length);
        }
    }

    // The body nests in the caller's construct as a loop that runs once:
    //   br header; header: LoopMerge merge continue; br body
    //   body: ... (every return: br merge); continue: br header; merge:
    // so a return from anywhere in it is a structured break.
    InlineFrame frame{};
    frame.merge_block = m_builder.allocate_id();
    frame.breakable_base = m_breakable_stack.size();
    frame.loop_base = m_loop_stack.size();
    if (needs_flag) {
        frame.returned_var = m_builder.emit_variable(
            m_builder.emit_type_pointer(SpvStorageClass::Private,
                                        m_type_uint),
            SpvStorageClass::Private);
        // A Private per-invocation variable the entry interface lists,
        // like the loop guards.
        m_loop_guard_vars.push_back(frame.returned_var);
        m_builder.emit_store(frame.returned_var,
                             m_builder.emit_constant_int(m_type_uint, 0u));
    }
    const u32 header_block = m_builder.allocate_id();
    const u32 body_block = m_builder.allocate_id();
    const u32 continue_block = m_builder.allocate_id();
    m_builder.emit_branch(header_block);
    m_builder.emit_label_id(header_block);
    m_builder.emit_loop_merge(frame.merge_block, continue_block);
    m_builder.emit_branch(body_block);
    m_builder.emit_label_id(body_block);
    m_block_terminated = false;
    m_inline_stack.push_back(frame);

    DxbcDecoder caller = m_decoder;
    m_decoder.set_position(found->second);
    m_call_stack.push_back(label);
    // The body's own flow constructs nest on top of the caller's.
    const usize if_depth = m_if_stack.size();
    const usize loop_depth = m_loop_stack.size();
    const usize switch_depth = m_switch_stack.size();
    bool ok = false;
    while (m_decoder.has_tokens(1)) {
        const usize instruction_start = m_decoder.position();
        const u32 opcode_token = m_decoder.read_token();
        const Sm4Opcode opcode = m_decoder.decode_opcode(opcode_token);
        if (static_cast<u32>(opcode) == 53u) {
            // CUSTOMDATA: its length is the next DWORD.
            if (!m_decoder.has_tokens(1)) {
                (void)fail("CUSTOMDATA truncated");
                break;
            }
            const u32 block_length = m_decoder.read_token();
            if (block_length < 2u) {
                (void)fail("CUSTOMDATA invalid length");
                break;
            }
            m_decoder.set_position(instruction_start + block_length);
            continue;
        }
        const u32 length = m_decoder.decode_instruction_length(opcode_token);
        const usize operands_begin = m_decoder.position();
        if (opcode == Sm4Opcode::RET) {
            if (m_if_stack.size() != if_depth ||
                m_loop_stack.size() != loop_depth ||
                m_switch_stack.size() != switch_depth) {
                // D3D11.3 22.7.15: a ret nested in the body's flow returns
                // to the caller -- out of every construct of the body.
                if (!emit_inline_return()) break;
                // A case label next: the ret ended that case's body, as a
                // break does (no fallthrough). Otherwise what follows goes
                // to a block no path reaches.
                const Sm4Opcode next =
                    m_decoder.has_tokens(1)
                        ? m_decoder.decode_opcode(m_decoder.peek_token())
                        : Sm4Opcode::RET;
                if (next == Sm4Opcode::CASE || next == Sm4Opcode::DEFAULT) {
                    m_block_terminated = true;
                } else {
                    m_builder.emit_label_id(m_builder.allocate_id());
                    m_block_terminated = false;
                }
                continue;
            }
            ok = true;
            break;
        }
        if (opcode == Sm4Opcode::LABEL) {
            (void)fail("CALL: subroutine is missing RET before the next LABEL");
            break;
        }
        u32 sample_controls = 0;
        u32 resource_dimension = 0;
        u32 extension = opcode_token;
        while ((extension & 0x80000000u) != 0u) {
            if (!m_decoder.has_tokens(1)) break;
            extension = m_decoder.read_token();
            if ((extension & 0x3Fu) == 1u) sample_controls = extension;
            if ((extension & 0x3Fu) == 2u) {
                resource_dimension = (extension >> 6) & 0x1Fu;
            }
        }
        if (!translate_uniformly_indexed(opcode_token, sample_controls,
                                         resource_dimension,
                                         operands_begin - 1u + length)) {
            break;
        }
        const usize consumed = 1 + (m_decoder.position() - operands_begin);
        if (consumed < length) m_decoder.skip_tokens(length - consumed);
    }
    m_call_stack.pop_back();
    m_inline_stack.pop_back();
    m_decoder = caller;
    if (!ok && m_result.failure.empty()) {
        return fail("CALL: subroutine is missing RET");
    }
    if (!ok) return false;
    // The body's final `ret` leaves the construct; then its continue target
    // (no path reaches it) and the merge block the caller resumes in.
    if (!m_block_terminated) m_builder.emit_branch(frame.merge_block);
    m_builder.emit_label_id(continue_block);
    m_builder.emit_branch(header_block);
    m_builder.emit_label_id(frame.merge_block);
    m_block_terminated = false;
    return true;
}

bool DxbcTranslator::emit_inline_return() {
    if (m_inline_stack.empty()) {
        return fail("RET: no inlined subroutine to return from");
    }
    const InlineFrame& frame = m_inline_stack.back();
    if (m_breakable_stack.size() > frame.breakable_base) {
        // Inside a loop or switch of the body: break it, flagged so each
        // enclosing construct of the body is left after its end as well
        // (emit_inline_return_propagation).
        if (frame.returned_var == 0u) {
            return fail("RET: nested return without a return flag");
        }
        m_builder.emit_store(frame.returned_var,
                             m_builder.emit_constant_int(m_type_uint, 1u));
        m_builder.emit_branch(m_breakable_stack.back().merge_block);
    } else {
        // Only selections between here and the body: a break of the
        // single-iteration loop construct.
        m_builder.emit_branch(frame.merge_block);
    }
    return true;
}

void DxbcTranslator::emit_inline_return_propagation() {
    if (m_inline_stack.empty()) return;
    const InlineFrame& frame = m_inline_stack.back();
    // Only after a construct of the body itself (already popped here).
    if (frame.returned_var == 0u ||
        m_breakable_stack.size() < frame.breakable_base) {
        return;
    }
    const u32 returned = m_builder.emit_inotequal(
        m_type_bool, m_builder.emit_load(m_type_uint, frame.returned_var),
        m_builder.emit_constant_int(m_type_uint, 0u));
    const u32 leave_block = m_builder.allocate_id();
    const u32 stay_block = m_builder.allocate_id();
    m_builder.emit_selection_merge(stay_block);
    m_builder.emit_branch_conditional(returned, leave_block, stay_block);
    m_builder.emit_label_id(leave_block);
    m_builder.emit_branch(m_breakable_stack.size() > frame.breakable_base
                              ? m_breakable_stack.back().merge_block
                              : frame.merge_block);
    m_builder.emit_label_id(stay_block);
}

// D3D11.3 22.7.10-17: `label l#` starts a subroutine that runs to its
// `ret`; `call`/`callc` may appear anywhere, including inside flow control,
// and a `ret`/`retc` inside the body -- nested or not -- returns to the
// caller. Recursion is not permitted. Each body becomes a SPIR-V function
// with no parameters: the r#/x# registers it shares with main move to
// Private storage, every other register file is already module scope.
bool DxbcTranslator::emit_subroutine_functions() {
    if (!scan_subroutine_labels()) return false;
    if (m_subroutine_labels.empty()) return true;

    // Call graph, for the recursion check (SPIR-V forbids it as well).
    std::map<u32, std::set<u32>> callees;
    for (const auto& [label, body] : m_subroutine_labels) {
        DxbcDecoder scan(m_container.bytecode.data(),
                         m_container.bytecode.size());
        scan.set_position(body);
        u32 depth = 0;
        while (scan.has_tokens(1)) {
            const usize start = scan.position();
            const u32 token = scan.read_token();
            const Sm4Opcode opcode = scan.decode_opcode(token);
            const u32 length = scan.decode_instruction_length(token);
            if (static_cast<u32>(opcode) == 53u || length == 0u) {
                return fail("CALL: unexpected token in a subroutine body");
            }
            if (opcode == Sm4Opcode::LABEL) break;
            if (opcode == Sm4Opcode::IF || opcode == Sm4Opcode::LOOP ||
                opcode == Sm4Opcode::SWITCH) {
                ++depth;
            } else if ((opcode == Sm4Opcode::ENDIF ||
                        opcode == Sm4Opcode::ENDLOOP ||
                        opcode == Sm4Opcode::ENDSWITCH) && depth > 0u) {
                --depth;
            } else if (opcode == Sm4Opcode::RET && depth == 0u) {
                break;
            } else if (opcode == Sm4Opcode::CALL ||
                       opcode == Sm4Opcode::CALLC) {
                Sm4Operand operand;
                if (opcode == Sm4Opcode::CALLC &&
                    !scan.decode_operand(operand)) {
                    return fail("CALLC: bad condition operand");
                }
                if (!scan.decode_operand(operand) ||
                    operand.type != Sm4OperandType::Label) {
                    return fail("CALL: expected a label operand");
                }
                callees[label].insert(operand.index0);
            } else if (opcode == Sm4Opcode::INTERFACE_CALL) {
                // Every body the call can reach under this binding.
                const u32 call_site = scan.read_token();
                Sm4Operand operand;
                if (!scan.decode_operand(operand) ||
                    operand.type != Sm4OperandType::Interface) {
                    return fail("FCALL: expected an interface operand");
                }
                // The slot a literal index names, or every slot of the
                // declaration a register index moves in.
                u32 first = operand.index0 + operand.index1;
                u32 last = first + 1u;
                if (operand.index1_relative) {
                    for (const auto& declaration : m_interfaces) {
                        if (operand.index0 >= declaration.base &&
                            operand.index0 <
                                declaration.base + declaration.array_size) {
                            first = declaration.base;
                            last = declaration.base + declaration.array_size;
                        }
                    }
                }
                for (u32 slot = first;
                     slot < last && slot < m_slot_bindings.size(); ++slot) {
                    const auto table =
                        m_function_tables.find(m_slot_bindings[slot].table);
                    if (table != m_function_tables.end() &&
                        call_site < table->second.size()) {
                        callees[label].insert(table->second[call_site] |
                                              kFunctionBodyKey);
                    }
                }
            }
            scan.set_position(start + length);
        }
    }
    std::map<u32, u32> state;  // 1 visiting, 2 done
    std::function<bool(u32)> acyclic = [&](u32 label) {
        u32& mark = state[label];
        if (mark == 1u) return false;
        if (mark == 2u) return true;
        mark = 1u;
        for (const u32 callee : callees[label]) {
            if (!acyclic(callee)) return false;
        }
        state[label] = 2u;
        return true;
    };
    for (const auto& [label, _] : m_subroutine_labels) {
        if (!acyclic(label)) return fail("CALL: recursion is not permitted");
    }

    m_temps_private = true;
    if (!declare_function_temps(2)) return false;
    for (const auto& [label, _] : m_subroutine_labels) {
        m_subroutine_functions[label] = m_builder.allocate_id();
    }
    // main is translated afterwards from the decoder's untouched position.
    const DxbcDecoder main_decoder = m_decoder;
    for (const auto& [label, body] : m_subroutine_labels) {
        const u32 function = m_subroutine_functions.at(label);
        const bool function_body = (label & kFunctionBodyKey) != 0u;
        m_builder.emit_function_id(function, m_type_void, m_func_type);
        m_builder.emit_name(function,
                            (function_body ? "fb" : "l") +
                                std::to_string(label & ~kFunctionBodyKey));
        (void)m_builder.emit_label();
        m_block_terminated = false;
        m_in_subroutine = true;
        m_current_body =
            function_body ? (label & ~kFunctionBodyKey) : kOutsideBodies;
        m_decoder.set_position(body);
        const bool translated =
            translate_instructions(m_container.bytecode.size() / 4u);
        m_in_subroutine = false;
        m_current_body = kOutsideBodies;
        if (!translated) return false;
        if (!m_stopped_at_ret) {
            return fail("CALL: subroutine is missing RET");
        }
        m_builder.emit_return();
        m_builder.emit_function_end();
    }
    m_decoder = main_decoder;
    return true;
}

// `call l#` outside a hull shader: the body is its own function.
bool DxbcTranslator::emit_subroutine_call(u32 label) {
    const auto function = m_subroutine_functions.find(label);
    if (function == m_subroutine_functions.end()) {
        return fail("CALL: unknown label");
    }
    (void)m_builder.emit_function_call(m_type_void, function->second);
    return true;
}

} // namespace vitreo::graphics::dxbc_spirv_detail
