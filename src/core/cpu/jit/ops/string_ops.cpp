// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — String ops (STOSD, MOVSB, MOVSD_STR — REP and non-REP)

#include "../jit_helpers.hpp"

#include <vitreo/diagnostics.hpp>

#include <cstdio>
#include <cstdlib>

namespace vitreo::cpu::jit {

namespace {
// MEASUREMENT KNOB (temporary): the element count at or below which a REP
// MOVS/STOS is walked inline instead of handed to the bulk C++ helper. The
// inline walk is one GUARDED guest access per element, so its cost is linear
// in the count while the helper's is a fixed call plus a bulk copy.
// The threshold is the immediate of one CMP, so the knob tops out at that
// field's 4095: a larger value is clamped there, with one line saying so,
// rather than masked (4096 once encoded CMP #0) or silently dropped.
u32 short_rep_elems() noexcept {
    static const u32 v = [] {
        if (const char* e = std::getenv("VITREO_JIT_SHORT_REP")) {
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(e, &end, 0);
            if (end != e && *end == '\0') {
                if (parsed > 0xFFFu) {
                    std::fprintf(stderr,
                                 "[vitreo] VITREO_JIT_SHORT_REP=%s exceeds the "
                                 "CMP immediate field; clamped to 4095\n", e);
                    return 0xFFFu;
                }
                return static_cast<u32>(parsed);
            }
        }
        return 64u;
    }();
    return v;
}
}  // namespace

void JitCompiler::emit_rep_movs_stos_or_helper(u32 elem_sz, bool is_stos,
                                               u32 helper_off) {
    auto emit_helper = [&]() {
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, helper_off >> 3));
        emit_helper_blr();
        // The REP helpers now report a faulting element as a pending
        // STATUS_ACCESS_VIOLATION instead of stopping silently; drain it
        // here or the block runs on past the fault.
        emit_exit_if_pending_raise();
    };

    // Unpinned blocks keep the C++ helper: the short path's whole point is
    // keeping the running count/pointers in host registers.
    // emit_guest_load/store may BLR, and emit_helper_blr round-trips every
    // pin through CpuState (emit_store_pins/emit_load_pins), so the walk
    // survives a cold memory helper without an explicit flush.
    const u8 ecx = m_pins_live ? m_pin_arm[static_cast<u8>(RegIndex::RCX)]
                               : 0xFF;
    const u8 esi = m_pins_live ? m_pin_arm[static_cast<u8>(RegIndex::RSI)]
                               : 0xFF;
    const u8 edi = m_pins_live ? m_pin_arm[static_cast<u8>(RegIndex::RDI)]
                               : 0xFF;
    const u8 eax = m_pins_live ? m_pin_arm[static_cast<u8>(RegIndex::RAX)]
                               : 0xFF;
    if (ecx == 0xFF || edi == 0xFF ||
        (!is_stos && esi == 0xFF) || (is_stos && eax == 0xFF)) {
        diagnostics::perf_note_rep_emit_helper();
        emit_helper();
        return;
    }
    diagnostics::perf_note_rep_emit_inline();

    // Address width, not data width. Intel SDM Vol. 1 §3.3.7 and Vol. 2
    // MOVS/STOS: the default address size is 64 in long mode and 32 in
    // protected mode, so the counter is RCX/ECX and the pointers RSI/RDI or
    // ESI/EDI. A 0x67 prefix picks the other width; the callers keep those
    // forms on the helper, so `m_block_is_64bit` decides it here. `elem_sz`
    // is independent — MOVSB in a 64-bit block moves one byte through 64-bit
    // RSI/RDI.
    const bool w64 = m_block_is_64bit;
    auto step_ptr = [&](u8 reg, bool backward) {
        if (backward) {
            emit(w64 ? arm64::sub_x_imm(reg, reg, elem_sz)
                     : arm64::sub_w_imm(reg, reg, elem_sz));
        } else {
            emit(w64 ? arm64::add_x_imm(reg, reg, elem_sz)
                     : arm64::add_w_imm(reg, reg, elem_sz));
        }
    };

    constexpr u8 kDf = REG_SCRATCH3;     // W3 — DF, consumed once
    constexpr u8 kVal = REG_SCRATCH1;    // X1/W1 — loaded element (MOVS)

    // Intel SDM Vol. 2, "REP/REPE/REPZ/REPNE/REPNZ": if the count register is
    // zero the instruction is not executed at all — RSI/RDI are NOT stepped.
    // In long mode that test is on all 64 bits of RCX: a count with zero low
    // half but a set high half is not zero.
    const usize done_cbz = m_code_buf.size();
    emit(0);  // CBZ (R|E)CX, done
    const u32 short_elems = short_rep_elems();
    emit(w64 ? arm64::cmp_x_imm(ecx, short_elems)
             : arm64::cmp_w_imm(ecx, short_elems));
    const usize b_hi = m_code_buf.size();
    emit(0);  // B.HI helper

    const u32 rf_off = cpu_rflags_offset();
    emit(arm64::ldr_x_uoff(kDf, REG_CPU_STATE, rf_off / 8));
    emit(0x530A2863u);  // UBFX W3, W3, #10, #1 — DF

    const usize df_cbz = m_code_buf.size();
    emit(0);  // CBZ W3, forward

    auto emit_elem = [&](bool backward) {
        if (is_stos) {
            if (elem_sz == 1) emit_guest_store8(eax, edi);
            else if (elem_sz == 2) emit_guest_store16(eax, edi);
            else if (elem_sz == 4) emit_guest_store32(eax, edi);
            else emit_guest_store64(eax, edi);
        } else {
            if (elem_sz == 1) emit_guest_load8(kVal, esi);
            else if (elem_sz == 2) emit_guest_load16(kVal, esi);
            else if (elem_sz == 4) emit_guest_load32(kVal, esi);
            else emit_guest_load64(kVal, esi);
            if (elem_sz == 1) emit_guest_store8(kVal, edi);
            else if (elem_sz == 2) emit_guest_store16(kVal, edi);
            else if (elem_sz == 4) emit_guest_store32(kVal, edi);
            else emit_guest_store64(kVal, edi);
            step_ptr(esi, backward);
        }
        step_ptr(edi, backward);
        emit(w64 ? arm64::subs_x_imm(ecx, ecx, 1)
                 : arm64::subs_w_imm(ecx, ecx, 1));
        // Width-correct writebacks. These emit nothing while the walk's
        // registers ARE their pins (the only case the gate above admits);
        // they exist so the loop stays correct if a pin ever stops backing
        // one of them, and they must go through emit_store_gpr_ptr so a
        // 32-bit block writes the W view and a 64-bit block the X view.
        if (!is_stos) emit_store_gpr_ptr(esi, static_cast<u8>(RegIndex::RSI));
        emit_store_gpr_ptr(edi, static_cast<u8>(RegIndex::RDI));
        emit_store_gpr_ptr(ecx, static_cast<u8>(RegIndex::RCX));
    };

    const usize loop_b = m_code_buf.size();
    emit_elem(true);
    const usize b_ne_b = m_code_buf.size();
    emit(0);  // B.NE loop_b
    const usize b_done_b = m_code_buf.size();
    emit(0);  // B done

    const usize loop_f = m_code_buf.size();
    emit_elem(false);
    const usize b_ne_f = m_code_buf.size();
    emit(0);  // B.NE loop_f
    const usize b_done_f = m_code_buf.size();
    emit(0);  // B done

    const usize helper_pos = m_code_buf.size();
    emit_helper();
    const usize done_pos = m_code_buf.size();

    const i32 done_off = static_cast<i32>(done_pos - done_cbz) * 4;
    m_code_buf[done_cbz] = w64 ? arm64::cbz_x(ecx, done_off)
                               : arm64::cbz_w(ecx, done_off);
    m_code_buf[b_hi] = arm64::b_cond(
        arm64::HI, static_cast<i32>(helper_pos - b_hi) * 4);
    m_code_buf[df_cbz] = arm64::cbz_w(
        kDf, static_cast<i32>(loop_f - df_cbz) * 4);
    m_code_buf[b_ne_b] = arm64::b_cond(
        arm64::NE, static_cast<i32>(loop_b - b_ne_b) * 4);
    m_code_buf[b_done_b] = arm64::b_imm(
        static_cast<i32>(done_pos - b_done_b) * 4);
    m_code_buf[b_ne_f] = arm64::b_cond(
        arm64::NE, static_cast<i32>(loop_f - b_ne_f) * 4);
    m_code_buf[b_done_f] = arm64::b_imm(
        static_cast<i32>(done_pos - b_done_f) * 4);
}

bool JitCompiler::compile_string_ops(const DecodedInstruction& instr,
                                      u64 instr_rip, u64 next_rip,
                                      bool is_last) {

    switch (instr.opcode) {

    // ---- STOSB / STOSW / STOSD — store AL/AX/EAX to [EDI], adjust EDI ±size ----
    case Opcode::STOSB:
    case Opcode::STOSW:
    case Opcode::STOSD: {
        const u32 elem_sz = (instr.opcode == Opcode::STOSB) ? 1u
                          : (instr.opcode == Opcode::STOSW) ? 2u : 4u;
        if (instr.prefixes.rep) {
            const u32 helper_off =
                  (instr.opcode == Opcode::STOSB) ? JIT_CTX_OFF_REP_STOSB
                : (instr.opcode == Opcode::STOSW) ? JIT_CTX_OFF_REP_STOSW
                                                  : JIT_CTX_OFF_REP_STOSD;
            if (instr.prefixes.address_size) {
                emit(arm64::mov_x(0, REG_CPU_STATE));
                emit(arm64::ldr_x_uoff(1, REG_JIT_CTX,
                                        JIT_CTX_OFF_MEMORY >> 3));
                emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, helper_off >> 3));
                emit_helper_blr();
                // The REP helpers now report a faulting element as a pending
                // STATUS_ACCESS_VIOLATION instead of stopping silently; drain it
                // here or the block runs on past the fault.
                emit_exit_if_pending_raise();
            } else {
                emit_rep_movs_stos_or_helper(elem_sz, true, helper_off);
            }
            break;
        }

        // DF lives in rflags bit 10 and is never written by lazy FlagOp
        // materialization — read it without settling pending ALU flags.
        emit_load_gpr32(REG_SCRATCH1, static_cast<u8>(RegIndex::RAX));
        emit_load_gpr_ptr(REG_SCRATCH0, static_cast<u8>(RegIndex::RDI));
        switch (elem_sz) {
        case 1: emit_guest_store8(REG_SCRATCH1, REG_SCRATCH0); break;
        case 2: emit_guest_store16(REG_SCRATCH1, REG_SCRATCH0); break;
        default: emit_guest_store32(REG_SCRATCH1, REG_SCRATCH0); break;
        }

        u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        emit(0x530A2863u); // UBFX W3, W3, #10, #1 — DF

        emit_load_gpr_ptr(REG_SCRATCH0, static_cast<u8>(RegIndex::RDI));

        usize cbz_pos = m_code_buf.size();
        emit(0);
        emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, elem_sz));
        usize b_pos = m_code_buf.size();
        emit(0);
        usize df_clear_pos = m_code_buf.size();
        emit(arm64::add_x_imm(REG_SCRATCH0, REG_SCRATCH0, elem_sz));
        usize skip_pos = m_code_buf.size();

        m_code_buf[cbz_pos] = arm64::cbz_w(REG_SCRATCH3,
            static_cast<i32>(df_clear_pos - cbz_pos) * 4);
        m_code_buf[b_pos] = arm64::b_imm(
            static_cast<i32>(skip_pos - b_pos) * 4);

        emit_store_gpr_ptr(REG_SCRATCH0, static_cast<u8>(RegIndex::RDI));
        break;
    }

    // ---- LODSB / LODSW / LODSD — load [ESI] into AL/AX/EAX, adjust ESI ±size ----
    // Intel SDM Vol. 2: LODS. REP forms share these opcodes via the REP prefix.
    case Opcode::LODSB:
    case Opcode::LODSW:
    case Opcode::LODSD: {
        if (instr.prefixes.rep) {
            const u32 helper_off =
                  (instr.opcode == Opcode::LODSB) ? JIT_CTX_OFF_REP_LODSB
                : (instr.opcode == Opcode::LODSW) ? JIT_CTX_OFF_REP_LODSW
                                                  : JIT_CTX_OFF_REP_LODSD;
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX,
                                    JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, helper_off >> 3));
            emit_helper_blr();
            // The REP helpers now report a faulting element as a pending
            // STATUS_ACCESS_VIOLATION instead of stopping silently; drain it
            // here or the block runs on past the fault.
            emit_exit_if_pending_raise();
            break;
        }
        const u32 elem_sz = (instr.opcode == Opcode::LODSB) ? 1u
                          : (instr.opcode == Opcode::LODSW) ? 2u : 4u;

        emit_load_gpr_ptr(REG_SCRATCH0, static_cast<u8>(RegIndex::RSI));
        switch (elem_sz) {
        case 1:
            emit_guest_load8(REG_SCRATCH1, REG_SCRATCH0);
            emit_store_gpr8(REG_SCRATCH1, static_cast<u8>(RegIndex::RAX));
            break;
        case 2:
            emit_guest_load16(REG_SCRATCH1, REG_SCRATCH0);
            emit_store_gpr16(REG_SCRATCH1, static_cast<u8>(RegIndex::RAX));
            break;
        default:
            emit_guest_load32(REG_SCRATCH1, REG_SCRATCH0);
            emit_store_gpr32(REG_SCRATCH1, static_cast<u8>(RegIndex::RAX));
            break;
        }

        // DF lives in rflags bit 10 — read without settling pending ALU flags.
        u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        emit(0x530A2863u); // UBFX W3, W3, #10, #1 — DF

        emit_load_gpr_ptr(REG_SCRATCH0, static_cast<u8>(RegIndex::RSI));

        usize cbz_pos = m_code_buf.size();
        emit(0);
        emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, elem_sz));
        usize b_pos = m_code_buf.size();
        emit(0);
        usize df_clear_pos = m_code_buf.size();
        emit(arm64::add_x_imm(REG_SCRATCH0, REG_SCRATCH0, elem_sz));
        usize skip_pos = m_code_buf.size();

        m_code_buf[cbz_pos] = arm64::cbz_w(REG_SCRATCH3,
            static_cast<i32>(df_clear_pos - cbz_pos) * 4);
        m_code_buf[b_pos] = arm64::b_imm(
            static_cast<i32>(skip_pos - b_pos) * 4);

        emit_store_gpr_ptr(REG_SCRATCH0, static_cast<u8>(RegIndex::RSI));
        break;
    }

    // ---- MOVSB — move byte [ESI] → [EDI], adjust ESI/EDI ----
    case Opcode::MOVSB: {
        if (instr.prefixes.rep) {
            if (instr.prefixes.address_size) {
                emit(arm64::mov_x(0, REG_CPU_STATE));
                emit(arm64::ldr_x_uoff(1, REG_JIT_CTX,
                                        JIT_CTX_OFF_MEMORY >> 3));
                emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                        JIT_CTX_OFF_REP_MOVSB >> 3));
                emit_helper_blr();
                // The REP helpers now report a faulting element as a pending
                // STATUS_ACCESS_VIOLATION instead of stopping silently; drain it
                // here or the block runs on past the fault.
                emit_exit_if_pending_raise();
            } else {
                emit_rep_movs_stos_or_helper(1, false, JIT_CTX_OFF_REP_MOVSB);
            }
        } else {
            // DF is a control flag — read rflags bit 10 without materializing
            // a pending lazy FlagOp.
            emit_load_gpr_ptr(REG_SCRATCH0, static_cast<u8>(RegIndex::RSI));
            emit_guest_load8(REG_SCRATCH2, REG_SCRATCH0);
            emit_load_gpr_ptr(REG_SCRATCH1, static_cast<u8>(RegIndex::RDI));
            emit_guest_store8(REG_SCRATCH2, REG_SCRATCH1);

            emit_load_gpr_ptr(REG_SCRATCH0, static_cast<u8>(RegIndex::RSI));
            emit_load_gpr_ptr(REG_SCRATCH1, static_cast<u8>(RegIndex::RDI));

            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit(0x530A2863u); // UBFX W3, W3, #10, #1

            usize cbz_pos = m_code_buf.size();
            emit(0);
            emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, 1));
            emit(arm64::sub_x_imm(REG_SCRATCH1, REG_SCRATCH1, 1));
            usize b_pos = m_code_buf.size();
            emit(0);
            usize df_clear_pos = m_code_buf.size();
            emit(arm64::add_x_imm(REG_SCRATCH0, REG_SCRATCH0, 1));
            emit(arm64::add_x_imm(REG_SCRATCH1, REG_SCRATCH1, 1));
            usize skip_pos = m_code_buf.size();

            m_code_buf[cbz_pos] = arm64::cbz_w(REG_SCRATCH3,
                static_cast<i32>(df_clear_pos - cbz_pos) * 4);
            m_code_buf[b_pos] = arm64::b_imm(
                static_cast<i32>(skip_pos - b_pos) * 4);

            emit_store_gpr_ptr(REG_SCRATCH0, static_cast<u8>(RegIndex::RSI));
            emit_store_gpr_ptr(REG_SCRATCH1, static_cast<u8>(RegIndex::RDI));
        }
        break;
    }

    // ---- MOVSW — move word [ESI] → [EDI], adjust ESI/EDI ----
    case Opcode::MOVSW: {
        if (instr.prefixes.rep) {
            if (instr.prefixes.address_size) {
                emit(arm64::mov_x(0, REG_CPU_STATE));
                emit(arm64::ldr_x_uoff(1, REG_JIT_CTX,
                                        JIT_CTX_OFF_MEMORY >> 3));
                emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                        JIT_CTX_OFF_REP_MOVSW >> 3));
                emit_helper_blr();
                // The REP helpers now report a faulting element as a pending
                // STATUS_ACCESS_VIOLATION instead of stopping silently; drain it
                // here or the block runs on past the fault.
                emit_exit_if_pending_raise();
            } else {
                emit_rep_movs_stos_or_helper(2, false, JIT_CTX_OFF_REP_MOVSW);
            }
            break;
        }
        // Non-REP MOVSW — same shape as MOVSB/MOVSD with elem_sz=2.
        emit_load_gpr_ptr(REG_SCRATCH0, static_cast<u8>(RegIndex::RSI));
        emit_guest_load16(REG_SCRATCH2, REG_SCRATCH0);
        emit_load_gpr_ptr(REG_SCRATCH1, static_cast<u8>(RegIndex::RDI));
        emit_guest_store16(REG_SCRATCH2, REG_SCRATCH1);

        emit_load_gpr_ptr(REG_SCRATCH0, static_cast<u8>(RegIndex::RSI));
        emit_load_gpr_ptr(REG_SCRATCH1, static_cast<u8>(RegIndex::RDI));

        u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        emit(0x530A2863u); // UBFX W3, W3, #10, #1

        usize cbz_pos = m_code_buf.size();
        emit(0);
        emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, 2));
        emit(arm64::sub_x_imm(REG_SCRATCH1, REG_SCRATCH1, 2));
        usize b_pos = m_code_buf.size();
        emit(0);
        usize df_clear_pos = m_code_buf.size();
        emit(arm64::add_x_imm(REG_SCRATCH0, REG_SCRATCH0, 2));
        emit(arm64::add_x_imm(REG_SCRATCH1, REG_SCRATCH1, 2));
        usize skip_pos = m_code_buf.size();

        m_code_buf[cbz_pos] = arm64::cbz_w(REG_SCRATCH3,
            static_cast<i32>(df_clear_pos - cbz_pos) * 4);
        m_code_buf[b_pos] = arm64::b_imm(
            static_cast<i32>(skip_pos - b_pos) * 4);

        emit_store_gpr_ptr(REG_SCRATCH0, static_cast<u8>(RegIndex::RSI));
        emit_store_gpr_ptr(REG_SCRATCH1, static_cast<u8>(RegIndex::RDI));
        break;
    }

    // ---- CMPSB / CMPSW / CMPSD — compare [ESI],[EDI]; ZF early-exit for REP* ----
    case Opcode::CMPSB:
    case Opcode::CMPSW:
    case Opcode::CMPSD_STR: {
        const u32 elem_sz = (instr.opcode == Opcode::CMPSB) ? 1u
                          : (instr.opcode == Opcode::CMPSW) ? 2u : 4u;

        if (instr.prefixes.rep && !instr.prefixes.repne) {
            // REPE/REPZ: loop while ECX>0 and ZF=1 (Intel SDM Vol. 2).
            const u32 helper_off =
                  (elem_sz == 1) ? JIT_CTX_OFF_REPE_CMPSB
                : (elem_sz == 2) ? JIT_CTX_OFF_REPE_CMPSW
                                 : JIT_CTX_OFF_REPE_CMPSD;
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX,
                                    JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, helper_off >> 3));
            emit_helper_blr();
            // The REP helpers now report a faulting element as a pending
            // STATUS_ACCESS_VIOLATION instead of stopping silently; drain it
            // here or the block runs on past the fault.
            emit_exit_if_pending_raise();
            break;
        }
        if (instr.prefixes.repne) {
            // REPNE/REPNZ: loop while ECX>0 and ZF=0.
            const u32 helper_off =
                  (elem_sz == 1) ? JIT_CTX_OFF_REPNE_CMPSB
                : (elem_sz == 2) ? JIT_CTX_OFF_REPNE_CMPSW
                                 : JIT_CTX_OFF_REPNE_CMPSD;
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX,
                                    JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, helper_off >> 3));
            emit_helper_blr();
            // The REP helpers now report a faulting element as a pending
            // STATUS_ACCESS_VIOLATION instead of stopping silently; drain it
            // here or the block runs on past the fault.
            emit_exit_if_pending_raise();
            break;
        }

        // Non-REP: single compare of [ESI] vs [EDI], then ±elem_sz on both.
        // Load both operands before the SUB (emit_guest_load uses X0 scratch).
        emit_load_gpr_ptr(REG_SCRATCH2, static_cast<u8>(RegIndex::RSI));
        CpuState::FlagOp flag_op;
        switch (elem_sz) {
        case 1:
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
            emit_load_gpr_ptr(REG_SCRATCH2, static_cast<u8>(RegIndex::RDI));
            emit_guest_load8(REG_SCRATCH1, REG_SCRATCH2);
            emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
            emit(arm64::and_w_imm(REG_SCRATCH0, REG_SCRATCH0, 0xFF)); // AND W0,W0,#0xFF
            flag_op = CpuState::FlagOp::Sub8;
            break;
        case 2:
            emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
            emit_load_gpr_ptr(REG_SCRATCH2, static_cast<u8>(RegIndex::RDI));
            emit_guest_load16(REG_SCRATCH1, REG_SCRATCH2);
            emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
            emit(arm64::and_w_imm(REG_SCRATCH0, REG_SCRATCH0, 0xFFFF)); // AND W0,W0,#0xFFFF
            flag_op = CpuState::FlagOp::Sub16;
            break;
        default:
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
            emit_load_gpr_ptr(REG_SCRATCH2, static_cast<u8>(RegIndex::RDI));
            emit_guest_load32(REG_SCRATCH1, REG_SCRATCH2);
            emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
            flag_op = CpuState::FlagOp::Sub32;
            break;
        }
        emit_set_lazy_flags(flag_op, REG_SCRATCH1, REG_SCRATCH0);

        u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        emit(0x530A2863u); // UBFX W3, W3, #10, #1 — DF

        emit_load_gpr_ptr(REG_SCRATCH0, static_cast<u8>(RegIndex::RSI));
        emit_load_gpr_ptr(REG_SCRATCH1, static_cast<u8>(RegIndex::RDI));

        usize cbz_pos = m_code_buf.size();
        emit(0);
        emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, elem_sz));
        emit(arm64::sub_x_imm(REG_SCRATCH1, REG_SCRATCH1, elem_sz));
        usize b_pos = m_code_buf.size();
        emit(0);
        usize df_clear_pos = m_code_buf.size();
        emit(arm64::add_x_imm(REG_SCRATCH0, REG_SCRATCH0, elem_sz));
        emit(arm64::add_x_imm(REG_SCRATCH1, REG_SCRATCH1, elem_sz));
        usize skip_pos = m_code_buf.size();

        m_code_buf[cbz_pos] = arm64::cbz_w(REG_SCRATCH3,
            static_cast<i32>(df_clear_pos - cbz_pos) * 4);
        m_code_buf[b_pos] = arm64::b_imm(
            static_cast<i32>(skip_pos - b_pos) * 4);

        emit_store_gpr_ptr(REG_SCRATCH0, static_cast<u8>(RegIndex::RSI));
        emit_store_gpr_ptr(REG_SCRATCH1, static_cast<u8>(RegIndex::RDI));
        break;
    }

    // ---- MOVSD_STR — move dword [ESI] → [EDI], adjust ESI/EDI ----
    case Opcode::MOVSD_STR: {
        if (instr.prefixes.rep) {
            if (instr.prefixes.address_size) {
                emit(arm64::mov_x(0, REG_CPU_STATE));
                emit(arm64::ldr_x_uoff(1, REG_JIT_CTX,
                                        JIT_CTX_OFF_MEMORY >> 3));
                emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                        JIT_CTX_OFF_REP_MOVSD >> 3));
                emit_helper_blr();
                // The REP helpers now report a faulting element as a pending
                // STATUS_ACCESS_VIOLATION instead of stopping silently; drain it
                // here or the block runs on past the fault.
                emit_exit_if_pending_raise();
            } else {
                emit_rep_movs_stos_or_helper(4, false, JIT_CTX_OFF_REP_MOVSD);
            }
        } else {
            // DF is a control flag — read rflags bit 10 without materializing.
            emit_load_gpr_ptr(REG_SCRATCH0, static_cast<u8>(RegIndex::RSI));
            emit_guest_load32(REG_SCRATCH2, REG_SCRATCH0);
            emit_load_gpr_ptr(REG_SCRATCH1, static_cast<u8>(RegIndex::RDI));
            emit_guest_store32(REG_SCRATCH2, REG_SCRATCH1);

            emit_load_gpr_ptr(REG_SCRATCH0, static_cast<u8>(RegIndex::RSI));
            emit_load_gpr_ptr(REG_SCRATCH1, static_cast<u8>(RegIndex::RDI));

            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit(0x530A2863u); // UBFX W3, W3, #10, #1

            usize cbz_pos = m_code_buf.size();
            emit(0);
            emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, 4));
            emit(arm64::sub_x_imm(REG_SCRATCH1, REG_SCRATCH1, 4));
            usize b_pos = m_code_buf.size();
            emit(0);
            usize df_clear_pos = m_code_buf.size();
            emit(arm64::add_x_imm(REG_SCRATCH0, REG_SCRATCH0, 4));
            emit(arm64::add_x_imm(REG_SCRATCH1, REG_SCRATCH1, 4));
            usize skip_pos = m_code_buf.size();

            m_code_buf[cbz_pos] = arm64::cbz_w(REG_SCRATCH3,
                static_cast<i32>(df_clear_pos - cbz_pos) * 4);
            m_code_buf[b_pos] = arm64::b_imm(
                static_cast<i32>(skip_pos - b_pos) * 4);

            emit_store_gpr_ptr(REG_SCRATCH0, static_cast<u8>(RegIndex::RSI));
            emit_store_gpr_ptr(REG_SCRATCH1, static_cast<u8>(RegIndex::RDI));
        }
        break;
    }

    // ---- SCASB / SCASW / SCASD — CMP AL/AX/EAX, [EDI]; adjust EDI ±size ----
    case Opcode::SCASB:
    case Opcode::SCASW:
    case Opcode::SCASD: {
        const u32 elem_sz = (instr.opcode == Opcode::SCASB) ? 1u
                          : (instr.opcode == Opcode::SCASW) ? 2u : 4u;

        if (instr.prefixes.repne) {
            // REPNE: stop on ZF=1 (match). SCASB → strlen-style host path.
            const u32 helper_off =
                  (elem_sz == 1) ? JIT_CTX_OFF_REPNE_SCASB
                : (elem_sz == 2) ? JIT_CTX_OFF_REPNE_SCASW
                                 : JIT_CTX_OFF_REPNE_SCASD;
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX,
                                    JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, helper_off >> 3));
            emit_helper_blr();
            // The REP helpers now report a faulting element as a pending
            // STATUS_ACCESS_VIOLATION instead of stopping silently; drain it
            // here or the block runs on past the fault.
            emit_exit_if_pending_raise();
            break;
        }
        if (instr.prefixes.rep) {
            // REPE: stop on ZF=0 (mismatch).
            const u32 helper_off =
                  (elem_sz == 1) ? JIT_CTX_OFF_REPE_SCASB
                : (elem_sz == 2) ? JIT_CTX_OFF_REPE_SCASW
                                 : JIT_CTX_OFF_REPE_SCASD;
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX,
                                    JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, helper_off >> 3));
            emit_helper_blr();
            // The REP helpers now report a faulting element as a pending
            // STATUS_ACCESS_VIOLATION instead of stopping silently; drain it
            // here or the block runs on past the fault.
            emit_exit_if_pending_raise();
            break;
        }

        // SCAS overwrites status flags via a new lazy Sub. DF (EDI step) is a
        // control flag readable from rflags without settling the prior FlagOp;
        // the prior op is simply replaced when we record Sub8/16/32 below.
        // Load mem into REG_SCRATCH1 first (emit_guest_load uses X0 as
        // slow-path scratch), then load AL/AX/EAX into REG_SCRATCH0.
        emit_load_gpr_ptr(REG_SCRATCH2, static_cast<u8>(RegIndex::RDI));
        CpuState::FlagOp flag_op;
        switch (elem_sz) {
        case 1:
            emit_guest_load8(REG_SCRATCH1, REG_SCRATCH2);
            emit_load_gpr8(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
            emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
            emit(arm64::and_w_imm(REG_SCRATCH0, REG_SCRATCH0, 0xFF)); // AND W0,W0,#0xFF
            flag_op = CpuState::FlagOp::Sub8;
            break;
        case 2:
            emit_guest_load16(REG_SCRATCH1, REG_SCRATCH2);
            emit_load_gpr16(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
            emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
            emit(arm64::and_w_imm(REG_SCRATCH0, REG_SCRATCH0, 0xFFFF)); // AND W0,W0,#0xFFFF
            flag_op = CpuState::FlagOp::Sub16;
            break;
        default:
            emit_guest_load32(REG_SCRATCH1, REG_SCRATCH2);
            emit_load_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RAX));
            emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
            flag_op = CpuState::FlagOp::Sub32;
            break;
        }
        emit_set_lazy_flags(flag_op, REG_SCRATCH1, REG_SCRATCH0);

        // Adjust EDI by ±elem_sz based on DF (bit 10 of rflags).
        u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        emit(0x530A2863u); // UBFX W3, W3, #10, #1 — DF bit

        emit_load_gpr_ptr(REG_SCRATCH0, static_cast<u8>(RegIndex::RDI));

        usize cbz_pos = m_code_buf.size();
        emit(0);
        emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, elem_sz));
        usize b_pos = m_code_buf.size();
        emit(0);
        usize df_clear_pos = m_code_buf.size();
        emit(arm64::add_x_imm(REG_SCRATCH0, REG_SCRATCH0, elem_sz));
        usize skip_pos = m_code_buf.size();

        m_code_buf[cbz_pos] = arm64::cbz_w(REG_SCRATCH3,
            static_cast<i32>(df_clear_pos - cbz_pos) * 4);
        m_code_buf[b_pos] = arm64::b_imm(
            static_cast<i32>(skip_pos - b_pos) * 4);

        emit_store_gpr_ptr(REG_SCRATCH0, static_cast<u8>(RegIndex::RDI));
        break;
    }

    default: return false;
    }

    if (is_last) {
        u32 ji_off = jit_iters_offset();
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
        emit_update_rip(next_rip);
        emit_epilogue(JitExecResult::Continue);
    }
    return true;
}

} // namespace vitreo::cpu::jit
