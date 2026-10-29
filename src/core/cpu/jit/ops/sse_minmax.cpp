// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — MINPS/MAXPS/MINPD/MAXPD and MINSS/MAXSS/MINSD/MAXSD
// (compile_sse_minmax, reached from compile_sse's pre-switch).
//
// Split out of ops/sse.cpp (ADR-0006 step 3) as pure code motion.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

bool JitCompiler::compile_sse_minmax(const DecodedInstruction& instr,
                                    u64 instr_rip, u64 next_rip, bool is_last) {
    // MINPS/MAXPS/MINPD/MAXPD and the scalar MINSS/MAXSS/MINSD/MAXSD: the
    // scalar forms select on lane 0 only, read an unaligned m32/m64, and
    // leave every other lane as SRC1 has it (Intel SDM Vol. 2; flags, #XM
    // record and upper lanes measured by tools/oracle_probe/
    // sse_minmax_scalar_probe). They used to be a C++ helper call every time,
    // ~3M calls/s on a 64-bit title menu (scalar min/max on doubles).
    const bool scalar = instr.opcode == Opcode::MINSS_XMM_XMMM32 ||
                        instr.opcode == Opcode::MINSD_XMM_XMMM64 ||
                        instr.opcode == Opcode::MAXSS_XMM_XMMM32 ||
                        instr.opcode == Opcode::MAXSD_XMM_XMMM64;
    const bool pd = instr.opcode == Opcode::MINPD_XMM_XMMM128 ||
                    instr.opcode == Opcode::MAXPD_XMM_XMMM128 ||
                    instr.opcode == Opcode::MINSD_XMM_XMMM64 ||
                    instr.opcode == Opcode::MAXSD_XMM_XMMM64;
    const bool minimum = instr.opcode == Opcode::MINPD_XMM_XMMM128 ||
                         instr.opcode == Opcode::MINPS_XMM_XMMM128 ||
                         instr.opcode == Opcode::MINSS_XMM_XMMM32 ||
                         instr.opcode == Opcode::MINSD_XMM_XMMM64;
    // Scalar forms are VEX.LIG (opcodes.cpp vex_l_is_reserved).
    const bool wide = instr.prefixes.vex && instr.prefixes.vex_l && !scalar;
    const u8 dst = instr.operands[0].reg & 15;
    const u8 first = instr.prefixes.vex ? instr.prefixes.vex_vvvv & 15 : dst;
    const auto& second = instr.operands[1];
    const unsigned halves = wide ? 2 : 1;
    const bool memory = is_memory_operand(second.type);
    const u32 staging = static_cast<u32>(offsetof(CpuState,sse) +
                                         offsetof(SseState,jit_vec_scratch));
    const u32 control_offset = static_cast<u32>(offsetof(CpuState,sse) +
                                               offsetof(SseState,mxcsr));
    const bool hold = should_hold_nzcv();
    if (hold) emit_nzcv_spill();
    // Stage ALL memory bytes before publishing any destination lane. Neither
    // the special-value guard nor the high pass may observe a partial write.
    if (memory) {
        // A failed checked read must identify this instruction, even after
        // earlier instructions in the block have already completed.
        emit_update_rip(instr_rip);
        if (scalar) {
            emit_compute_ea(REG_CALL_TMP,second.mem,next_rip);
            if (pd) {
                emit_guest_load64(REG_SCRATCH0,REG_CALL_TMP);
                emit(arm64::str_x_uoff(REG_SCRATCH0,REG_CPU_STATE,staging>>3));
            } else {
                emit_guest_load32(REG_SCRATCH0,REG_CALL_TMP);
                emit(arm64::str_w_uoff(REG_SCRATCH0,REG_CPU_STATE,staging>>2));
            }
        }
        for (unsigned word=0;word<halves*2 && !scalar;++word) {
            emit_compute_ea(REG_CALL_TMP,second.mem,next_rip);
            if (word==0 && !instr.prefixes.vex) emit_align_check(REG_CALL_TMP,false);
            if (word) emit(arm64::add_x_imm(REG_CALL_TMP,REG_CALL_TMP,word*sizeof(u64)));
            emit_guest_load64(REG_SCRATCH0,REG_CALL_TMP);
            emit(arm64::str_x_uoff(REG_SCRATCH0,REG_CPU_STATE,(staging+word*sizeof(u64))>>3));
        }
    }
    u32 a_offsets[2] = {};
    u32 b_offsets[2] = {};
    for (unsigned half=0;half<halves;++half) {
        const u32 a = half ? ymm_high_u64_off(first,0) : xmm_u64_off(first,0);
        const u32 b = memory ? staging+half*sizeof(XmmReg) :
            (half ? ymm_high_u64_off(second.reg & 15,0) : xmm_u64_off(second.reg & 15,0));
        a_offsets[half] = a;
        b_offsets[half] = b;
        if (scalar) {
            // LDR S/D zero the rest of the register, so the whole-register
            // classification and selection below see lane 0 and zeros.
            emit(pd ? arm64::ldr_d_uoff(0,REG_CPU_STATE,a>>3)
                    : arm64::ldr_s_uoff(0,REG_CPU_STATE,a>>2));
            emit(pd ? arm64::ldr_d_uoff(1,REG_CPU_STATE,b>>3)
                    : arm64::ldr_s_uoff(1,REG_CPU_STATE,b>>2));
            continue;
        }
        emit(arm64::ldr_q_uoff(half?4:0,REG_CPU_STATE,a>>4));
        emit(arm64::ldr_q_uoff(half?5:1,REG_CPU_STATE,b>>4));
    }
    emit(arm64::ldr_w_uoff(REG_SCRATCH2,REG_CPU_STATE,control_offset>>2));
    // Every exit to a cold arm is a CBZ/CBNZ (imm19): the arms are placed
    // out of line, which can be beyond a TBZ's imm14 reach in a large block.
    std::vector<usize> to_interp_cbnz;  // CBNZ W3: an exception is unmasked
    std::vector<usize> to_lanes_cbnz;   // CBNZ W3: integer lane arm
    std::vector<usize> to_lanes_cbz;    // CBZ W3: integer lane arm
    // No native side effects before an unmasked exception: the interpreter
    // computes the complete 128/256-bit exception union and commits atomically.
    // W3 = the IM/DM pair inverted: nonzero iff either is unmasked.
    emit(arm64::mvn_w(REG_SCRATCH3,REG_SCRATCH2));
    emit(arm64::ubfx_w(REG_SCRATCH3,REG_SCRATCH3,
                       static_cast<u8>(std::countr_zero(u32(MXCSR_IM))),2));
    static_assert(MXCSR_DM == MXCSR_IM << 1, "IM and DM are adjacent");
    to_interp_cbnz.push_back(m_code_buf.size()); emit(0);
    const usize no_daz=m_code_buf.size(); emit(0);
    // FZ+DAZ is supported even on hosts without FEAT_AFP: the fix-up below
    // flushes a selected denormal by multiplying it, which needs FZ. DAZ
    // without FTZ takes the integer lane arm.
    emit(arm64::ubfx_w(REG_SCRATCH3,REG_SCRATCH2,
                       static_cast<u8>(std::countr_zero(u32(MXCSR_FTZ))),1));
    to_lanes_cbz.push_back(m_code_buf.size()); emit(0);
    const usize flushed=m_code_buf.size(); emit(0);
    const usize classify=m_code_buf.size();
    // Without DAZ, any denormal input takes the lane arm (it sets DE, which
    // has no host equivalent), so the native compare below only ever sees
    // zeros, normals, infinities and NaNs. FTZ cannot change those -- it
    // flushes OUTPUTS, and a selection has none -- so FTZ-only, the MXCSR a
    // 64-bit title publishes, is native too. (It took the slow arm on every execution.)
    const arm64::VSize size = pd ? arm64::VS_D : arm64::VS_S;
    emit(arm64::eor_v16b(6,6,6));
    for (unsigned half=0;half<halves;++half) for (unsigned source=0;source<2;++source) {
        const u8 v=static_cast<u8>((half?4:0)+source);
        // Exponent zero AND magnitude nonzero is a denormal. Integer-only
        // classification preserves NaN payloads and does not manufacture IE.
        emit(arm64::shl_v_imm(2,v,1,size));
        emit(arm64::ushr_v_imm(3,2,pd ? std::numeric_limits<f64>::digits
                                                 : std::numeric_limits<f32>::digits,size));
        emit(arm64::cmeq_v_zero(3,3,size));
        emit(arm64::cmeq_v_zero(2,2,size));
        emit(arm64::bic_v16b(2,3,2));
        emit(arm64::orr_v16b(6,6,2));
    }
    emit(arm64::umaxv_v(6,6,arm64::VS_B));
    emit(arm64::umov_w_b(REG_SCRATCH3,6,0));
    to_lanes_cbnz.push_back(m_code_buf.size()); emit(0);
    const usize native=m_code_buf.size();
    m_code_buf[no_daz]=arm64::tbz(REG_SCRATCH2,std::countr_zero(u32(MXCSR_DAZ)),
                                static_cast<i32>(classify-no_daz)*4);
    m_code_buf[flushed]=arm64::b_imm(static_cast<i32>(native-flushed)*4);
    const auto fs=pd?arm64::FS_D:arm64::FS_S;
    for (unsigned half=0;half<halves;++half) {
        if (half) {emit(arm64::mov_v16b(0,4));emit(arm64::mov_v16b(1,5));}
        emit(minimum ? arm64::fcmgt_v(2,1,0,fs) : arm64::fcmgt_v(2,0,1,fs));
        emit(arm64::mov_v16b(3,0));
        emit(arm64::mov_v16b(0,2));
        emit(arm64::bsl_v16b(0,3,1));
        const usize raw=m_code_buf.size(); emit(0);
        // Only DAZ can change a selected denormal into signed zero. FTZ by
        // itself must NEVER multiply the result (native oracle regression).
        if (pd) emit_mov_imm64(REG_SCRATCH0,std::bit_cast<i64>(f64{1}));
        else emit_mov_imm32(REG_SCRATCH0,std::bit_cast<u32>(f32{1}));
        emit(arm64::dup_v_gpr(2,REG_SCRATCH0,size));
        emit(arm64::fmul_v(2,0,2,fs));
        emit(arm64::fcmeq_v(3,0,0,fs));
        emit(arm64::bit_v16b(0,2,3));
        const usize store=m_code_buf.size();
        m_code_buf[raw]=arm64::tbz(REG_SCRATCH2,std::countr_zero(u32(MXCSR_DAZ)),
                                 static_cast<i32>(store-raw)*4);
        const u32 offset=half?ymm_high_u64_off(dst,0):xmm_u64_off(dst,0);
        if (!scalar) {
            emit(arm64::str_q_uoff(0,REG_CPU_STATE,offset>>4));
        } else if (first == dst) {
            // Lane 0 only; the rest of the destination is SRC1 already.
            emit(pd ? arm64::str_d_uoff(0,REG_CPU_STATE,offset>>3)
                    : arm64::str_s_uoff(0,REG_CPU_STATE,offset>>2));
        } else {
            // VEX: DEST[127:lane] := SRC1[127:lane].
            emit(arm64::ldr_q_uoff(3,REG_CPU_STATE,xmm_u64_off(first,0)>>4));
            emit(arm64::ins_v_elem(3,0,0,0,pd ? arm64::VS_D : arm64::VS_S));
            emit(arm64::str_q_uoff(3,REG_CPU_STATE,offset>>4));
        }
    }
    if (instr.prefixes.vex && !wide) {
        emit(arm64::eor_v16b(0,0,0));
        emit(arm64::str_q_uoff(0,REG_CPU_STATE,ymm_high_u64_off(dst,0)>>4));
    }
    // The lane arm: the interpreter's selection (minmax_lane in
    // interpreter/ops/sse_arith.cpp, Intel SDM Vol. 2 MINPS..MAXSD) on the
    // integer images, for a denormal operand without DAZ and for DAZ without
    // FTZ, with IM and DM both masked. Per lane: a NaN on either side raises
    // IE and returns SRC2; DAZ makes a denormal its signed zero (SRC2 too,
    // when it is returned for a NaN); without DAZ a denormal raises DE; equal
    // values and a pair of zeros return SRC2; anything else is the ordered
    // sign-magnitude compare. Nothing here can fault, so IE/DE go straight
    // into MXCSR. Entered before any destination byte is written; reads its
    // operands again from CpuState (the staging scratch for m*), uses V0-V7,
    // W0, W3 and W5, writes no NZCV and calls nothing. W2 holds MXCSR.
    std::vector<ColdArmEntry> lane_entries;
    for (const usize position : to_lanes_cbnz) {
        lane_entries.push_back({position, [](i32 disp) {
            return arm64::cbnz_w(REG_SCRATCH3, disp);
        }});
    }
    for (const usize position : to_lanes_cbz) {
        lane_entries.push_back({position, [](i32 disp) {
            return arm64::cbz_w(REG_SCRATCH3, disp);
        }});
    }
    const bool vex = instr.prefixes.vex;
    defer_cold_arm(std::move(lane_entries),
        [this, scalar, pd, minimum, wide, vex, halves, first, dst,
         a_offsets, b_offsets, control_offset]() {
        const arm64::VSize sz = pd ? arm64::VS_D : arm64::VS_S;
        const u32 bits = pd ? 64u : 32u;
        const auto dup_const = [&](u8 v, u64 value) {
            if (pd) emit_mov_imm64(REG_SCRATCH0, static_cast<i64>(value));
            else emit_mov_imm32(REG_SCRATCH0, static_cast<u32>(value));
            emit(arm64::dup_v_gpr(v, REG_SCRATCH0, sz));
        };
        const u64 infinity = pd ? std::bit_cast<u64>(std::numeric_limits<f64>::infinity())
                                : std::bit_cast<u32>(std::numeric_limits<f32>::infinity());
        const u64 normal = pd ? std::bit_cast<u64>(std::numeric_limits<f64>::min())
                              : std::bit_cast<u32>(std::numeric_limits<f32>::min());
        emit(arm64::movz_w(REG_EA_TMP, 0));  // raised
        for (unsigned half = 0; half < halves; ++half) {
            if (scalar) {
                emit(pd ? arm64::ldr_d_uoff(0, REG_CPU_STATE, a_offsets[half] >> 3)
                        : arm64::ldr_s_uoff(0, REG_CPU_STATE, a_offsets[half] >> 2));
                emit(pd ? arm64::ldr_d_uoff(1, REG_CPU_STATE, b_offsets[half] >> 3)
                        : arm64::ldr_s_uoff(1, REG_CPU_STATE, b_offsets[half] >> 2));
            } else {
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, a_offsets[half] >> 4));
                emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, b_offsets[half] >> 4));
            }
            // V2/V3 = |a|/|b| images, V4 = NaN on either side.
            dup_const(7, infinity);
            emit(arm64::shl_v_imm(2, 0, 1, sz));
            emit(arm64::ushr_v_imm(2, 2, 1, sz));
            emit(arm64::shl_v_imm(3, 1, 1, sz));
            emit(arm64::ushr_v_imm(3, 3, 1, sz));
            emit(arm64::cmhi_v(4, 2, 7, sz));
            emit(arm64::cmhi_v(5, 3, 7, sz));
            emit(arm64::orr_v16b(4, 4, 5));
            emit(arm64::umaxv_v(5, 4, arm64::VS_B));
            emit(arm64::umov_w_b(REG_SCRATCH3, 5, 0));
            emit(arm64::lsr_w_imm(REG_SCRATCH3, REG_SCRATCH3, 7));
            static_assert(MXCSR_IE == 1u << 0 && MXCSR_DE == 1u << 1);
            emit(arm64::orr_w_lsl(REG_EA_TMP, REG_EA_TMP, REG_SCRATCH3, 0));
            // V5/V6 = a/b is a denormal: nonzero magnitude below the
            // smallest normal.
            dup_const(7, normal);
            emit(arm64::cmhi_v(5, 7, 2, sz));
            emit(arm64::cmeq_v_zero(6, 2, sz));
            emit(arm64::bic_v16b(5, 5, 6));
            emit(arm64::cmhi_v(6, 7, 3, sz));
            emit(arm64::cmeq_v_zero(7, 3, sz));
            emit(arm64::bic_v16b(6, 6, 7));
            const usize to_daz = m_code_buf.size(); emit(0);
            // No DAZ: DE for a denormal in a lane whose operands are not NaN.
            emit(arm64::orr_v16b(5, 5, 6));
            emit(arm64::bic_v16b(5, 5, 4));
            emit(arm64::umaxv_v(5, 5, arm64::VS_B));
            emit(arm64::umov_w_b(REG_SCRATCH3, 5, 0));
            emit(arm64::lsr_w_imm(REG_SCRATCH3, REG_SCRATCH3, 7));
            emit(arm64::orr_w_lsl(REG_EA_TMP, REG_EA_TMP, REG_SCRATCH3, 1));
            const usize to_select = m_code_buf.size(); emit(0);
            const usize daz = m_code_buf.size();
            // DAZ: a denormal keeps only its sign, and its magnitude is zero.
            emit(arm64::and_v16b(5, 5, 2));
            emit(arm64::bic_v16b(0, 0, 5));
            emit(arm64::bic_v16b(2, 2, 5));
            emit(arm64::and_v16b(6, 6, 3));
            emit(arm64::bic_v16b(1, 1, 6));
            emit(arm64::bic_v16b(3, 3, 6));
            const usize select = m_code_buf.size();
            m_code_buf[to_daz] = arm64::tbnz(REG_SCRATCH2,
                std::countr_zero(u32(MXCSR_DAZ)),
                static_cast<i32>(daz - to_daz) * 4);
            m_code_buf[to_select] =
                arm64::b_imm(static_cast<i32>(select - to_select) * 4);
            // V5 = SRC2 forced: a NaN, or both magnitudes zero. (Equal
            // values compare neither less nor greater below.)
            emit(arm64::orr_v16b(5, 2, 3));
            emit(arm64::cmeq_v_zero(5, 5, sz));
            emit(arm64::orr_v16b(5, 5, 4));
            // Sign-magnitude to two's-complement order keys.
            emit(arm64::sshr_v_imm(6, 0, bits - 1, sz));
            emit(arm64::ushr_v_imm(6, 6, 1, sz));
            emit(arm64::eor_v16b(6, 6, 0));
            emit(arm64::sshr_v_imm(7, 1, bits - 1, sz));
            emit(arm64::ushr_v_imm(7, 7, 1, sz));
            emit(arm64::eor_v16b(7, 7, 1));
            emit(minimum ? arm64::cmgt_v(6, 7, 6, sz) : arm64::cmgt_v(6, 6, 7, sz));
            emit(arm64::bic_v16b(6, 6, 5));
            emit(arm64::bsl_v16b(6, 0, 1));
            const u32 offset = half ? ymm_high_u64_off(dst, 0) : xmm_u64_off(dst, 0);
            if (!scalar) {
                emit(arm64::str_q_uoff(6, REG_CPU_STATE, offset >> 4));
            } else if (first == dst) {
                emit(pd ? arm64::str_d_uoff(6, REG_CPU_STATE, offset >> 3)
                        : arm64::str_s_uoff(6, REG_CPU_STATE, offset >> 2));
            } else {
                emit(arm64::ldr_q_uoff(3, REG_CPU_STATE, xmm_u64_off(first, 0) >> 4));
                emit(arm64::ins_v_elem(3, 0, 6, 0, sz));
                emit(arm64::str_q_uoff(3, REG_CPU_STATE, offset >> 4));
            }
        }
        if (vex && !wide) {
            emit(arm64::eor_v16b(0, 0, 0));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, ymm_high_u64_off(dst, 0) >> 4));
        }
        emit(arm64::orr_w(REG_SCRATCH2, REG_SCRATCH2, REG_EA_TMP));
        emit(arm64::str_w_uoff(REG_SCRATCH2, REG_CPU_STATE, control_offset >> 2));
    }, /*semantic_helper=*/false);
    // The interpreter arm, for an unmasked IM or DM: re-decode and interpret
    // the instruction, which faults precisely or completes. Out of line, so
    // the native path pays no taken branch around it. No host pointer into
    // transient lowering input is embedded, so compiled memo/shared-arena
    // reuse remains safe after the original compiler and BasicBlock have
    // been destroyed.
    std::vector<ColdArmEntry> entries;
    for (const usize position : to_interp_cbnz) {
        entries.push_back({position, [](i32 disp) {
            return arm64::cbnz_w(REG_SCRATCH3, disp);
        }});
    }
    defer_cold_arm(std::move(entries), [this, instr_rip]() {
        emit(arm64::mov_x(0,REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1,REG_JIT_CTX,JIT_CTX_OFF_MEMORY>>3));
        emit_mov_imm64(2,static_cast<i64>(instr_rip));
        emit(arm64::ldr_x_uoff(16,REG_JIT_CTX,JIT_CTX_OFF_INTERP_STEP>>3));
        emit_helper_blr();
        emit_exit_if_pending_raise();
    }, /*semantic_helper=*/true);
    if (hold) emit_nzcv_fill();
    finish_vector_block(next_rip,is_last);
    return true;
}

} // namespace vitreo::cpu::jit
