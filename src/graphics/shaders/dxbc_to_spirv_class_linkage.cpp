// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_class_linkage.cpp
/// @brief Dynamic shader linkage (D3D11.3 7.19).

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

// -----------------------------------------------------------------------------
// Dynamic shader linkage (D3D11.3 7.19)
// -----------------------------------------------------------------------------
//
// A program that declares interfaces is translated for one binding at a
// time: the runtime specialises it when *SetShader names the class
// instances (7.19.6, "Selecting the set of functions to use between draw
// calls allows the driver to recalculate the hardware requirements"), so
// every interface slot's function table and this[] data are constants here
// (DxbcContainer::slot_bindings). Without a binding -- the program being
// validated when it is created -- every slot is unbound: an fcall calls
// nothing, and a uniformly indexed register tries every slot declared.

// dcl_function_body fb#, dcl_function_table ft# = {fb#...} and dcl_interface
// fp#[arraySize][callSites] = {ft#...} (22.3.49-51), and the t#/s#/cb#
// operands each function body indexes by register. Token layouts are the
// tokenized-program format's (DirectX-Headers D3D12TokenizedProgramFormat.hpp)
// and match fxc 10.1's listing: dcl_function_table {id, count, fb#...};
// dcl_interface {id, call sites, table count | array size << 16, ft#...}
// (OpcodeToken0 bit 11: dcl_interface_dynamicindexed).
bool DxbcTranslator::scan_class_linkage() {
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    if (!scan.has_tokens(2)) return true;
    scan.skip_tokens(2);
    u32 body = kOutsideBodies;
    while (scan.has_tokens(1)) {
        const usize start = scan.position();
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7FFu;
        if (opcode == 53u) {
            if (!scan.has_tokens(1)) return fail("CUSTOMDATA truncated");
            const u32 block = scan.read_token();
            if (block < 2u) return fail("CUSTOMDATA invalid length");
            scan.set_position(start + block);
            continue;
        }
        const u32 length = scan.decode_instruction_length(token);
        if ((opcode == 120u || (opcode >= 144u && opcode <= 146u)) &&
            (token & 0x80000000u) != 0u) {
            // The format lets these four carry their length in the next
            // DWORD (a table past 127 DWORDs); fxc was not seen to emit it.
            return fail("class linkage: an extended-length declaration is "
                        "not implemented");
        }
        if (length == 0u || !scan.has_tokens(length - 1u)) {
            return fail("class linkage: invalid instruction length");
        }
        const auto word = [&](u32 i) {
            DxbcDecoder at = scan;
            at.set_position(start + i);
            return at.read_token();
        };
        if (opcode == 145u) {
            const u32 count = length >= 3u ? word(2) : 0u;
            if (length < 3u || 3u + count > length) {
                return fail("dcl_function_table: truncated");
            }
            std::vector<u32> bodies;
            for (u32 i = 0; i < count; ++i) bodies.push_back(word(3u + i));
            m_function_tables[word(1)] = std::move(bodies);
        } else if (opcode == 146u) {
            const u32 packed = length >= 4u ? word(3) : 0u;
            if (length < 4u || 4u + (packed & 0xFFFFu) > length) {
                return fail("dcl_interface: truncated");
            }
            InterfaceDeclaration declaration{};
            declaration.base = word(1);
            declaration.call_sites = word(2);
            declaration.array_size = packed >> 16;
            for (u32 i = 0; i < (packed & 0xFFFFu); ++i) {
                declaration.tables.push_back(word(4u + i));
            }
            m_interface_slots = std::max(
                m_interface_slots, declaration.base + declaration.array_size);
            m_interfaces.push_back(std::move(declaration));
        } else if (opcode == 44u) {
            DxbcDecoder at = scan;
            Sm4Operand label;
            body = at.decode_operand(label) &&
                           label.type == Sm4OperandType::FunctionBody
                       ? label.index0
                       : kOutsideBodies;
        } else if (opcode != 120u && !(opcode >= 88u && opcode <= 106u) &&
                   !(opcode >= 143u && opcode <= 162u) && opcode != 206u &&
                   length > 1u) {
            // The instruction's register-indexed t#/s#/cb# operands.
            u32 dimension = 0;
            u32 return_tokens = 0;
            u32 extension = token;
            DxbcDecoder at = scan;
            while ((extension & 0x80000000u) != 0u && at.has_tokens(1)) {
                extension = at.read_token();
                if ((extension & 0x3Fu) == 2u) {
                    dimension = (extension >> 6) & 0x1Fu;
                } else if ((extension & 0x3Fu) == 3u) {
                    return_tokens = (extension >> 6) & 0xFFFFu;
                }
            }
            while (at.position() < start + length) {
                Sm4Operand operand;
                if (!at.decode_operand(operand)) break;
                if (!operand.index0_relative ||
                    (operand.type != Sm4OperandType::Resource &&
                     operand.type != Sm4OperandType::Sampler &&
                     operand.type != Sm4OperandType::ConstantBuffer)) {
                    continue;
                }
                m_uniform_index_uses[body].push_back(
                    {operand.type, operand.index0,
                     operand.type == Sm4OperandType::Resource ? dimension
                                                              : 0u,
                     operand.type == Sm4OperandType::Resource
                         ? return_tokens
                         : 0u});
            }
        }
        scan.set_position(start + length);
    }
    if (m_interface_slots > 253u) {
        // 22.7.20: "There are at most 253 slots for 'this' data."
        return fail("dcl_interface: more than 253 interface slots");
    }
    for (const auto& declaration : m_interfaces) {
        for (const u32 table : declaration.tables) {
            const auto found = m_function_tables.find(table);
            if (found == m_function_tables.end()) {
                return fail("dcl_interface: undeclared function table ft" +
                            std::to_string(table));
            }
            // 22.3.51: the call-site count "must match the number of bodies
            // in each table referenced in the decl".
            if (found->second.size() != declaration.call_sites) {
                return fail("dcl_interface: ft" + std::to_string(table) +
                            " does not have one body per call site");
            }
        }
    }
    m_slot_bindings = m_container.slot_bindings;
    if (m_slot_bindings.empty()) {
        m_slot_bindings.resize(m_interface_slots);
    } else if (m_slot_bindings.size() != m_interface_slots) {
        return fail("class linkage: a binding of " +
                    std::to_string(m_slot_bindings.size()) +
                    " slots for a program with " +
                    std::to_string(m_interface_slots));
    }
    for (u32 slot = 0; slot < m_slot_bindings.size(); ++slot) {
        const u32 table = m_slot_bindings[slot].table;
        if (table == kDxbcUnboundTable) continue;
        const auto declaration = std::find_if(
            m_interfaces.begin(), m_interfaces.end(), [&](const auto& d) {
                return slot >= d.base && slot < d.base + d.array_size;
            });
        if (declaration == m_interfaces.end() ||
            std::find(declaration->tables.begin(), declaration->tables.end(),
                      table) == declaration->tables.end()) {
            return fail("class linkage: ft" + std::to_string(table) +
                        " is not a choice of interface slot " +
                        std::to_string(slot));
        }
    }
    return true;
}

std::vector<u32> DxbcTranslator::slots_reaching(u32 body) const {
    std::vector<u32> slots;
    for (u32 slot = 0; slot < m_slot_bindings.size(); ++slot) {
        const auto table = m_function_tables.find(m_slot_bindings[slot].table);
        if (table == m_function_tables.end()) continue;
        if (body == kOutsideBodies ||
            std::find(table->second.begin(), table->second.end(), body) !=
                table->second.end()) {
            slots.push_back(slot);
        }
    }
    return slots;
}

// The registers a uniformly indexed t#/s#/cb# can name here. A class method
// indexes its instance's by this[i].z (textures), .w (samplers) or .x
// (constant buffers) plus the member's place (22.7.20), so with a binding
// they are those of every slot whose class runs this body; unbound (the
// program validated at creation), every declared register of the kind.
// A t# must also have the dimension the instruction names.
std::vector<u32> DxbcTranslator::uniform_index_candidates(
    Sm4OperandType type, u32 offset, u32 dimension) const {
    std::set<u32> slots;
    const bool bound =
        std::any_of(m_slot_bindings.begin(), m_slot_bindings.end(),
                    [](const auto& b) { return b.table != kDxbcUnboundTable; });
    const u32 component = type == Sm4OperandType::ConstantBuffer ? 0u
        : type == Sm4OperandType::Resource ? 2u : 3u;
    if (bound) {
        for (const u32 slot : slots_reaching(m_current_body)) {
            slots.insert(m_slot_bindings[slot].this_data[component] + offset);
        }
    } else if (type == Sm4OperandType::ConstantBuffer) {
        for (const auto& [slot, _] : m_cbuffer_vars) slots.insert(slot);
    } else if (type == Sm4OperandType::Sampler) {
        for (const auto& [slot, _] : m_sampler_vars) slots.insert(slot);
    } else {
        for (const auto& [slot, _] : m_texture_vars) slots.insert(slot);
        for (const auto& [slot, _] : m_typed_buffer_srv_vars) slots.insert(slot);
        for (const auto& [slot, _] : m_raw_srv_vars) slots.insert(slot);
        for (const auto& [slot, _] : m_structured_srv_vars) slots.insert(slot);
    }
    std::vector<u32> candidates;
    for (const u32 slot : slots) {
        if (type == Sm4OperandType::ConstantBuffer) {
            if (m_cbuffer_vars.contains(slot)) candidates.push_back(slot);
            continue;
        }
        if (type == Sm4OperandType::Sampler) {
            if (m_sampler_vars.contains(slot)) candidates.push_back(slot);
            continue;
        }
        // D3D10_SB_RESOURCE_DIMENSION of the resource-dim extension; 0 when
        // the instruction names none.
        bool matches = false;
        if (m_texture_vars.contains(slot)) {
            const auto dim = m_texture_dimensions.find(slot);
            const bool arrayed = m_texture_arrays.contains(slot);
            const bool ms = m_texture_multisampled.contains(slot);
            const bool one_d = m_texture_1d.contains(slot);
            const SpvDim spv = dim != m_texture_dimensions.end()
                ? dim->second : SpvDim::Dim2D;
            switch (dimension) {
            case 0: matches = true; break;
            case 2: matches = one_d && !arrayed; break;
            case 3: matches = spv == SpvDim::Dim2D && !arrayed && !ms && !one_d; break;
            case 4: matches = ms && !arrayed; break;
            case 5: matches = spv == SpvDim::Dim3D; break;
            case 6: matches = spv == SpvDim::DimCube && !arrayed; break;
            case 7: matches = one_d && arrayed; break;
            case 8: matches = spv == SpvDim::Dim2D && arrayed && !ms && !one_d; break;
            case 9: matches = ms && arrayed; break;
            case 10: matches = spv == SpvDim::DimCube && arrayed; break;
            default: matches = false; break;
            }
        } else if (m_typed_buffer_srv_vars.contains(slot)) {
            matches = dimension == 0u || dimension == 1u;
        } else if (m_raw_srv_vars.contains(slot)) {
            matches = dimension == 0u || dimension == 11u;
        } else if (m_structured_srv_vars.contains(slot)) {
            matches = dimension == 0u || dimension == 12u;
        }
        if (matches) candidates.push_back(slot);
    }
    return candidates;
}

// The registers the bound instances name that the program does not declare
// (CreateClassInstance places an instance's data anywhere): a constant
// buffer of the full 4096 elements, a sampler, or a t# of the dimension and
// return type the method's instruction names. D3D11.3 7.11.2: a constant
// buffer past the API's 14 reads as unbound, so none is declared for it.
bool DxbcTranslator::add_class_linkage_declarations(
    std::map<u32, u32>& cbuffers, std::map<u32, u32>& textures,
    std::map<u32, u32>& return_tokens, std::set<u32>& samplers) {
    const bool bound =
        std::any_of(m_slot_bindings.begin(), m_slot_bindings.end(),
                    [](const auto& b) { return b.table != kDxbcUnboundTable; });
    if (!bound) return true;
    const std::set<u32> raw = scan_raw_srvs();
    const std::map<u32, u32> structured = scan_structured_srvs();
    for (const auto& [body, uses] : m_uniform_index_uses) {
        for (const u32 slot : slots_reaching(body)) {
            const auto& data = m_slot_bindings[slot].this_data;
            for (const auto& use : uses) {
                if (use.type == Sm4OperandType::ConstantBuffer) {
                    const u32 reg = data[0] + use.offset;
                    if (reg < 14u && !cbuffers.contains(reg)) {
                        cbuffers[reg] = 4096u;
                    }
                } else if (use.type == Sm4OperandType::Sampler) {
                    const u32 reg = data[3] + use.offset;
                    if (reg < 16u) samplers.insert(reg);
                } else {
                    const u32 reg = data[2] + use.offset;
                    if (reg >= 128u || textures.contains(reg) ||
                        raw.contains(reg) || structured.contains(reg)) {
                        continue;
                    }
                    if (use.dimension < 1u || use.dimension > 10u ||
                        use.return_tokens == 0u) {
                        return fail("class linkage: an instance's t" +
                                    std::to_string(reg) +
                                    " is undeclared and its instruction does "
                                    "not name a typed resource");
                    }
                    textures[reg] = use.dimension;
                    return_tokens[reg] = use.return_tokens;
                }
            }
        }
    }
    return true;
}

u32 DxbcTranslator::relative_index_value(Sm4OperandType type, u32 reg,
                                         u32 component, u32 offset) {
    (void)type;  // callers accept r# only
    const u32 bits = m_builder.emit_bitcast(
        m_type_uvec4, m_builder.emit_load(m_type_vec4, get_temp_variable(reg)));
    u32 value = m_builder.emit_composite_extract(m_type_uint, bits,
                                                 {component & 3u});
    if (offset != 0u) {
        value = m_builder.emit_iadd(
            m_type_uint, value, m_builder.emit_constant_int(m_type_uint, offset));
    }
    return value;
}

// 22.7.20: this[i] is four UINTs -- constant buffer, vector offset, texture,
// sampler -- set by the class instance bound at interface slot i; a literal
// or r#.c + literal index, anywhere in the program. An index past the
// declared slots reads 0 ("undefined results").
bool DxbcTranslator::load_this_pointer(const Sm4Operand& src, u32& out_id) {
    const auto value_of = [&](u32 slot) {
        std::array<u32, 4> data{};
        if (slot < m_slot_bindings.size()) data = m_slot_bindings[slot].this_data;
        return m_builder.emit_constant_composite(
            m_type_uvec4, {m_builder.emit_constant_int(m_type_uint, data[0]),
                           m_builder.emit_constant_int(m_type_uint, data[1]),
                           m_builder.emit_constant_int(m_type_uint, data[2]),
                           m_builder.emit_constant_int(m_type_uint, data[3])});
    };
    u32 value = 0;
    if (!src.index0_relative) {
        value = value_of(src.index0);
    } else {
        if (src.index0_relative_type != Sm4OperandType::Temp) {
            return fail("this[]: unsupported relative index source");
        }
        const u32 selector = relative_index_value(
            src.index0_relative_type, src.index0_relative_register,
            src.index0_relative_component, src.index0);
        value = value_of(~0u);
        for (u32 slot = 0; slot < m_slot_bindings.size(); ++slot) {
            value = m_builder.emit_select(
                m_type_uvec4,
                m_builder.emit_integer_compare(
                    SpvOp::OpIEqual, m_type_bool, selector,
                    m_builder.emit_constant_int(m_type_uint, slot)),
                value_of(slot), value);
        }
    }
    out_id = m_builder.emit_bitcast(m_type_vec4, value);
    return true;
}

// 22.7.19 fcall fp#[arrayIndex][callSite]: a DWORD call site, then the
// interface operand -- index0 fp#, index1 the array index, literal or
// r#.c + literal (fxc 10.1's tokens). fp# + arrayIndex is the interface
// slot; its bound class's function table names the body for the call site.
// A register index is a switch over the slots of the declaration fp# lies in
// (an index outside them is undefined, 22.7.19 (2), and calls nothing).
bool DxbcTranslator::emit_interface_call(u32 opcode_token) {
    if ((opcode_token & 0x00002000u) != 0u || !m_decoder.has_tokens(1)) {
        return fail("FCALL: bad instruction");
    }
    const u32 call_site = m_decoder.read_token();
    Sm4Operand target;
    if (!m_decoder.decode_operand(target) ||
        target.type != Sm4OperandType::Interface || target.index0_relative) {
        return fail("FCALL: expected an interface operand fp#[index]");
    }
    const auto declaration = std::find_if(
        m_interfaces.begin(), m_interfaces.end(), [&](const auto& d) {
            return target.index0 >= d.base &&
                   target.index0 < d.base + d.array_size;
        });
    if (declaration == m_interfaces.end() ||
        call_site >= declaration->call_sites) {
        return fail("FCALL: fp" + std::to_string(target.index0) +
                    " call site " + std::to_string(call_site) +
                    " is not declared");
    }
    const auto body_at = [&](u32 slot) -> u32 {
        if (slot >= m_slot_bindings.size()) return kOutsideBodies;
        const auto table = m_function_tables.find(m_slot_bindings[slot].table);
        if (table == m_function_tables.end()) return kOutsideBodies;
        return table->second[call_site];
    };
    const auto call_body = [&](u32 body) -> bool {
        if (!hs_as_compute()) {
            return emit_subroutine_call(body | kFunctionBodyKey);
        }
        // A hull-shader phase inlines what it calls (see CALL).
        const u32 saved = m_current_body;
        m_current_body = body;
        const bool ok = translate_subroutine(body | kFunctionBodyKey);
        m_current_body = saved;
        return ok;
    };
    if (hs_as_compute() && (!m_if_stack.empty() || !m_loop_stack.empty() ||
                            !m_switch_stack.empty())) {
        return fail("FCALL: invocation inside structured flow of a hull "
                    "shader phase is not implemented");
    }
    if (!target.index1_relative) {
        const u32 body = body_at(target.index0 + target.index1);
        // Unbound: the program is being validated, not run.
        return body == kOutsideBodies ? true : call_body(body);
    }
    if (target.relative_type != Sm4OperandType::Temp) {
        return fail("FCALL: unsupported relative index source");
    }
    std::map<u32, std::vector<u32>> slots_of_body;
    for (u32 slot = declaration->base;
         slot < declaration->base + declaration->array_size; ++slot) {
        const u32 body = body_at(slot);
        if (body != kOutsideBodies) slots_of_body[body].push_back(slot);
    }
    if (slots_of_body.empty()) return true;
    const u32 selector = relative_index_value(
        target.relative_type, target.relative_index0,
        target.relative_component, target.index0 + target.index1);
    const u32 merge = m_builder.allocate_id();
    std::vector<std::pair<u32, u32>> targets;
    std::vector<std::pair<u32, u32>> blocks;  // body -> block
    for (const auto& [body, slots] : slots_of_body) {
        const u32 block = m_builder.allocate_id();
        blocks.emplace_back(body, block);
        for (const u32 slot : slots) targets.emplace_back(slot, block);
    }
    m_builder.emit_selection_merge(merge);
    m_builder.emit_switch(selector, merge, targets);
    for (const auto& [body, block] : blocks) {
        m_builder.emit_label_id(block);
        if (!call_body(body)) return false;
        m_builder.emit_branch(merge);
    }
    m_builder.emit_label_id(merge);
    return true;
}

// 7.11: a t# or s# indexed by a register holds the same value across the
// draw ("uniform"), so the instruction is translated once per register the
// index can name -- a switch on the index, each case the instruction with
// that register fixed -- and an index naming none of them does nothing.
bool DxbcTranslator::translate_uniformly_indexed(u32 opcode_token,
                                                 u32 sample_controls,
                                                 u32 resource_dimension,
                                                 usize instruction_end) {
    const usize operands_begin = m_decoder.position();
    std::vector<std::pair<u32, Sm4Operand>> indexed;
    m_pending_array_operands.clear();
    if ((opcode_token & 0x7FFu) != 120u && (opcode_token & 0x7FFu) != 53u) {
        DxbcDecoder probe = m_decoder;
        probe.clear_index0_overrides();
        probe.begin_instruction_operands();
        for (u32 ordinal = 0; probe.position() < instruction_end; ++ordinal) {
            Sm4Operand operand;
            if (!probe.decode_operand(operand)) break;
            if (operand.index0_relative &&
                (operand.type == Sm4OperandType::Resource ||
                 operand.type == Sm4OperandType::Sampler)) {
                indexed.emplace_back(ordinal, operand);
            }
            // A 5.1 t#/s#/u# array element: bound for the instruction by
            // translate_instruction. A constant buffer's is read where the
            // element is (load_vec4): one instruction may name two.
            if (operand.has_register_index &&
                operand.type != Sm4OperandType::ConstantBuffer) {
                const ResourceRange* range =
                    find_range(register_class_of(operand.type),
                               operand.index0);
                if (range != nullptr && range->is_array) {
                    m_pending_array_operands.push_back(operand);
                }
            }
        }
    }
    m_decoder.begin_instruction_operands();
    if (indexed.empty()) {
        return translate_instruction(opcode_token, sample_controls);
    }
    std::function<bool(usize)> emit_level = [&](usize level) -> bool {
        if (level == indexed.size()) {
            m_decoder.set_position(operands_begin);
            m_decoder.begin_instruction_operands();
            return translate_instruction(opcode_token, sample_controls);
        }
        const auto& [ordinal, operand] = indexed[level];
        if (operand.index0_relative_type != Sm4OperandType::Temp) {
            return fail("uniform index: unsupported relative index source");
        }
        const auto candidates = uniform_index_candidates(
            operand.type, operand.index0,
            operand.type == Sm4OperandType::Resource ? resource_dimension
                                                     : 0u);
        if (candidates.empty()) return true;
        const u32 selector = relative_index_value(
            operand.index0_relative_type, operand.index0_relative_register,
            operand.index0_relative_component, operand.index0);
        const u32 merge = m_builder.allocate_id();
        std::vector<std::pair<u32, u32>> targets;
        for (const u32 slot : candidates) {
            targets.emplace_back(slot, m_builder.allocate_id());
        }
        m_builder.emit_selection_merge(merge);
        m_builder.emit_switch(selector, merge, targets);
        for (const auto& [slot, block] : targets) {
            m_builder.emit_label_id(block);
            m_decoder.set_index0_override(ordinal, slot);
            if (!emit_level(level + 1)) return false;
            m_builder.emit_branch(merge);
        }
        m_builder.emit_label_id(merge);
        return true;
    };
    const bool ok = emit_level(0);
    m_decoder.clear_index0_overrides();
    return ok;
}

} // namespace vitreo::graphics::dxbc_spirv_detail
