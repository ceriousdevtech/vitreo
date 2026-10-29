// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — compile_memory_ops' 32/16-bit bit-test family: BT, BTS, BTR
// and BTC r/m32 and r/m16 with an imm8 or register bit offset, and the
// emit_bt_write_cf / emit_add_bit_base_offset members they share with
// bit_ops64.cpp.
//
// One family of compile_memory_ops's opcode space, split out of
// ops/memory_ops.cpp as pure code motion: the case arms are verbatim and in
// their original order. compile_memory_ops (ops/memory_ops.cpp) owns the
// dispatch chain and the block tail.

#include "../jit_helpers.hpp"
#include "core/cpu/lock_prefix.hpp"

#include <atomic>
#include <optional>

namespace vitreo::cpu::jit {

// Intel SDM Vol. 2, BT/BTS/BTR/BTC: with a memory bit base and a register bit
// offset the processor accesses the word/dword at
// BitBase + (BitOffset DIV operand_size) * operand_size_in_bytes, and the
// offset is a *signed* value of the operand size. The byte offset therefore
// has to be sign-extended and added to the full effective address: in long
// mode that address is 64-bit, so folding it with a 32-bit ADD truncated every
// bit base above 4 GB and made BTS/BTR/BTC write into unrelated low memory.
// A 32-bit guest keeps 32-bit wraparound, which is what x86 does there.
// BT/BTS/BTR/BTC define CF and leave the other arithmetic flags undefined, so
// the tested bit goes out through the CF overlay: a pending ALU FlagOp stays
// intact and can still answer ZF. `bit_reg` holds 0 or 1.
void JitCompiler::emit_bt_write_cf(u8 bit_reg) {
    emit(arm64::add_w_imm(REG_SCRATCH3, bit_reg, 1));
    emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, cf_overlay_offset()));
    const u32 rf_off = cpu_rflags_offset();
    emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
    emit(arm64::movz_w(REG_SCRATCH2, 1));
    emit(0x8A220063u);  // BIC X3, X3, X2
    emit(0xAA000063u | (static_cast<u32>(bit_reg) << 16));  // ORR X3, X3, Xbit
    emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
}

void JitCompiler::emit_add_bit_base_offset(u8 ea_reg, u8 byte_off_reg) {
    if (m_block_is_64bit) {
        emit(arm64::sxtw_x(byte_off_reg, byte_off_reg));
        emit(arm64::add_x(ea_reg, ea_reg, byte_off_reg));
    } else {
        emit(arm64::add_w(ea_reg, ea_reg, byte_off_reg));
    }
}

// nullopt = not in this family; false = declined; true = the arm reached
// `break`. See jit_compiler_lowering.hpp.
std::optional<bool> JitCompiler::compile_memory_ops_bt(
    const DecodedInstruction& instr, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];

    switch (instr.opcode) {

    // ---- BT / BTS / BTR / BTC r/m32, imm8 ----
    // Intel SDM Vol. 2: imm8 selects a bit in the operand. Memory form with
    // imm >= 32 addresses outside this dword — leave that to the interpreter.
    case Opcode::BT_RM32_IMM8:
    case Opcode::BTS_RM32_IMM8:
    case Opcode::BTR_RM32_IMM8:
    case Opcode::BTC_RM32_IMM8: {
        bool src_is_mem = !is_register_operand(op0.type);
        if (src_is_mem && (op1.imm & ~static_cast<i64>(31)) != 0) return false;
        if (src_is_mem) {
            emit_compute_ea(REG_CALL_TMP, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_CALL_TMP);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }
        u8 bit_pos = static_cast<u8>(op1.imm & 31);
        // UBFX W4, W0, #bit_pos, #1 — selected bit is CF; leave FlagOp intact.
        emit(arm64::ubfx_w(REG_SCRATCH4, REG_SCRATCH0, bit_pos, 1));
        // CF before the write-back: a memory store can BLR the slow-path
        // helper, which clobbers the caller-saved W4 holding the tested bit.
        emit_bt_write_cf(REG_SCRATCH4);
        if (instr.opcode != Opcode::BT_RM32_IMM8) {
            emit_mov_imm32(REG_SCRATCH2, 1u << bit_pos);
            switch (instr.opcode) {
            case Opcode::BTS_RM32_IMM8:
                emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2));
                break;
            case Opcode::BTR_RM32_IMM8:
                emit(0x0A200000u | (static_cast<u32>(REG_SCRATCH2) << 16)
                                 | (static_cast<u32>(REG_SCRATCH0) << 5)
                                 | static_cast<u32>(REG_SCRATCH0));
                break;
            case Opcode::BTC_RM32_IMM8:
                emit(arm64::eor_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2));
                break;
            default:
                break;
            }
            if (src_is_mem) {
                emit_guest_store32(REG_SCRATCH0, REG_CALL_TMP);
            } else {
                emit_store_gpr32(REG_SCRATCH0, op0.reg);
            }
        }
        break;
    }

    // ---- BT / BTS / BTR / BTC r/m32, r32 (register form) ----
    // Reads bit (src & 31) from dst, writes it to CF. BTS/BTR/BTC also
    // modify dst accordingly. Arith flags other than CF are undefined;
    // fuzz mask compares CF+ZF and we preserve ZF.
    case Opcode::BT_RM32_R32:
    case Opcode::BTS_RM32_R32:
    case Opcode::BTR_RM32_R32:
    case Opcode::BTC_RM32_R32: {
        const bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            // SDM: CF = Bit[BitBase + (BitOffset/32)*4, BitOffset MOD 32]
            emit_compute_ea(REG_CALL_TMP, op0.mem, next_rip);
            emit_load_gpr32(REG_SCRATCH1, op1.reg);
            emit(arm64::asr_w_imm(REG_SCRATCH3, REG_SCRATCH1, 5));
            emit(arm64::lsl_w_imm(REG_SCRATCH3, REG_SCRATCH3, 2));
            emit_add_bit_base_offset(REG_CALL_TMP, REG_SCRATCH3);
            emit_guest_load32(REG_SCRATCH0, REG_CALL_TMP);
            // The load can BLR a helper, which clobbers the caller-saved W1.
            // The guest register is untouched by BT, so re-read it.
            emit_load_gpr32(REG_SCRATCH1, op1.reg);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
            emit_load_gpr32(REG_SCRATCH1, op1.reg);
        }
        emit_mov_imm32(REG_SCRATCH2, 31);
        emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH2)); // bit &= 31

        // bit_val = (dst >> bit) & 1  -> REG_SCRATCH4 (so the later BIC/ORR
        // pattern using Rm=SCRATCH4 encodes identically to BT_IMM8).
        emit(arm64::lsrv_w(REG_SCRATCH4, REG_SCRATCH0, REG_SCRATCH1));
        emit_mov_imm32(REG_SCRATCH2, 1);
        emit(arm64::and_w(REG_SCRATCH4, REG_SCRATCH4, REG_SCRATCH2));

        // CF before the write-back: a memory store can BLR the slow-path
        // helper, which clobbers the caller-saved W4 holding the tested bit.
        emit_bt_write_cf(REG_SCRATCH4);

        if (instr.opcode != Opcode::BT_RM32_R32) {
            // mask = 1 << bit   -> REG_SCRATCH2
            emit_mov_imm32(REG_SCRATCH2, 1);
            emit(arm64::lslv_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH1));
            switch (instr.opcode) {
            case Opcode::BTS_RM32_R32:
                emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2));
                break;
            case Opcode::BTR_RM32_R32:
                // BIC W0, W0, W2  (32-bit). Encoding: 0x0A200000 | (Rm<<16)
                // | (Rn<<5) | Rd.
                emit(0x0A200000u | (static_cast<u32>(REG_SCRATCH2) << 16)
                                 | (static_cast<u32>(REG_SCRATCH0) << 5)
                                 | static_cast<u32>(REG_SCRATCH0));
                break;
            case Opcode::BTC_RM32_R32:
                emit(arm64::eor_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2));
                break;
            default: break;
            }
            if (dst_is_mem) {
                emit_guest_store32(REG_SCRATCH0, REG_CALL_TMP);
            } else {
                emit_store_gpr32(REG_SCRATCH0, op0.reg);
            }
        }
        break;
    }

    // ---- BT / BTS / BTR / BTC r/m16, imm8 ----
    // Intel SDM Vol. 2: 16-bit operand uses bit MOD 16. Memory form with
    // imm >= 16 addresses outside this word — leave that to the interpreter.
    case Opcode::BT_RM16_IMM8:
    case Opcode::BTS_RM16_IMM8:
    case Opcode::BTR_RM16_IMM8:
    case Opcode::BTC_RM16_IMM8: {
        bool src_is_mem = !is_register_operand(op0.type);
        if (src_is_mem && (op1.imm & ~static_cast<i64>(15)) != 0) return false;
        if (src_is_mem) {
            emit_compute_ea(REG_CALL_TMP, op0.mem, next_rip);
            emit_guest_load16(REG_SCRATCH0, REG_CALL_TMP);
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        }
        u8 bit_pos = static_cast<u8>(op1.imm & 15);
        // UBFX W4, W0, #bit_pos, #1 — selected bit is CF; leave FlagOp intact.
        emit(arm64::ubfx_w(REG_SCRATCH4, REG_SCRATCH0, bit_pos, 1));
        // CF overlay before store: emit_store_gpr16 / emit_guest_store16
        // clobber W4 (BFI merge / region-end scratch).
        emit_bt_write_cf(REG_SCRATCH4);
        if (instr.opcode != Opcode::BT_RM16_IMM8) {
            emit_mov_imm32(REG_SCRATCH2, 1u << bit_pos);
            switch (instr.opcode) {
            case Opcode::BTS_RM16_IMM8:
                emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2));
                break;
            case Opcode::BTR_RM16_IMM8:
                emit(0x0A200000u | (static_cast<u32>(REG_SCRATCH2) << 16)
                                 | (static_cast<u32>(REG_SCRATCH0) << 5)
                                 | static_cast<u32>(REG_SCRATCH0));
                break;
            case Opcode::BTC_RM16_IMM8:
                emit(arm64::eor_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2));
                break;
            default:
                break;
            }
            if (src_is_mem) {
                emit_guest_store16(REG_SCRATCH0, REG_CALL_TMP);
            } else {
                emit_store_gpr16(REG_SCRATCH0, op0.reg);
            }
        }
        break;
    }

    // ---- BT / BTS / BTR / BTC r/m16, r16 ----
    // SDM: CF = Bit[BitBase + (BitOffset/16)*2, BitOffset MOD 16]
    case Opcode::BT_RM16_R16:
    case Opcode::BTS_RM16_R16:
    case Opcode::BTR_RM16_R16:
    case Opcode::BTC_RM16_R16: {
        const bool dst_is_mem = !is_register_operand(op0.type);
        if (dst_is_mem) {
            emit_compute_ea(REG_CALL_TMP, op0.mem, next_rip);
            emit_load_gpr16(REG_SCRATCH1, op1.reg);
            emit(arm64::sxth_w(REG_SCRATCH1, REG_SCRATCH1));
            emit(arm64::asr_w_imm(REG_SCRATCH3, REG_SCRATCH1, 4));
            emit(arm64::lsl_w_imm(REG_SCRATCH3, REG_SCRATCH3, 1));
            emit_add_bit_base_offset(REG_CALL_TMP, REG_SCRATCH3);
            emit_guest_load16(REG_SCRATCH0, REG_CALL_TMP);
            // The load can BLR a helper, which clobbers the caller-saved W1.
            emit_load_gpr16(REG_SCRATCH1, op1.reg);
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
            emit_load_gpr16(REG_SCRATCH1, op1.reg);
        }
        emit_mov_imm32(REG_SCRATCH2, 15);
        emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH2)); // bit &= 15

        // bit_val = (dst >> bit) & 1  -> REG_SCRATCH4
        emit(arm64::lsrv_w(REG_SCRATCH4, REG_SCRATCH0, REG_SCRATCH1));
        emit_mov_imm32(REG_SCRATCH2, 1);
        emit(arm64::and_w(REG_SCRATCH4, REG_SCRATCH4, REG_SCRATCH2));

        emit_bt_write_cf(REG_SCRATCH4);

        if (instr.opcode != Opcode::BT_RM16_R16) {
            emit_mov_imm32(REG_SCRATCH2, 1);
            emit(arm64::lslv_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH1));
            switch (instr.opcode) {
            case Opcode::BTS_RM16_R16:
                emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2));
                break;
            case Opcode::BTR_RM16_R16:
                emit(0x0A200000u | (static_cast<u32>(REG_SCRATCH2) << 16)
                                 | (static_cast<u32>(REG_SCRATCH0) << 5)
                                 | static_cast<u32>(REG_SCRATCH0));
                break;
            case Opcode::BTC_RM16_R16:
                emit(arm64::eor_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH2));
                break;
            default: break;
            }
            if (dst_is_mem) {
                emit_guest_store16(REG_SCRATCH0, REG_CALL_TMP);
            } else {
                emit_store_gpr16(REG_SCRATCH0, op0.reg);
            }
        }
        break;
    }

    default: return std::nullopt;
    }
    return true;
}

} // namespace vitreo::cpu::jit
