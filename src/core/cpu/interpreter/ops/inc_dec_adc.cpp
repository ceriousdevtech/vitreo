// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// INC/DEC/NEG/ADC/SBB execution (CPU-020)

#include "exec_ops.hpp"
#include "helpers.hpp"

namespace vitreo::cpu::ops {

// ============================================================================
// INC
// ============================================================================

ExecResult exec_inc(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto dst = read_operand(ctx, instr, 0);
    if (!dst) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u8 size = operand_size(instr.operands[0].type);

    // Preserve CF from a pending JIT FlagOp (flags.cf() is stale until
    // settled). Then retire the op so a later interpreter Jcc cannot
    // rematerialize the compare and undo INC's ZF/SF/OF/PF.
    const bool saved_cf = ctx.cpu.lazy_cf();
    ctx.cpu.set_flag_op(CpuState::FlagOp::None);

    switch (size) {
        case 1: {
            u8 a = static_cast<u8>(dst.value());
            u8 r = a + 1;
            ctx.cpu.flags.update_flags_add(a, static_cast<u8>(1), r);
            auto w = write_operand(ctx, instr, 0, r);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
        case 2: {
            u16 a = static_cast<u16>(dst.value());
            u16 r = a + 1;
            ctx.cpu.flags.update_flags_add(a, static_cast<u16>(1), r);
            auto w = write_operand(ctx, instr, 0, r);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
        case 4: {
            u32 a = static_cast<u32>(dst.value());
            u32 r = a + 1;
            ctx.cpu.flags.update_flags_add(a, static_cast<u32>(1), r);
            auto w = write_operand(ctx, instr, 0, r);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
        default: {
            u64 a = dst.value();
            u64 r = a + 1;
            ctx.cpu.flags.update_flags_add(a, static_cast<u64>(1), r);
            auto w = write_operand(ctx, instr, 0, r);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
    }

    // Restore CF
    ctx.cpu.flags.set_cf(saved_cf);
    return ExecResult::Continue;
}

// ============================================================================
// DEC
// ============================================================================

ExecResult exec_dec(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto dst = read_operand(ctx, instr, 0);
    if (!dst) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u8 size = operand_size(instr.operands[0].type);

    const bool saved_cf = ctx.cpu.lazy_cf();
    ctx.cpu.set_flag_op(CpuState::FlagOp::None);

    switch (size) {
        case 1: {
            u8 a = static_cast<u8>(dst.value());
            u8 r = a - 1;
            ctx.cpu.flags.update_flags_sub(a, static_cast<u8>(1), r);
            auto w = write_operand(ctx, instr, 0, r);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
        case 2: {
            u16 a = static_cast<u16>(dst.value());
            u16 r = a - 1;
            ctx.cpu.flags.update_flags_sub(a, static_cast<u16>(1), r);
            auto w = write_operand(ctx, instr, 0, r);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
        case 4: {
            u32 a = static_cast<u32>(dst.value());
            u32 r = a - 1;
            ctx.cpu.flags.update_flags_sub(a, static_cast<u32>(1), r);
            auto w = write_operand(ctx, instr, 0, r);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
        default: {
            u64 a = dst.value();
            u64 r = a - 1;
            ctx.cpu.flags.update_flags_sub(a, static_cast<u64>(1), r);
            auto w = write_operand(ctx, instr, 0, r);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
    }

    // Restore CF
    ctx.cpu.flags.set_cf(saved_cf);
    return ExecResult::Continue;
}

// ============================================================================
// NEG
// ============================================================================

ExecResult exec_neg(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto dst = read_operand(ctx, instr, 0);
    if (!dst) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u8 size = operand_size(instr.operands[0].type);
    ctx.cpu.set_flag_op(CpuState::FlagOp::None);

    switch (size) {
        case 1: {
            u8 val = static_cast<u8>(dst.value());
            u8 result = static_cast<u8>(0) - val;
            ctx.cpu.flags.update_flags_sub(static_cast<u8>(0), val, result);
            ctx.cpu.flags.set_cf(val != 0);
            auto w = write_operand(ctx, instr, 0, result);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
        case 2: {
            u16 val = static_cast<u16>(dst.value());
            u16 result = static_cast<u16>(0) - val;
            ctx.cpu.flags.update_flags_sub(static_cast<u16>(0), val, result);
            ctx.cpu.flags.set_cf(val != 0);
            auto w = write_operand(ctx, instr, 0, result);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
        case 4: {
            u32 val = static_cast<u32>(dst.value());
            u32 result = static_cast<u32>(0) - val;
            ctx.cpu.flags.update_flags_sub(static_cast<u32>(0), val, result);
            ctx.cpu.flags.set_cf(val != 0);
            auto w = write_operand(ctx, instr, 0, result);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
        default: {
            u64 val = dst.value();
            u64 result = static_cast<u64>(0) - val;
            ctx.cpu.flags.update_flags_sub(static_cast<u64>(0), val, result);
            ctx.cpu.flags.set_cf(val != 0);
            auto w = write_operand(ctx, instr, 0, result);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
    }

    return ExecResult::Continue;
}

// ============================================================================
// ADC
// ============================================================================

ExecResult exec_adc(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto dst = read_operand(ctx, instr, 0);
    auto src = read_operand(ctx, instr, 1);
    if (!dst || !src) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u8 size = operand_size(instr.operands[0].type);
    u64 cf_in = ctx.cpu.lazy_cf() ? 1 : 0;
    ctx.cpu.set_flag_op(CpuState::FlagOp::None);

    switch (size) {
        case 1: {
            u8 a = static_cast<u8>(dst.value());
            u8 b = static_cast<u8>(src.value());
            u16 wide = static_cast<u16>(a) + static_cast<u16>(b) + static_cast<u16>(cf_in);
            u8 r = static_cast<u8>(wide);

            // Flags
            ctx.cpu.flags.update_zsp(r);
            ctx.cpu.flags.set_cf(wide > 0xFF);
            constexpr u8 sign_bit = static_cast<u8>(1) << 7;
            ctx.cpu.flags.set_of(((a ^ r) & (b ^ r) & sign_bit) != 0);
            ctx.cpu.flags.set_af(((a ^ b ^ r) & 0x10) != 0);

            auto w = write_operand(ctx, instr, 0, r);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
        case 2: {
            u16 a = static_cast<u16>(dst.value());
            u16 b = static_cast<u16>(src.value());
            u32 wide = static_cast<u32>(a) + static_cast<u32>(b) + static_cast<u32>(cf_in);
            u16 r = static_cast<u16>(wide);

            ctx.cpu.flags.update_zsp(r);
            ctx.cpu.flags.set_cf(wide > 0xFFFF);
            constexpr u16 sign_bit = static_cast<u16>(1) << 15;
            ctx.cpu.flags.set_of(((a ^ r) & (b ^ r) & sign_bit) != 0);
            ctx.cpu.flags.set_af(((a ^ b ^ r) & 0x10) != 0);

            auto w = write_operand(ctx, instr, 0, r);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
        case 4: {
            u32 a = static_cast<u32>(dst.value());
            u32 b = static_cast<u32>(src.value());
            u64 wide = static_cast<u64>(a) + static_cast<u64>(b) + cf_in;
            u32 r = static_cast<u32>(wide);

            ctx.cpu.flags.update_zsp(r);
            ctx.cpu.flags.set_cf(wide > 0xFFFFFFFFULL);
            constexpr u32 sign_bit = static_cast<u32>(1) << 31;
            ctx.cpu.flags.set_of(((a ^ r) & (b ^ r) & sign_bit) != 0);
            ctx.cpu.flags.set_af(((a ^ b ^ r) & 0x10) != 0);

            auto w = write_operand(ctx, instr, 0, r);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
        default: {
            u64 a = dst.value();
            u64 b = src.value();
            u64 r = a + b + cf_in;

            ctx.cpu.flags.update_zsp(r);
            // CF: carry out of 64-bit add with carry
            ctx.cpu.flags.set_cf((r < a) || (cf_in && r == a));
            constexpr u64 sign_bit = static_cast<u64>(1) << 63;
            ctx.cpu.flags.set_of(((a ^ r) & (b ^ r) & sign_bit) != 0);
            ctx.cpu.flags.set_af(((a ^ b ^ r) & 0x10) != 0);

            auto w = write_operand(ctx, instr, 0, r);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
    }

    return ExecResult::Continue;
}

// ============================================================================
// SBB
// ============================================================================

ExecResult exec_sbb(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto dst = read_operand(ctx, instr, 0);
    auto src = read_operand(ctx, instr, 1);
    if (!dst || !src) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u8 size = operand_size(instr.operands[0].type);
    // Use lazy_cf() so pending lazy flags (e.g. after JIT→interpreter
    // handoff, or NEG that left FlagOp::Neg32) contribute to CF-in.
    u64 cf_in = ctx.cpu.lazy_cf() ? 1 : 0;
    ctx.cpu.set_flag_op(CpuState::FlagOp::None);

    switch (size) {
        case 1: {
            u8 a = static_cast<u8>(dst.value());
            u8 b = static_cast<u8>(src.value());
            u8 r = a - b - static_cast<u8>(cf_in);

            ctx.cpu.flags.update_zsp(r);
            // CF: borrow out
            ctx.cpu.flags.set_cf(static_cast<u16>(a) < static_cast<u16>(b) + static_cast<u16>(cf_in));
            constexpr u8 sign_bit = static_cast<u8>(1) << 7;
            ctx.cpu.flags.set_of(((a ^ b) & (a ^ r) & sign_bit) != 0);
            ctx.cpu.flags.set_af(((a ^ b ^ r) & 0x10) != 0);

            auto w = write_operand(ctx, instr, 0, r);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
        case 2: {
            u16 a = static_cast<u16>(dst.value());
            u16 b = static_cast<u16>(src.value());
            u16 r = a - b - static_cast<u16>(cf_in);

            ctx.cpu.flags.update_zsp(r);
            ctx.cpu.flags.set_cf(static_cast<u32>(a) < static_cast<u32>(b) + static_cast<u32>(cf_in));
            constexpr u16 sign_bit = static_cast<u16>(1) << 15;
            ctx.cpu.flags.set_of(((a ^ b) & (a ^ r) & sign_bit) != 0);
            ctx.cpu.flags.set_af(((a ^ b ^ r) & 0x10) != 0);

            auto w = write_operand(ctx, instr, 0, r);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
        case 4: {
            u32 a = static_cast<u32>(dst.value());
            u32 b = static_cast<u32>(src.value());
            u32 r = a - b - static_cast<u32>(cf_in);

            ctx.cpu.flags.update_zsp(r);
            ctx.cpu.flags.set_cf(static_cast<u64>(a) < static_cast<u64>(b) + cf_in);
            constexpr u32 sign_bit = static_cast<u32>(1) << 31;
            ctx.cpu.flags.set_of(((a ^ b) & (a ^ r) & sign_bit) != 0);
            ctx.cpu.flags.set_af(((a ^ b ^ r) & 0x10) != 0);

            auto w = write_operand(ctx, instr, 0, r);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
        default: {
            u64 a = dst.value();
            u64 b = src.value();
            u64 r = a - b - cf_in;

            ctx.cpu.flags.update_zsp(r);
            // CF: borrow out of 64-bit
            ctx.cpu.flags.set_cf((a < b) || (cf_in && a == b));
            constexpr u64 sign_bit = static_cast<u64>(1) << 63;
            ctx.cpu.flags.set_of(((a ^ b) & (a ^ r) & sign_bit) != 0);
            ctx.cpu.flags.set_af(((a ^ b ^ r) & 0x10) != 0);

            auto w = write_operand(ctx, instr, 0, r);
            if (!w) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            break;
        }
    }

    return ExecResult::Continue;
}

} // namespace vitreo::cpu::ops
