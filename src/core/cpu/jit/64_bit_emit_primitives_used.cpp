// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler.cpp
/// @brief Copy-and-patch JIT compiler implementation (CPU-040)
///
/// Compiles decoded x86 BasicBlocks into ARM64 machine code using
/// direct instruction encoding. Uses lazy flag evaluation to avoid
/// computing x86 flags after every ALU instruction.
///
/// Register convention for JIT code:
///   X19 = CpuState*        (callee-saved, pinned)
///   X20 = JitContext*       (callee-saved, pinned — helper fn ptrs + memory)
///   W0-W15 = scratch registers
///   X16-X17 = intra-procedure scratch

#include <cstdint>
///   X30 = LR (saved/restored in prologue/epilogue for BLR calls)

#include "jit_compiler.hpp"
#include "stencils.hpp"
#include "jit_helpers.hpp"
#include "idiom_detector.hpp"
#include "linear_fault.hpp"
#include "../../memory/jit_region_registry.hpp"
#include "../../memory/mem_watch.hpp"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>

/// @file 64_bit_emit_primitives_used.cpp
/// @brief 64-bit emit primitives (used by 64-bit native codegen)
///
/// Closed cut from jit_compiler.cpp — verified with plan_extraction.py.

namespace vitreo::cpu::jit {

// ============================================================================
// 64-bit emit primitives (used by 64-bit native codegen)
// ============================================================================

void JitCompiler::emit_load_gpr64_mem(u8 arm_reg, u8 x86_reg) {
    u32 off = gpr_off(x86_reg);
    if (fits_ldr_x_offset(off)) {
        emit(arm64::ldr_x_uoff(arm_reg, REG_CPU_STATE, off >> 3));
    } else {
        emit_mov_imm32(REG_SCRATCH4, off);
        emit(arm64::ldr_x_reg(arm_reg, REG_CPU_STATE, REG_SCRATCH4));
    }
}

void JitCompiler::emit_store_gpr64_mem(u8 arm_reg, u8 x86_reg) {
    u32 off = gpr_off(x86_reg);
    if (fits_ldr_x_offset(off)) {
        emit(arm64::str_x_uoff(arm_reg, REG_CPU_STATE, off >> 3));
    } else {
        emit_mov_imm32(REG_SCRATCH4, off);
        emit(arm64::str_x_reg(arm_reg, REG_CPU_STATE, REG_SCRATCH4));
    }
    // No zero-extend: 64-bit slot is fully written.
}

void JitCompiler::emit_load_gpr64(u8 arm_reg, u8 x86_reg) {
    AttrScope attr_scope(*this, EmitAttribution::GprState);
    // Only 64-bit blocks hold a full 64-bit value in the pin. In a 32-bit
    // block the pin carries just bits 31:0, so a 64-bit read has to come from
    // CpuState.
    if (m_block_is_64bit && m_pins_live && x86_reg < 16 &&
        m_pin_arm[x86_reg] != 0xFF) {
        const u8 pin = m_pin_arm[x86_reg];
        if (arm_reg != pin) emit(arm64::mov_x(arm_reg, pin));
        return;
    }
    emit_load_gpr64_mem(arm_reg, x86_reg);
}

void JitCompiler::emit_store_gpr64(u8 arm_reg, u8 x86_reg) {
    AttrScope attr_scope(*this, EmitAttribution::GprState);
    if (m_block_is_64bit && m_pins_live && x86_reg < 16 &&
        m_pin_arm[x86_reg] != 0xFF) {
        if (x86_reg == static_cast<u8>(RegIndex::RSP)) m_esp_frame_live = false;
        const u8 pin = m_pin_arm[x86_reg];
        if (arm_reg != pin) emit(arm64::mov_x(pin, arm_reg));
        return;
    }
    emit_store_gpr64_mem(arm_reg, x86_reg);
}

void JitCompiler::emit_mov_imm64(u8 arm_reg, i64 value) {
    // Arm Instruction Set Reference Guide D2.106-108: MOVN complements the
    // entire shifted immediate, MOVZ zeros the other lanes, and MOVK replaces
    // one lane. None modifies NZCV. encode_mov_imm64 picks the cheaper initial
    // fill and patches differing lanes; the translation store re-encodes
    // relocated host addresses with the same function.
    u32 words[4];
    const u32 count = encode_mov_imm64(arm_reg, static_cast<u64>(value), words);
    for (u32 i = 0; i < count; ++i) emit(words[i]);
}

void JitCompiler::emit_host_addr_imm64(u8 arm_reg, i64 value) {
    const usize start = m_code_buf.size();
    emit_mov_imm64(arm_reg, value);
    if (m_ts_foreign_emission != 0) return;
    m_ts_relocs.push_back(StoredHostReloc{
        static_cast<u32>(start), static_cast<u8>(m_code_buf.size() - start), arm_reg,
        static_cast<u64>(value)});
}

void JitCompiler::emit_rotate64_partial_flags(u8 result_reg, bool cf_from_msb,
                                             bool define_of) {
    // Intel SDM Vol. 2 RCL/RCR/ROL/ROR: dest is already written. Only CF is
    // defined for every nonzero count; OF is defined only when count==1.
    // ZF/SF/PF/AF stay with any pending FlagOp (same overlay scheme as the
    // 32-bit ROR stencil). Encoding: overlay 1 = forced 0, 2 = forced 1.
    const u32 rf_off = cpu_rflags_offset();
    emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
    emit(arm64::movz_w(4, 1));
    emit(0x8A240063u); // BIC X3, X3, X4 — clear CF in the rflags copy
    if (cf_from_msb) {
        // UBFX X4, Xresult, #63, #1
        emit(0xD3400000u | (63u << 16) | (63u << 10)
             | (static_cast<u32>(result_reg) << 5) | 4u);
    } else {
        // UBFX X4, Xresult, #0, #1
        emit(0xD3400000u | (0u << 16) | (0u << 10)
             | (static_cast<u32>(result_reg) << 5) | 4u);
    }
    emit(0xAA040063u); // ORR X3, X3, X4
    emit(arm64::add_w_imm(REG_SCRATCH1, 4, 1));
    emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE, cf_overlay_offset()));

    if (define_of) {
        emit(arm64::movz_w(4, 0x800));
        emit(0x8A240063u); // BIC X3, X3, X4 — clear OF (bit 11)
        // ROL 1: CF = new bit0; OF = new MSB XOR CF → bit63 XOR bit0.
        // ROR 1: CF = new MSB; OF = MSB XOR (MSB-1) → bit63 XOR bit62.
        emit(0xD3400000u | (63u << 16) | (63u << 10)
             | (static_cast<u32>(result_reg) << 5) | 2u); // X2 = bit63
        if (cf_from_msb) {
            emit(0xD3400000u | (62u << 16) | (62u << 10)
                 | (static_cast<u32>(result_reg) << 5) | 4u); // X4 = bit62
        } else {
            emit(0xD3400000u | (0u << 16) | (0u << 10)
                 | (static_cast<u32>(result_reg) << 5) | 4u); // X4 = bit0
        }
        emit(0x4A020084u); // EOR W4, W4, W2
        emit(arm64::add_w_imm(REG_SCRATCH1, 4, 1));
        emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE, of_overlay_offset()));
        emit(0x53155084u); // LSL W4, W4, #11
        emit(0xAA040063u); // ORR X3, X3, X4
    }
    emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
}

void JitCompiler::emit_double_shift_of(u8 orig_reg, u8 result_reg,
                                       bool wide) {
    if (m_omit_lazy_current) return;
    // Intel SDM Vol. 2 (SHLD/SHRD): OF is defined only for a count of 1, and
    // is set when the sign bit changed. Everything else about OF is
    // undefined, so only the caller's count==1 path reaches here.
    const u8 msb = wide ? 63u : 31u;
    const u32 rf_off = cpu_rflags_offset();
    emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
    emit(arm64::movz_w(4, 0x800));
    emit(0x8A240063u); // BIC X3, X3, X4 — clear OF (bit 11)
    emit(arm64::ubfx_x(REG_SCRATCH2, orig_reg, msb, 1));
    emit(arm64::ubfx_x(4, result_reg, msb, 1));
    emit(0x4A020084u); // EOR W4, W4, W2 — sign changed?
    emit(arm64::add_w_imm(REG_SCRATCH1, 4, 1));
    emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE, of_overlay_offset()));
    emit(0x53155084u); // LSL W4, W4, #11
    emit(0xAA040063u); // ORR X3, X3, X4
    emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
}

void JitCompiler::emit_set_lazy_flags64(CpuState::FlagOp op,
                                         u8 src_arm_reg,
                                         u8 dst_arm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::Flags);
    // Within-block liveness says this publisher's record is dead — but an
    // escaping Jcc's TAKEN edge still leaves with these flags live, and NZCV
    // kind bits carry only N/Z/C/V, never AF/PF. Record what was skipped so
    // queue_mid_block_jcc_exit re-emits it on that edge (wide: 64-bit
    // operands must be replayed X-width). This is what lets flag_liveness
    // omit the inline store for 64-bit at all: measured at 11 emitted ARM per
    // `CMP EAX, imm32` when the store had to stay inline.
    if (m_omit_lazy_current) {
        // VITREO_JIT_NO_DEFER64=1 drops the deferral (and with it the
        // omission's re-emit contract) for bisection.
        static const bool no_defer =
            std::getenv("VITREO_JIT_NO_DEFER64") != nullptr;
        if (no_defer) return;
        // The record is dead, the overlay is not — see
        // m_omit_lazy_keep_overlay.
        if (m_omit_lazy_keep_overlay) emit_clear_cf_overlay();
        m_pending_stub_store.valid = true;
        m_pending_stub_store.op = op;
        m_pending_stub_store.src = src_arm_reg;
        m_pending_stub_store.dst = dst_arm_reg;
        m_pending_stub_store.wide = true;
        return;
    }

    // Name the pending carry producer for the NEXT instruction, so a carry
    // consumer emits the one matching formula instead of the eight-way
    // runtime FlagOp dispatch. The 32-bit path has done this since the
    // fusion work (instruction_compilation.cpp); a 64-bit block published
    // nothing, so `m_flags.incoming_carry_source` was permanently Unknown and
    // every 64-bit CF capture paid the dispatch — the
    // machinery-is-not-wired shape again.
    //
    // Published HERE rather than at the compile sites because the formula
    // reads back exactly the record written below: deriving the source from
    // the op being recorded is what keeps the two from drifting apart, and a
    // width mismatch between record and formula is a silent wrong-CF bug.
    //
    // Deliberately NOT published on the deferred path above: that path emits
    // no store, so flag_src/flag_dst still hold the previous producer's
    // operands. Liveness says no in-block consumer reads them, and a named
    // source would turn a wrong liveness answer from slow into incorrect.
    switch (op) {
    case CpuState::FlagOp::Add64:
        m_flags.pending_carry_source = CarrySource::Add64;
        break;
    case CpuState::FlagOp::Sub64:
        m_flags.pending_carry_source = CarrySource::Sub64;
        break;
    // Intel SDM Vol. 2, AND/OR/XOR/TEST: "The OF and CF flags are cleared."
    case CpuState::FlagOp::And64:
    case CpuState::FlagOp::Or64:
    case CpuState::FlagOp::Xor64:
        m_flags.pending_carry_source = CarrySource::Logic64;
        break;
    default:
        // Unknown is the conservative state: the consumer keeps the dispatch.
        break;
    }

    // Hand the actual emission to the shared publisher rather than repeating
    // it. This used to be a second, independently maintained copy of the same
    // four stores, which is why the two-store form landed on the 32-bit path
    // and silently missed x64 entirely -- caught only by an emission test that
    // asserted the STP.
    //
    // `wide` is the whole difference: a 64-bit publisher's flag_src/flag_dst
    // are full 64-bit values, so they go out X-width and unmodified. The
    // shared emitter pairs them into ONE STP for exactly this case, because
    // there is no zero-extend to stage through a scratch first.
    emit_lazy_flag_store_raw(op, src_arm_reg, dst_arm_reg, /*wide=*/true);
}

void JitCompiler::maybe_publish_result_nz64(u8 result_reg) {
    // Result-producing instructions such as DEC and nonzero-count shifts
    // define SF/ZF from the result (Intel SDM Vol. 2). Their other flags have
    // instruction-specific rules, so neither C nor V is authoritative here:
    // NzOnly lets fused_arm_cond accept only JZ/JNZ/JS/JNS. The lazy record
    // written by the caller remains the reference for every other flag read;
    // this TST is only a branch fast-path.
    //
    // Publish only when the next instruction is such a Jcc: any non-None
    // m_flags.nzcv makes should_hold_nzcv() tax the following instruction's memory
    // ops with an NZCV spill/fill, so an unconsumed publish is a net loss.
    switch (m_next_opcode) {
    case Opcode::JZ_REL8:  case Opcode::JZ_REL32:
    case Opcode::JNZ_REL8: case Opcode::JNZ_REL32:
    case Opcode::JS_REL8:  case Opcode::JS_REL32:
    case Opcode::JNS_REL8: case Opcode::JNS_REL32:
        break;
    default:
        return;
    }
    emit(arm64::ands_x(31, result_reg, result_reg));  // TST result, result
    m_flags.nzcv = NzcvState::NzOnly;
}

void JitCompiler::emit_shift64_cf_imm(u8 orig_reg, u8 count, bool left) {
    if (m_omit_lazy_current) return;
    const u8 bit = left ? static_cast<u8>(64u - count)
                        : static_cast<u8>(count - 1u);
    emit(arm64::ubfx_x(REG_SCRATCH4, orig_reg, bit, 1));
    const u32 rf_off = cpu_rflags_offset();
    emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
    emit(arm64::movz_w(REG_SCRATCH2, 1));
    emit(0x8A220063u); // BIC X3, X3, X2
    emit(0xAA040063u); // ORR X3, X3, X4
    emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
    emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
    emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE, cf_overlay_offset()));
}

void JitCompiler::emit_shift64_cf(u8 orig_reg, u8 count_reg, bool left) {
    if (m_omit_lazy_current) return;
    if (left) {
        emit_mov_imm32(REG_SCRATCH4, 64u);
        emit(arm64::sub_w(REG_SCRATCH4, REG_SCRATCH4, count_reg));
        emit(arm64::lsrv_x(REG_SCRATCH4, orig_reg, REG_SCRATCH4));
    } else {
        emit(arm64::sub_x_imm(REG_SCRATCH4, count_reg, 1));
        emit(arm64::lsrv_x(REG_SCRATCH4, orig_reg, REG_SCRATCH4));
    }
    emit(arm64::ubfx_x(REG_SCRATCH4, REG_SCRATCH4, 0, 1));
    const u32 rf_off = cpu_rflags_offset();
    emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
    emit(arm64::movz_w(REG_SCRATCH2, 1));
    emit(0x8A220063u); // BIC X3, X3, X2
    emit(0xAA040063u); // ORR X3, X3, X4
    emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
    emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
    emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE, cf_overlay_offset()));
}

// emit_compute_ea64 — 64-bit effective address. Loads base/index as full 64-bit
// GPRs, supports full u64 RIP-relative addressing, and adds segment base in
// 64-bit. Result is left in Xdst (full 64 bits). Same operand semantics as
// emit_compute_ea but never truncates.
void JitCompiler::emit_compute_ea64(u8 dst_arm_reg, const MemoryOperand& mem,
                                     u64 rip_val, bool include_segment_base) {
    AttrScope attr_scope(*this, EmitAttribution::Address);
    bool have_value = false;

    // Same aliasing hazard as the 32-bit path: REG_EA_TMP is the internal
    // scratch, so a caller requesting its result there would otherwise have the
    // partially-built address overwritten by the index or displacement.
    const u8 ea_tmp = (dst_arm_reg == REG_EA_TMP) ? REG_SCRATCH4 : REG_EA_TMP;

    if (mem.has_absolute) {
        // A0-A3 moffs carries a full 64-bit absolute address in long mode.
        emit_mov_imm64(dst_arm_reg, static_cast<i64>(mem.absolute));
        have_value = true;
    } else if (mem.is_rip_relative()) {
        // rip_val + sign-extended disp — full 64-bit.
        u64 ea = rip_val + static_cast<u64>(static_cast<i64>(mem.disp));
        emit_mov_imm64(dst_arm_reg, static_cast<i64>(ea));
        have_value = true;
    } else if (mem.is_disp_only()) {
        emit_mov_imm64(dst_arm_reg, static_cast<i64>(mem.disp));
        have_value = true;
    } else {
        // A pinned base is read in place by the first instruction that
        // consumes it (the index ADD, the displacement ADD/SUB) instead of
        // being copied into dst first: Intel SDM Vol. 1 §3.7.5 defines the
        // EA as base + index*scale + disp, so every form below computes the
        // same 64-bit sum with one fewer MOV per memory operand.
        const u8 base_pin = mem.has_base() ? pinned_gpr64(mem.base) : 0xFF;
        u8 value_reg = dst_arm_reg;
        if (mem.has_base()) {
            if (base_pin != 0xFF) {
                value_reg = base_pin;
            } else {
                emit_load_gpr64(dst_arm_reg, mem.base);
            }
            have_value = true;
        }
        if (mem.has_index()) {
            const u8 index_pin = pinned_gpr64(mem.index);
            const u8 index_reg = index_pin != 0xFF ? index_pin : ea_tmp;
            if (index_pin == 0xFF) emit_load_gpr64(ea_tmp, mem.index);
            u8 shift = 0;
            if (mem.scale == 2) shift = 1;
            else if (mem.scale == 4) shift = 2;
            else if (mem.scale == 8) shift = 3;
            if (have_value) {
                // ADD Xd, Xbase, Xm, LSL #shift (works with shift==0 too)
                emit(arm64::add_x_lsl(dst_arm_reg, value_reg,
                                       index_reg, shift));
                value_reg = dst_arm_reg;
            } else {
                if (shift != 0) {
                    emit(arm64::lsl_x_imm(dst_arm_reg, index_reg, shift));
                } else {
                    emit(arm64::mov_x(dst_arm_reg, index_reg));
                }
                have_value = true;
            }
        }
        if (mem.disp != 0) {
            i64 d = static_cast<i64>(mem.disp);
            if (!have_value) {
                emit_mov_imm64(dst_arm_reg, d);
                have_value = true;
            } else if (d > 0 && d < 4096) {
                emit(arm64::add_x_imm(dst_arm_reg, value_reg,
                                       static_cast<u32>(d)));
            } else if (d < 0 && -d < 4096) {
                emit(arm64::sub_x_imm(dst_arm_reg, value_reg,
                                       static_cast<u32>(-d)));
            } else {
                emit_mov_imm64(ea_tmp, d);
                emit(arm64::add_x(dst_arm_reg, value_reg, ea_tmp));
            }
            value_reg = dst_arm_reg;
        }
        if (have_value && value_reg != dst_arm_reg) {
            // [pinned base] alone: the EA is the pin itself.
            emit(arm64::mov_x(dst_arm_reg, value_reg));
        }
    }
    if (!have_value) {
        emit(arm64::movz_x(dst_arm_reg, 0, 0));
    }

    if (include_segment_base && mem.segment != SegReg::Default) {
        u32 base_off = seg_base_offset(mem.segment);
        emit(arm64::ldr_x_uoff(ea_tmp, REG_CPU_STATE, base_off >> 3));
        emit(arm64::add_x(dst_arm_reg, dst_arm_reg, ea_tmp));
    }

    // Any segment override is treated as non-constant: FS/GS add a runtime
    // base, and the others are not worth distinguishing here.
    m_const_ea.valid = false;
    if (mem.segment == SegReg::Default &&
        (mem.has_absolute || mem.is_rip_relative() || mem.is_disp_only())) {
        const u64 value =
            mem.has_absolute ? static_cast<u64>(mem.absolute)
            : mem.is_rip_relative()
                ? rip_val + static_cast<u64>(static_cast<i64>(mem.disp))
                : static_cast<u64>(static_cast<i64>(mem.disp));
        m_const_ea = ConstEa{true, dst_arm_reg, value, m_code_buf.size()};
    }
}

u8 JitCompiler::emit_ea64(u8 dst_arm_reg, const MemoryOperand& mem,
                          u64 rip_val, bool include_segment_base) {
    AttrScope attr_scope(*this, EmitAttribution::Address);
    if ((!include_segment_base || mem.segment == SegReg::Default) &&
        !mem.has_absolute && !mem.is_rip_relative() && !mem.is_disp_only() &&
        mem.has_base() && !mem.has_index()) {
        const u8 pin = pinned_gpr64(mem.base);
        if (pin != 0xFF) {
            // Intel SDM Vol.1 §3.3.7: 64-bit EA is the 64-bit base plus a
            // sign-extended displacement. A zero disp is the pin itself. A
            // displacement that fits ARM64 ADD/SUB #imm12 lands in X9 so the
            // linear64 hot path and helper ABI share it without a second copy
            // out of a guard scratch (X3/X4/X8/X17).
            if (mem.disp == 0) return pin;
            const i64 d = static_cast<i64>(mem.disp);
            if (d > 0 && d < 4096) {
                emit(arm64::add_x_imm(9, pin, static_cast<u32>(d)));
                return 9;
            }
            if (d < 0 && -d < 4096) {
                emit(arm64::sub_x_imm(9, pin, static_cast<u32>(-d)));
                return 9;
            }
            if (d > 0 && (d & 0xFFF) == 0 && (d >> 12) < 4096) {
                emit(arm64::add_x_imm_lsl12(9, pin, static_cast<u32>(d >> 12)));
                return 9;
            }
            if (d < 0 && ((-d) & 0xFFF) == 0 && ((-d) >> 12) < 4096) {
                emit(arm64::sub_x_imm_lsl12(
                    9, pin, static_cast<u32>((-d) >> 12)));
                return 9;
            }
            emit_compute_ea64(9, mem, rip_val, include_segment_base);
            return 9;
        }
    }
    emit_compute_ea64(dst_arm_reg, mem, rip_val, include_segment_base);
    return dst_arm_reg;
}

// emit_guest_load64 / emit_guest_store64
//
// For 64-bit blocks the guest may target addresses that exceed 32 bits
// (the default x64 image base is 0x140000000). The inline 32-bit region fast-path
// cannot validate addresses above 4 GB, so long-mode guest memory access
// goes through the C helpers (read64/write64). That path is slower than the
// 32-bit inline path and it accepts the full canonical address.

// 128-bit validated guest access, long mode only.
//
// The same dispatch the scalar widths use, over the primitives Tier 0 already
// has: `emit_linear64_load128`/`store128` when the linear base is pinned,
// `emit_x64_region_load128`/`store128` otherwise. Both keep the LDR Q / STR Q
// AT THE SITE rather than inside a helper -- a fault under BLR would sit under
// an extra frame the Mach trampoline does not pop -- and hand an out-of-window
// or straddling address to the shared vec128 cold stub, which splits it into
// two checked 8-byte helper calls and stages the halves through CpuState
// rather than holding them in V0-V7 across the call.
//
// That last part is why a 128-bit access needs no relaxation of the IR's rule
// that a vector value must not be live across a may-fault op: on the hot path
// the vector is produced by the load or consumed by the store, never staged
// across anything, and on the cold path it never sits in a caller-saved V
// register at all.
void JitCompiler::emit_guest_load128(u8 vec_reg, u8 addr_arm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    if (m_cfg.linear64 && m_linear_base_pinned)
        emit_linear64_load128(vec_reg, addr_arm_reg);
    else
        emit_x64_region_load128(vec_reg, addr_arm_reg);
}

void JitCompiler::emit_guest_store128(u8 vec_reg, u8 addr_arm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    if (m_store_ring_stores) {
        emit(arm64::sub_x_imm(31, 31, 64));
        emit(arm64::stp_x_off(10, 11, 31, 0));
        emit(arm64::str_q_uoff(vec_reg, 31, 1));
        emit(arm64::stp_x_off(addr_arm_reg, 9, 31, 4));
        emit(arm64::ldp_x_off(10, 11, 31, 2));
        emit(arm64::ldr_x_uoff(9, 31, 4));
        emit_store_ring_record_vec(9);
        emit(arm64::ldr_x_uoff(9, 31, 5));
        emit(arm64::ldp_x_off(10, 11, 31, 0));
        emit(arm64::add_x_imm(31, 31, 64));
    }
    if (m_cfg.linear64 && m_linear_base_pinned)
        emit_linear64_store128(vec_reg, addr_arm_reg);
    else
        emit_x64_region_store128(vec_reg, addr_arm_reg);
}

void JitCompiler::emit_guest_load64(u8 dst_arm_reg, u8 addr_arm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    // 32-bit guests (RCT3) run with linear memory: a checked 64-bit load can
    // use the same base+uxtw path as load32 and avoid tlb_find on every
    // FLD_M64 / FADD_M64. Full 64-bit guests still need the C helper because
    // the inline region path only validates 32-bit addresses.
    if (!m_block_is_64bit && m_cfg.linear_jit && m_memory.is_linear()) {
        u8 d = (dst_arm_reg != 0) ? dst_arm_reg : 0;
        emit(arm64::mov_w(9, addr_arm_reg));
        const u8 base = m_linear_base_pinned ? REG_LINEAR_BASE : REG_SCRATCH3;
        if (!m_linear_base_pinned) {
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_JIT_CTX,
                                    JIT_CTX_OFF_LINEAR_BASE >> 3));
        }
        emit(arm64::ldr_x_uxtw(d, base, 9));
        emit_tso_barrier();
        return;
    }

    // Long mode: the full-width region slots validate addresses above 4 GB, so
    // a 64-bit load no longer has to pay a helper call per access.
    if (m_block_is_64bit) {
        if (m_cfg.linear64 && m_linear_base_pinned) {
            emit_linear64_load(dst_arm_reg, addr_arm_reg, 8);
        } else {
            emit_x64_region_load(dst_arm_reg, addr_arm_reg, 8);
        }
        return;
    }

    // X17 (IP1) holds nothing here. Not X16: a MOVZ/MOVK run into IP0 reads
    // as a call target (JitEmissionGolden normalizes it by symbol).
    emit_publish_rip_for_raising_helper(17);
    // Arg setup: X0 = JitContext*, X1 = addr (64-bit).
    if (addr_arm_reg != 1) {
        emit(arm64::mov_x(1, addr_arm_reg));
    }
    emit(arm64::mov_x(0, REG_JIT_CTX));
    emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_READ64 >> 3));
    emit_helper_blr();  // BLR X16
    emit_exit_if_pending_raise();
    if (dst_arm_reg != 0) {
        emit(arm64::mov_x(dst_arm_reg, 0));
    }
    emit_tso_barrier();
}

void JitCompiler::emit_guest_store64(u8 src_arm_reg, u8 addr_arm_reg) {
    publish_pending_stub_store();
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    const bool hold = should_hold_nzcv();
    // The linear64 fast path is NZCV-clean end to end (ORR/LSR/CBNZ bounds,
    // LDRB/CBNZ check byte, ADD, STLR), so the flag save/restore belongs in
    // its cold stub rather than inline on every store. That inline MRS/STR +
    // LDR/MSR was the measured cost of a store sitting between an x86 flag
    // producer and its consumer.
    const bool linear_fast =
        m_block_is_64bit && m_cfg.linear64 && m_linear_base_pinned;
    if (hold && !linear_fast) emit_nzcv_spill();
    (void)emit_store_ring_helper(src_arm_reg, addr_arm_reg, 8);
    if (m_block_is_64bit) {
        if (linear_fast) {
            m_flags.stub_spill_nzcv = hold;
            emit_linear64_store(src_arm_reg, addr_arm_reg, 8);
            m_flags.stub_spill_nzcv = false;
        } else {
            emit_x64_region_store(src_arm_reg, addr_arm_reg, 8);
            if (hold) emit_nzcv_fill();
            return;
        }
        return;
    }
    emit_tso_barrier();
    // 32-bit linear guests: same SMC-guarded UXTW path as store32. Movie
    // decode / FPU m64 / MOVQ were paying jit_mem_write64 (probe + TLB)
    // on every qword store.
    if (m_cfg.linear_jit && m_memory.is_linear()) {
        emit_linear_store(src_arm_reg, addr_arm_reg, 8);
        emit_tso_store_tail_barrier();
        if (hold) emit_nzcv_fill();
        return;
    }
    // Arg setup: X0 = JitContext*, X1 = addr, X2 = value.
    // Stash src into X17 first so it can't be clobbered by any subsequent
    // moves into X1/X2 (the previous version had a bug where calling with
    // src=X0/X1 and addr=X2 would clobber addr before it was moved to X1).
    emit(arm64::mov_x(17, src_arm_reg));
    if (addr_arm_reg != 1) {
        emit(arm64::mov_x(1, addr_arm_reg));
    }
    // X2 is free between the staging above and the value move below.
    emit_publish_rip_for_raising_helper(2);
    emit(arm64::mov_x(2, 17));
    emit(arm64::mov_x(0, REG_JIT_CTX));
    emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_WRITE64 >> 3));
    emit_helper_blr();
    emit_exit_if_pending_raise();
    emit_tso_store_tail_barrier();
    if (hold) emit_nzcv_fill();
}

void JitCompiler::emit_exit_if_pending_raise(bool restore_spilled_nzcv,
                                             u8 spilled_nzcv_kind,
                                             bool flag_in_w16) {
    AttrScope attr_scope(*this, EmitAttribution::Raise);
    constexpr u32 active_offset = static_cast<u32>(
        offsetof(CpuState, pending_raise) +
        offsetof(CpuState::PendingRaiseException, active));
    static_assert(active_offset < 4096);

    // Two words at the call site; the ~13-word exit body is emitted ONCE per
    // block by emit_flush_raise_exit_stub() and every site branches to it.
    // W16 is safe to reuse: it held the helper pointer for the BLR that just
    // returned, so it is dead here.
    if (!flag_in_w16) emit(arm64::ldrb_uoff(16, REG_CPU_STATE, active_offset));
    const bool restore = restore_spilled_nzcv || m_flags.nzcv_hold_depth != 0;
    if (restore) {
        if (spilled_nzcv_kind == 0xFF) {
            const NzcvState state = m_flags.nzcv != NzcvState::None
                ? m_flags.nzcv : m_flags.incoming_nzcv;
            spilled_nzcv_kind = nzcv_kind_byte(state);
        }
        m_raise_restore_nzcv_branch_pos[spilled_nzcv_kind]
            .push_back(m_code_buf.size());
    } else {
        m_raise_branch_pos.push_back(m_code_buf.size());
    }
    emit(0); // CBNZ W16, .raise -- patched at flush
}

void JitCompiler::emit_persist_spilled_nzcv() {
    AttrScope attr_scope(*this, EmitAttribution::Flags);
    // Linked tail spilled NZCV to [SP,#8] before the L1 SUBS/CMP. On miss
    // those flags are gone from the host NZCV register; copy the slot into
    // CpuState so execute() can MSR before the next block.
    emit(arm64::ldrb_uoff(4, REG_CPU_STATE, nzcv_kind_offset()));
    const usize cbz = m_code_buf.size();
    emit(0);
    emit(arm64::ldr_x_uoff(11, 31, 1));
    emit(arm64::str_w_uoff(11, REG_CPU_STATE, nzcv_bits_offset() >> 2));
    m_code_buf[cbz] = arm64::cbz_w(
        4, static_cast<i32>(m_code_buf.size() - cbz) * 4);
}

void JitCompiler::emit_persist_live_nzcv() {
    AttrScope attr_scope(*this, EmitAttribution::Flags);
    // Counterpart to emit_persist_spilled_nzcv for a tail that never spilled
    // because nothing on the path clobbers NZCV. Host NZCV still holds the
    // predecessor's flags here, so read them straight out of PSTATE instead
    // of out of the (never written) [SP,#8] slot.
    emit(arm64::ldrb_uoff(4, REG_CPU_STATE, nzcv_kind_offset()));
    const usize cbz = m_code_buf.size();
    emit(0);
    emit(arm64::mrs_nzcv(11));
    emit(arm64::str_w_uoff(11, REG_CPU_STATE, nzcv_bits_offset() >> 2));
    m_code_buf[cbz] = arm64::cbz_w(
        4, static_cast<i32>(m_code_buf.size() - cbz) * 4);
}

void JitCompiler::emit_discard_native_call_frames() {
    // Native guest-CALL continuation frames sit between SP and this entry's
    // dispatcher LR frame, whose address execute() published. Restoring SP
    // discards all of them at once and leaves the LR frame on top for
    // emit_epilogue (or the fault trampoline) to pop. X17 is free at every
    // exit; neither instruction modifies NZCV.
    emit(arm64::ldr_x_uoff(17, REG_JIT_CTX, JIT_CTX_OFF_NATIVE_SP_BASE >> 3));
    emit(arm64::mov_sp_x(17));
}

void JitCompiler::emit_native_call_landing_pad() {
    // Entered only by a patched guest-CALL BL (emit_linked_call_continue),
    // with X30 = host continuation after the BL and X21 = the guest return
    // RIP the CALL just pushed. Pushes one kNativeCallFrameSize frame so the
    // callee's RET can return natively, then joins the ordinary linked entry
    // right below. Below native_sp_floor it pushes nothing: the RET then
    // fails its frame check and takes the L1 tail, which is always correct.
    // Nothing here reads or writes NZCV.
    static_assert(kNativeCallFrameSize == 32 &&
                  kNativeCallGuestRipOffset == 16 &&
                  kNativeCallTagOffset == 24,
                  "landing-pad STP encodings follow the frame ABI");
    const usize skip = m_code_buf.size();
    emit(0);  // B .link_entry -- the dispatcher prologue pushes no frame
    m_call_entry_pos = static_cast<u32>(m_code_buf.size());
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX, JIT_CTX_OFF_NATIVE_SP_FLOOR >> 3));
    emit(arm64::sub_x_sp_reg(3, 3));  // X3 = SP - floor
    const usize below = m_code_buf.size();
    emit(0);  // TBNZ X3, #63, .link_entry
    emit(0xA9BE7FFEu);  // STP X30, XZR, [SP, #-32]!
    emit(arm64::stp_x_off(REG_CALL_TMP, REG_JIT_CTX, 31,
                          kNativeCallGuestRipOffset / sizeof(u64)));
    const usize entry = m_code_buf.size();
    m_code_buf[skip] = arm64::b_imm(static_cast<i32>(entry - skip) * 4);
    m_code_buf[below] =
        arm64::tbnz(3, 63, static_cast<i32>(entry - below) * 4);
}

void JitCompiler::emit_native_ret_check(u8 target_reg) {
    AttrScope attr_scope(*this, EmitAttribution::ExitProbe);
    if (!m_cfg.native_callret_enabled || !m_block_is_64bit) return;
    // Return natively only to a continuation frame this chain's landing pad
    // pushed for exactly this guest RIP. The tag is the owning JitContext*:
    // the {0, 0} pair execute() places above the dispatcher LR frame can
    // never match it, and no guest value can reach host stack memory. Any
    // other frame shape -- none at all, a frame for a different return RIP
    // (longjmp, PUSH+RET, a skipped return) -- falls through to the caller's
    // ordinary tail, which is correct for every guest RIP. The continuation
    // a frame names belongs to a CALL whose return RIP is that frame's, so
    // even a frame left behind by an abandoned activation sends this RIP to
    // the code that resumes exactly it.
    //
    // No hop budget: every uncounted step of a chain -- a forward edge, a
    // forward call, a native return to the instruction after its CALL --
    // strictly increases the (frame return RIPs..., current RIP) sequence in
    // lexicographic order, and frames are bounded by native_sp_floor; backward
    // edges and backward calls keep their counted budget.
    assert(target_reg != 3 && target_reg != 4);
    emit(arm64::ldp_x_off(3, 4, 31, kNativeCallGuestRipOffset / sizeof(u64)));
    emit(arm64::eor_x(3, 3, target_reg));
    emit(arm64::eor_x(4, 4, REG_JIT_CTX));
    emit(arm64::orr_x(3, 3, 4));
    const usize miss = m_code_buf.size();
    emit(0);  // CBNZ X3, .miss
    emit(0xF84207FEu);  // LDR X30, [SP], #32
    emit(arm64::ret());
    m_code_buf[miss] =
        arm64::cbnz_x(3, static_cast<i32>(m_code_buf.size() - miss) * 4);
}

void JitCompiler::emit_epilogue(JitExecResult result, bool clear_kind) {
    AttrScope attr_scope(*this, EmitAttribution::ExitDispatch);
    emit_store_pins();
    if (clear_kind) {
        emit_store_nzcv_kind(NzcvState::None);
    }
    // A native guest-CALL chain may exit through any ordinary epilogue:
    // linked miss, helper-raised exception, unsupported instruction, or the
    // hop budget. Discard its continuation frames before restoring the one
    // dispatcher LR frame. The feature is compile-time gated, so the default
    // path emits no reset.
    if (m_cfg.native_callret_enabled && m_block_is_64bit) {
        emit_discard_native_call_frames();
    }
    if (result == JitExecResult::Continue && m_block_is_64bit &&
        m_cfg.linear64 && materialize_process_raise_exit() != nullptr &&
        m_jit_ctx.continue_exit != nullptr) {
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_CONTINUE_EXIT >> 3));
        emit(0xD61F0200u);  // BR X16
        return;
    }
    // Restore LR saved in prologue, then return
    // LDR X30, [SP], #16
    emit(0xF84107FEu);
    emit(arm64::movz_w(0, static_cast<u16>(result)));
    emit(arm64::ret());
}

} // namespace vitreo::cpu::jit
