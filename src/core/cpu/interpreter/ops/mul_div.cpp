// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Multiply/Divide instruction execution (MUL, IMUL, DIV, IDIV, CBW/CWDE/CDQE/CWD/CDQ/CQO)

#include "exec_ops.hpp"
#include "helpers.hpp"

namespace vitreo::cpu::ops {

// ============================================================================
// MUL - Unsigned Multiply
// ============================================================================

ExecResult exec_mul(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // Source is operand 0 (Group 3 decodes r/m as operand 0)
    auto src_result = read_operand(ctx, instr, 0);
    if (!src_result) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    u64 src = src_result.value();

    switch (instr.opcode) {
        case Opcode::MUL_RM8: {
            u16 al = ctx.cpu.gpr.get8l(static_cast<u8>(RegIndex::RAX));
            u16 result = al * static_cast<u16>(static_cast<u8>(src));
            ctx.cpu.gpr.set16(static_cast<u8>(RegIndex::RAX), result);
            bool upper = (result >> 8) != 0;
            ctx.cpu.flags.set_cf(upper);
            ctx.cpu.flags.set_of(upper);
            return ExecResult::Continue;
        }
        case Opcode::MUL_RM16: {
            u32 ax = ctx.cpu.gpr.get16(static_cast<u8>(RegIndex::RAX));
            u32 result = ax * static_cast<u32>(static_cast<u16>(src));
            ctx.cpu.gpr.set16(static_cast<u8>(RegIndex::RAX), static_cast<u16>(result));
            ctx.cpu.gpr.set16(static_cast<u8>(RegIndex::RDX), static_cast<u16>(result >> 16));
            bool upper = (result >> 16) != 0;
            ctx.cpu.flags.set_cf(upper);
            ctx.cpu.flags.set_of(upper);
            return ExecResult::Continue;
        }
        case Opcode::MUL_RM32: {
            u64 eax = ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RAX));
            u64 result = eax * static_cast<u64>(static_cast<u32>(src));
            // Zero-extend by using set32 (which clears upper 32 bits)
            ctx.cpu.gpr.set32(static_cast<u8>(RegIndex::RAX), static_cast<u32>(result));
            ctx.cpu.gpr.set32(static_cast<u8>(RegIndex::RDX), static_cast<u32>(result >> 32));
            bool upper = (result >> 32) != 0;
            ctx.cpu.flags.set_cf(upper);
            ctx.cpu.flags.set_of(upper);
            return ExecResult::Continue;
        }
        case Opcode::MUL_RM64: {
            using u128 = unsigned __int128;
            u128 rax = ctx.cpu.gpr.get64(static_cast<u8>(RegIndex::RAX));
            u128 result = rax * static_cast<u128>(src);
            ctx.cpu.gpr.set64(static_cast<u8>(RegIndex::RAX), static_cast<u64>(result));
            ctx.cpu.gpr.set64(static_cast<u8>(RegIndex::RDX), static_cast<u64>(result >> 64));
            bool upper = static_cast<u64>(result >> 64) != 0;
            ctx.cpu.flags.set_cf(upper);
            ctx.cpu.flags.set_of(upper);
            return ExecResult::Continue;
        }
        default:
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
    }
}

// ============================================================================
// IMUL - Signed Multiply
// ============================================================================

ExecResult exec_imul(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    switch (instr.opcode) {
        // ==== One-operand forms (F6 /5, F7 /5) ====
        case Opcode::IMUL_RM8: {
            auto src_result = read_operand(ctx, instr, 0);
            if (!src_result) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            i16 al = static_cast<i8>(ctx.cpu.gpr.get8l(static_cast<u8>(RegIndex::RAX)));
            i16 result = al * static_cast<i16>(static_cast<i8>(static_cast<u8>(src_result.value())));
            ctx.cpu.gpr.set16(static_cast<u8>(RegIndex::RAX), static_cast<u16>(result));
            // CF=OF=1 if upper half is not sign extension of lower half
            bool overflow = (result != static_cast<i8>(result));
            ctx.cpu.flags.set_cf(overflow);
            ctx.cpu.flags.set_of(overflow);
            return ExecResult::Continue;
        }
        case Opcode::IMUL_RM16: {
            auto src_result = read_operand(ctx, instr, 0);
            if (!src_result) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            i32 ax = static_cast<i16>(ctx.cpu.gpr.get16(static_cast<u8>(RegIndex::RAX)));
            i32 result = ax * static_cast<i32>(static_cast<i16>(static_cast<u16>(src_result.value())));
            ctx.cpu.gpr.set16(static_cast<u8>(RegIndex::RAX), static_cast<u16>(result));
            ctx.cpu.gpr.set16(static_cast<u8>(RegIndex::RDX), static_cast<u16>(static_cast<u32>(result) >> 16));
            bool overflow = (result != static_cast<i16>(result));
            ctx.cpu.flags.set_cf(overflow);
            ctx.cpu.flags.set_of(overflow);
            return ExecResult::Continue;
        }
        case Opcode::IMUL_RM32: {
            auto src_result = read_operand(ctx, instr, 0);
            if (!src_result) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            i64 eax = static_cast<i32>(ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RAX)));
            i64 result = eax * static_cast<i64>(static_cast<i32>(static_cast<u32>(src_result.value())));
            ctx.cpu.gpr.set32(static_cast<u8>(RegIndex::RAX), static_cast<u32>(result));
            ctx.cpu.gpr.set32(static_cast<u8>(RegIndex::RDX), static_cast<u32>(static_cast<u64>(result) >> 32));
            bool overflow = (result != static_cast<i32>(result));
            ctx.cpu.flags.set_cf(overflow);
            ctx.cpu.flags.set_of(overflow);
            return ExecResult::Continue;
        }
        case Opcode::IMUL_RM64: {
            auto src_result = read_operand(ctx, instr, 0);
            if (!src_result) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            using i128 = __int128;
            i128 rax = static_cast<i64>(ctx.cpu.gpr.get64(static_cast<u8>(RegIndex::RAX)));
            i128 result = rax * static_cast<i128>(static_cast<i64>(src_result.value()));
            ctx.cpu.gpr.set64(static_cast<u8>(RegIndex::RAX), static_cast<u64>(result));
            ctx.cpu.gpr.set64(static_cast<u8>(RegIndex::RDX), static_cast<u64>(static_cast<unsigned __int128>(result) >> 64));
            bool overflow = (result != static_cast<i64>(result));
            ctx.cpu.flags.set_cf(overflow);
            ctx.cpu.flags.set_of(overflow);
            return ExecResult::Continue;
        }

        // ==== Two-operand forms (0F AF /r): dst = dst * src ====
        case Opcode::IMUL_R16_RM16: {
            auto dst_result = read_operand(ctx, instr, 0);
            auto src_result = read_operand(ctx, instr, 1);
            if (!dst_result || !src_result) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            i32 a = static_cast<i16>(static_cast<u16>(dst_result.value()));
            i32 b = static_cast<i16>(static_cast<u16>(src_result.value()));
            i32 result = a * b;
            auto wr = write_operand(ctx, instr, 0, static_cast<u64>(static_cast<u16>(result)));
            if (!wr) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            bool overflow = (result != static_cast<i16>(result));
            ctx.cpu.flags.set_cf(overflow);
            ctx.cpu.flags.set_of(overflow);
            return ExecResult::Continue;
        }
        case Opcode::IMUL_R32_RM32: {
            auto dst_result = read_operand(ctx, instr, 0);
            auto src_result = read_operand(ctx, instr, 1);
            if (!dst_result || !src_result) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            i64 a = static_cast<i32>(static_cast<u32>(dst_result.value()));
            i64 b = static_cast<i32>(static_cast<u32>(src_result.value()));
            i64 result = a * b;
            auto wr = write_operand(ctx, instr, 0, static_cast<u64>(static_cast<u32>(result)));
            if (!wr) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            bool overflow = (result != static_cast<i32>(result));
            ctx.cpu.flags.set_cf(overflow);
            ctx.cpu.flags.set_of(overflow);
            return ExecResult::Continue;
        }
        case Opcode::IMUL_R64_RM64: {
            auto dst_result = read_operand(ctx, instr, 0);
            auto src_result = read_operand(ctx, instr, 1);
            if (!dst_result || !src_result) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            using i128 = __int128;
            i128 a = static_cast<i64>(dst_result.value());
            i128 b = static_cast<i64>(src_result.value());
            i128 result = a * b;
            auto wr = write_operand(ctx, instr, 0, static_cast<u64>(result));
            if (!wr) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            bool overflow = (result != static_cast<i64>(result));
            ctx.cpu.flags.set_cf(overflow);
            ctx.cpu.flags.set_of(overflow);
            return ExecResult::Continue;
        }

        // ==== Three-operand forms: dst = src * imm ====
        case Opcode::IMUL_R16_RM16_IMM8:
        case Opcode::IMUL_R16_RM16_IMM16: {
            auto src_result = read_operand(ctx, instr, 1);
            auto imm_result = read_operand(ctx, instr, 2);
            if (!src_result || !imm_result) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            i32 a = static_cast<i16>(static_cast<u16>(src_result.value()));
            // For IMM8, the immediate is sign-extended by the decoder
            i32 b = static_cast<i16>(static_cast<u16>(imm_result.value()));
            if (instr.opcode == Opcode::IMUL_R16_RM16_IMM8) {
                b = static_cast<i8>(static_cast<u8>(imm_result.value()));
            }
            i32 result = a * b;
            auto wr = write_operand(ctx, instr, 0, static_cast<u64>(static_cast<u16>(result)));
            if (!wr) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            bool overflow = (result != static_cast<i16>(result));
            ctx.cpu.flags.set_cf(overflow);
            ctx.cpu.flags.set_of(overflow);
            return ExecResult::Continue;
        }
        case Opcode::IMUL_R32_RM32_IMM8:
        case Opcode::IMUL_R32_RM32_IMM32: {
            auto src_result = read_operand(ctx, instr, 1);
            auto imm_result = read_operand(ctx, instr, 2);
            if (!src_result || !imm_result) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            i64 a = static_cast<i32>(static_cast<u32>(src_result.value()));
            i64 b;
            if (instr.opcode == Opcode::IMUL_R32_RM32_IMM8) {
                b = static_cast<i8>(static_cast<u8>(imm_result.value()));
            } else {
                b = static_cast<i32>(static_cast<u32>(imm_result.value()));
            }
            i64 result = a * b;
            auto wr = write_operand(ctx, instr, 0, static_cast<u64>(static_cast<u32>(result)));
            if (!wr) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            bool overflow = (result != static_cast<i32>(result));
            ctx.cpu.flags.set_cf(overflow);
            ctx.cpu.flags.set_of(overflow);
            return ExecResult::Continue;
        }
        case Opcode::IMUL_R64_RM64_IMM8:
        case Opcode::IMUL_R64_RM64_IMM32: {
            auto src_result = read_operand(ctx, instr, 1);
            auto imm_result = read_operand(ctx, instr, 2);
            if (!src_result || !imm_result) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            using i128 = __int128;
            i128 a = static_cast<i64>(src_result.value());
            i128 b;
            if (instr.opcode == Opcode::IMUL_R64_RM64_IMM8) {
                b = static_cast<i8>(static_cast<u8>(imm_result.value()));
            } else {
                b = static_cast<i32>(static_cast<u32>(imm_result.value()));
            }
            i128 result = a * b;
            auto wr = write_operand(ctx, instr, 0, static_cast<u64>(result));
            if (!wr) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            bool overflow = (result != static_cast<i64>(result));
            ctx.cpu.flags.set_cf(overflow);
            ctx.cpu.flags.set_of(overflow);
            return ExecResult::Continue;
        }

        default:
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
    }
}

// ============================================================================
// DIV - Unsigned Divide
// ============================================================================

ExecResult exec_div(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto src_result = read_operand(ctx, instr, 0);
    if (!src_result) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    u64 src = src_result.value();

    switch (instr.opcode) {
        case Opcode::DIV_RM8: {
            u8 divisor = static_cast<u8>(src);
            if (divisor == 0) {
                ctx.exception = CpuException::DivideError;
                return ExecResult::Exception;
            }
            u16 ax = ctx.cpu.gpr.get16(static_cast<u8>(RegIndex::RAX));
            u16 quotient = ax / divisor;
            if (quotient > 0xFF) {
                ctx.exception = CpuException::DivideError;
                return ExecResult::Exception;
            }
            u8 remainder = static_cast<u8>(ax % divisor);
            // AL = quotient, AH = remainder
            // Write AH first then AL to avoid overwriting
            ctx.cpu.gpr.set8l(static_cast<u8>(RegIndex::RAX), static_cast<u8>(quotient));
            ctx.cpu.gpr.set8h(0, remainder);  // AH
            return ExecResult::Continue;
        }
        case Opcode::DIV_RM16: {
            u16 divisor = static_cast<u16>(src);
            if (divisor == 0) {
                ctx.exception = CpuException::DivideError;
                return ExecResult::Exception;
            }
            u32 dividend = (static_cast<u32>(ctx.cpu.gpr.get16(static_cast<u8>(RegIndex::RDX))) << 16) |
                           ctx.cpu.gpr.get16(static_cast<u8>(RegIndex::RAX));
            u32 quotient = dividend / divisor;
            if (quotient > 0xFFFF) {
                ctx.exception = CpuException::DivideError;
                return ExecResult::Exception;
            }
            u16 remainder = static_cast<u16>(dividend % divisor);
            ctx.cpu.gpr.set16(static_cast<u8>(RegIndex::RAX), static_cast<u16>(quotient));
            ctx.cpu.gpr.set16(static_cast<u8>(RegIndex::RDX), remainder);
            return ExecResult::Continue;
        }
        case Opcode::DIV_RM32: {
            u32 divisor = static_cast<u32>(src);
            if (divisor == 0) {
                ctx.exception = CpuException::DivideError;
                return ExecResult::Exception;
            }
            u64 dividend = (static_cast<u64>(ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RDX))) << 32) |
                           ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RAX));
            u64 quotient = dividend / divisor;
            if (quotient > 0xFFFFFFFFULL) {
                ctx.exception = CpuException::DivideError;
                return ExecResult::Exception;
            }
            u32 remainder = static_cast<u32>(dividend % divisor);
            // Use set32 for zero-extension
            ctx.cpu.gpr.set32(static_cast<u8>(RegIndex::RAX), static_cast<u32>(quotient));
            ctx.cpu.gpr.set32(static_cast<u8>(RegIndex::RDX), remainder);
            return ExecResult::Continue;
        }
        case Opcode::DIV_RM64: {
            u64 divisor = src;
            if (divisor == 0) {
                ctx.exception = CpuException::DivideError;
                return ExecResult::Exception;
            }
            using u128 = unsigned __int128;
            u128 dividend = (static_cast<u128>(ctx.cpu.gpr.get64(static_cast<u8>(RegIndex::RDX))) << 64) |
                            ctx.cpu.gpr.get64(static_cast<u8>(RegIndex::RAX));
            u128 quotient = dividend / divisor;
            if (quotient > static_cast<u128>(0xFFFFFFFFFFFFFFFFULL)) {
                ctx.exception = CpuException::DivideError;
                return ExecResult::Exception;
            }
            u64 remainder = static_cast<u64>(dividend % divisor);
            ctx.cpu.gpr.set64(static_cast<u8>(RegIndex::RAX), static_cast<u64>(quotient));
            ctx.cpu.gpr.set64(static_cast<u8>(RegIndex::RDX), remainder);
            return ExecResult::Continue;
        }
        default:
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
    }
}

// ============================================================================
// IDIV - Signed Divide
// ============================================================================

ExecResult exec_idiv(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto src_result = read_operand(ctx, instr, 0);
    if (!src_result) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    u64 src = src_result.value();

    switch (instr.opcode) {
        case Opcode::IDIV_RM8: {
            i8 divisor = static_cast<i8>(static_cast<u8>(src));
            if (divisor == 0) {
                ctx.exception = CpuException::DivideError;
                return ExecResult::Exception;
            }
            i16 ax = static_cast<i16>(ctx.cpu.gpr.get16(static_cast<u8>(RegIndex::RAX)));
            i16 quotient = ax / divisor;
            if (quotient > 127 || quotient < -128) {
                ctx.exception = CpuException::DivideError;
                return ExecResult::Exception;
            }
            i8 remainder = static_cast<i8>(ax % divisor);
            ctx.cpu.gpr.set8l(static_cast<u8>(RegIndex::RAX), static_cast<u8>(quotient));
            ctx.cpu.gpr.set8h(0, static_cast<u8>(remainder));
            return ExecResult::Continue;
        }
        case Opcode::IDIV_RM16: {
            i16 divisor = static_cast<i16>(static_cast<u16>(src));
            if (divisor == 0) {
                ctx.exception = CpuException::DivideError;
                return ExecResult::Exception;
            }
            i32 dividend = static_cast<i32>(
                (static_cast<u32>(ctx.cpu.gpr.get16(static_cast<u8>(RegIndex::RDX))) << 16) |
                ctx.cpu.gpr.get16(static_cast<u8>(RegIndex::RAX)));
            i32 quotient = dividend / divisor;
            if (quotient > 32767 || quotient < -32768) {
                ctx.exception = CpuException::DivideError;
                return ExecResult::Exception;
            }
            i16 remainder = static_cast<i16>(dividend % divisor);
            ctx.cpu.gpr.set16(static_cast<u8>(RegIndex::RAX), static_cast<u16>(quotient));
            ctx.cpu.gpr.set16(static_cast<u8>(RegIndex::RDX), static_cast<u16>(remainder));
            return ExecResult::Continue;
        }
        case Opcode::IDIV_RM32: {
            i32 divisor = static_cast<i32>(static_cast<u32>(src));
            if (divisor == 0) {
                ctx.exception = CpuException::DivideError;
                return ExecResult::Exception;
            }
            i64 dividend = static_cast<i64>(
                (static_cast<u64>(ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RDX))) << 32) |
                ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RAX)));
            i64 quotient = dividend / divisor;
            if (quotient > 2147483647LL || quotient < -2147483648LL) {
                ctx.exception = CpuException::DivideError;
                return ExecResult::Exception;
            }
            i32 remainder = static_cast<i32>(dividend % divisor);
            ctx.cpu.gpr.set32(static_cast<u8>(RegIndex::RAX), static_cast<u32>(quotient));
            ctx.cpu.gpr.set32(static_cast<u8>(RegIndex::RDX), static_cast<u32>(remainder));
            return ExecResult::Continue;
        }
        case Opcode::IDIV_RM64: {
            i64 divisor = static_cast<i64>(src);
            if (divisor == 0) {
                ctx.exception = CpuException::DivideError;
                return ExecResult::Exception;
            }
            using i128 = __int128;
            i128 dividend = (static_cast<i128>(static_cast<i64>(ctx.cpu.gpr.get64(static_cast<u8>(RegIndex::RDX)))) << 64) |
                            static_cast<unsigned __int128>(ctx.cpu.gpr.get64(static_cast<u8>(RegIndex::RAX)));
            i128 quotient = dividend / divisor;
            // Check if quotient fits in i64
            if (quotient > static_cast<i128>(0x7FFFFFFFFFFFFFFFLL) ||
                quotient < static_cast<i128>(-0x7FFFFFFFFFFFFFFFLL - 1)) {
                ctx.exception = CpuException::DivideError;
                return ExecResult::Exception;
            }
            i64 remainder = static_cast<i64>(dividend % divisor);
            ctx.cpu.gpr.set64(static_cast<u8>(RegIndex::RAX), static_cast<u64>(quotient));
            ctx.cpu.gpr.set64(static_cast<u8>(RegIndex::RDX), static_cast<u64>(remainder));
            return ExecResult::Continue;
        }
        default:
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
    }
}

// ============================================================================
// CBW/CWDE/CDQE/CWD/CDQ/CQO - Sign Extension
// ============================================================================

ExecResult exec_cdq_cqo(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    switch (instr.opcode) {
        case Opcode::CBW: {
            // AX = sign-extend(AL)
            i8 al = static_cast<i8>(ctx.cpu.gpr.get8l(static_cast<u8>(RegIndex::RAX)));
            ctx.cpu.gpr.set16(static_cast<u8>(RegIndex::RAX), static_cast<u16>(static_cast<i16>(al)));
            return ExecResult::Continue;
        }
        case Opcode::CWDE: {
            // EAX = sign-extend(AX)
            i16 ax = static_cast<i16>(ctx.cpu.gpr.get16(static_cast<u8>(RegIndex::RAX)));
            ctx.cpu.gpr.set32(static_cast<u8>(RegIndex::RAX), static_cast<u32>(static_cast<i32>(ax)));
            return ExecResult::Continue;
        }
        case Opcode::CDQE: {
            // RAX = sign-extend(EAX)
            i32 eax = static_cast<i32>(ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RAX)));
            ctx.cpu.gpr.set64(static_cast<u8>(RegIndex::RAX), static_cast<u64>(static_cast<i64>(eax)));
            return ExecResult::Continue;
        }
        case Opcode::CWD: {
            // DX:AX = sign-extend(AX)
            i16 ax = static_cast<i16>(ctx.cpu.gpr.get16(static_cast<u8>(RegIndex::RAX)));
            i32 extended = static_cast<i32>(ax);
            ctx.cpu.gpr.set16(static_cast<u8>(RegIndex::RAX), static_cast<u16>(extended));
            ctx.cpu.gpr.set16(static_cast<u8>(RegIndex::RDX), static_cast<u16>(static_cast<u32>(extended) >> 16));
            return ExecResult::Continue;
        }
        case Opcode::CDQ: {
            // Intel SDM Vol. 2 CWD/CDQ/CQO: EDX := SignExtend(EAX). RAX is
            // unchanged — a 32-bit write of EAX would zero-extend RAX[63:32].
            i32 eax = static_cast<i32>(ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RAX)));
            ctx.cpu.gpr.set32(static_cast<u8>(RegIndex::RDX),
                              eax < 0 ? 0xFFFFFFFFu : 0u);
            return ExecResult::Continue;
        }
        case Opcode::CQO: {
            // RDX:RAX = sign-extend(RAX)
            i64 rax = static_cast<i64>(ctx.cpu.gpr.get64(static_cast<u8>(RegIndex::RAX)));
            // RDX = all 1s if negative, all 0s if positive
            ctx.cpu.gpr.set64(static_cast<u8>(RegIndex::RDX), static_cast<u64>(rax >> 63));
            return ExecResult::Continue;
        }
        default:
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
    }
}

} // namespace vitreo::cpu::ops
