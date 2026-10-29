// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Shared interpreter helpers

#pragma once

#include "../interpreter.hpp"

namespace vitreo::cpu::ops {

/// Sentinel value for "no register" in memory operands
constexpr u8 NO_REG = 255;

/// Calculate effective address for memory operand
[[nodiscard]] Result<u64, Error> calc_effective_address(
    ExecContext& ctx,
    const MemoryOperand& mem
) noexcept;

/// Read value from memory of given size
[[nodiscard]] Result<u64, Error> read_memory(
    ExecContext& ctx,
    u64 addr,
    u8 size
) noexcept;

/// Write value to memory of given size
[[nodiscard]] VoidResult write_memory(
    ExecContext& ctx,
    u64 addr,
    u64 value,
    u8 size
) noexcept;

/// DS:(E/R)DI linear address for MASKMOVQ / MASKMOVDQU (Intel SDM Vol. 2).
/// Address-size and segment-override prefixes apply; EDI/RDI is not updated.
[[nodiscard]] Result<u64, Error> maskmov_ds_edi_address(
    ExecContext& ctx,
    const DecodedInstruction& instr
) noexcept;

/// Read register value
[[nodiscard]] u64 read_register(
    ExecContext& ctx,
    OperandType type,
    u8 reg
) noexcept;

/// Write register value
void write_register(
    ExecContext& ctx,
    OperandType type,
    u8 reg,
    u64 value
) noexcept;

/// Check if operand type is a register
[[nodiscard]] constexpr bool is_reg_type(OperandType type) noexcept {
    return type == OperandType::Reg8 || type == OperandType::Reg16 ||
           type == OperandType::Reg32 || type == OperandType::Reg64 ||
           type == OperandType::Reg128 || type == OperandType::RegSeg;
}

/// Check if operand type is memory
[[nodiscard]] constexpr bool is_mem_type(OperandType type) noexcept {
    return type == OperandType::Mem8 || type == OperandType::Mem16 ||
           type == OperandType::Mem32 || type == OperandType::Mem64 ||
           type == OperandType::Mem80 || type == OperandType::Mem128 ||
           type == OperandType::Mem256 || type == OperandType::MemUnknown;
}

/// Check if operand type is immediate
[[nodiscard]] constexpr bool is_imm_type(OperandType type) noexcept {
    return type == OperandType::Imm8 || type == OperandType::Imm16 ||
           type == OperandType::Imm32 || type == OperandType::Imm64;
}

/// Check if operand type is relative offset
[[nodiscard]] constexpr bool is_rel_type(OperandType type) noexcept {
    return type == OperandType::Rel8 || type == OperandType::Rel16 ||
           type == OperandType::Rel32;
}

/// Get memory operand size
[[nodiscard]] constexpr u8 mem_operand_size(OperandType type) noexcept {
    switch (type) {
        case OperandType::Mem8: return 1;
        case OperandType::Mem16: return 2;
        case OperandType::Mem32: return 4;
        case OperandType::Mem64: return 8;
        case OperandType::Mem80: return 10;
        case OperandType::Mem128: return 16;
        case OperandType::Mem256: return 32;
        default: return 0;
    }
}

} // namespace vitreo::cpu::ops
