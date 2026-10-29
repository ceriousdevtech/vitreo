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
    DxbcDecoder caller = m_decoder;
    m_decoder.set_position(found->second);
    m_call_stack.push_back(label);
    bool ok = false;
    while (m_decoder.has_tokens(1)) {
        const u32 opcode_token = m_decoder.read_token();
        const Sm4Opcode opcode = m_decoder.decode_opcode(opcode_token);
        const u32 length = m_decoder.decode_instruction_length(opcode_token);
        const usize operands_begin = m_decoder.position();
        if (opcode == Sm4Opcode::RET) {
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
    m_decoder = caller;
    if (!ok && m_result.failure.empty()) {
        return fail("CALL: subroutine is missing RET");
    }
    return ok;
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
    declare_function_temps(2);
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
