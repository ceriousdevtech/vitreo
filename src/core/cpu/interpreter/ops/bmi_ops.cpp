// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// BMI1/BMI2 instruction execution (ANDN, BEXTR, BLSI, BLSMSK, BLSR, BZHI,
// MULX, PDEP, PEXT, RORX, SARX, SHRX, SHLX, TZCNT, LZCNT, POPCNT)

#include "exec_ops.hpp"
#include "helpers.hpp"

namespace vitreo::cpu::ops {

namespace {

// Read r/m32 operand value as u32
Result<u32, Error> read_rm32(ExecContext& ctx, const DecodedInstruction& instr, int idx) noexcept {
    auto val = read_operand(ctx, instr, idx);
    if (!val) return Result<u32, Error>::err(val.error());
    return Result<u32, Error>::ok(static_cast<u32>(val.value()));
}

// Compute PF for low byte
bool compute_pf(u32 result) noexcept {
    u8 low = static_cast<u8>(result);
    u8 p = low ^ (low >> 4);
    p ^= (p >> 2);
    p ^= (p >> 1);
    return (~p) & 1;
}

// Update SF, ZF, PF from 32-bit result (common BMI1 pattern)
void update_szp_flags(ExecContext& ctx, u32 result) noexcept {
    ctx.cpu.flags.set_sf((result >> 31) & 1);
    ctx.cpu.flags.set_zf(result == 0);
    ctx.cpu.flags.set_pf(compute_pf(result));
}

} // anonymous namespace

ExecResult exec_bmi_ops(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    switch (instr.opcode) {

    // =========================================================================
    // ANDN r32, r32, r/m32 — dest = ~src1 & src2
    // VEX.NDS: vvvv = src1, ModR/M reg = dest, r/m = src2
    // Flags: SF, ZF set per result; OF=CF=0; AF undefined
    // =========================================================================
    case Opcode::ANDN_R32_R32_RM32: {
        u32 src1 = ctx.cpu.gpr.get32(instr.prefixes.vex_vvvv);
        auto src2 = read_rm32(ctx, instr, 1);
        if (!src2) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u32 result = ~src1 & src2.value();
        ctx.cpu.gpr.set32(instr.operands[0].reg, result);
        update_szp_flags(ctx, result);
        ctx.cpu.flags.set_of(false);
        ctx.cpu.flags.set_cf(false);
        return ExecResult::Continue;
    }

    // =========================================================================
    // BEXTR r32, r/m32, r32 — bit field extract
    // VEX.NDS: reg = dest, r/m = source, vvvv = control
    // Flags: ZF set if result is 0; OF=CF=0
    // =========================================================================
    case Opcode::BEXTR_R32_RM32_R32: {
        auto src = read_rm32(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u32 ctrl = ctx.cpu.gpr.get32(instr.prefixes.vex_vvvv);
        u8 start = ctrl & 0xFF;
        u8 len = (ctrl >> 8) & 0xFF;
        u32 result = 0;
        if (start < 32 && len > 0) {
            result = src.value() >> start;
            if (len < 32) result &= (1u << len) - 1;
        }
        ctx.cpu.gpr.set32(instr.operands[0].reg, result);
        ctx.cpu.flags.set_zf(result == 0);
        ctx.cpu.flags.set_of(false);
        ctx.cpu.flags.set_cf(false);
        return ExecResult::Continue;
    }

    // =========================================================================
    // BLSI r32, r/m32 — isolate lowest set bit: dest = src & (-src)
    // VEX.NDD: vvvv = dest, r/m = source (operand[0])
    // Flags: SF, ZF per result; CF = (source != 0); OF=0
    // =========================================================================
    case Opcode::BLSI_R32_RM32: {
        auto src = read_rm32(ctx, instr, 0);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u32 val = src.value();
        u32 result = val & (static_cast<u32>(-static_cast<i32>(val)));
        ctx.cpu.gpr.set32(instr.prefixes.vex_vvvv, result);
        update_szp_flags(ctx, result);
        ctx.cpu.flags.set_cf(val != 0);
        ctx.cpu.flags.set_of(false);
        return ExecResult::Continue;
    }

    // =========================================================================
    // BLSMSK r32, r/m32 — mask up to lowest set bit: dest = src ^ (src - 1)
    // VEX.NDD: vvvv = dest, r/m = source (operand[0])
    // Flags: SF per result; ZF=0; CF = (source == 0); OF=0
    // =========================================================================
    case Opcode::BLSMSK_R32_RM32: {
        auto src = read_rm32(ctx, instr, 0);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u32 val = src.value();
        u32 result = val ^ (val - 1);
        ctx.cpu.gpr.set32(instr.prefixes.vex_vvvv, result);
        update_szp_flags(ctx, result);
        ctx.cpu.flags.set_cf(val == 0);
        ctx.cpu.flags.set_of(false);
        return ExecResult::Continue;
    }

    // =========================================================================
    // BLSR r32, r/m32 — reset lowest set bit: dest = src & (src - 1)
    // VEX.NDD: vvvv = dest, r/m = source (operand[0])
    // Flags: SF, ZF per result; CF = (source == 0); OF=0
    // =========================================================================
    case Opcode::BLSR_R32_RM32: {
        auto src = read_rm32(ctx, instr, 0);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u32 val = src.value();
        u32 result = val & (val - 1);
        ctx.cpu.gpr.set32(instr.prefixes.vex_vvvv, result);
        update_szp_flags(ctx, result);
        ctx.cpu.flags.set_cf(val == 0);
        ctx.cpu.flags.set_of(false);
        return ExecResult::Continue;
    }

    // =========================================================================
    // BZHI r32, r/m32, r32 — zero high bits starting from bit position
    // VEX.NDS: reg = dest, r/m = source, vvvv = index (bits[7:0])
    // Flags: SF, ZF per result; CF = (index[7:0] > 31); OF=0
    // =========================================================================
    case Opcode::BZHI_R32_RM32_R32: {
        auto src = read_rm32(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u32 index = ctx.cpu.gpr.get32(instr.prefixes.vex_vvvv) & 0xFF;
        u32 result;
        if (index >= 32) {
            result = src.value();
        } else {
            result = src.value() & ((1u << index) - 1);
        }
        ctx.cpu.gpr.set32(instr.operands[0].reg, result);
        update_szp_flags(ctx, result);
        ctx.cpu.flags.set_cf(index > 31);
        ctx.cpu.flags.set_of(false);
        return ExecResult::Continue;
    }

    // =========================================================================
    // MULX r32a, r32b, r/m32 — unsigned multiply EDX * src → hi:lo
    // VEX.NDD: reg = hi dest, vvvv = lo dest, r/m = source, implicit EDX
    // Flags: none modified
    // =========================================================================
    case Opcode::MULX_R32_R32_RM32: {
        auto src = read_rm32(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u64 edx_val = ctx.cpu.gpr.get32(2); // EDX
        u64 product = edx_val * static_cast<u64>(src.value());
        u32 lo = static_cast<u32>(product);
        u32 hi = static_cast<u32>(product >> 32);
        ctx.cpu.gpr.set32(instr.prefixes.vex_vvvv, lo);
        ctx.cpu.gpr.set32(instr.operands[0].reg, hi);
        return ExecResult::Continue;
    }

    // =========================================================================
    // PDEP r32, r32, r/m32 — parallel bits deposit
    // VEX.NDS: reg = dest, vvvv = source, r/m = mask
    // Flags: none modified
    // =========================================================================
    case Opcode::PDEP_R32_R32_RM32: {
        u32 source = ctx.cpu.gpr.get32(instr.prefixes.vex_vvvv);
        auto mask_val = read_rm32(ctx, instr, 1);
        if (!mask_val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u32 mask = mask_val.value();
        u32 result = 0;
        u32 src_bit = 0;
        for (u32 bit = 0; bit < 32 && mask != 0; bit++) {
            if (mask & 1) {
                if (source & (1u << src_bit)) result |= (1u << bit);
                src_bit++;
            }
            mask >>= 1;
        }
        ctx.cpu.gpr.set32(instr.operands[0].reg, result);
        return ExecResult::Continue;
    }

    // =========================================================================
    // PEXT r32, r32, r/m32 — parallel bits extract
    // VEX.NDS: reg = dest, vvvv = source, r/m = mask
    // Flags: none modified
    // =========================================================================
    case Opcode::PEXT_R32_R32_RM32: {
        u32 source = ctx.cpu.gpr.get32(instr.prefixes.vex_vvvv);
        auto mask_val = read_rm32(ctx, instr, 1);
        if (!mask_val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u32 mask = mask_val.value();
        u32 result = 0;
        u32 dst_bit = 0;
        for (u32 bit = 0; bit < 32 && mask != 0; bit++) {
            if (mask & 1) {
                if (source & (1u << bit)) result |= (1u << dst_bit);
                dst_bit++;
            }
            mask >>= 1;
        }
        ctx.cpu.gpr.set32(instr.operands[0].reg, result);
        return ExecResult::Continue;
    }

    // =========================================================================
    // RORX r32, r/m32, imm8 — rotate right without affecting flags
    // =========================================================================
    case Opcode::RORX_R32_RM32_IMM8: {
        auto src = read_rm32(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u8 count = static_cast<u8>(instr.operands[2].imm) & 0x1F;
        u32 val = src.value();
        u32 result = (count == 0) ? val : (val >> count) | (val << (32 - count));
        ctx.cpu.gpr.set32(instr.operands[0].reg, result);
        return ExecResult::Continue;
    }

    // =========================================================================
    // SARX r32, r/m32, r32 — arithmetic shift right without flags
    // VEX.NDS: reg = dest, r/m = source, vvvv = shift count
    // =========================================================================
    case Opcode::SARX_R32_RM32_R32: {
        auto src = read_rm32(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u32 count = ctx.cpu.gpr.get32(instr.prefixes.vex_vvvv) & 0x1F;
        i32 val = static_cast<i32>(src.value());
        u32 result = static_cast<u32>(val >> count);
        ctx.cpu.gpr.set32(instr.operands[0].reg, result);
        return ExecResult::Continue;
    }

    // =========================================================================
    // SHRX r32, r/m32, r32 — logical shift right without flags
    // =========================================================================
    case Opcode::SHRX_R32_RM32_R32: {
        auto src = read_rm32(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u32 count = ctx.cpu.gpr.get32(instr.prefixes.vex_vvvv) & 0x1F;
        u32 result = src.value() >> count;
        ctx.cpu.gpr.set32(instr.operands[0].reg, result);
        return ExecResult::Continue;
    }

    // =========================================================================
    // SHLX r32, r/m32, r32 — logical shift left without flags
    // =========================================================================
    case Opcode::SHLX_R32_RM32_R32: {
        auto src = read_rm32(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u32 count = ctx.cpu.gpr.get32(instr.prefixes.vex_vvvv) & 0x1F;
        u32 result = src.value() << count;
        ctx.cpu.gpr.set32(instr.operands[0].reg, result);
        return ExecResult::Continue;
    }

    // =========================================================================
    // TZCNT r32, r/m32 — count trailing zero bits
    // Flags: CF = (source == 0); ZF = (result == 0)
    // =========================================================================
    case Opcode::TZCNT_R32_RM32: {
        auto src = read_operand(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u32 val = static_cast<u32>(src.value());
        u32 result = (val == 0) ? 32 : static_cast<u32>(__builtin_ctz(val));
        auto wr = write_operand(ctx, instr, 0, result);
        if (!wr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        ctx.cpu.flags.set_cf(val == 0);
        ctx.cpu.flags.set_zf(result == 0);
        return ExecResult::Continue;
    }

    // =========================================================================
    // LZCNT r32, r/m32 — count leading zero bits
    // Flags: CF = (source == 0); ZF = (result == 0)
    // =========================================================================
    case Opcode::LZCNT_R32_RM32: {
        auto src = read_operand(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u32 val = static_cast<u32>(src.value());
        u32 result = (val == 0) ? 32 : static_cast<u32>(__builtin_clz(val));
        auto wr = write_operand(ctx, instr, 0, result);
        if (!wr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        ctx.cpu.flags.set_cf(val == 0);
        ctx.cpu.flags.set_zf(result == 0);
        return ExecResult::Continue;
    }

    // =========================================================================
    // TZCNT r16, r/m16 — count trailing zero bits (16-bit form)
    // Only low 16 bits of destination are written; upper 16 bits preserved.
    // =========================================================================
    case Opcode::TZCNT_R16_RM16: {
        auto src = read_operand(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u16 val = static_cast<u16>(src.value());
        u16 result = (val == 0) ? 16 : static_cast<u16>(__builtin_ctz(val));
        auto wr = write_operand(ctx, instr, 0, result);
        if (!wr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        ctx.cpu.flags.set_cf(val == 0);
        ctx.cpu.flags.set_zf(result == 0);
        return ExecResult::Continue;
    }

    // =========================================================================
    // LZCNT r16, r/m16 — count leading zero bits (16-bit form)
    // =========================================================================
    case Opcode::LZCNT_R16_RM16: {
        auto src = read_operand(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u16 val = static_cast<u16>(src.value());
        u16 result;
        if (val == 0) result = 16;
        else {
            // __builtin_clz takes u32; shift val into high bits to count from MSB of 16.
            result = static_cast<u16>(__builtin_clz(static_cast<u32>(val)) - 16);
        }
        auto wr = write_operand(ctx, instr, 0, result);
        if (!wr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        ctx.cpu.flags.set_cf(val == 0);
        ctx.cpu.flags.set_zf(result == 0);
        return ExecResult::Continue;
    }

    // =========================================================================
    // POPCNT r32, r/m32 — count set bits
    // Flags: ZF = (result == 0); OF=SF=AF=CF=PF=0
    // =========================================================================
    case Opcode::POPCNT_R32_RM32: {
        auto src = read_operand(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u32 result = static_cast<u32>(__builtin_popcount(static_cast<u32>(src.value())));
        auto wr = write_operand(ctx, instr, 0, result);
        if (!wr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        ctx.cpu.flags.set_zf(result == 0);
        ctx.cpu.flags.set_of(false);
        ctx.cpu.flags.set_sf(false);
        ctx.cpu.flags.set_cf(false);
        ctx.cpu.flags.set_pf(false);
        ctx.cpu.flags.set_af(false);
        return ExecResult::Continue;
    }

    case Opcode::ANDN_R64_R64_RM64: {
        u64 src1 = ctx.cpu.gpr.get64(instr.prefixes.vex_vvvv);
        auto src2 = read_operand(ctx, instr, 1);
        if (!src2) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u64 result = ~src1 & src2.value();
        ctx.cpu.gpr.set64(instr.operands[0].reg, result);
        ctx.cpu.flags.set_sf((result >> 63) & 1);
        ctx.cpu.flags.set_zf(result == 0);
        ctx.cpu.flags.set_pf(compute_pf(static_cast<u32>(result)));
        ctx.cpu.flags.set_of(false);
        ctx.cpu.flags.set_cf(false);
        return ExecResult::Continue;
    }

    case Opcode::BEXTR_R64_RM64_R64: {
        auto src = read_operand(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u64 ctrl = ctx.cpu.gpr.get64(instr.prefixes.vex_vvvv);
        u8 start = static_cast<u8>(ctrl);
        u8 len = static_cast<u8>(ctrl >> 8);
        u64 result = 0;
        if (start < 64 && len > 0) {
            result = src.value() >> start;
            if (len < 64) result &= (1ull << len) - 1;
        }
        ctx.cpu.gpr.set64(instr.operands[0].reg, result);
        ctx.cpu.flags.set_zf(result == 0);
        ctx.cpu.flags.set_of(false);
        ctx.cpu.flags.set_cf(false);
        return ExecResult::Continue;
    }

    case Opcode::BLSI_R64_RM64: {
        auto src = read_operand(ctx, instr, 0);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u64 val = src.value();
        u64 result = val & (0ull - val);
        ctx.cpu.gpr.set64(instr.prefixes.vex_vvvv, result);
        ctx.cpu.flags.set_sf((result >> 63) & 1);
        ctx.cpu.flags.set_zf(result == 0);
        ctx.cpu.flags.set_pf(compute_pf(static_cast<u32>(result)));
        ctx.cpu.flags.set_cf(val != 0);
        ctx.cpu.flags.set_of(false);
        return ExecResult::Continue;
    }

    case Opcode::BLSMSK_R64_RM64: {
        auto src = read_operand(ctx, instr, 0);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u64 val = src.value();
        u64 result = val ^ (val - 1);
        ctx.cpu.gpr.set64(instr.prefixes.vex_vvvv, result);
        ctx.cpu.flags.set_sf((result >> 63) & 1);
        ctx.cpu.flags.set_zf(false);
        ctx.cpu.flags.set_pf(compute_pf(static_cast<u32>(result)));
        ctx.cpu.flags.set_cf(val == 0);
        ctx.cpu.flags.set_of(false);
        return ExecResult::Continue;
    }

    case Opcode::BLSR_R64_RM64: {
        auto src = read_operand(ctx, instr, 0);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u64 val = src.value();
        u64 result = val & (val - 1);
        ctx.cpu.gpr.set64(instr.prefixes.vex_vvvv, result);
        ctx.cpu.flags.set_sf((result >> 63) & 1);
        ctx.cpu.flags.set_zf(result == 0);
        ctx.cpu.flags.set_pf(compute_pf(static_cast<u32>(result)));
        ctx.cpu.flags.set_cf(val == 0);
        ctx.cpu.flags.set_of(false);
        return ExecResult::Continue;
    }

    case Opcode::BZHI_R64_RM64_R64: {
        auto src = read_operand(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u64 index = ctx.cpu.gpr.get64(instr.prefixes.vex_vvvv) & 0xFF;
        u64 result;
        if (index >= 64) result = src.value();
        else if (index == 0) result = 0;
        else result = src.value() & ((1ull << index) - 1);
        ctx.cpu.gpr.set64(instr.operands[0].reg, result);
        ctx.cpu.flags.set_sf((result >> 63) & 1);
        ctx.cpu.flags.set_zf(result == 0);
        ctx.cpu.flags.set_pf(compute_pf(static_cast<u32>(result)));
        ctx.cpu.flags.set_cf(index > 63);
        ctx.cpu.flags.set_of(false);
        return ExecResult::Continue;
    }

    case Opcode::MULX_R64_R64_RM64: {
        auto src = read_operand(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        __uint128_t product = static_cast<__uint128_t>(ctx.cpu.gpr.get64(2)) * src.value();
        ctx.cpu.gpr.set64(instr.prefixes.vex_vvvv, static_cast<u64>(product));
        ctx.cpu.gpr.set64(instr.operands[0].reg, static_cast<u64>(product >> 64));
        return ExecResult::Continue;
    }

    case Opcode::PDEP_R64_R64_RM64: {
        u64 source = ctx.cpu.gpr.get64(instr.prefixes.vex_vvvv);
        auto mask_val = read_operand(ctx, instr, 1);
        if (!mask_val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u64 mask = mask_val.value();
        u64 result = 0;
        u32 src_bit = 0;
        for (u32 bit = 0; bit < 64 && mask != 0; bit++) {
            if (mask & 1) {
                if (source & (1ull << src_bit)) result |= (1ull << bit);
                src_bit++;
            }
            mask >>= 1;
        }
        ctx.cpu.gpr.set64(instr.operands[0].reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PEXT_R64_R64_RM64: {
        u64 source = ctx.cpu.gpr.get64(instr.prefixes.vex_vvvv);
        auto mask_val = read_operand(ctx, instr, 1);
        if (!mask_val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u64 mask = mask_val.value();
        u64 result = 0;
        u32 dst_bit = 0;
        for (u32 bit = 0; bit < 64 && mask != 0; bit++) {
            if (mask & 1) {
                if (source & (1ull << bit)) result |= (1ull << dst_bit);
                dst_bit++;
            }
            mask >>= 1;
        }
        ctx.cpu.gpr.set64(instr.operands[0].reg, result);
        return ExecResult::Continue;
    }

    case Opcode::RORX_R64_RM64_IMM8: {
        auto src = read_operand(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u8 count = static_cast<u8>(instr.operands[2].imm) & 0x3F;
        u64 val = src.value();
        u64 result = (count == 0) ? val : ((val >> count) | (val << (64 - count)));
        ctx.cpu.gpr.set64(instr.operands[0].reg, result);
        return ExecResult::Continue;
    }

    case Opcode::SARX_R64_RM64_R64: {
        auto src = read_operand(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u64 count = ctx.cpu.gpr.get64(instr.prefixes.vex_vvvv) & 0x3F;
        i64 val = static_cast<i64>(src.value());
        ctx.cpu.gpr.set64(instr.operands[0].reg, static_cast<u64>(val >> count));
        return ExecResult::Continue;
    }

    case Opcode::SHRX_R64_RM64_R64: {
        auto src = read_operand(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u64 count = ctx.cpu.gpr.get64(instr.prefixes.vex_vvvv) & 0x3F;
        ctx.cpu.gpr.set64(instr.operands[0].reg, src.value() >> count);
        return ExecResult::Continue;
    }

    case Opcode::SHLX_R64_RM64_R64: {
        auto src = read_operand(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u64 count = ctx.cpu.gpr.get64(instr.prefixes.vex_vvvv) & 0x3F;
        ctx.cpu.gpr.set64(instr.operands[0].reg, src.value() << count);
        return ExecResult::Continue;
    }

    case Opcode::TZCNT_R64_RM64: {
        auto src = read_operand(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u64 val = src.value();
        u64 result = (val == 0) ? 64 : static_cast<u64>(__builtin_ctzll(val));
        auto wr = write_operand(ctx, instr, 0, result);
        if (!wr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        ctx.cpu.flags.set_cf(val == 0);
        ctx.cpu.flags.set_zf(result == 0);
        return ExecResult::Continue;
    }

    case Opcode::LZCNT_R64_RM64: {
        auto src = read_operand(ctx, instr, 1);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u64 val = src.value();
        u64 result = (val == 0) ? 64 : static_cast<u64>(__builtin_clzll(val));
        auto wr = write_operand(ctx, instr, 0, result);
        if (!wr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        ctx.cpu.flags.set_cf(val == 0);
        ctx.cpu.flags.set_zf(result == 0);
        return ExecResult::Continue;
    }

    default:
        ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }
}

} // namespace vitreo::cpu::ops
