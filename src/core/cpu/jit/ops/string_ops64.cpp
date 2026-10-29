// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — 64-bit block lowering: the quadword string ops. STOSQ,
// MOVSQ, CMPSQ and SCASQ.
//
// One family of compile_64bit's opcode space, split out of ops/ops64.cpp
// (ADR-0006 step 3) as pure code motion: the case arms are verbatim.
// compile_64bit (ops/ops64.cpp) owns the dispatch chain; see the
// declaration in jit_compiler.hpp for the nullopt / false / true contract.

#include "ops64_internal.hpp"

namespace vitreo::cpu::jit {

// nullopt means "not one of mine", so compile_64bit tries the next family.
// `break` means what it meant before the split: run the end-of-block tail
// and report success.
std::optional<bool> JitCompiler::compile_string_ops64(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // STOSQ — Intel SDM Vol. 2 STOS: store RAX to [RDI], then
    // RDI += DF ? -8 : +8. AddressSize=64 → RDI/RCX, matching jit_rep_stosq.
    // REP STOSQ takes the inline short-count loop (bulk helper kept as its
    // long-count tail); a 0x67 prefix makes the pointers 32-bit, which the
    // inline path does not model, so those stay on the helper.
    case Opcode::STOSQ: {
        if (instr.prefixes.rep) {
            if (instr.prefixes.address_size) {
                emit(arm64::mov_x(0, REG_CPU_STATE));
                emit(arm64::ldr_x_uoff(1, REG_JIT_CTX,
                                        JIT_CTX_OFF_MEMORY >> 3));
                emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                        JIT_CTX_OFF_REP_STOSQ >> 3));
                emit_helper_blr();
            } else {
                emit_rep_movs_stos_or_helper(8, true, JIT_CTX_OFF_REP_STOSQ);
            }
            break;
        }
        emit_load_gpr64(REG_SCRATCH1, static_cast<u8>(RegIndex::RAX));
        emit_load_gpr64(REG_SCRATCH0, static_cast<u8>(RegIndex::RDI));
        emit_guest_store64(REG_SCRATCH1, REG_SCRATCH0);

        u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        emit(0x530A2863u); // UBFX W3, W3, #10, #1 — DF

        emit_load_gpr64(REG_SCRATCH0, static_cast<u8>(RegIndex::RDI));
        usize cbz_pos = m_code_buf.size();
        emit(0);
        emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, 8));
        usize b_pos = m_code_buf.size();
        emit(0);
        usize df_clear_pos = m_code_buf.size();
        emit(arm64::add_x_imm(REG_SCRATCH0, REG_SCRATCH0, 8));
        usize skip_pos = m_code_buf.size();
        m_code_buf[cbz_pos] = arm64::cbz_w(REG_SCRATCH3,
            static_cast<i32>(df_clear_pos - cbz_pos) * 4);
        m_code_buf[b_pos] = arm64::b_imm(
            static_cast<i32>(skip_pos - b_pos) * 4);
        emit_store_gpr64(REG_SCRATCH0, static_cast<u8>(RegIndex::RDI));
        break;
    }

    // MOVSQ — Intel SDM Vol. 2 MOVS: copy qword [RSI] → [RDI], then
    // RSI/RDI += DF ? -8 : +8. AddressSize=64 → RSI/RDI/RCX, matching
    // jit_rep_movsq. REP MOVSQ takes the inline short-count loop; see STOSQ
    // above for why the 0x67 form stays on the helper.
    case Opcode::MOVSQ: {
        if (instr.prefixes.rep) {
            if (instr.prefixes.address_size) {
                emit(arm64::mov_x(0, REG_CPU_STATE));
                emit(arm64::ldr_x_uoff(1, REG_JIT_CTX,
                                        JIT_CTX_OFF_MEMORY >> 3));
                emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                        JIT_CTX_OFF_REP_MOVSQ >> 3));
                emit_helper_blr();
            } else {
                emit_rep_movs_stos_or_helper(8, false, JIT_CTX_OFF_REP_MOVSQ);
            }
            break;
        }
        emit_load_gpr64(REG_SCRATCH0, static_cast<u8>(RegIndex::RSI));
        emit_guest_load64(REG_SCRATCH1, REG_SCRATCH0);
        emit_load_gpr64(REG_SCRATCH0, static_cast<u8>(RegIndex::RDI));
        emit_guest_store64(REG_SCRATCH1, REG_SCRATCH0);

        u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        emit(0x530A2863u); // UBFX W3, W3, #10, #1 — DF

        emit_load_gpr64(REG_SCRATCH0, static_cast<u8>(RegIndex::RSI));
        emit_load_gpr64(REG_SCRATCH1, static_cast<u8>(RegIndex::RDI));
        usize cbz_pos = m_code_buf.size();
        emit(0);
        emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, 8));
        emit(arm64::sub_x_imm(REG_SCRATCH1, REG_SCRATCH1, 8));
        usize b_pos = m_code_buf.size();
        emit(0);
        usize df_clear_pos = m_code_buf.size();
        emit(arm64::add_x_imm(REG_SCRATCH0, REG_SCRATCH0, 8));
        emit(arm64::add_x_imm(REG_SCRATCH1, REG_SCRATCH1, 8));
        usize skip_pos = m_code_buf.size();
        m_code_buf[cbz_pos] = arm64::cbz_w(REG_SCRATCH3,
            static_cast<i32>(df_clear_pos - cbz_pos) * 4);
        m_code_buf[b_pos] = arm64::b_imm(
            static_cast<i32>(skip_pos - b_pos) * 4);
        emit_store_gpr64(REG_SCRATCH0, static_cast<u8>(RegIndex::RSI));
        emit_store_gpr64(REG_SCRATCH1, static_cast<u8>(RegIndex::RDI));
        break;
    }

    // CMPSQ — Intel SDM Vol. 2 CMPS: CMP qword [RSI],[RDI]; ZF early-exit
    // for REPE/REPNE. AddressSize=64 → RSI/RDI/RCX.
    case Opcode::CMPSQ: {
        if (instr.prefixes.rep && !instr.prefixes.repne) {
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX,
                                    JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                    JIT_CTX_OFF_REPE_CMPSQ >> 3));
            emit_helper_blr();
            break;
        }
        if (instr.prefixes.repne) {
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX,
                                    JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                    JIT_CTX_OFF_REPNE_CMPSQ >> 3));
            emit_helper_blr();
            break;
        }
        emit_load_gpr64(REG_SCRATCH2, static_cast<u8>(RegIndex::RSI));
        emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        emit_load_gpr64(REG_SCRATCH2, static_cast<u8>(RegIndex::RDI));
        emit_guest_load64(REG_SCRATCH1, REG_SCRATCH2);
        emit(arm64::sub_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags64(CpuState::FlagOp::Sub64,
                              REG_SCRATCH1, REG_SCRATCH0);

        u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        emit(0x530A2863u); // UBFX W3, W3, #10, #1 — DF

        emit_load_gpr64(REG_SCRATCH0, static_cast<u8>(RegIndex::RSI));
        emit_load_gpr64(REG_SCRATCH1, static_cast<u8>(RegIndex::RDI));
        usize cbz_pos = m_code_buf.size();
        emit(0);
        emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, 8));
        emit(arm64::sub_x_imm(REG_SCRATCH1, REG_SCRATCH1, 8));
        usize b_pos = m_code_buf.size();
        emit(0);
        usize df_clear_pos = m_code_buf.size();
        emit(arm64::add_x_imm(REG_SCRATCH0, REG_SCRATCH0, 8));
        emit(arm64::add_x_imm(REG_SCRATCH1, REG_SCRATCH1, 8));
        usize skip_pos = m_code_buf.size();
        m_code_buf[cbz_pos] = arm64::cbz_w(REG_SCRATCH3,
            static_cast<i32>(df_clear_pos - cbz_pos) * 4);
        m_code_buf[b_pos] = arm64::b_imm(
            static_cast<i32>(skip_pos - b_pos) * 4);
        emit_store_gpr64(REG_SCRATCH0, static_cast<u8>(RegIndex::RSI));
        emit_store_gpr64(REG_SCRATCH1, static_cast<u8>(RegIndex::RDI));
        break;
    }

    // SCASQ — Intel SDM Vol. 2 SCAS: CMP RAX, [RDI]; adjust RDI ±8.
    // REPE/REPNE use the bulk helpers (AddressSize=64 → RDI/RCX).
    case Opcode::SCASQ: {
        if (instr.prefixes.repne) {
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX,
                                    JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                    JIT_CTX_OFF_REPNE_SCASQ >> 3));
            emit_helper_blr();
            break;
        }
        if (instr.prefixes.rep) {
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX,
                                    JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                    JIT_CTX_OFF_REPE_SCASQ >> 3));
            emit_helper_blr();
            break;
        }
        emit_load_gpr64(REG_SCRATCH2, static_cast<u8>(RegIndex::RDI));
        emit_guest_load64(REG_SCRATCH1, REG_SCRATCH2);
        emit_load_gpr64(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
        emit(arm64::sub_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_set_lazy_flags64(CpuState::FlagOp::Sub64,
                              REG_SCRATCH1, REG_SCRATCH0);

        u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        emit(0x530A2863u); // UBFX W3, W3, #10, #1 — DF

        emit_load_gpr64(REG_SCRATCH0, static_cast<u8>(RegIndex::RDI));
        usize cbz_pos = m_code_buf.size();
        emit(0);
        emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, 8));
        usize b_pos = m_code_buf.size();
        emit(0);
        usize df_clear_pos = m_code_buf.size();
        emit(arm64::add_x_imm(REG_SCRATCH0, REG_SCRATCH0, 8));
        usize skip_pos = m_code_buf.size();
        m_code_buf[cbz_pos] = arm64::cbz_w(REG_SCRATCH3,
            static_cast<i32>(df_clear_pos - cbz_pos) * 4);
        m_code_buf[b_pos] = arm64::b_imm(
            static_cast<i32>(skip_pos - b_pos) * 4);
        emit_store_gpr64(REG_SCRATCH0, static_cast<u8>(RegIndex::RDI));
        break;
    }

    default: return std::nullopt;
    }

    finish_op64_block(next_rip, is_last);
    return true;
}

}  // namespace vitreo::cpu::jit
