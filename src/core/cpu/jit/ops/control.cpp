// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — Control flow opcodes (Jcc, JMP, CALL, RET, LEAVE)

#include "../jit_helpers.hpp"
#include <vitreo/diagnostics.hpp>
#include <atomic>
#include <cstdlib>

namespace vitreo::cpu::jit {

namespace {

// ---------------------------------------------------------------------------
// MS 32-bit CRT 64-bit-shift helper inliner
//
// MSVC-compiled 32-bit binaries emit frequent calls to the CRT 64-bit
// shift helpers `__allshl` and `__aullshr` for any 64-bit shift on x86.
// In LZMA-style decompression loops these dominate execution: each
// iteration calls into a ~6-instruction helper twice, paying the JIT
// dispatcher round-trip (CALL → block lookup → BLR → RET → block lookup →
// BLR) on every call. Recognizing the helper from its byte signature and
// emitting an inline ARM64 64-bit shift removes that overhead entirely.
//
// Helper signatures (verified by disassembly of a 32-bit MSVC image):
//   __allshl :  80 F9 40 73 ?? 80 F9 20 73 ?? 0F A5 C2 D3 E0 C3
//   __aullshr:  80 F9 40 73 ?? 80 F9 20 73 ?? 0F AD D0 D3 EA C3
//
// Semantics (Microsoft x86 fastcall-ish stdcall, EDX:EAX shifted by CL):
//   __allshl(EDX:EAX, CL)  →  EDX:EAX = (u64(EDX:EAX) << (CL & 0xFF))
//                            (CL ≥ 64 ⇒ result = 0; CL ≥ 32 ⇒ EAX = 0)
//   __aullshr(EDX:EAX, CL) →  EDX:EAX = (u64(EDX:EAX) >> (CL & 0xFF))
//                            (CL ≥ 64 ⇒ result = 0)
// Helpers preserve ECX, do NOT preserve EFLAGS.
// ---------------------------------------------------------------------------

enum class CrtShiftHelper { None, Allshl, Aullshr };

CrtShiftHelper match_crt_shift_helper(GuestMemory& mem, u64 target) noexcept {
    u8 b[16] = {};
    auto rd = mem.read_bytes(static_cast<GuestAddr>(target), b, sizeof(b));
    if (rd.is_err()) return CrtShiftHelper::None;
    // Common prologue: CMP CL,0x40 / JAE / CMP CL,0x20 / JAE
    if (b[0] != 0x80 || b[1] != 0xF9 || b[2] != 0x40) return CrtShiftHelper::None;
    if (b[3] != 0x73) return CrtShiftHelper::None;
    if (b[5] != 0x80 || b[6] != 0xF9 || b[7] != 0x20) return CrtShiftHelper::None;
    if (b[8] != 0x73) return CrtShiftHelper::None;
    // Differentiating tail
    // __allshl : 0F A5 C2  D3 E0  C3   (shld edx,eax,cl ; shl eax,cl ; ret)
    // __aullshr: 0F AD D0  D3 EA  C3   (shrd eax,edx,cl ; shr edx,cl ; ret)
    if (b[10] == 0x0F && b[11] == 0xA5 && b[12] == 0xC2 &&
        b[13] == 0xD3 && b[14] == 0xE0 && b[15] == 0xC3) {
        return CrtShiftHelper::Allshl;
    }
    if (b[10] == 0x0F && b[11] == 0xAD && b[12] == 0xD0 &&
        b[13] == 0xD3 && b[14] == 0xEA && b[15] == 0xC3) {
        return CrtShiftHelper::Aullshr;
    }
    return CrtShiftHelper::None;
}

// Hit counter for diagnostics (visible via VITREO_TRACE).
std::atomic<u64> g_crt_shift_inline_hits{0};

} // namespace

bool JitCompiler::try_emit_crt_shift_inline(u64 target, u64 next_rip, bool is_last) {
    static const bool s_disabled =
        (std::getenv("VITREO_DISABLE_CRT_SHIFT_INLINE") != nullptr);
    if (s_disabled) return false;

    auto kind = match_crt_shift_helper(m_memory, target);
    if (kind == CrtShiftHelper::None) return false;

    using namespace arm64;
    constexpr u8 EAX_IDX = static_cast<u8>(RegIndex::RAX);
    constexpr u8 ECX_IDX = static_cast<u8>(RegIndex::RCX);
    constexpr u8 EDX_IDX = static_cast<u8>(RegIndex::RDX);

    // Layout of scratch registers:
    //   W0/X0 = lo (EAX)
    //   W1/X1 = hi (EDX)
    //   W2/X2 = cl (ECX, masked to byte)
    //   X3    = full 64-bit value (combined / shifted)
    //   X4    = high 32 bits after split (for EDX writeback)

    emit_load_gpr32(REG_SCRATCH0, EAX_IDX);          // W0 = EAX
    emit_load_gpr32(REG_SCRATCH1, EDX_IDX);          // W1 = EDX
    emit_load_gpr32(REG_SCRATCH2, ECX_IDX);          // W2 = ECX
    emit(uxtb_w(REG_SCRATCH2, REG_SCRATCH2));        // W2 = CL (zero-extended)

    // X3 = X0 | (X1 << 32)   — combine lo:hi into 64-bit value
    emit(orr_x_lsl(REG_SCRATCH3, REG_SCRATCH0, REG_SCRATCH1, 32));

    // If CL >= 64, force value to zero (covers _allshl/_aullshr CL>=64 case).
    emit(cmp_w_imm(REG_SCRATCH2, 64));
    emit(csel_x(REG_SCRATCH3, /*Xn=*/31, /*Xm=*/REG_SCRATCH3, /*HS=CS*/CS));

    if (kind == CrtShiftHelper::Allshl) {
        emit(lslv_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2));
    } else {
        emit(lsrv_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2));
    }

    // Split back to EAX/EDX. emit_store_gpr32 stores W (low 32) and zeroes hi64.
    emit_store_gpr32(REG_SCRATCH3, EAX_IDX);
    emit(lsr_x_imm(REG_SCRATCH4, REG_SCRATCH3, 32));
    emit_store_gpr32(REG_SCRATCH4, EDX_IDX);

    // Diagnostics: count hits at compile time.
    g_crt_shift_inline_hits.fetch_add(1, std::memory_order_relaxed);
    static const bool trace =
        (std::getenv("VITREO_TRACE_CRT_INLINE") != nullptr);
    if (trace) {
        std::fprintf(stderr,
                     "[crt-inline] %s target=0x%llx -> inlined (#%llu)\n",
                     kind == CrtShiftHelper::Allshl ? "_allshl" : "_aullshr",
                     (unsigned long long)target,
                     (unsigned long long)g_crt_shift_inline_hits.load(
                         std::memory_order_relaxed));
    }

    // The CALL is replaced by inline code: no stack push, no RIP transfer.
    // RIP advances to next_rip, exactly as if the helper had returned.
    if (is_last) {
        u32 ji_off = jit_iters_offset();
        emit_update_rip(next_rip);
        emit_tail_link_or_exit(next_rip);
    }
    return true;
}

bool JitCompiler::compile_control(const DecodedInstruction& instr,
                                   u64 instr_rip, u64 next_rip,
                                   bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    const bool is_high_addr_mode = ((instr_rip >> 32) != 0) || ((next_rip >> 32) != 0);

    switch (instr.opcode) {

    // ---- Jcc — all 32 conditional branch variants ----
    case Opcode::JZ_REL8:  case Opcode::JZ_REL32:
    case Opcode::JNZ_REL8: case Opcode::JNZ_REL32:
    case Opcode::JB_REL8:  case Opcode::JB_REL32:
    case Opcode::JNB_REL8: case Opcode::JNB_REL32:
    case Opcode::JBE_REL8: case Opcode::JBE_REL32:
    case Opcode::JNBE_REL8: case Opcode::JNBE_REL32:
    case Opcode::JL_REL8:  case Opcode::JL_REL32:
    case Opcode::JNL_REL8: case Opcode::JNL_REL32:
    case Opcode::JLE_REL8: case Opcode::JLE_REL32:
    case Opcode::JNLE_REL8: case Opcode::JNLE_REL32:
    case Opcode::JS_REL8:  case Opcode::JS_REL32:
    case Opcode::JNS_REL8: case Opcode::JNS_REL32:
    case Opcode::JO_REL8:  case Opcode::JO_REL32:
    case Opcode::JNO_REL8: case Opcode::JNO_REL32:
    case Opcode::JP_REL8:  case Opcode::JP_REL32:
    case Opcode::JNP_REL8: case Opcode::JNP_REL32: {
        u8 cond = jcc_condition_code(instr.opcode);
        if (cond == 0xFF) return false;

        u64 target = next_rip + static_cast<u64>(static_cast<i64>(op0.imm));

        // Flag fusion: if the immediately-preceding instruction was a CMP/TEST
        // that left NZCV valid, Jcc is a native B.cond. Mid-block taken exits
        // go to a cold stub. Loop-back and terminal use the same fusion when
        // fac >= 0. Intel SDM Vol. 2 Jcc reads the prior flag-writing insn.
        int fac = fused_arm_cond(m_flags.incoming_nzcv, cond);

        u32 loop_arm_pos = m_body_start_pos;
        const bool is_loop_back = find_in_block_loop_pos(target, loop_arm_pos);

        // Mid-block forward Jcc: keep fall-through in this translation and
        // park the taken exit after the body (B.cond / CBNZ). Inline taken
        // stubs between CSET and the next insn split the hot path.
        if (!is_last && !is_loop_back) {
            queue_mid_block_jcc_exit(fac, cond, target);
            return true;
        }

        // Fuse whenever the immediate predecessor left NZCV for this cc.
        // Loop-back used to force CSET+CBZ so a later SUBS (iter cap) could
        // not be mistaken for leftover host NZCV. That tax is unnecessary
        // when fac >= 0: the Jcc reads the predecessor, not the cap check.
        // Do not fuse when fac < 0 (CMP; MOVAPS; JNE — MOVAPS clobbers NZCV).
        const bool fused_jcc = fac >= 0;
        const bool jcc_first_carried =
            !fused_jcc && !is_loop_back && can_try_carried_jcc(cond);
        const int carried_ac = jcc_first_carried
            ? fused_arm_cond(NzcvState::Sub, cond, KindGuarded{}) : -1;
        NzcvState jcc_carry = NzcvState::None;
        if (fused_jcc) {
            jcc_carry = m_flags.incoming_nzcv;
            m_flags.nzcv = m_flags.incoming_nzcv;
            diagnostics::perf_note_jcc_emit_fused();
        } else if (jcc_first_carried && carried_ac >= 0) {
            jcc_carry = NzcvState::Carried;
            m_flags.nzcv = NzcvState::Carried;
            diagnostics::perf_note_jcc_emit_carried();
        } else {
            if (m_incoming_stub_store.valid) {
                emit_lazy_flag_store_raw(m_incoming_stub_store.op,
                                         m_incoming_stub_store.src,
                                         m_incoming_stub_store.dst,
                                         m_incoming_stub_store.wide);
                note_elided_record_republished();
            }
            jcc_carry = emit_eval_jcc(cond, fac);
            diagnostics::perf_note_jcc_emit_helper();
        }

        if (is_loop_back) {
            usize skip_pos = m_code_buf.size();
            emit(0);
            // A mid-block loop header skips the body-start tier site.
            if (loop_arm_pos != m_body_start_pos) emit_tier_entry_counter();

            if (!m_block_is_64bit) {
                static_assert(MAX_IN_BLOCK_LOOP_ITERS == 0xFFFFu);
                emit(arm64::add_w_imm(REG_LOOP_CTR, REG_LOOP_CTR, 1));
                emit(arm64::tbnz(REG_LOOP_CTR, 16, 8));
                i32 loop_off = static_cast<i32>(loop_arm_pos
                             - m_code_buf.size()) * 4;
                emit(arm64::b_imm(loop_off));
                emit(arm64::sub_w_imm(REG_LOOP_CTR, REG_LOOP_CTR, 1));
            } else {
                emit(arm64::movz_w(REG_SCRATCH1,
                                   static_cast<u16>(MAX_IN_BLOCK_LOOP_ITERS)));
                emit(arm64::subs_w(31, REG_LOOP_CTR, REG_SCRATCH1));
                usize bcs_pos = m_code_buf.size();
                emit(0);

                emit(arm64::add_w_imm(REG_LOOP_CTR, REG_LOOP_CTR, 1));
                i32 loop_off = static_cast<i32>(loop_arm_pos
                             - m_code_buf.size()) * 4;
                emit(arm64::b_imm(loop_off));

                usize exit_pos = m_code_buf.size();
                i32 bcs_off = static_cast<i32>(exit_pos - bcs_pos) * 4;
                m_code_buf[bcs_pos] = arm64::b_cond(arm64::CS, bcs_off);
            }

            if (fused_jcc && m_incoming_stub_store.valid) {
                emit_lazy_flag_store_raw(m_incoming_stub_store.op,
                                         m_incoming_stub_store.src,
                                         m_incoming_stub_store.dst,
                                         m_incoming_stub_store.wide);
            }
            u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                                    ji_off >> 2));
            emit_update_rip(instr_rip);
            emit_epilogue(JitExecResult::Continue);

            usize nt_pos = m_code_buf.size();
            i32 skip_off = static_cast<i32>(nt_pos - skip_pos) * 4;
            if (fused_jcc) {
                m_code_buf[skip_pos] = arm64::b_cond(
                    static_cast<u8>(fac) ^ 1u, skip_off);
            } else {
                m_code_buf[skip_pos] = arm64::cbz_w(0, skip_off);
            }

            emit_update_rip(next_rip);
            // Stage B1 (extended): link Jcc loop-back fall-through leg.
            emit_tail_link_or_exit(next_rip, jcc_carry);
        } else {
            usize kind_cbz = static_cast<usize>(-1);
            if (jcc_first_carried && carried_ac >= 0) {
                emit(arm64::ldrb_uoff(1, REG_CPU_STATE, nzcv_kind_offset()));
                kind_cbz = m_code_buf.size();
                emit(0);  // CBZ W1, .lazy
            }

            usize skip_pos = m_code_buf.size();
            emit(0);

            const usize taken_pos = m_code_buf.size();
            u32 ji_off = jit_iters_offset();
            emit_update_rip(target);
            // Stage B1 (extended): link Jcc taken leg.
            emit_tail_link_or_exit(target, jcc_carry, LinkEdge::TermJcc);

            usize lazy_cbnz = static_cast<usize>(-1);
            if (kind_cbz != static_cast<usize>(-1)) {
                const usize lazy = m_code_buf.size();
                m_code_buf[kind_cbz] = arm64::cbz_w(
                    1, static_cast<i32>(lazy - kind_cbz) * 4);
                emit_eval_lazy_condition(cond);
                emit_store_nzcv_kind(NzcvState::None);
                lazy_cbnz = m_code_buf.size();
                emit(0);  // CBNZ W0, .taken
            }

            usize nt_pos = m_code_buf.size();
            i32 skip_off = static_cast<i32>(nt_pos - skip_pos) * 4;
            if (fused_jcc) {
                m_code_buf[skip_pos] = arm64::b_cond(
                    static_cast<u8>(fac) ^ 1u, skip_off);
            } else if (kind_cbz != static_cast<usize>(-1)) {
                m_code_buf[skip_pos] = arm64::b_cond(
                    static_cast<u8>(carried_ac) ^ 1u, skip_off);
                m_code_buf[lazy_cbnz] = arm64::cbnz_w(
                    0, static_cast<i32>(taken_pos - lazy_cbnz) * 4);
            } else {
                m_code_buf[skip_pos] = arm64::cbz_w(0, skip_off);
            }

            emit_update_rip(next_rip);
            // Stage B1 (extended): link Jcc fall-through leg.
            emit_tail_link_or_exit(next_rip, jcc_carry, LinkEdge::TermFall);
        }
        return true;
    }

    // ---- LOOP / LOOPE / LOOPNE / JCXZ / JECXZ (rel8) ----
    // Intel SDM Vol. 2: AddressSize selects CX vs ECX (32-bit mode). LOOP*
    // decrements first, then tests; JCXZ/JECXZ only test for zero.
    case Opcode::LOOP_REL8:
    case Opcode::LOOPE_REL8:
    case Opcode::LOOPNE_REL8:
    case Opcode::JCXZ_REL8:
    case Opcode::JECXZ_REL8:
    case Opcode::JRCXZ_REL8: {
        // Intel SDM Vol. 2 LOOP / JrCXZ: the count register follows the
        // ADDRESS size — CX/ECX in 32-bit mode (67H selects CX), RCX in
        // 64-bit mode. Decrementing ECX in a 64-bit block both loses the top
        // half of RCX and takes the wrong branch when only the low half is
        // zero. 67H in long mode selects ECX, which neither this lowering nor
        // the interpreter models, so leave that form to the delegate.
        if (m_block_is_64bit && instr.prefixes.address_size) return false;
        u64 target = next_rip + static_cast<u64>(static_cast<i64>(op0.imm));
        // Intel SDM Vol. 2 LOOP/JrCXZ: a non-canonical target is #GP(0) at
        // the branch with nothing committed -- not even LOOP's decrement
        // (noncanonical_branch_probe). The target is a translation-time
        // constant, so such a branch is left to the interpreter.
        if (m_block_is_64bit &&
            static_cast<u64>(static_cast<i64>(target << 16) >> 16) != target) {
            return false;
        }
        const bool use_rcx = m_block_is_64bit;
        const bool use_cx = !m_block_is_64bit && instr.prefixes.address_size;
        const u8 rcx = static_cast<u8>(RegIndex::RCX);
        const bool is_jcxz =
            instr.opcode == Opcode::JCXZ_REL8 ||
            instr.opcode == Opcode::JECXZ_REL8 ||
            instr.opcode == Opcode::JRCXZ_REL8;

        if (is_jcxz) {
            if (use_rcx) {
                emit_load_gpr64(REG_SCRATCH0, rcx);
                emit(arm64::cmp_x_imm(REG_SCRATCH0, 0));
            } else if (use_cx) {
                emit_load_gpr16(REG_SCRATCH0, rcx);
                emit(arm64::cmp_w_imm(REG_SCRATCH0, 0));
            } else {
                emit_load_gpr32(REG_SCRATCH0, rcx);
                emit(arm64::cmp_w_imm(REG_SCRATCH0, 0));
            }
            emit(arm64::cset_w(0, arm64::EQ));
        } else {
            if (use_rcx) {
                emit_load_gpr64(REG_SCRATCH0, rcx);
                emit(arm64::subs_x_imm(REG_SCRATCH0, REG_SCRATCH0, 1));
                emit_store_gpr64(REG_SCRATCH0, rcx);
            } else if (use_cx) {
                emit_load_gpr16(REG_SCRATCH0, rcx);
                emit(arm64::subs_w_imm(REG_SCRATCH0, REG_SCRATCH0, 1));
                emit_store_gpr16(REG_SCRATCH0, rcx);
            } else {
                emit_load_gpr32(REG_SCRATCH0, rcx);
                emit(arm64::subs_w_imm(REG_SCRATCH0, REG_SCRATCH0, 1));
                emit_store_gpr32(REG_SCRATCH0, rcx);
            }
            // NZCV still reflects count after SUBS (store is flag-neutral).
            if (instr.opcode == Opcode::LOOP_REL8) {
                emit(arm64::cset_w(0, arm64::NE));
            } else {
                // LOOPE/LOOPNE also need ZF from architectural flags. Count
                // SUBS already clobbered NZCV, so always use the cond helper
                // (never fuse). Preserve count!=0 in callee-saved W21.
                emit(arm64::cset_w(REG_CALL_TMP, arm64::NE));
                const u8 zf_cc =
                    (instr.opcode == Opcode::LOOPE_REL8) ? 0x4u : 0x5u;
                emit(arm64::mov_x(0, REG_CPU_STATE));
                emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                        (JIT_CTX_OFF_COND_FNS + zf_cc * 8u) >> 3));
                emit_helper_blr();
                emit(arm64::and_w(0, 0, REG_CALL_TMP));
            }
        }

        u32 loop_arm_pos = m_body_start_pos;
        const bool is_loop_back = find_in_block_loop_pos(target, loop_arm_pos);

        if (!is_last && !is_loop_back) {
            const usize fallthrough_pos = m_code_buf.size();
            emit(0); // CBZ W0, continue translating the fall-through path

            const u32 ji_off = jit_iters_offset();
            emit_update_rip(target);
            emit_tail_link_or_exit(target);

            const usize continue_pos = m_code_buf.size();
            m_code_buf[fallthrough_pos] = arm64::cbz_w(
                0, static_cast<i32>(continue_pos - fallthrough_pos) * 4);
        } else if (is_loop_back) {
            usize cbz_pos = m_code_buf.size();
            emit(0);
            // A mid-block loop header skips the body-start tier site.
            if (loop_arm_pos != m_body_start_pos) emit_tier_entry_counter();

            if (!m_block_is_64bit) {
                static_assert(MAX_IN_BLOCK_LOOP_ITERS == 0xFFFFu);
                emit(arm64::add_w_imm(REG_LOOP_CTR, REG_LOOP_CTR, 1));
                emit(arm64::tbnz(REG_LOOP_CTR, 16, 8));
                i32 loop_off = static_cast<i32>(loop_arm_pos
                             - m_code_buf.size()) * 4;
                emit(arm64::b_imm(loop_off));
                emit(arm64::sub_w_imm(REG_LOOP_CTR, REG_LOOP_CTR, 1));
            } else {
                emit(arm64::movz_w(REG_SCRATCH1,
                                   static_cast<u16>(MAX_IN_BLOCK_LOOP_ITERS)));
                emit(arm64::subs_w(31, REG_LOOP_CTR, REG_SCRATCH1));
                usize bcs_pos = m_code_buf.size();
                emit(0);

                emit(arm64::add_w_imm(REG_LOOP_CTR, REG_LOOP_CTR, 1));
                i32 loop_off = static_cast<i32>(loop_arm_pos
                             - m_code_buf.size()) * 4;
                emit(arm64::b_imm(loop_off));

                usize exit_pos = m_code_buf.size();
                i32 bcs_off = static_cast<i32>(exit_pos - bcs_pos) * 4;
                m_code_buf[bcs_pos] = arm64::b_cond(arm64::CS, bcs_off);
            }

            u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                                    ji_off >> 2));
            // LOOP has already retired its CX/ECX decrement. Resuming PE32 at
            // the opcode would decrement it twice; continue from its target.
            emit_update_rip(m_block_is_64bit ? instr_rip : target);
            emit_epilogue(JitExecResult::Continue);

            usize nt_pos = m_code_buf.size();
            i32 cbz_off = static_cast<i32>(nt_pos - cbz_pos) * 4;
            m_code_buf[cbz_pos] = arm64::cbz_w(0, cbz_off);

            emit_update_rip(next_rip);
            emit_tail_link_or_exit(next_rip);
        } else {
            usize cbz_pos = m_code_buf.size();
            emit(0);

            u32 ji_off = jit_iters_offset();
            emit_update_rip(target);
            emit_tail_link_or_exit(target);

            usize nt_pos = m_code_buf.size();
            i32 cbz_off = static_cast<i32>(nt_pos - cbz_pos) * 4;
            m_code_buf[cbz_pos] = arm64::cbz_w(0, cbz_off);

            emit_update_rip(next_rip);
            emit_tail_link_or_exit(next_rip);
        }
        return true;
    }

    // ---- JMP rel8/rel32 ----
    case Opcode::JMP_REL8:
    case Opcode::JMP_REL32: {
        u64 target = next_rip + static_cast<u64>(
                         static_cast<i64>(op0.imm));
        u32 ji_off = jit_iters_offset();
        emit_update_rip(target);
        // Stage B1: try to chain straight into the target's compiled
        // code without round-tripping through the dispatcher.  Falls
        // back to the standard epilogue on cache miss / unlinked target.
        emit_tail_link_or_exit(target, m_flags.incoming_nzcv,
                               LinkEdge::JmpRel);
        return true;
    }

    // ---- JMP r/m32 — indirect jump ----
    case Opcode::JMP_RM32: {
        if (is_high_addr_mode) {
            return false;
        }
        if (is_register_operand(op0.type)) {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        }
        if (!emit_linked_tail_reg(REG_SCRATCH0, NzcvState::None, false, false,
                                  /*api_call_tail=*/true)) {
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                   rip_offset() >> 3));
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                                   jit_iters_offset() >> 2));
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    // ---- CALL rel32 ----
    case Opcode::CALL_REL32: {
        if (is_high_addr_mode) {
            return false;
        }
        u64 target = next_rip + static_cast<u64>(
                         static_cast<i64>(op0.imm));
        // Fast path: recognize MS CRT 64-bit shift helpers (__allshl /
        // __aullshr) by guest byte signature and emit inline ARM64.
        if (try_emit_crt_shift_inline(target, next_rip, is_last)) {
            return true;
        }
        // ESP commits only after the return address is written (SDM Vol. 2:
        // a stack-write fault on CALL is precise, and the interpreter's
        // `push_value` matches). The new ESP is held in X21, which is
        // callee-saved and so survives the store helper; it is dead again
        // before emit_rsb_push, which must not run over a live X21.
        emit_load_gpr32(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
        emit(arm64::sub_x_imm(REG_CALL_TMP, REG_CALL_TMP, 4));
        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(next_rip));
        emit_guest_store32(REG_SCRATCH1, REG_CALL_TMP);
        emit_store_gpr32(REG_CALL_TMP, static_cast<u8>(RegIndex::RSP));
        // Push return site onto the software RSB before linking to callee.
        emit_rsb_push(next_rip);
        u32 ji_off = jit_iters_offset();
        emit_update_rip(target);
        // Stage B1 (extended): link CALL rel32 target.
        emit_tail_link_or_exit(target);
        return true;
    }

    // ---- CALL r/m32 — indirect call ----
    case Opcode::CALL_RM32: {
        if (is_high_addr_mode) {
            return false;
        }
        // Read the callee BEFORE the push -- x86 evaluates the operand first,
        // and `call [esp-4]` would otherwise read the return address this
        // instruction is about to write.
        //
        // It must land in REG_CALL_TMP (X21) and not a scratch. The
        // return-address store below can take its cold stub, and that stub
        // calls the write helper: `emit_helper_blr` reloads only the pins and
        // the linear base, so every caller-saved register dies across it --
        // W1 (REG_SCRATCH1) included, which the stub additionally uses as its
        // own flag-replay temporary. Holding the callee there meant a taken
        // stub replaced the branch target with a leftover stack address, and
        // the guest jumped onto its own stack. X21 is in AAPCS64's
        // callee-saved X19-X28, which is exactly why this register exists.
        //
        // Reached in practice because the linear check byte has HOST page
        // granularity (16 KiB): a guest stack whose top page shares a host
        // page with a MEM_WRITE_WATCH allocation sends every push to the stub.
        // A 32-bit managed runtime maps its code heap directly above the
        // thread stack, so the stack's top host page is shared with that heap.
        if (is_register_operand(op0.type)) {
            emit_load_gpr32(REG_CALL_TMP, op0.reg);
        } else {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32(REG_CALL_TMP, REG_SCRATCH2);
        }
        // ESP commits only after the return address is written, as above.
        // The callee occupies SCRATCH1 across this whole sequence, so rather
        // than add a second long-lived register the new ESP is recomputed
        // from the ESP slot after the store — the slot still reads the
        // pre-push value precisely because nothing has committed yet.
        emit_load_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RSP));
        emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, 4));
        emit_mov_imm32(REG_SCRATCH2, static_cast<u32>(next_rip));
        emit_guest_store32(REG_SCRATCH2, REG_SCRATCH0);
        emit_load_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RSP));
        emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, 4));
        emit_store_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RSP));
        emit_rsb_push(next_rip);

        if (!emit_linked_tail_reg(REG_CALL_TMP, NzcvState::None, false, false,
                                  /*api_call_tail=*/true)) {
            emit(arm64::str_x_uoff(REG_CALL_TMP, REG_CPU_STATE,
                                   rip_offset() >> 3));
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                                   jit_iters_offset() >> 2));
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    // ---- RET / RET imm16 ----
    // Intel SDM Vol. 2 RET (near). OperandSize 32 pops EIP (4 bytes); 66H
    // selects OperandSize 16: a 2-byte pop and EIP := tempEIP AND 0000FFFFH
    // (the flat CS limit cannot fault it). Then, StackAddressSize being 32
    // for the flat SS, ESP := ESP + SRC with SRC the UNSIGNED imm16 -- a
    // release of up to 65539, past ADD's 12-bit field. The W-form ESP store
    // wraps the sum modulo 2^32. A faulting pop commits nothing.
    case Opcode::RET_NEAR:
    case Opcode::RET_NEAR_IMM16: {
        if (is_high_addr_mode) {
            return false;
        }
        const bool op16 = instr.prefixes.operand_size;
        const u32 release = (op16 ? 2u : 4u) +
            (instr.opcode == Opcode::RET_NEAR_IMM16
                 ? static_cast<u32>(static_cast<u16>(op0.imm)) : 0u);
        emit_load_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RSP));
        if (op16) {
            emit_guest_load16(REG_SCRATCH1, REG_SCRATCH0);
        } else {
            emit_guest_load32(REG_SCRATCH1, REG_SCRATCH0);
        }
        emit_load_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RSP));
        emit_add_x_uimm24(REG_SCRATCH0, REG_SCRATCH0, release);
        emit_store_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RSP));
        u32 off = rip_offset();
        emit(0x2A0103E0u | static_cast<u32>(REG_SCRATCH1));
        if (fits_ldr_x_offset(off)) {
            emit(arm64::str_x_uoff(REG_SCRATCH1, REG_CPU_STATE, off >> 3));
        }
        if (!emit_linked_tail_ret(REG_SCRATCH1)) {
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                                   jit_iters_offset() >> 2));
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    // ---- Far JMP / CALL — Intel SDM Vol. 2, same-privilege user-mode ----
    // Load offset then CS selector (ptr immediate or m16:16/32). Far CALL
    // pushes return CS then return IP (slot 4, or 2 with 66H); RETF pops
    // IP then CS. No call gates / privilege changes.
    case Opcode::JMP_PTR16_16:
    case Opcode::JMP_PTR16_32:
    case Opcode::JMP_M16_16:
    case Opcode::JMP_M16_32:
    case Opcode::CALL_PTR16_16:
    case Opcode::CALL_PTR16_32:
    case Opcode::CALL_M16_16:
    case Opcode::CALL_M16_32: {
        if (is_high_addr_mode) {
            return false;
        }
        const bool is_call = instr.opcode == Opcode::CALL_PTR16_16 ||
                             instr.opcode == Opcode::CALL_PTR16_32 ||
                             instr.opcode == Opcode::CALL_M16_16 ||
                             instr.opcode == Opcode::CALL_M16_32;
        const bool op16 = instr.opcode == Opcode::JMP_PTR16_16 ||
                          instr.opcode == Opcode::CALL_PTR16_16 ||
                          instr.opcode == Opcode::JMP_M16_16 ||
                          instr.opcode == Opcode::CALL_M16_16;
        const bool is_mem = instr.opcode == Opcode::JMP_M16_16 ||
                            instr.opcode == Opcode::JMP_M16_32 ||
                            instr.opcode == Opcode::CALL_M16_16 ||
                            instr.opcode == Opcode::CALL_M16_32;
        const u8 slot = op16 ? 2 : 4;
        const u32 cs_off = static_cast<u32>(
            offsetof(CpuState, seg) + offsetof(SegmentRegs, cs));
        const auto emit_push_far_return = [&]() {
            // Push CS then IP. Interpreter push_value does the same, so a
            // stack fault leaves CS:RIP unchanged after the failed store.
            emit(arm64::add_x_imm(REG_SCRATCH2, REG_CPU_STATE, cs_off));
            emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH2, 31));
            emit_load_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RSP));
            emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, slot));
            emit_store_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RSP));
            if (op16) {
                emit_guest_store16(REG_SCRATCH1, REG_SCRATCH0);
            } else {
                emit_guest_store32(REG_SCRATCH1, REG_SCRATCH0);
            }
            emit_load_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RSP));
            emit(arm64::sub_x_imm(REG_SCRATCH0, REG_SCRATCH0, slot));
            emit_store_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RSP));
            emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(next_rip));
            if (op16) {
                emit_guest_store16(REG_SCRATCH1, REG_SCRATCH0);
            } else {
                emit_guest_store32(REG_SCRATCH1, REG_SCRATCH0);
            }
        };
        const auto emit_store_cs_rip_from_regs = [&]() {
            // W1 = selector, X0 = offset (zero-extended).
            emit(arm64::strh_uoff(REG_SCRATCH1, REG_CPU_STATE, cs_off >> 1));
            const u32 rip_off = rip_offset();
            if (fits_ldr_x_offset(rip_off)) {
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                       rip_off >> 3));
            }
        };

        if (is_mem) {
            if (!is_memory_operand(op0.type)) {
                return false;
            }
            const u32 off_size = op16 ? 2u : 4u;
            emit_compute_ea(REG_CALL_TMP, op0.mem, next_rip);
            if (op16) {
                emit_guest_load16(REG_SCRATCH0, REG_CALL_TMP);
            } else {
                emit_guest_load32(REG_SCRATCH0, REG_CALL_TMP);
            }
            // Stash offset in the high half of X21 so the selector load
            // (which clobbers X0–X2) cannot drop it. EA stays in W21.
            emit(arm64::orr_x_lsl(REG_CALL_TMP, REG_CALL_TMP,
                                  REG_SCRATCH0, 32));
            emit(arm64::mov_w(REG_SCRATCH2, REG_CALL_TMP));
            emit(arm64::add_x_imm(REG_SCRATCH2, REG_SCRATCH2, off_size));
            emit_guest_load16(REG_SCRATCH1, REG_SCRATCH2);
            emit(arm64::lsr_x_imm(REG_SCRATCH0, REG_CALL_TMP, 32));
            if (is_call) {
                // Preserve offset|selector across the guest stores that
                // clobber X0–X2. EA was computed before ESP changes.
                emit(arm64::orr_x_lsl(REG_CALL_TMP, REG_SCRATCH0,
                                      REG_SCRATCH1, 32));
                emit_push_far_return();
                emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_CALL_TMP, 32));
                emit(arm64::mov_w(REG_SCRATCH0, REG_CALL_TMP));
            }
            emit_store_cs_rip_from_regs();
            u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_epilogue(JitExecResult::Continue);
            return true;
        }

        if (is_call) {
            emit_push_far_return();
        }
        u64 target = static_cast<u64>(op0.imm);
        target = op16 ? (target & 0xFFFFu) : (target & 0xFFFFFFFFu);
        emit_mov_imm32(REG_SCRATCH1, static_cast<u16>(op1.imm));
        emit(arm64::strh_uoff(REG_SCRATCH1, REG_CPU_STATE, cs_off >> 1));
        u32 ji_off = jit_iters_offset();
        emit_update_rip(target);
        emit_tail_link_or_exit(target);
        return true;
    }

    // ---- LEAVE — MOV (E|R)SP, (E|R)BP; POP (E|R)BP ----
    // Intel SDM Vol. 2 LEAVE: in 64-bit mode the stack address size is 64, so
    // both registers are full width and the popped slot is 8 bytes. The 32-bit
    // body would zero RSP[63:32] and RBP[63:32] and read the wrong 4 bytes.
    // 66H (a 2-byte pop into BP) is not modelled here.
    case Opcode::LEAVE: {
        if (instr.prefixes.operand_size) {
            return false;
        }
        // Intel SDM Vol. 2 LEAVE is ONE instruction: a faulting pop leaves
        // RSP/ESP as it was (stack_access_fault_probe.expected, `leave`), so
        // the slot is loaded through the old frame pointer and nothing is
        // stored until the load has retired.
        if (m_block_is_64bit) {
            const u8 bp = static_cast<u8>(RegIndex::RBP);
            const u8 sp = static_cast<u8>(RegIndex::RSP);
            emit_load_gpr64(REG_CALL_TMP, bp);
            emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
            emit(arm64::add_x_imm(REG_CALL_TMP, REG_CALL_TMP, 8));
            emit_store_gpr64(REG_CALL_TMP, sp);
            emit_store_gpr64(REG_SCRATCH0, bp);
            break;
        }
        if (is_high_addr_mode) {
            return false;
        }
        emit_load_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RBP));
        emit_guest_load32(REG_SCRATCH1, REG_SCRATCH0);
        // The load may have run a helper; EBP is still the frame pointer.
        emit_load_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RBP));
        emit(arm64::add_x_imm(REG_SCRATCH0, REG_SCRATCH0, 4));
        emit_store_gpr32(REG_SCRATCH0, static_cast<u8>(RegIndex::RSP));
        emit_store_gpr32(REG_SCRATCH1, static_cast<u8>(RegIndex::RBP));
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
