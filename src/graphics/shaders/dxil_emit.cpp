// SPDX-License-Identifier: LGPL-2.1-only
/// @file dxil_emit.cpp
/// @brief SM5.1 token encoding for the DXIL lowering
/// (d3d12TokenizedProgramFormat.hpp, Windows SDK).

#include "dxil_lower_internal.hpp"

namespace vitreo::graphics::dxil_lower {

namespace {

// OperandToken0 fields.
constexpr u32 kComp0 = 0, kComp1 = 1, kComp4 = 2;
constexpr u32 kModeMask = 0, kModeSwizzle = 1, kModeSelect1 = 2;
// D3D10_SB_OPERAND_INDEX_REPRESENTATION.
constexpr u32 kIndexImm32 = 0, kIndexImm32PlusRelative = 3;

u32 token(Sm4OperandType type, u32 comps, u32 mode, u32 bits, u32 dim) {
    u32 t = comps;
    if (comps == kComp4) t |= (mode << 2) | (bits << 4);
    t |= static_cast<u32>(type) << 12;
    t |= dim << 20;
    return t;
}

void set_repr(u32& t, u32 dim_index, u32 repr) { t |= repr << (22 + 3 * dim_index); }

/// The relative-index register: r#.c in select-1 form.
void append_relative(std::vector<u32>& out, u32 reg, u32 comp) {
    out.push_back(token(Sm4OperandType::Temp, kComp4, kModeSelect1, comp, 1));
    out.push_back(reg);
}

void append_index(Operand& op, u32 dim_index, const Index& index) {
    if (!index.relative) {
        set_repr(op.tokens[0], dim_index, kIndexImm32);
        op.tokens.push_back(index.immediate);
        return;
    }
    set_repr(op.tokens[0], dim_index, kIndexImm32PlusRelative);
    op.tokens.push_back(index.immediate);
    append_relative(op.tokens, index.reg, index.comp);
}

/// Inserts an extended operand token (modifier and / or non-uniform) after
/// OperandToken0.
void extend(Operand& op, u32 modifier, bool nonuniform) {
    if (modifier == 0 && !nonuniform) return;
    op.tokens[0] |= 1u << 31;
    // D3D10_SB_EXTENDED_OPERAND_MODIFIER = 1, modifier bits 6-13,
    // D3D12_SB_OPERAND_NON_UNIFORM bit 17.
    const u32 ext = 1u | (modifier << 6) | (nonuniform ? (1u << 17) : 0u);
    op.tokens.insert(op.tokens.begin() + 1, ext);
}

}  // namespace

Operand dst_temp(u32 reg, u32 mask) {
    Operand op;
    op.tokens.push_back(token(Sm4OperandType::Temp, kComp4, kModeMask, mask, 1));
    op.tokens.push_back(reg);
    return op;
}

Operand src_temp(u32 reg, u32 swizzle, u32 modifier) {
    Operand op;
    op.tokens.push_back(token(Sm4OperandType::Temp, kComp4, kModeSwizzle, swizzle, 1));
    op.tokens.push_back(reg);
    extend(op, modifier, false);
    return op;
}

Operand src_temp_scalar(u32 reg, u32 comp, u32 modifier) {
    return src_temp(reg, swizzle_of(comp), modifier);
}

Operand src_temp_select(u32 reg, u32 comp) {
    Operand op;
    op.tokens.push_back(token(Sm4OperandType::Temp, kComp4, kModeSelect1, comp, 1));
    op.tokens.push_back(reg);
    return op;
}

Operand imm4(u32 x, u32 y, u32 z, u32 w) {
    Operand op;
    op.tokens.push_back(token(Sm4OperandType::Immediate32, kComp4, kModeSwizzle, 0xE4, 0));
    op.tokens.insert(op.tokens.end(), {x, y, z, w});
    return op;
}

Operand label_operand(u32 label) {
    Operand op;
    op.tokens.push_back(token(Sm4OperandType::Label, kComp0, 0, 0, 1));
    op.tokens.push_back(label);
    return op;
}

Operand imm1(u32 value) {
    Operand op;
    op.tokens.push_back(token(Sm4OperandType::Immediate32, kComp1, 0, 0, 0));
    op.tokens.push_back(value);
    return op;
}

Operand null_operand() {
    Operand op;
    op.tokens.push_back(token(Sm4OperandType::Null, kComp0, 0, 0, 0));
    return op;
}

Operand special_src(Sm4OperandType type, u32 swizzle) {
    Operand op;
    if (type == Sm4OperandType::InputThreadIdInGroupFlattened ||
        type == Sm4OperandType::InputPrimitiveID ||
        type == Sm4OperandType::InputGSInstanceID ||
        type == Sm4OperandType::OutputControlPointId ||
        type == Sm4OperandType::InputForkInstanceId ||
        type == Sm4OperandType::InputJoinInstanceId) {
        op.tokens.push_back(token(type, kComp1, 0, 0, 0));
    } else {
        op.tokens.push_back(token(type, kComp4, kModeSwizzle, swizzle, 0));
    }
    return op;
}

Operand special_dcl(Sm4OperandType type, u32 mask) {
    Operand op;
    if (type == Sm4OperandType::InputThreadIdInGroupFlattened ||
        type == Sm4OperandType::InputPrimitiveID ||
        type == Sm4OperandType::InputGSInstanceID ||
        type == Sm4OperandType::OutputControlPointId) {
        op.tokens.push_back(token(type, kComp1, 0, 0, 0));
    } else {
        op.tokens.push_back(token(type, kComp4, kModeMask, mask, 0));
    }
    return op;
}

Operand resource(Sm4OperandType type, u32 range_id, const Index& reg,
                 bool nonuniform, u32 swizzle, u32 mask) {
    Operand op;
    if (swizzle == ~0u) {
        op.tokens.push_back(token(type, kComp4, kModeMask, mask, 2));
    } else if (type == Sm4OperandType::Sampler) {
        op.tokens.push_back(token(type, kComp0, 0, 0, 2));
    } else {
        op.tokens.push_back(token(type, kComp4, kModeSwizzle, swizzle, 2));
    }
    append_index(op, 0, Index{false, range_id, 0, 0});
    append_index(op, 1, reg);
    extend(op, 0, nonuniform);
    return op;
}

Operand cbuffer(u32 range_id, const Index& reg, const Index& row, bool nonuniform,
                u32 swizzle) {
    Operand op;
    op.tokens.push_back(token(Sm4OperandType::ConstantBuffer, kComp4, kModeSwizzle, swizzle, 3));
    append_index(op, 0, Index{false, range_id, 0, 0});
    append_index(op, 1, reg);
    append_index(op, 2, row);
    extend(op, 0, nonuniform);
    return op;
}

Operand indexable(u32 array, const Index& element, u32 swizzle_or_mask, bool destination) {
    Operand op;
    op.tokens.push_back(token(Sm4OperandType::IndexableTemp, kComp4,
                              destination ? kModeMask : kModeSwizzle, swizzle_or_mask, 2));
    append_index(op, 0, Index{false, array, 0, 0});
    append_index(op, 1, element);
    return op;
}

Operand tgsm(u32 id, u32 swizzle_or_mask, bool destination) {
    Operand op;
    op.tokens.push_back(token(Sm4OperandType::ThreadGroupSharedMemory, kComp4,
                              destination ? kModeMask : kModeSwizzle, swizzle_or_mask, 1));
    op.tokens.push_back(id);
    return op;
}

Operand range_dcl(Sm4OperandType type, u32 range_id, u32 lower, u32 upper) {
    Operand op;
    if (type == Sm4OperandType::Sampler || type == Sm4OperandType::ConstantBuffer ||
        type == Sm4OperandType::Resource || type == Sm4OperandType::UnorderedAccessView) {
        // dcl_constantbuffer's operand is 4-component; t# / u# / s#
        // declarations name no components.
        const bool four = type == Sm4OperandType::ConstantBuffer;
        op.tokens.push_back(four ? token(type, kComp4, kModeSwizzle, 0xE4, 3)
                                 : token(type, kComp0, 0, 0, 3));
    }
    op.tokens.insert(op.tokens.end(), {range_id, lower, upper});
    return op;
}

void Emitter::inst(u32 opcode_and_flags, std::initializer_list<const Operand*> ops,
                   std::initializer_list<u32> extra) {
    usize length = 1 + extra.size();
    for (const Operand* op : ops) length += op->tokens.size();
    m_code.push_back(opcode_and_flags | (static_cast<u32>(length) << 24));
    for (const Operand* op : ops) m_code.insert(m_code.end(), op->tokens.begin(), op->tokens.end());
    m_code.insert(m_code.end(), extra.begin(), extra.end());
    const u32 opcode = opcode_and_flags & 0x7FFu;
    m_last_jump = opcode == static_cast<u32>(Sm4Opcode::BREAK) ||
                  opcode == static_cast<u32>(Sm4Opcode::CONTINUE) ||
                  opcode == static_cast<u32>(Sm4Opcode::RET);
}

void Emitter::inst_ext(u32 opcode_and_flags, u32 extended, std::initializer_list<const Operand*> ops) {
    // OpcodeToken0 bit 31: an extended opcode token follows it.
    usize length = 2;
    for (const Operand* op : ops) length += op->tokens.size();
    m_code.push_back(opcode_and_flags | (1u << 31) | (static_cast<u32>(length) << 24));
    m_code.push_back(extended);
    for (const Operand* op : ops) m_code.insert(m_code.end(), op->tokens.begin(), op->tokens.end());
    m_last_jump = false;
}

}  // namespace vitreo::graphics::dxil_lower
