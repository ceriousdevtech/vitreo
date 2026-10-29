// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Bit manipulation instruction execution (BSF, BSR, BT, BTS, BTR, BTC, BSWAP) - CPU-025

#include "exec_ops.hpp"
#include "helpers.hpp"

namespace vitreo::cpu::ops {

namespace {

/// Get operand byte-size from BSF/BSR opcode
u8 bsf_bsr_op_size(Opcode op) noexcept {
    switch (op) {
        case Opcode::BSF_R16_RM16:
        case Opcode::BSR_R16_RM16:
            return 2;
        case Opcode::BSF_R32_RM32:
        case Opcode::BSR_R32_RM32:
            return 4;
        case Opcode::BSF_R64_RM64:
        case Opcode::BSR_R64_RM64:
            return 8;
        default:
            return 4;
    }
}

/// Get operand byte-size from BT/BTS/BTR/BTC opcode
u8 bt_op_size(Opcode op) noexcept {
    switch (op) {
        case Opcode::BT_RM16_R16:
        case Opcode::BT_RM16_IMM8:
        case Opcode::BTS_RM16_R16:
        case Opcode::BTS_RM16_IMM8:
        case Opcode::BTR_RM16_R16:
        case Opcode::BTR_RM16_IMM8:
        case Opcode::BTC_RM16_R16:
        case Opcode::BTC_RM16_IMM8:
            return 2;
        case Opcode::BT_RM32_R32:
        case Opcode::BT_RM32_IMM8:
        case Opcode::BTS_RM32_R32:
        case Opcode::BTS_RM32_IMM8:
        case Opcode::BTR_RM32_R32:
        case Opcode::BTR_RM32_IMM8:
        case Opcode::BTC_RM32_R32:
        case Opcode::BTC_RM32_IMM8:
            return 4;
        case Opcode::BT_RM64_R64:
        case Opcode::BT_RM64_IMM8:
        case Opcode::BTS_RM64_R64:
        case Opcode::BTS_RM64_IMM8:
        case Opcode::BTR_RM64_R64:
        case Opcode::BTR_RM64_IMM8:
        case Opcode::BTC_RM64_R64:
        case Opcode::BTC_RM64_IMM8:
            return 8;
        default:
            return 4;
    }
}

/// Check if this is a BTS opcode (test and set)
bool is_bts(Opcode op) noexcept {
    return op == Opcode::BTS_RM16_R16 || op == Opcode::BTS_RM32_R32 ||
           op == Opcode::BTS_RM64_R64 || op == Opcode::BTS_RM16_IMM8 ||
           op == Opcode::BTS_RM32_IMM8 || op == Opcode::BTS_RM64_IMM8;
}

/// Check if this is a BTR opcode (test and reset)
bool is_btr(Opcode op) noexcept {
    return op == Opcode::BTR_RM16_R16 || op == Opcode::BTR_RM32_R32 ||
           op == Opcode::BTR_RM64_R64 || op == Opcode::BTR_RM16_IMM8 ||
           op == Opcode::BTR_RM32_IMM8 || op == Opcode::BTR_RM64_IMM8;
}

/// Check if this is a BTC opcode (test and complement)
bool is_btc(Opcode op) noexcept {
    return op == Opcode::BTC_RM16_R16 || op == Opcode::BTC_RM32_R32 ||
           op == Opcode::BTC_RM64_R64 || op == Opcode::BTC_RM16_IMM8 ||
           op == Opcode::BTC_RM32_IMM8 || op == Opcode::BTC_RM64_IMM8;
}

} // anonymous namespace

ExecResult exec_bsf(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // BSF r, r/m: operand 0 = destination reg, operand 1 = source r/m
    auto src = read_operand(ctx, instr, 1);
    if (!src) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u8 size = bsf_bsr_op_size(instr.opcode);
    u64 value = src.value();

    // Mask to operand size
    switch (size) {
        case 2: value = static_cast<u16>(value); break;
        case 4: value = static_cast<u32>(value); break;
        default: break;
    }

    if (value == 0) {
        // Source is zero: ZF=1, destination undefined (leave unchanged)
        ctx.cpu.flags.set_zf(true);
        ctx.cpu.flags.set_cf(false);
        return ExecResult::Continue;
    }

    ctx.cpu.flags.set_zf(false);
    // CF is architecturally undefined.
    ctx.cpu.flags.set_cf(true);

    u64 result;
    if (size == 8) {
        result = static_cast<u64>(__builtin_ctzll(value));
    } else {
        result = static_cast<u64>(__builtin_ctz(static_cast<u32>(value)));
    }

    auto wr = write_operand(ctx, instr, 0, result);
    if (!wr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    // PF and SF are architecturally undefined. Compute them from the input
    // value, as a TEST of src would.
    {
        u8 lo = static_cast<u8>(value);
        ctx.cpu.flags.set_pf((__builtin_popcount(lo) & 1) == 0);
        u8 bits = size * 8;
        ctx.cpu.flags.set_sf(((value >> (bits - 1)) & 1) != 0);
        ctx.cpu.flags.set_of(false);
        ctx.cpu.flags.set_af(false);
    }

    return ExecResult::Continue;
}

ExecResult exec_bsr(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // BSR r, r/m: operand 0 = destination reg, operand 1 = source r/m
    auto src = read_operand(ctx, instr, 1);
    if (!src) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u8 size = bsf_bsr_op_size(instr.opcode);
    u64 value = src.value();

    // Mask to operand size
    switch (size) {
        case 2: value = static_cast<u16>(value); break;
        case 4: value = static_cast<u32>(value); break;
        default: break;
    }

    if (value == 0) {
        ctx.cpu.flags.set_zf(true);
        ctx.cpu.flags.set_cf(false);
        return ExecResult::Continue;
    }

    ctx.cpu.flags.set_zf(false);
    // CF is architecturally undefined.
    ctx.cpu.flags.set_cf(true);

    u64 result;
    if (size == 8) {
        result = 63 - static_cast<u64>(__builtin_clzll(value));
    } else if (size == 4) {
        result = 31 - static_cast<u64>(__builtin_clz(static_cast<u32>(value)));
    } else {
        // 16-bit: clz on u32 gives leading zeros of 32-bit value, so adjust
        result = 15 - static_cast<u64>(__builtin_clz(static_cast<u32>(value)) - 16);
    }

    auto wr = write_operand(ctx, instr, 0, result);
    if (!wr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    // PF and SF are architecturally undefined. Compute them from the input
    // value, as a TEST of src would.
    {
        u8 lo = static_cast<u8>(value);
        ctx.cpu.flags.set_pf((__builtin_popcount(lo) & 1) == 0);
        u8 bits = size * 8;
        ctx.cpu.flags.set_sf(((value >> (bits - 1)) & 1) != 0);
        ctx.cpu.flags.set_of(false);
        ctx.cpu.flags.set_af(false);
    }

    return ExecResult::Continue;
}

ExecResult exec_bt(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // BT/BTS/BTR/BTC: operand 0 = bit base (r/m), operand 1 = bit index (reg or imm8)
    auto bit_idx = read_operand(ctx, instr, 1);
    if (!bit_idx) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u8 size = bt_op_size(instr.opcode);
    u8 bits = size * 8;

    // Memory bit-base: the bit index addresses a bit *string*. Unlike the
    // register form, it is NOT reduced modulo the operand size — a register
    // offset is signed and can select a bit far outside the addressed operand,
    // shifting which byte is touched (byte = addr + (index >> 3), bit =
    // index & 7). Masking it (as the register form does) makes every index
    // alias into the low chunk by index % bits, which silently corrupts any
    // bitmap built with `bts [mem], reg` — e.g. the 256-bit character tables in
    // strcspn/strspn/strpbrk, where ':' (0x3a) and 'z' (0x7a) would collide at
    // bit 26 and truncate host names mid-string.
    if (is_memory_operand(instr.operands[0].type)) {
        auto base = calc_effective_address(ctx, instr.operands[0].mem);
        if (!base) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        // Register offsets are signed at their operand width; an imm8 offset is
        // an unsigned bit position (0..255).
        i64 idx;
        if (is_immediate_operand(instr.operands[1].type)) {
            idx = static_cast<i64>(bit_idx.value() & 0xFF);
        } else {
            const u64 raw = bit_idx.value();
            switch (operand_size(instr.operands[1].type)) {
                case 2:  idx = static_cast<i16>(raw); break;
                case 8:  idx = static_cast<i64>(raw); break;
                default: idx = static_cast<i32>(raw); break;
            }
        }
        const i64 byte_off = idx >> 3;             // arithmetic (floor) /8
        const u32 bit_in_byte = static_cast<u32>(idx & 7);
        const u64 addr = base.value() + static_cast<u64>(byte_off);
        auto byte = read_memory(ctx, addr, 1);
        if (!byte) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u8 bv = static_cast<u8>(byte.value());
        ctx.cpu.write_cf_only(((bv >> bit_in_byte) & 1) != 0);

        if (is_bts(instr.opcode)) {
            bv |= static_cast<u8>(1u << bit_in_byte);
        } else if (is_btr(instr.opcode)) {
            bv &= static_cast<u8>(~(1u << bit_in_byte));
        } else if (is_btc(instr.opcode)) {
            bv ^= static_cast<u8>(1u << bit_in_byte);
        } else {
            return ExecResult::Continue;  // plain BT: no write-back
        }
        auto wr = write_memory(ctx, addr, bv, 1);
        if (!wr) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        return ExecResult::Continue;
    }

    // Register bit-base: index taken modulo the operand size.
    auto base_val = read_operand(ctx, instr, 0);
    if (!base_val) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    u64 value = base_val.value();
    u64 bit_pos = static_cast<u64>(bit_idx.value()) % bits;

    // CF = selected bit
    bool cf = (value >> bit_pos) & 1;
    ctx.cpu.write_cf_only(cf);

    // BTS/BTR/BTC modify the value and write back
    if (is_bts(instr.opcode)) {
        value |= (1ULL << bit_pos);
    } else if (is_btr(instr.opcode)) {
        value &= ~(1ULL << bit_pos);
    } else if (is_btc(instr.opcode)) {
        value ^= (1ULL << bit_pos);
    } else {
        // Plain BT: no write-back needed
        return ExecResult::Continue;
    }

    // Mask to operand size
    switch (size) {
        case 2: value = static_cast<u16>(value); break;
        case 4: value = static_cast<u32>(value); break;
        default: break;
    }

    auto wr = write_operand(ctx, instr, 0, value);
    if (!wr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    return ExecResult::Continue;
}

ExecResult exec_bswap(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto val = read_operand(ctx, instr, 0);
    if (!val) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u64 result;
    if (instr.opcode == Opcode::BSWAP_R64) {
        result = __builtin_bswap64(val.value());
    } else {
        result = __builtin_bswap32(static_cast<u32>(val.value()));
    }

    auto wr = write_operand(ctx, instr, 0, result);
    if (!wr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    return ExecResult::Continue;
}

ExecResult exec_popcnt(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto src = read_operand(ctx, instr, 1);
    if (!src) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u8 size = operand_size(instr.operands[0].type);
    u64 val = src.value();
    u32 count;
    if (size == 8) {
        count = static_cast<u32>(__builtin_popcountll(val));
    } else if (size == 4) {
        count = static_cast<u32>(__builtin_popcount(static_cast<u32>(val)));
    } else {
        count = static_cast<u32>(__builtin_popcount(
            static_cast<u32>(val) & 0xFFFF));
    }

    auto wr = write_operand(ctx, instr, 0, count);
    if (!wr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    ctx.cpu.flags.set_zf(count == 0);
    ctx.cpu.flags.set_of(false);
    ctx.cpu.flags.set_sf(false);
    ctx.cpu.flags.set_cf(false);
    ctx.cpu.flags.set_pf(false);
    ctx.cpu.flags.set_af(false);
    return ExecResult::Continue;
}

} // namespace vitreo::cpu::ops
