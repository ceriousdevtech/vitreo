// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Rotate instruction execution (ROL, ROR, RCL, RCR) - CPU-023

#include "exec_ops.hpp"
#include "helpers.hpp"

namespace vitreo::cpu::ops {

namespace {

// ============================================================================
// Operand-size helpers
// ============================================================================

u8 rol_op_size(Opcode op) noexcept {
    switch (op) {
        case Opcode::ROL_RM8_1:  case Opcode::ROL_RM8_CL:  case Opcode::ROL_RM8_IMM8:  return 1;
        case Opcode::ROL_RM16_1: case Opcode::ROL_RM16_CL: case Opcode::ROL_RM16_IMM8: return 2;
        case Opcode::ROL_RM32_1: case Opcode::ROL_RM32_CL: case Opcode::ROL_RM32_IMM8: return 4;
        case Opcode::ROL_RM64_1: case Opcode::ROL_RM64_CL: case Opcode::ROL_RM64_IMM8: return 8;
        default: return 4;
    }
}

u8 ror_op_size(Opcode op) noexcept {
    switch (op) {
        case Opcode::ROR_RM8_1:  case Opcode::ROR_RM8_CL:  case Opcode::ROR_RM8_IMM8:  return 1;
        case Opcode::ROR_RM16_1: case Opcode::ROR_RM16_CL: case Opcode::ROR_RM16_IMM8: return 2;
        case Opcode::ROR_RM32_1: case Opcode::ROR_RM32_CL: case Opcode::ROR_RM32_IMM8: return 4;
        case Opcode::ROR_RM64_1: case Opcode::ROR_RM64_CL: case Opcode::ROR_RM64_IMM8: return 8;
        default: return 4;
    }
}

u8 rcl_op_size(Opcode op) noexcept {
    switch (op) {
        case Opcode::RCL_RM8_1:  case Opcode::RCL_RM8_CL:  case Opcode::RCL_RM8_IMM8:  return 1;
        case Opcode::RCL_RM16_1: case Opcode::RCL_RM16_CL: case Opcode::RCL_RM16_IMM8: return 2;
        case Opcode::RCL_RM32_1: case Opcode::RCL_RM32_CL: case Opcode::RCL_RM32_IMM8: return 4;
        case Opcode::RCL_RM64_1: case Opcode::RCL_RM64_CL: case Opcode::RCL_RM64_IMM8: return 8;
        default: return 4;
    }
}

u8 rcr_op_size(Opcode op) noexcept {
    switch (op) {
        case Opcode::RCR_RM8_1:  case Opcode::RCR_RM8_CL:  case Opcode::RCR_RM8_IMM8:  return 1;
        case Opcode::RCR_RM16_1: case Opcode::RCR_RM16_CL: case Opcode::RCR_RM16_IMM8: return 2;
        case Opcode::RCR_RM32_1: case Opcode::RCR_RM32_CL: case Opcode::RCR_RM32_IMM8: return 4;
        case Opcode::RCR_RM64_1: case Opcode::RCR_RM64_CL: case Opcode::RCR_RM64_IMM8: return 8;
        default: return 4;
    }
}

/// Mask value to operand size
u64 mask_to_size(u64 value, u8 size) noexcept {
    switch (size) {
        case 1: return static_cast<u8>(value);
        case 2: return static_cast<u16>(value);
        case 4: return static_cast<u32>(value);
        default: return value;
    }
}

} // anonymous namespace

// ============================================================================
// ROL - Rotate Left
// ============================================================================

ExecResult exec_rol(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto dst = read_operand(ctx, instr, 0);
    auto cnt = read_operand(ctx, instr, 1);
    if (!dst || !cnt) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    u8 size = rol_op_size(instr.opcode);
    u8 bits = size * 8;
    u64 value = mask_to_size(dst.value(), size);
    u8 count = static_cast<u8>(cnt.value()) & ((size == 8) ? 63 : 31);
    
    if (count == 0) {
        // Intel SDM Vol. 2 ROL/ROR/RCL/RCR: a masked count of zero leaves the
        // FLAGS untouched. The destination is still written, and in 64-bit
        // mode a 32-bit register write zero-extends into bits 63:32 (Intel SDM
        // Vol. 1 3.4.1.1) — there is no exception for a zero count. Verified
        // on real x86-64: `ror eax, cl` with CL=0 turns RAX
        // 0xAAAAAAAADEADBEEF into 0xDEADBEEF.
        //
        // Returning early here made the interpreter and the JIT agree on the
        // wrong answer, which is why JitParity64Test could not see it.
        if (!write_operand(ctx, instr, 0, value)) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        return ExecResult::Continue;
    }
    
    // Effective count reduced modulo the bit width
    u8 eff = count % bits;
    
    u64 result;
    if (eff == 0) {
        // Full rotation(s), value unchanged but flags still updated
        result = value;
    } else {
        result = mask_to_size((value << eff) | (value >> (bits - eff)), size);
    }
    
    // Write result
    auto wr = write_operand(ctx, instr, 0, result);
    if (!wr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    // CF = LSB of result (bit 0). ZF/SF/PF/AF stay with any pending FlagOp.
    bool cf = result & 1;
    ctx.cpu.write_cf_only(cf);
    
    // OF defined only for 1-bit rotate: MSB of result XOR CF
    if (count == 1) {
        bool msb = (result >> (bits - 1)) & 1;
        ctx.cpu.write_of_only(msb ^ cf);
    }
    
    return ExecResult::Continue;
}

// ============================================================================
// ROR - Rotate Right
// ============================================================================

ExecResult exec_ror(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto dst = read_operand(ctx, instr, 0);
    auto cnt = read_operand(ctx, instr, 1);
    if (!dst || !cnt) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    u8 size = ror_op_size(instr.opcode);
    u8 bits = size * 8;
    u64 value = mask_to_size(dst.value(), size);
    u8 count = static_cast<u8>(cnt.value()) & ((size == 8) ? 63 : 31);
    
    if (count == 0) {
        // Intel SDM Vol. 2 ROL/ROR/RCL/RCR: a masked count of zero leaves the
        // FLAGS untouched. The destination is still written, and in 64-bit
        // mode a 32-bit register write zero-extends into bits 63:32 (Intel SDM
        // Vol. 1 3.4.1.1) — there is no exception for a zero count. Verified
        // on real x86-64: `ror eax, cl` with CL=0 turns RAX
        // 0xAAAAAAAADEADBEEF into 0xDEADBEEF.
        //
        // Returning early here made the interpreter and the JIT agree on the
        // wrong answer, which is why JitParity64Test could not see it.
        if (!write_operand(ctx, instr, 0, value)) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        return ExecResult::Continue;
    }
    
    // Effective count reduced modulo the bit width
    u8 eff = count % bits;
    
    u64 result;
    if (eff == 0) {
        result = value;
    } else {
        result = mask_to_size((value >> eff) | (value << (bits - eff)), size);
    }
    
    // Write result
    auto wr = write_operand(ctx, instr, 0, result);
    if (!wr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    // CF = MSB of result. ZF/SF/PF/AF stay with any pending FlagOp.
    bool cf = (result >> (bits - 1)) & 1;
    ctx.cpu.write_cf_only(cf);
    
    // OF defined only for 1-bit rotate: MSB XOR (MSB-1) of result
    if (count == 1) {
        bool msb = (result >> (bits - 1)) & 1;
        bool msb_1 = (result >> (bits - 2)) & 1;
        ctx.cpu.write_of_only(msb ^ msb_1);
    }
    
    return ExecResult::Continue;
}

// ============================================================================
// RCL - Rotate Left Through Carry
// ============================================================================

ExecResult exec_rcl(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto dst = read_operand(ctx, instr, 0);
    auto cnt = read_operand(ctx, instr, 1);
    if (!dst || !cnt) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    u8 size = rcl_op_size(instr.opcode);
    u8 bits = size * 8;
    u64 value = mask_to_size(dst.value(), size);
    u8 count = static_cast<u8>(cnt.value()) & ((size == 8) ? 63 : 31);
    
    if (count == 0) {
        // Intel SDM Vol. 2 ROL/ROR/RCL/RCR: a masked count of zero leaves the
        // FLAGS untouched. The destination is still written, and in 64-bit
        // mode a 32-bit register write zero-extends into bits 63:32 (Intel SDM
        // Vol. 1 3.4.1.1) — there is no exception for a zero count. Verified
        // on real x86-64: `ror eax, cl` with CL=0 turns RAX
        // 0xAAAAAAAADEADBEEF into 0xDEADBEEF.
        //
        // Returning early here made the interpreter and the JIT agree on the
        // wrong answer, which is why JitParity64Test could not see it.
        if (!write_operand(ctx, instr, 0, value)) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        return ExecResult::Continue;
    }
    
    // RCL rotates through (bits+1) positions (value bits + CF)
    // Effective count mod (bits+1)
    u8 width = bits + 1;
    u8 eff = count % width;
    
    if (eff == 0) {
        // Full cycle(s), no change
        return ExecResult::Continue;
    }
    
    bool old_cf = ctx.cpu.lazy_cf();
    
    // Perform rotation one bit at a time for correctness across all sizes
    // Build extended value: CF in bit position 'bits', value in lower bits
    // For 8-bit: 9-bit rotation; 16-bit: 17-bit; 32/64: use u128 logic via loop
    
    // Use iterative rotation for simplicity and correctness
    u64 v = value;
    bool cf = old_cf;
    for (u8 i = 0; i < eff; ++i) {
        bool top_bit = (v >> (bits - 1)) & 1;
        v = mask_to_size((v << 1) | (cf ? 1 : 0), size);
        cf = top_bit;
    }
    
    u64 result = v;
    
    // Write result
    auto wr = write_operand(ctx, instr, 0, result);
    if (!wr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    ctx.cpu.write_cf_only(cf);
    
    // OF defined only for 1-bit rotate: MSB of result XOR CF
    if (count == 1) {
        bool msb = (result >> (bits - 1)) & 1;
        ctx.cpu.write_of_only(msb ^ cf);
    }
    
    return ExecResult::Continue;
}

// ============================================================================
// RCR - Rotate Right Through Carry
// ============================================================================

ExecResult exec_rcr(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto dst = read_operand(ctx, instr, 0);
    auto cnt = read_operand(ctx, instr, 1);
    if (!dst || !cnt) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    u8 size = rcr_op_size(instr.opcode);
    u8 bits = size * 8;
    u64 value = mask_to_size(dst.value(), size);
    u8 count = static_cast<u8>(cnt.value()) & ((size == 8) ? 63 : 31);
    
    if (count == 0) {
        // Intel SDM Vol. 2 ROL/ROR/RCL/RCR: a masked count of zero leaves the
        // FLAGS untouched. The destination is still written, and in 64-bit
        // mode a 32-bit register write zero-extends into bits 63:32 (Intel SDM
        // Vol. 1 3.4.1.1) — there is no exception for a zero count. Verified
        // on real x86-64: `ror eax, cl` with CL=0 turns RAX
        // 0xAAAAAAAADEADBEEF into 0xDEADBEEF.
        //
        // Returning early here made the interpreter and the JIT agree on the
        // wrong answer, which is why JitParity64Test could not see it.
        if (!write_operand(ctx, instr, 0, value)) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        return ExecResult::Continue;
    }
    
    // RCR rotates right through (bits+1) positions (value bits + CF)
    u8 width = bits + 1;
    u8 eff = count % width;
    
    if (eff == 0) {
        return ExecResult::Continue;
    }
    
    bool old_cf = ctx.cpu.lazy_cf();
    
    // OF defined only for 1-bit rotate: MSB of original value XOR old CF
    // Must capture before mutation
    bool of_val = false;
    if (count == 1) {
        bool msb = (value >> (bits - 1)) & 1;
        of_val = msb ^ old_cf;
    }
    
    // Iterative right rotation through carry
    u64 v = value;
    bool cf = old_cf;
    for (u8 i = 0; i < eff; ++i) {
        bool low_bit = v & 1;
        v = mask_to_size((v >> 1) | (cf ? (static_cast<u64>(1) << (bits - 1)) : 0), size);
        cf = low_bit;
    }
    
    u64 result = v;
    
    // Write result
    auto wr = write_operand(ctx, instr, 0, result);
    if (!wr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    ctx.cpu.write_cf_only(cf);
    
    if (count == 1) {
        ctx.cpu.write_of_only(of_val);
    }
    
    return ExecResult::Continue;
}

} // namespace vitreo::cpu::ops
