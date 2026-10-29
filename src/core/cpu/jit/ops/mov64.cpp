// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — 64-bit block lowering: the MOV family. MOV r64/imm64,
// MOV r/m64, the byte MOVs, LEA r64/r32, MOVSXD, MOVSX/MOVZX, the
// 32- and 16-bit MOV forms inside a 64-bit block, and CDQE/CDQ/CQO.
//
// One family of compile_64bit's opcode space, split out of ops/ops64.cpp
// (ADR-0006 step 3) as pure code motion: the case arms are verbatim.
// compile_64bit (ops/ops64.cpp) owns the dispatch chain; see the
// declaration in jit_compiler.hpp for the nullopt / false / true contract.

#include "ops64_internal.hpp"

namespace vitreo::cpu::jit {

namespace {
// Launch-time diagnostic switches, not guest environment variables. Re-reading
// environ while compiling every ALU instruction serializes compiler threads
// on libc's environment lock. Preserve the switches without that hot-path work.
const bool no_lea32 = std::getenv("VITREO_JIT_64BIT_NO_LEA32") != nullptr;
const bool no_movzx16 = std::getenv("VITREO_JIT_64BIT_NO_MOVZX16") != nullptr;
}  // namespace

void JitCompiler::emit_movx64(const DecodedInstruction& instr, u64 next_rip,
                              u8 src_bits, bool sign, bool dst64) {
    const Operand& op0 = instr.operands[0];
    const Operand& op1 = instr.operands[1];
    const u8 pin = pinned_gpr64(op0.reg);
    const u8 d = pin != 0xFF ? pin : u8{REG_SCRATCH0};
    if (is_register_operand(op1.type)) {
        if (src_bits == 8) {
            emit_load_gpr8(d, op1.reg, instr.prefixes.has_rex());
        } else if (src_bits == 16) {
            emit_load_gpr16(d, op1.reg);
        } else {
            emit_load_gpr32(d, op1.reg);
        }
    } else {
        // The access writes `d` only after it has read [ea] (a pinned base
        // that is also the destination is consumed first), and faults before
        // writing anything.
        const u8 ea = emit_ea64(REG_SCRATCH4, op1.mem, next_rip);
        emit_m128_align_check_if_required(ea, instr, op1);
        if (src_bits == 8) {
            emit_guest_load8(d, ea);
        } else if (src_bits == 16) {
            emit_guest_load16_64(d, ea);
        } else {
            emit_guest_load32(d, ea);
        }
    }
    if (sign) {
        if (src_bits == 8) {
            emit(dst64 ? arm64::sxtb_x(d, d) : arm64::sxtb_w(d, d));
        } else if (src_bits == 16) {
            emit(dst64 ? arm64::sxth_x(d, d) : arm64::sxth_w(d, d));
        } else {
            emit(arm64::sxtw_x(d, d));
        }
    }
    if (pin == 0xFF) {
        if (dst64) emit_store_gpr64(d, op0.reg);
        else       emit_store_gpr32(d, op0.reg);
    } else if (op0.reg == static_cast<u8>(RegIndex::RSP)) {
        m_esp_frame_live = false;
    }
}

// nullopt means "not one of mine", so compile_64bit tries the next family.
// `break` means what it meant before the split: run the end-of-block tail
// and report success.
std::optional<bool> JitCompiler::compile_mov64(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // ========================================================================
    // MOV family
    // ========================================================================

    // MOV R64, IMM64  — full 64-bit immediate
    case Opcode::MOV_R64_IMM64: {
        emit_mov_imm64(REG_SCRATCH0, sx_imm64(op1));
        emit_store_gpr64(REG_SCRATCH0, op0.reg);
        break;
    }

    // MOV R64, R/M64
    case Opcode::MOV_R64_RM64: {
        if (is_register_operand(op1.type)) {
            if (emit_mov_gpr64_direct(op0.reg, op1.reg)) break;
            emit_load_gpr64(REG_SCRATCH0, op1.reg);
        } else {
            const u8 pin = is_register_operand(op0.type)
                               ? pinned_gpr64(op0.reg)
                               : u8{0xFF};
            const u8 ea = emit_ea64(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load64(pin != 0xFF ? pin : REG_SCRATCH0, ea);
            if (pin != 0xFF) {
                if (op0.reg == static_cast<u8>(RegIndex::RSP)) {
                    m_esp_frame_live = false;
                }
                break;
            }
        }
        emit_store_gpr64(REG_SCRATCH0, op0.reg);
        break;
    }

    // MOV R/M64, R64
    case Opcode::MOV_RM64_R64: {
        if (!is_register_operand(op0.type) &&
            linear64_store_reads_value_in_place()) {
            const u8 pin = pinned_gpr64(op1.reg);
            if (pin != 0xFF) {
                const u8 ea = emit_ea64(REG_SCRATCH2, op0.mem, next_rip);
                emit_guest_store64(pin, ea);
                break;
            }
        }
        if (is_register_operand(op0.type) &&
            emit_mov_gpr64_direct(op0.reg, op1.reg)) {
            break;
        }
        emit_load_gpr64(REG_SCRATCH0, op1.reg);
        if (is_register_operand(op0.type)) {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        } else {
            const u8 ea = emit_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store64(REG_SCRATCH0, ea);
        }
        break;
    }

    // MOV R/M64, IMM32 (sign-extended to 64)
    case Opcode::MOV_RM64_IMM32: {
        emit_mov_imm64(REG_SCRATCH0, sx_imm64(op1));
        if (is_register_operand(op0.type)) {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        } else {
            const u8 ea = emit_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store64(REG_SCRATCH0, ea);
        }
        break;
    }

    // Byte MOVs preserve every destination bit outside the selected byte and
    // never affect flags. In long mode, register encodings 4-7 select
    // AH/CH/DH/BH without REX and SPL/BPL/SIL/DIL with REX; REX extensions
    // select R8B-R15B. Store the byte directly into CpuState so an r8 write
    // cannot accidentally acquire the zero-extension semantics of an r32
    // write.
    case Opcode::MOV_R8_IMM8:
    case Opcode::MOV_RM8_R8:
    case Opcode::MOV_R8_RM8:
    case Opcode::MOV_RM8_IMM8: {
        const bool has_rex = instr.prefixes.has_rex();
        // Must go through emit_store_gpr8: a 64-bit block keeps the low eight
        // GPRs in pinned ARM registers, so a byte written straight to CpuState
        // is overwritten by the pin spill at the next helper call or epilogue.
        auto store_reg8 = [&](u8 src, u8 reg) {
            emit_store_gpr8(src, reg, has_rex);
        };
        auto read_mem8 = [&](u8 dst, const Operand& operand) {
            const u8 ea = emit_ea64(REG_SCRATCH4, operand.mem, next_rip);
            emit_guest_load8(dst, ea);
        };
        auto write_mem8 = [&](u8 src, const Operand& operand) {
            const u8 ea = emit_ea64(REG_SCRATCH4, operand.mem, next_rip);
            emit_guest_store8(src, ea);
        };

        switch (instr.opcode) {
        case Opcode::MOV_R8_IMM8:
            emit_mov_imm32(REG_SCRATCH0,
                           static_cast<u32>(op1.imm) & 0xFFu);
            store_reg8(REG_SCRATCH0, op0.reg);
            break;
        case Opcode::MOV_RM8_R8:
            emit_load_gpr8(REG_SCRATCH0, op1.reg, has_rex);
            if (is_mem(op0)) write_mem8(REG_SCRATCH0, op0);
            else store_reg8(REG_SCRATCH0, op0.reg);
            break;
        case Opcode::MOV_R8_RM8:
            if (is_mem(op1)) read_mem8(REG_SCRATCH0, op1);
            else emit_load_gpr8(REG_SCRATCH0, op1.reg, has_rex);
            store_reg8(REG_SCRATCH0, op0.reg);
            break;
        case Opcode::MOV_RM8_IMM8:
            emit_mov_imm32(REG_SCRATCH0,
                           static_cast<u32>(op1.imm) & 0xFFu);
            if (is_mem(op0)) write_mem8(REG_SCRATCH0, op0);
            else store_reg8(REG_SCRATCH0, op0.reg);
            break;
        default:
            return false;
        }
        break;
    }

    // LEA R64, m  — compute EA without segment base, no flag effect.
    case Opcode::LEA_R64_M: {
        // Into a pinned destination directly. emit_compute_ea64 reads a
        // pinned base or index in the same instruction that writes its
        // destination, but it LOADS an unpinned base into the destination
        // before adding the index -- so a destination that is also the index
        // keeps the scratch route.
        const u8 dst_pin = pinned_gpr64(op0.reg);
        if (dst_pin != 0xFF &&
            !(op1.mem.has_index() && op1.mem.index == op0.reg)) {
            emit_compute_ea64(dst_pin, op1.mem, next_rip,
                              /*include_segment_base=*/false);
            if (op0.reg == static_cast<u8>(RegIndex::RSP)) {
                m_esp_frame_live = false;
            }
            break;
        }
        emit_compute_ea64(REG_SCRATCH0, op1.mem, next_rip,
                          /*include_segment_base=*/false);
        emit_store_gpr64(REG_SCRATCH0, op0.reg);
        break;
    }

    // LEA r32,m in long mode computes a 64-bit effective address, truncates
    // it to 32 bits, and then zero-extends through the architectural r32
    // write. LEA never adds a segment base and never reads guest memory.
    case Opcode::LEA_R32_M: {
        if (no_lea32) return false;
        // A pinned destination is computed in W form directly: ADD/SUB/LSL on
        // the low 32 bits wrap exactly as the 64-bit sum truncated to 32 does,
        // and the W write zero-extends DEST[63:32] (Intel SDM Vol. 2 LEA,
        // operand size 32 with address size 64). Saves the copy-out of X0.
        const MemoryOperand& m = op1.mem;
        const u8 dst_pin = pinned_gpr32(op0.reg);
        if (dst_pin != 0xFF && m.has_base() && !m.has_absolute &&
            !m.is_rip_relative() && !m.vsib) {
            const auto w_of = [&](u8 reg, u8 scratch) -> u8 {
                const u8 p = pinned_gpr32(reg);
                if (p != 0xFF) return p;
                emit_load_gpr32(scratch, reg);
                return scratch;
            };
            u8 cur = w_of(m.base, REG_SCRATCH2);
            if (m.has_index()) {
                const u8 index = w_of(m.index, REG_SCRATCH3);
                const u8 shift = m.scale == 8 ? 3 : m.scale == 4 ? 2
                               : m.scale == 2 ? 1 : 0;
                emit(arm64::add_w_lsl(dst_pin, cur, index, shift));
                cur = dst_pin;
            }
            const i64 d = static_cast<i64>(m.disp);
            if (d > 0 && d < 4096) {
                emit(arm64::add_w_imm(dst_pin, cur, static_cast<u32>(d)));
            } else if (d < 0 && -d < 4096) {
                emit(arm64::sub_w_imm(dst_pin, cur, static_cast<u32>(-d)));
            } else if (d != 0) {
                emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(d));
                emit(arm64::add_w(dst_pin, cur, REG_SCRATCH1));
            } else if (cur != dst_pin) {
                emit(arm64::mov_w(dst_pin, cur));
            }
            if (op0.reg == static_cast<u8>(RegIndex::RSP)) {
                m_esp_frame_live = false;
            }
            break;
        }
        emit_compute_ea64(REG_SCRATCH0, op1.mem, next_rip,
                          /*include_segment_base=*/false);
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    // MOVSXD R64, R/M32 — sign-extend 32 → 64 (the canonical 64-bit promote)
    case Opcode::MOVSXD_R64_RM32: {
        emit_movx64(instr, next_rip, 32, true, true);
        break;
    }

    // MOVSX r64,r/m8 — sign-extend the selected byte to 64 bits. REX.W
    // selects the 64-bit destination and also disables the legacy high-byte
    // register names. EFLAGS are unchanged (Intel SDM Vol. 2, MOVSX/MOVSXD).
    case Opcode::MOVSX_R64_RM8: {
        emit_movx64(instr, next_rip, 8, true, true);
        break;
    }

    // MOVZX r64,r/m8 and MOVZX/MOVSX r64,r/m16.
    //
    // These had no lowering in either bitness chain, so every one of them took
    // the interpreter delegate in x64 code — 42 forms at 0% native, found by
    // the ISA matrix. `MOVSX_R64_RM8` right above was the only member of the
    // family that had ever been written.
    //
    // The zero-extending forms need no explicit 64-bit step: an ARM W-register
    // write zero-extends into X, so UXTB/UXTH already produce the full 64-bit
    // result that Intel SDM Vol. 2 MOVZX specifies.
    case Opcode::MOVZX_R64_RM8: {
        emit_movx64(instr, next_rip, 8, false, true);
        break;
    }

    case Opcode::MOVZX_R64_RM16: {
        emit_movx64(instr, next_rip, 16, false, true);
        break;
    }

    case Opcode::MOVSX_R64_RM16: {
        emit_movx64(instr, next_rip, 16, true, true);
        break;
    }

    // MOV r32, r/m32  /  MOV r/m32, r32
    case Opcode::MOV_R32_RM32:
    case Opcode::MOV_RM32_R32: {
        if (is_mem(op0) && is_register_operand(op1.type) &&
            linear64_store_reads_value_in_place()) {
            // The store reads only W<pin>, which is exactly r32.
            const u8 pin = pinned_gpr32(op1.reg);
            if (pin != 0xFF) {
                const u8 ea = emit_ea64(REG_SCRATCH4, op0.mem, next_rip);
                emit_guest_store32_64(pin, ea);
                break;
            }
        }
        if (is_register_operand(op0.type) && is_mem(op1)) {
            const u8 pin = pinned_gpr32(op0.reg);
            if (pin != 0xFF) {
                const u8 ea = emit_ea64(REG_SCRATCH4, op1.mem, next_rip);
                emit_guest_load32_64(pin, ea);
                if (op0.reg == static_cast<u8>(RegIndex::RSP)) {
                    m_esp_frame_live = false;
                }
                break;
            }
        }
        if (is_register_operand(op0.type) && is_register_operand(op1.type)) {
            // r32 <- r32 without staging through X0. A 32-bit write zero-
            // extends into bits 63:32 (Intel SDM Vol. 1 3.4.1.1), so the
            // pinned form is always a MOV Wd,Wn -- even for MOV EAX,EAX --
            // and the CpuState forms are LDR W (zero-extending) or the
            // primitive that stores W and clears the high word.
            const u8 dst_pin = pinned_gpr32(op0.reg);
            const u8 src_pin = pinned_gpr32(op1.reg);
            if (dst_pin != 0xFF || src_pin != 0xFF) {
                if (dst_pin != 0xFF && src_pin != 0xFF) {
                    emit(arm64::mov_w(dst_pin, src_pin));
                } else if (dst_pin != 0xFF) {
                    emit_load_gpr32_mem(dst_pin, op1.reg);
                } else {
                    emit_store_gpr32_mem(src_pin, op0.reg);
                }
                if (op0.reg == static_cast<u8>(RegIndex::RSP)) {
                    m_esp_frame_live = false;
                }
                break;
            }
        }
        // Load source into REG_SCRATCH0 (W).
        if (is_register_operand(op1.type)) {
            emit_load_gpr32(REG_SCRATCH0, op1.reg);
        } else if (is_mem(op1)) {
            const u8 ea = emit_ea64(REG_SCRATCH4, op1.mem, next_rip);
            emit_guest_load32_64(REG_SCRATCH0, ea);
        } else {
            return false;
        }
        // Store to dest.
        if (is_register_operand(op0.type)) {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        } else if (is_mem(op0)) {
            // Stash value, compute EA, then call write32.
            emit(arm64::mov_w(17, REG_SCRATCH0));
            const u8 ea = emit_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_guest_store32_64(17, ea);
        } else {
            return false;
        }
        break;
    }

    // MOV r/m32, imm32  — destination is reg or memory
    case Opcode::MOV_R32_IMM32:
    case Opcode::MOV_RM32_IMM32: {
        if (is_register_operand(op0.type)) {
            emit_mov_imm32(REG_SCRATCH0, static_cast<u32>(op1.imm));
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        } else if (is_mem(op0)) {
            const u8 ea = emit_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_mov_imm32(2, static_cast<u32>(op1.imm));
            emit_guest_store32_64(2, ea);
        } else {
            return false;
        }
        break;
    }

    // MOV r16, r/m16. A memory source must use a full 64-bit EA in long
    // mode; the 32-bit guest-memory stencil would truncate addresses such as
    // the default x64 image mappings at 0x140000000.
    case Opcode::MOV_R16_RM16: {
        if (is_register_operand(op1.type)) {
            emit_load_gpr16(REG_SCRATCH0, op1.reg);
        } else if (is_mem(op1)) {
            const u8 ea = emit_ea64(REG_SCRATCH4, op1.mem, next_rip);
            emit_guest_load16_64(REG_SCRATCH0, ea);
        } else {
            return false;
        }
        emit_store_gpr16(REG_SCRATCH0, op0.reg);
        break;
    }

    // MOV r/m16, r16. The helper preserves access checks, exception
    // reporting, write protection, and executable-page invalidation.
    case Opcode::MOV_RM16_R16: {
        if (is_register_operand(op0.type)) {
            emit_load_gpr16(REG_SCRATCH0, op1.reg);
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        } else if (is_mem(op0)) {
            const u8 ea = emit_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_load_gpr16(2, op1.reg);
            emit_guest_store16_64(2, ea);
        } else {
            return false;
        }
        break;
    }

    case Opcode::MOV_R16_IMM16:
    case Opcode::MOV_RM16_IMM16: {
        u32 imm = static_cast<u32>(op1.imm) & 0xFFFFu;
        if (is_register_operand(op0.type)) {
            emit_mov_imm32(REG_SCRATCH0, imm);
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        } else if (is_mem(op0)) {
            const u8 ea = emit_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_mov_imm32(2, imm);
            emit_guest_store16_64(2, ea);
        } else {
            return false;
        }
        break;
    }

    // MOVZX/MOVSX r32, r/m8|r/m16 — register-source only.
    // (r8 index 4-7 in non-REX form is AH/CH/DH/BH; emit_load_gpr8 handles
    // this. In 64-bit mode with REX, decoder produces SPL/BPL/SIL/DIL as
    // higher indices, so AH/CH/DH/BH won't appear unless the instruction
    // truly intends them.)
    case Opcode::MOVZX_R32_RM8: {
        emit_movx64(instr, next_rip, 8, false, false);
        break;
    }
    case Opcode::MOVZX_R32_RM16: {
        if (no_movzx16) return false;
        emit_movx64(instr, next_rip, 16, false, false);
        break;
    }
    case Opcode::MOVSX_R32_RM8: {
        emit_movx64(instr, next_rip, 8, true, false);
        break;
    }
    case Opcode::MOVSX_R32_RM16: {
        emit_movx64(instr, next_rip, 16, true, false);
        break;
    }

    // ========================================================================
    // Remaining x64 hot delegates (CDQE / BSWAP32 / SBB64 / ADC64 /
    // BT64 / BTC64 / MOVQ xmm↔gpr64). Validated against the x64 oracle catalog.
    // ========================================================================

    // CDQE — sign-extend EAX into RAX. Flag-neutral. One ARM SXTW.
    case Opcode::CDQE: {
        emit_load_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        // SXTW Xd, Wn
        emit(0x93407C00u
             | (static_cast<u32>(REG_SCRATCH0) << 5)
             | static_cast<u32>(REG_SCRATCH0));
        emit_store_gpr64(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        break;
    }

    // CDQ — EDX := SignExtend(EAX). Intel SDM Vol. 2: RAX is unchanged.
    case Opcode::CDQ: {
        emit_load_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit(arm64::asr_w_imm(REG_SCRATCH1, REG_SCRATCH0, 31));
        emit_store_gpr32(REG_SCRATCH1, static_cast<u8>(RegIndex::RDX));
        break;
    }

    // CQO — RDX := SignExtend(RAX). REX.W form of CDQ. Flag-neutral.
    case Opcode::CQO: {
        emit_load_gpr64(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit(arm64::asr_x_imm(REG_SCRATCH1, REG_SCRATCH0, 63));
        emit_store_gpr64(REG_SCRATCH1, static_cast<u8>(RegIndex::RDX));
        break;
    }

    default: return std::nullopt;
    }

    finish_op64_block(next_rip, is_last);
    return true;
}

}  // namespace vitreo::cpu::jit
