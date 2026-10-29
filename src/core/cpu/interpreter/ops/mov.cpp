// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// MOV instruction execution

#include "exec_ops.hpp"
#include "helpers.hpp"

namespace vitreo::cpu::ops {

ExecResult exec_mov(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto src = read_operand(ctx, instr, 1);
    if (!src) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    auto r = write_operand(ctx, instr, 0, src.value());
    if (!r) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    return ExecResult::Continue;
}

ExecResult exec_movzx_movsx(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    switch (instr.opcode) {
        // MOVZX: zero-extension (already handled by read_operand)
        case Opcode::MOVZX_R16_RM8:
        case Opcode::MOVZX_R32_RM8:
        case Opcode::MOVZX_R64_RM8:
        case Opcode::MOVZX_R32_RM16:
        case Opcode::MOVZX_R64_RM16: {
            auto src = read_operand(ctx, instr, 1);
            if (!src) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            auto r = write_operand(ctx, instr, 0, src.value());
            if (!r) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            return ExecResult::Continue;
        }
        
        // MOVSX: sign-extension from 8-bit
        case Opcode::MOVSX_R16_RM8:
        case Opcode::MOVSX_R32_RM8:
        case Opcode::MOVSX_R64_RM8: {
            auto src = read_operand(ctx, instr, 1);
            if (!src) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            i64 signed_val = static_cast<i64>(static_cast<i8>(src.value()));
            auto r = write_operand(ctx, instr, 0, static_cast<u64>(signed_val));
            if (!r) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            return ExecResult::Continue;
        }
        
        // MOVSX: sign-extension from 16-bit
        case Opcode::MOVSX_R32_RM16:
        case Opcode::MOVSX_R64_RM16: {
            auto src = read_operand(ctx, instr, 1);
            if (!src) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            i64 signed_val = static_cast<i64>(static_cast<i16>(src.value()));
            auto r = write_operand(ctx, instr, 0, static_cast<u64>(signed_val));
            if (!r) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            return ExecResult::Continue;
        }
        
        // MOVSXD: sign-extension from 32-bit
        case Opcode::MOVSXD_R64_RM32: {
            auto src = read_operand(ctx, instr, 1);
            if (!src) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            i64 signed_val = static_cast<i64>(static_cast<i32>(src.value()));
            auto r = write_operand(ctx, instr, 0, static_cast<u64>(signed_val));
            if (!r) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            return ExecResult::Continue;
        }
        
        default:
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
    }
}

} // namespace vitreo::cpu::ops
