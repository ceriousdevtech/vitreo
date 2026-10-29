// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Shift instruction execution (SHL, SHR, SAR) - CPU-017

#include "exec_ops.hpp"
#include "helpers.hpp"

namespace vitreo::cpu::ops {

namespace {

/// Get operand byte-size from SHL opcode
u8 shl_op_size(Opcode op) noexcept {
    switch (op) {
        case Opcode::SHL_RM8_1:  case Opcode::SHL_RM8_CL:  case Opcode::SHL_RM8_IMM8:  return 1;
        case Opcode::SHL_RM16_1: case Opcode::SHL_RM16_CL: case Opcode::SHL_RM16_IMM8: return 2;
        case Opcode::SHL_RM32_1: case Opcode::SHL_RM32_CL: case Opcode::SHL_RM32_IMM8: return 4;
        case Opcode::SHL_RM64_1: case Opcode::SHL_RM64_CL: case Opcode::SHL_RM64_IMM8: return 8;
        default: return 4;
    }
}

/// Get operand byte-size from SHR opcode
u8 shr_op_size(Opcode op) noexcept {
    switch (op) {
        case Opcode::SHR_RM8_1:  case Opcode::SHR_RM8_CL:  case Opcode::SHR_RM8_IMM8:  return 1;
        case Opcode::SHR_RM16_1: case Opcode::SHR_RM16_CL: case Opcode::SHR_RM16_IMM8: return 2;
        case Opcode::SHR_RM32_1: case Opcode::SHR_RM32_CL: case Opcode::SHR_RM32_IMM8: return 4;
        case Opcode::SHR_RM64_1: case Opcode::SHR_RM64_CL: case Opcode::SHR_RM64_IMM8: return 8;
        default: return 4;
    }
}

/// Get operand byte-size from SAR opcode
u8 sar_op_size(Opcode op) noexcept {
    switch (op) {
        case Opcode::SAR_RM8_1:  case Opcode::SAR_RM8_CL:  case Opcode::SAR_RM8_IMM8:  return 1;
        case Opcode::SAR_RM16_1: case Opcode::SAR_RM16_CL: case Opcode::SAR_RM16_IMM8: return 2;
        case Opcode::SAR_RM32_1: case Opcode::SAR_RM32_CL: case Opcode::SAR_RM32_IMM8: return 4;
        case Opcode::SAR_RM64_1: case Opcode::SAR_RM64_CL: case Opcode::SAR_RM64_IMM8: return 8;
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

/// Update ZF, SF, PF flags based on result and size
void update_zsp_by_size(ExecContext& ctx, u64 result, u8 size) noexcept {
    switch (size) {
        case 1: ctx.cpu.flags.update_zsp<u8>(static_cast<u8>(result));   break;
        case 2: ctx.cpu.flags.update_zsp<u16>(static_cast<u16>(result)); break;
        case 4: ctx.cpu.flags.update_zsp<u32>(static_cast<u32>(result)); break;
        case 8: ctx.cpu.flags.update_zsp<u64>(result);                   break;
        default: break;
    }
}

} // anonymous namespace

ExecResult exec_shl(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto dst = read_operand(ctx, instr, 0);
    auto cnt = read_operand(ctx, instr, 1);
    if (!dst || !cnt) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    u8 size = shl_op_size(instr.opcode);
    u8 bits = size * 8;
    u64 value = mask_to_size(dst.value(), size);
    u8 count = static_cast<u8>(cnt.value()) & ((size == 8) ? 63 : 31);
    
    if (count == 0) {
        // Flags unchanged. In long mode a 32-bit dest write still happens
        // (zero-extends the GPR); Intel silicon agrees.
        if (size == 4 && ctx.cpu.is_64bit()) {
            auto wr = write_operand(ctx, instr, 0, value);
            if (!wr) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
        }
        return ExecResult::Continue;
    }
    
    // CF = last bit shifted out = bit (bits - count) of original value. An
    // 8- or 16-bit operand masks its count to 5 bits, so the count can pass
    // the width; Intel SDM Vol. 2 leaves CF undefined there, every bit has
    // already gone, and CF is 0 (the JIT's Shl8/Shl16 records agree). The
    // guard also keeps the shift amount from going negative, which is UB.
    const bool cf = count <= bits && ((value >> (bits - count)) & 1) != 0;
    
    u64 result = mask_to_size(value << count, size);
    
    // Write result
    auto wr = write_operand(ctx, instr, 0, result);
    if (!wr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    // Update flags
    ctx.cpu.flags.set_cf(cf);
    update_zsp_by_size(ctx, result, size);
    
    // OF defined only for count == 1: MSB(result) XOR CF
    if (count == 1) {
        bool msb = (result >> (bits - 1)) & 1;
        ctx.cpu.flags.set_of(msb ^ cf);
    }
    
    return ExecResult::Continue;
}

ExecResult exec_shr(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto dst = read_operand(ctx, instr, 0);
    auto cnt = read_operand(ctx, instr, 1);
    if (!dst || !cnt) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    u8 size = shr_op_size(instr.opcode);
    u8 bits = size * 8;
    u64 value = mask_to_size(dst.value(), size);
    u8 count = static_cast<u8>(cnt.value()) & ((size == 8) ? 63 : 31);
    
    if (count == 0) {
        if (size == 4 && ctx.cpu.is_64bit()) {
            auto wr = write_operand(ctx, instr, 0, value);
            if (!wr) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
        }
        return ExecResult::Continue;
    }
    
    // CF = last bit shifted out = bit (count - 1) of original value
    bool cf = (value >> (count - 1)) & 1;
    
    u64 result = mask_to_size(value >> count, size);
    
    // Write result
    auto wr = write_operand(ctx, instr, 0, result);
    if (!wr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    // Update flags
    ctx.cpu.flags.set_cf(cf);
    update_zsp_by_size(ctx, result, size);
    
    // OF defined only for count == 1: MSB of original value
    if (count == 1) {
        bool msb_orig = (value >> (bits - 1)) & 1;
        ctx.cpu.flags.set_of(msb_orig);
    }
    
    return ExecResult::Continue;
}

ExecResult exec_sar(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto dst = read_operand(ctx, instr, 0);
    auto cnt = read_operand(ctx, instr, 1);
    if (!dst || !cnt) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    u8 size = sar_op_size(instr.opcode);
    u8 bits = size * 8;
    u64 value = mask_to_size(dst.value(), size);
    u8 count = static_cast<u8>(cnt.value()) & ((size == 8) ? 63 : 31);
    
    if (count == 0) {
        if (size == 4 && ctx.cpu.is_64bit()) {
            auto wr = write_operand(ctx, instr, 0, value);
            if (!wr) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
        }
        return ExecResult::Continue;
    }
    
    // CF = last bit shifted out.
    // If count >= operand width, all bits shifted out become the sign bit.
    bool cf;
    if (count >= bits) {
        cf = (value >> (bits - 1)) & 1;
    } else {
        cf = (value >> (count - 1)) & 1;
    }
    
    // Arithmetic right shift: sign-extend, shift, then mask
    u64 result;
    switch (size) {
        case 1: {
            i8 sv = static_cast<i8>(value);
            result = static_cast<u8>(sv >> count);
            break;
        }
        case 2: {
            i16 sv = static_cast<i16>(value);
            result = static_cast<u16>(sv >> count);
            break;
        }
        case 4: {
            i32 sv = static_cast<i32>(value);
            result = static_cast<u32>(sv >> count);
            break;
        }
        case 8: {
            i64 sv = static_cast<i64>(value);
            result = static_cast<u64>(sv >> count);
            break;
        }
        default:
            result = 0;
            break;
    }
    
    // Write result
    auto wr = write_operand(ctx, instr, 0, result);
    if (!wr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    // Update flags
    ctx.cpu.flags.set_cf(cf);
    update_zsp_by_size(ctx, result, size);
    
    // OF defined only for count == 1: always 0 for SAR
    if (count == 1) {
        ctx.cpu.flags.set_of(false);
    }
    
    return ExecResult::Continue;
}

/// Get operand byte-size from SHLD opcode
u8 shld_op_size(Opcode op) noexcept {
    switch (op) {
        case Opcode::SHLD_RM16_R16_IMM8: case Opcode::SHLD_RM16_R16_CL: return 2;
        case Opcode::SHLD_RM32_R32_IMM8: case Opcode::SHLD_RM32_R32_CL: return 4;
        case Opcode::SHLD_RM64_R64_IMM8: case Opcode::SHLD_RM64_R64_CL: return 8;
        default: return 4;
    }
}

ExecResult exec_shld(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // SHLD dest, src, count: shift dest left by count, filling from src
    auto dst = read_operand(ctx, instr, 0);
    auto src = read_operand(ctx, instr, 1);
    auto cnt = read_operand(ctx, instr, 2);
    if (!dst || !src || !cnt) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    u8 size = shld_op_size(instr.opcode);
    u8 bits = size * 8;
    u64 dest_val = mask_to_size(dst.value(), size);
    u64 src_val = mask_to_size(src.value(), size);
    u8 count = static_cast<u8>(cnt.value()) & ((size == 8) ? 63 : 31);
    
    if (count == 0) {
        return ExecResult::Continue;
    }
    
    // For SHLD: result = (dest << count) | (src >> (bits - count))
    // CF = last bit shifted out = bit (bits - count) of dest
    bool cf = (dest_val >> (bits - count)) & 1;
    
    u64 result;
    if (count >= bits) {
        result = src_val << (count - bits);
    } else {
        result = (dest_val << count) | (src_val >> (bits - count));
    }
    result = mask_to_size(result, size);
    
    auto wr = write_operand(ctx, instr, 0, result);
    if (!wr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    ctx.cpu.flags.set_cf(cf);
    update_zsp_by_size(ctx, result, size);
    
    // OF: SDM says defined only for count==1. Empirically the oracle agrees
    // for IMM8-form (only count==1 sets OF via sign-change), but for the CL
    // form the oracle computes sign-change for any non-zero count.
    bool is_cl_form = (instr.opcode == Opcode::SHLD_RM16_R16_CL ||
                       instr.opcode == Opcode::SHLD_RM32_R32_CL ||
                       instr.opcode == Opcode::SHLD_RM64_R64_CL);
    if (count == 1 || is_cl_form) {
        bool msb_orig = (dest_val >> (bits - 1)) & 1;
        bool msb_result = (result >> (bits - 1)) & 1;
        ctx.cpu.flags.set_of(msb_orig != msb_result);
    }
    
    return ExecResult::Continue;
}

/// Get operand byte-size from SHRD opcode
u8 shrd_op_size(Opcode op) noexcept {
    switch (op) {
        case Opcode::SHRD_RM16_R16_IMM8: case Opcode::SHRD_RM16_R16_CL: return 2;
        case Opcode::SHRD_RM32_R32_IMM8: case Opcode::SHRD_RM32_R32_CL: return 4;
        case Opcode::SHRD_RM64_R64_IMM8: case Opcode::SHRD_RM64_R64_CL: return 8;
        default: return 4;
    }
}

ExecResult exec_shrd(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // SHRD dest, src, count: shift dest right by count, filling from src
    auto dst = read_operand(ctx, instr, 0);
    auto src = read_operand(ctx, instr, 1);
    auto cnt = read_operand(ctx, instr, 2);
    if (!dst || !src || !cnt) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    u8 size = shrd_op_size(instr.opcode);
    u8 bits = size * 8;
    u64 dest_val = mask_to_size(dst.value(), size);
    u64 src_val = mask_to_size(src.value(), size);
    u8 count = static_cast<u8>(cnt.value()) & ((size == 8) ? 63 : 31);
    
    if (count == 0) {
        return ExecResult::Continue;
    }
    
    // For SHRD: result = (dest >> count) | (src << (bits - count))
    // CF = last bit shifted out = bit (count - 1) of dest
    bool cf = (dest_val >> (count - 1)) & 1;
    
    u64 result;
    if (count >= bits) {
        result = src_val >> (count - bits);
    } else {
        result = (dest_val >> count) | (src_val << (bits - count));
    }
    result = mask_to_size(result, size);
    
    auto wr = write_operand(ctx, instr, 0, result);
    if (!wr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    ctx.cpu.flags.set_cf(cf);
    update_zsp_by_size(ctx, result, size);
    
    // OF: see note in exec_shld.
    bool is_cl_form = (instr.opcode == Opcode::SHRD_RM16_R16_CL ||
                       instr.opcode == Opcode::SHRD_RM32_R32_CL ||
                       instr.opcode == Opcode::SHRD_RM64_R64_CL);
    if (count == 1 || is_cl_form) {
        bool msb_orig = (dest_val >> (bits - 1)) & 1;
        bool msb_result = (result >> (bits - 1)) & 1;
        ctx.cpu.flags.set_of(msb_orig != msb_result);
    }
    
    return ExecResult::Continue;
}

} // namespace vitreo::cpu::ops
