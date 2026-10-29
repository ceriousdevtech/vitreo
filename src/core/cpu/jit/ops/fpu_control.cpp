// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — x87 FPU ops: control and state. EMMS, FFREE, FNSTSW AX,
// FLDCW/FNSTCW, FNINIT, FNSTENV/FLDENV, FNSAVE/FRSTOR, FNOP/FWAIT, FNCLEX
// and FINCSTP/FDECSTP (Intel SDM Vol. 1 8.3.11-8.3.12).
//
// One family of compile_fpu_native's native lowerings, split out of
// ops/fpu.cpp (ADR-0006) as pure code motion: the blocks are verbatim and in
// their original order. compile_fpu_native (ops/fpu.cpp) owns the dispatch
// chain and the jit_fpu_op helper fallback; the declaration in
// jit_compiler.hpp states what the nullopt return means.

#include "../jit_helpers.hpp"
#include "fpu_internal.hpp"

#include <optional>

namespace vitreo::cpu::jit {

std::optional<bool> JitCompiler::compile_fpu_control(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const auto& op0 = instr.operands[0];

    // Intel SDM Vol. 2 EMMS: x87FPUTagWord := FFFFH (all tags empty).
    if (instr.opcode == Opcode::EMMS) {
        constexpr u32 tw_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, tag_word));
        emit_mov_imm32(REG_SCRATCH0, 0xFFFF);
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, tw_offset));
        emit(arm64::strh_reg(REG_SCRATCH0, REG_SCRATCH3, 31));
        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    // Intel SDM Vol. 2 FFREE: TAG(ST(i)) ← Empty (11B); TOP and contents
    // unchanged. Match interpreter set_tag(st_index(i), Empty).
    if (instr.opcode == Opcode::FFREE_STI && op0.type == OperandType::Imm8) {
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        constexpr u32 tag_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, tag_word));
        const u8 sti = static_cast<u8>(op0.imm) & 7;

        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
        emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
        emit(arm64::movz_w(REG_SCRATCH4, 7));
        emit(arm64::ubfx_w(REG_EA_TMP, REG_SCRATCH1, 11, 3));
        emit(arm64::movz_w(REG_SCRATCH2, sti));
        emit(arm64::add_w(REG_EA_TMP, REG_EA_TMP, REG_SCRATCH2));
        emit(arm64::and_w(REG_EA_TMP, REG_EA_TMP, REG_SCRATCH4));
        emit(arm64::lsl_w_imm(REG_EA_TMP, REG_EA_TMP, 1));

        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, tag_offset));
        emit(arm64::ldrh_reg(REG_SCRATCH2, REG_SCRATCH3, 31));
        emit(arm64::movz_w(REG_SCRATCH4,
                           static_cast<u16>(FpuTag::Empty)));
        emit(arm64::lslv_w(REG_SCRATCH4, REG_SCRATCH4, REG_EA_TMP));
        emit(arm64::orr_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::strh_reg(REG_SCRATCH2, REG_SCRATCH3, 31));

        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    if (instr.opcode == Opcode::FNSTSW_AX) {
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        // The scaled unsigned-offset LDRH reaches 2*4095 bytes; CpuState puts
        // fpu.status_word far inside that, but assert it rather than emit a
        // silently truncated offset if the struct ever grows.
        static_assert(status_offset % 2 == 0 && (status_offset >> 1) <= 4095,
                      "fpu.status_word out of LDRH unsigned-offset range");

        emit(arm64::ldrh_uoff(REG_SCRATCH0, REG_CPU_STATE, status_offset >> 1));

        // Intel SDM Vol. 2 FNSTSW AX: AX := FSW. Bits 63:16 of RAX are NOT
        // written. BFI inserts exactly bits 15:0 and leaves the rest of the
        // destination alone, so the old spill-to-CpuState / 16-bit store /
        // reload round trip is unnecessary -- RAX is pinned in X23 in both
        // bitnesses (jit_compiler.cpp:2903) and the pin is authoritative.
        //
        // The X form is mandatory in a 64-bit block: BFI Wd would zero
        // RAX[63:32]. In a 32-bit block the W form is correct AND preferred,
        // because a 32-bit pin holds only EAX and emit_store_pins deliberately
        // writes back 32 bits (jit_compiler.cpp:305-315).
        const u8 rax = static_cast<u8>(RegIndex::RAX);
        if (m_pins_live && m_pin_arm[rax] != 0xFF) {
            const u8 pin = m_pin_arm[rax];
            emit(m_block_is_64bit
                     ? arm64::bfi_x(pin, REG_SCRATCH0, 0, 16)
                     : arm64::bfi_w(pin, REG_SCRATCH0, 0, 16));
        } else {
            // VITREO_JIT_NO_PIN: CpuState is authoritative. A 16-bit store
            // leaves RAX[63:16] untouched by construction. The spill/reload
            // this replaces were already no-ops in this configuration
            // (jit_compiler.cpp:300).
            emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, gpr_off(rax)));
            emit(arm64::strh_reg(REG_SCRATCH0, REG_SCRATCH3, 31));
        }

        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    if (instr.opcode == Opcode::FLDCW_M16 && is_memory_operand(op0.type)) {
        constexpr u32 cw_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, control_word));
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
        // Hardware forces the control word's reserved bits: bit 6 reads back
        // set, bit 7 and bits 13-15 clear (measured on Windows 2026-08-30 --
        // FLDCW 0xFFFF reads back 0x1F7F, FLDCW 0x0000 reads back 0x0040).
        // Mirrors FpuState::sanitize_control_word.
        emit_mov_imm32(REG_SCRATCH1, FpuState::kControlWordKeepMask);
        emit(arm64::and_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_mov_imm32(REG_SCRATCH1, FpuState::kControlWordForcedBits);
        emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, cw_offset));
        emit(arm64::strh_reg(REG_SCRATCH0, REG_SCRATCH3, 31));
        // The control word selects the rounding mode, and the native
        // lowerings round on the host unit -- recording the word without
        // installing it leaves every later result rounded to nearest.
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
            &jit_fpu_apply_loaded_control_word)));
        emit_helper_blr();
        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    if (instr.opcode == Opcode::FNSTCW_M16 && is_memory_operand(op0.type)) {
        // Intel SDM Vol. 2, FSTCW/FNSTCW: FNSTCW stores the current 16-bit
        // x87 control word to m2byte without first checking for pending x87
        // exceptions; EFLAGS are unchanged. Keep the full computed EA here.
        // Delegating through jit_fpu_op rebuilt it as an i32 displacement,
        // which cannot represent a 64-bit guest destination above 4 GiB.
        constexpr u32 cw_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, control_word));
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, cw_offset));
        emit(arm64::ldrh_reg(REG_SCRATCH0, REG_SCRATCH3, 31));
        emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    if (instr.opcode == Opcode::FNINIT) {
        constexpr u32 cw_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, control_word));
        constexpr u32 sw_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        constexpr u32 tw_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, tag_word));
        constexpr u32 fop_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, fop));
        constexpr u32 fip_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, fip));
        constexpr u32 fdp_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, fdp));
        emit_mov_imm32(REG_SCRATCH0, 0x037F);
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, cw_offset));
        emit(arm64::strh_reg(REG_SCRATCH0, REG_SCRATCH3, 31));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, sw_offset));
        emit(arm64::strh_reg(31, REG_SCRATCH3, 31));
        emit_mov_imm32(REG_SCRATCH0, 0xFFFF);
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, tw_offset));
        emit(arm64::strh_reg(REG_SCRATCH0, REG_SCRATCH3, 31));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, fop_offset));
        emit(arm64::strh_reg(31, REG_SCRATCH3, 31));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, fip_offset));
        emit(arm64::str_x_uoff(31, REG_SCRATCH3, 0));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, fdp_offset));
        emit(arm64::str_x_uoff(31, REG_SCRATCH3, 0));
        // FNINIT resets the control word to 0x037F (round to nearest) and
        // clears the exception flags; both live on the host unit as well.
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
            &jit_fpu_clear_host)));
        emit_helper_blr();
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
            &jit_fpu_apply_rounding)));
        emit_helper_blr();
        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    if ((instr.opcode == Opcode::FNSTENV_M28 ||
         instr.opcode == Opcode::FLDENV_M28) &&
        is_memory_operand(op0.type)) {
        constexpr u32 cw_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, control_word));
        constexpr u32 sw_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        constexpr u32 tw_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, tag_word));
        if (instr.opcode == Opcode::FNSTENV_M28) {
            // The environment reads are where a guest asks for exception
            // state, so the host's accumulated flags have to be folded into
            // fpu.status_word before it is stored -- exec_fpu does the same.
            // Before emit_compute_ea: the helper call clobbers the scratch
            // registers the EA would be sitting in.
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                &jit_fpu_harvest)));
            emit_helper_blr();
        }
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH2));
        if (instr.opcode == Opcode::FNSTENV_M28) {
            // Each 16-bit field goes into a 32-bit slot whose reserved upper
            // half real hardware fills with ones (kFpuEnvReservedHigh), so OR
            // it in rather than zero-extending the halfword.
            //
            // Re-materialise the mask before EVERY use: emit_guest_store32
            // does a bounds-checked store and clobbers the scratch registers,
            // so a value hoisted above the first store does not survive to the
            // second.
            const auto store_field = [&](u32 field_offset, u32 image_offset) {
                emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, field_offset));
                emit(arm64::ldrh_reg(REG_SCRATCH0, REG_SCRATCH3, 31));
                emit_mov_imm32(REG_SCRATCH1, kFpuEnvReservedHigh);
                emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
                if (image_offset == 0) {
                    emit_guest_store32(REG_SCRATCH0, REG_CALL_TMP);
                } else {
                    emit(arm64::add_x_imm(REG_SCRATCH2, REG_CALL_TMP,
                                          image_offset));
                    emit_guest_store32(REG_SCRATCH0, REG_SCRATCH2);
                }
            };
            store_field(cw_offset, 0);
            store_field(sw_offset, 4);
            store_field(tw_offset, 8);
            // FIP / FCS|FOP / FDP stay zero; FDS's reserved half is ones.
            for (u32 off = 12; off <= 20; off += 4) {
                emit(arm64::add_x_imm(REG_SCRATCH2, REG_CALL_TMP, off));
                emit_guest_store32(31, REG_SCRATCH2);
            }
            emit_mov_imm32(REG_SCRATCH0, kFpuEnvReservedHigh);
            emit(arm64::add_x_imm(REG_SCRATCH2, REG_CALL_TMP, 24));
            emit_guest_store32(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, cw_offset));
            emit(arm64::ldrh_reg(REG_SCRATCH0, REG_SCRATCH3, 31));
            emit(arm64::movz_w(REG_SCRATCH4, 0x3F));
            emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH4));
            emit(arm64::strh_reg(REG_SCRATCH0, REG_SCRATCH3, 31));
        } else {
            emit_guest_load16(REG_SCRATCH0, REG_CALL_TMP);
            // Hardware forces the control word's reserved bits: bit 6 reads back
                // set, bit 7 and bits 13-15 clear (measured on Windows 2026-08-30 --
                // FLDCW 0xFFFF reads back 0x1F7F, FLDCW 0x0000 reads back 0x0040).
                // Mirrors FpuState::sanitize_control_word.
            emit_mov_imm32(REG_SCRATCH1, FpuState::kControlWordKeepMask);
            emit(arm64::and_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
            emit_mov_imm32(REG_SCRATCH1, FpuState::kControlWordForcedBits);
            emit(arm64::orr_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
            emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, cw_offset));
            emit(arm64::strh_reg(REG_SCRATCH0, REG_SCRATCH3, 31));
            emit(arm64::add_x_imm(REG_SCRATCH2, REG_CALL_TMP, 4));
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, sw_offset));
            emit(arm64::strh_reg(REG_SCRATCH0, REG_SCRATCH3, 31));
            emit(arm64::add_x_imm(REG_SCRATCH2, REG_CALL_TMP, 8));
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, tw_offset));
            emit(arm64::strh_reg(REG_SCRATCH0, REG_SCRATCH3, 31));
            // FLDENV restores a control word, and with it a rounding mode the
            // host unit has to be pointed at.
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                &jit_fpu_apply_rounding)));
            emit_helper_blr();
        }
        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    if ((instr.opcode == Opcode::FNSAVE_M108 ||
         instr.opcode == Opcode::FRSTOR_M108) &&
        is_memory_operand(op0.type)) {
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
            instr.opcode == Opcode::FNSAVE_M108 ? &jit_fnsave : &jit_frstor)));
        emit_helper_blr();
        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    // The shared waiting precheck at compile_fpu entry owns #MF delivery for
    // both instructions. With ES clear their primary operation is a no-op.
    if (instr.opcode == Opcode::FNOP || instr.opcode == Opcode::FWAIT) {
        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    if (instr.opcode == Opcode::FNCLEX) {
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
        emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
        emit_mov_imm32(REG_SCRATCH4, 0x7F00);
        emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
        // The host's accumulated flags go too, or the next harvest resurrects
        // exactly what was just cleared.
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
            &jit_fpu_clear_host)));
        emit_helper_blr();
        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    if (instr.opcode == Opcode::FINCSTP ||
        instr.opcode == Opcode::FDECSTP) {
        constexpr u32 status_offset = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
        emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
        emit(arm64::movz_w(REG_SCRATCH4, 7));
        emit(arm64::ubfx_w(REG_SCRATCH2, REG_SCRATCH1, 11, 3));
        emit(arm64::movz_w(REG_SCRATCH4, 1));
        if (instr.opcode == Opcode::FINCSTP) {
            emit(arm64::add_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        } else {
            emit(arm64::sub_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        }
        emit(arm64::movz_w(REG_SCRATCH4, 7));
        emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        emit_mov_imm32(REG_SCRATCH4, FPU_SW_TOP_MASK | FPU_SW_C1);
        emit(arm64::mvn_w(REG_SCRATCH4, REG_SCRATCH4));
        emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
        emit(arm64::lsl_w_imm(REG_SCRATCH2, REG_SCRATCH2, 11));
        emit(arm64::orr_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH2));
        emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
        if (is_last) {
            const u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(next_rip);
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    return std::nullopt;
}

} // namespace vitreo::cpu::jit
