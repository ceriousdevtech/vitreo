// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — shared vector emit primitives: vec_off, the x86 FMA NaN
// fix-up, the SSE register copy and 128-bit fast load/store, the vector
// block tail and the XMM qword store.
//
// Split out of ops/sse.cpp (ADR-0006 step 3) as pure code motion.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

u32 JitCompiler::vec_off(u8 reg) const noexcept {
    return m_vec_hi_lane ? ymm_high_u64_off(reg, 0) : xmm_u64_off(reg, 0);
}

void JitCompiler::emit_x86_fma_nan_fixup(u8 r, u8 a, u8 b, u8 c,
                                         bool dbl, u8 t0, u8 t1, u8 t2) {
    // With FPCR.AH (FEAT_AFP), installed by the SSE environment, the host
    // already IS the x86 rule this fixup builds: measured on this hardware,
    // FMLA/FMLS and FMADD/FMSUB/FNMADD/FNMSUB return the quieted NaN of Vn,
    // then Vm, then the addend -- the x86 order when a/b are the formula's
    // multiplicands in order, as every caller passes them (oracle:
    // tools/oracle_probe/fma_nan_priority_probe.expected) -- never negate a
    // NaN, and produce 0xFFC00000 / 0xFFF8000000000000 for an invalid
    // operation; FADD/FSUB the same for ADDSUB's two operands.
    if (math::sse_alternate_nan_handling()) return;
    const arm64::FSize fs = dbl ? arm64::FS_D : arm64::FS_S;
    // 1. A NaN result with no NaN input (inf * 0, inf - inf) is x86's QNaN
    //    indefinite, sign SET; ARM produces the positive default NaN. Setting
    //    the sign on every NaN lane is enough: lanes with a NaN input are
    //    overwritten below.
    emit(arm64::fcmeq_v(t0, r, r, fs));           // t0 = r is ordered
    emit(arm64::eor_v16b(t1, t1, t1));
    emit(arm64::fneg_v(t1, t1, fs));              // t1 = sign bit (-0.0)
    emit(arm64::bic_v16b(t1, t1, t0));            // sign only on NaN lanes
    emit(arm64::orr_v16b(r, r, t1));
    // 2. NaN inputs, lowest priority first so the highest wins: c, b, a.
    if (dbl) emit(arm64::movz_x(REG_SCRATCH0, 0x0008, 48));
    else     emit(arm64::movz_x(REG_SCRATCH0, 0x0040, 16));
    emit(arm64::dup_v_gpr(t1, REG_SCRATCH0, dbl ? arm64::VS_D : arm64::VS_S));
    for (const u8 src : {c, b, a}) {
        emit(arm64::fcmeq_v(t0, src, src, fs));
        emit(arm64::not_v16b(t0, t0));            // src is NaN
        emit(arm64::orr_v16b(t2, src, t1));       // quiet(src), sign kept
        emit(arm64::bit_v16b(r, t2, t0));
    }
}

void JitCompiler::emit_sse_reg_copy(u8 dst_xmm, u8 src_xmm, u16 fallback_op_id) {
    const u32 dst_off = xmm_u64_off(dst_xmm, 0);
    const u32 src_off = xmm_u64_off(src_xmm, 0);
    // A staging copy inside a larger lowering: it writes CpuState only, so
    // the destination's cache entry (if any) is stale from here on.
    if (dst_xmm < kXmmCacheCount) {
        const bool src_cached = xmm_cache_on() && src_xmm < kXmmCacheCount &&
                                (m_xmm_cache & (1u << src_xmm)) != 0;
        if (src_xmm != dst_xmm) {
            m_xmm_cache = static_cast<u16>(m_xmm_cache & ~(1u << dst_xmm));
        }
        if (src_cached && fits_ldr_q_offset(dst_off)) {
            emit(arm64::str_q_uoff(static_cast<u8>(kXmmCacheBase + src_xmm),
                                   REG_CPU_STATE, dst_off >> 4));
            return;
        }
    }
    if (fits_ldr_q_offset(dst_off) && fits_ldr_q_offset(src_off)) {
        const bool pe32_forward =
            !m_block_is_64bit && !m_current_original_vex
            && !m_vec_lane_split;
        if (!pe32_forward || m_xmm_v0_forward_in != src_xmm) {
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, src_off >> 4));
        }
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, dst_off >> 4));
        if (pe32_forward) {
            m_xmm_v0_forward_out = dst_xmm;
            m_xmm_v0_alias_out = src_xmm;
        }
        return;
    }

    emit(arm64::mov_x(0, REG_CPU_STATE));
    emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
    emit(arm64::movz_w(2, fallback_op_id));
    emit(arm64::movz_w(3, dst_xmm));
    emit(arm64::movz_w(4, src_xmm));
    EMIT_SSE_OP_CALL();
}

void JitCompiler::emit_sse_load_mem128_fast(u8 dst_xmm, u8 guest_addr_reg, u32 mem_op_id) {
    const u32 off_lo = xmm_u64_off(dst_xmm, 0);
    const u32 off_hi = xmm_u64_off(dst_xmm, 1);
    auto emit_sse_helper = [&]() {
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::movz_w(2, mem_op_id));
        emit(arm64::movz_w(3, dst_xmm));
        emit(arm64::mov_x(4, guest_addr_reg));
        EMIT_SSE_OP_CALL();
    };
    // Intel SDM Vol. 1 §3.3.7: long-mode EA is 64-bit. The 32-bit region
    // path below does `mov w9, addr` and aliases 0x14000xxxx onto heap.
    // The linear64 guest base validates the whole 64-bit EA, so a long-mode
    // block can land all 16 bytes inline instead of calling the C helper.
    if (m_block_is_64bit && m_cfg.linear64 && m_cfg.linear64_vec128 && m_linear_base_pinned &&
        fits_ldr_q_offset(off_lo)) {
        emit_linear64_load128(0, guest_addr_reg);
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_lo >> 4));
        return;
    }
    if (m_block_is_64bit ||
        !fits_ldr_x_offset(off_lo) || !fits_ldr_x_offset(off_hi)) {
        emit_sse_helper();
        return;
    }
    if (m_cfg.linear_jit && m_memory.is_linear()) {
        emit(arm64::mov_w(9, guest_addr_reg));
        const u8 base = m_linear_base_pinned ? REG_LINEAR_BASE : REG_SCRATCH3;
        if (!m_linear_base_pinned) {
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_JIT_CTX,
                                    JIT_CTX_OFF_LINEAR_BASE >> 3));
        }
        emit(arm64::add_x_uxtw(REG_SCRATCH3, base, 9));
        if (fits_ldr_q_offset(off_lo)) {
            emit(arm64::ldr_q_uoff(0, REG_SCRATCH3, 0));
            // Acquire: guest load, then fence. The STR Q below writes
            // CpuState, not guest memory, so it needs no ordering.
            emit_vector_tso_fence();
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, off_lo >> 4));
        } else {
            emit(arm64::ldr_x_uoff(10, REG_SCRATCH3, 0));
            emit(arm64::ldr_x_uoff(11, REG_SCRATCH3, 1));
            // One fence for the pair: the two halves are one 128-bit
            // guest access, and ordering it needs the barrier after the
            // last of them, not between them.
            emit_vector_tso_fence();
            emit(arm64::str_x_uoff(10, REG_CPU_STATE, off_lo >> 3));
            emit(arm64::str_x_uoff(11, REG_CPU_STATE, off_hi >> 3));
        }
        return;
    }

    emit(arm64::mov_w(9, guest_addr_reg));

    emit(arm64::ldr_w_uoff(3, REG_JIT_CTX, JIT_CTX_OFF_REGION_BASE >> 2));
    emit(arm64::subs_w(31, 9, 3));
    u32 r1_blo_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::ldr_w_uoff(4, REG_JIT_CTX, JIT_CTX_OFF_REGION_END >> 2));
    emit(arm64::add_x_imm(8, 9, 15));
    emit(arm64::subs_w(31, 8, 4));
    u32 r1_bhs_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::sub_w(4, 9, 3));
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX, JIT_CTX_OFF_REGION_HOST >> 3));
    emit(arm64::ldr_x_reg(10, 3, 4));
    emit(arm64::mov_x(8, 4));
    emit(arm64::add_x_imm(8, 8, 8));
    emit(arm64::ldr_x_reg(11, 3, 8));
    // Acquire, one fence for the 128-bit pair: after both guest loads.
    emit_vector_tso_fence();
    emit(arm64::str_x_uoff(10, REG_CPU_STATE, off_lo >> 3));
    emit(arm64::str_x_uoff(11, REG_CPU_STATE, off_hi >> 3));
    u32 r1_done_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    u32 r2_idx = static_cast<u32>(m_code_buf.size());
    emit(arm64::ldr_w_uoff(3, REG_JIT_CTX, JIT_CTX_OFF_REGION2_BASE >> 2));
    emit(arm64::subs_w(31, 9, 3));
    u32 r2_blo_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::ldr_w_uoff(4, REG_JIT_CTX, JIT_CTX_OFF_REGION2_END >> 2));
    emit(arm64::add_x_imm(8, 9, 15));
    emit(arm64::subs_w(31, 8, 4));
    u32 r2_bhs_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::sub_w(4, 9, 3));
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX, JIT_CTX_OFF_REGION2_HOST >> 3));
    emit(arm64::ldr_x_reg(10, 3, 4));
    emit(arm64::mov_x(8, 4));
    emit(arm64::add_x_imm(8, 8, 8));
    emit(arm64::ldr_x_reg(11, 3, 8));
    // Acquire, one fence for the 128-bit pair: after both guest loads.
    emit_vector_tso_fence();
    emit(arm64::str_x_uoff(10, REG_CPU_STATE, off_lo >> 3));
    emit(arm64::str_x_uoff(11, REG_CPU_STATE, off_hi >> 3));
    u32 r2_done_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    u32 slow_idx = static_cast<u32>(m_code_buf.size());
    emit(arm64::mov_x(0, REG_CPU_STATE));
    emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
    emit(arm64::movz_w(2, mem_op_id));
    emit(arm64::movz_w(3, dst_xmm));
    emit(arm64::mov_w(4, 9));
    EMIT_SSE_OP_CALL();

    u32 done_idx = static_cast<u32>(m_code_buf.size());
    m_code_buf[r1_blo_idx]  = arm64::b_cond(arm64::CC,
                            static_cast<i32>(r2_idx - r1_blo_idx) * 4);
    m_code_buf[r1_bhs_idx]  = arm64::b_cond(arm64::CS,
                            static_cast<i32>(r2_idx - r1_bhs_idx) * 4);
    m_code_buf[r1_done_idx] = arm64::b_imm(
                            static_cast<i32>(done_idx - r1_done_idx) * 4);
    m_code_buf[r2_blo_idx]  = arm64::b_cond(arm64::CC,
                            static_cast<i32>(slow_idx - r2_blo_idx) * 4);
    m_code_buf[r2_bhs_idx]  = arm64::b_cond(arm64::CS,
                            static_cast<i32>(slow_idx - r2_bhs_idx) * 4);
    m_code_buf[r2_done_idx] = arm64::b_imm(
                            static_cast<i32>(done_idx - r2_done_idx) * 4);
}

void JitCompiler::emit_vec_store_span_helper(u8 addr_reg, u32 lo_off,
                                             u32 hi_off, u32 width,
                                             u64 instr_rip) {
    // jit_mem_write_span(ctx, addr, lo, hi, width): one write_bytes of the
    // whole operand, which raises the access violation itself. The address
    // moves first because it may sit in X0, which the RIP publication uses.
    if (m_block_is_64bit) emit(arm64::mov_x(1, addr_reg));
    else emit(arm64::mov_w(1, addr_reg));
    emit_mov_imm64(0, static_cast<i64>(instr_rip));
    emit(arm64::str_x_uoff(0, REG_CPU_STATE, rip_offset() >> 3));
    emit_mov_imm32(2, lo_off);
    emit(arm64::add_x(2, REG_CPU_STATE, 2));
    if (width > 16) {
        emit_mov_imm32(3, hi_off);
        emit(arm64::add_x(3, REG_CPU_STATE, 3));
    } else {
        emit(arm64::movz_x(3, 0, 0));
    }
    emit(arm64::movz_x(4, static_cast<u16>(width), 0));
    emit(arm64::mov_x(0, REG_JIT_CTX));
    emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_WRITE_SPAN >> 3));
    // Release: fence, then the guest store.
    emit_vector_tso_fence();
    emit_memory_helper_blr();
    emit_exit_if_pending_raise(false, 0xFF);
}

void JitCompiler::emit_vec_store_span(u8 addr_reg, u32 lo_off, u32 hi_off,
                                      u32 width, u64 instr_rip) {
    const bool inline_ok =
        !m_block_is_64bit && m_cfg.linear_jit && m_cfg.linear32_check_map &&
        m_memory.is_linear() &&
        fits_ldr_q_offset(lo_off) && (width <= 16 || fits_ldr_q_offset(hi_off));
    if (!inline_ok) {
        emit_vec_store_span_helper(addr_reg, lo_off, hi_off, width, instr_rip);
        return;
    }
    const u32 last = width - 1;
    emit(arm64::mov_w(9, addr_reg));
    // An operand that runs into the next guest page takes the helper: a host
    // STR that faults on its second page may already have written its first,
    // and x86 writes none of a faulting store.
    emit(arm64::ubfx_w(6, 9, 0, 12));
    emit(arm64::add_w_imm(6, 6, last));
    emit(arm64::lsr_w_imm(6, 6, 12));
    const u32 cross_cbnz = static_cast<u32>(m_code_buf.size());
    emit(0);
    // One guest page, so one host page: its linear check byte, the probe
    // the scalar linear store uses, is clear unless the page holds
    // translated code, a write watch, or guest pages of mixed protection.
    // Any of those takes the helper.
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX, JIT_CTX_OFF_LINEAR_CHECK_MAP >> 3));
    emit(arm64::ubfx_x(4, 9, static_cast<u8>(kLinearHostPageShift),
                       m_cfg.linear32_check_index_bits));
    emit(arm64::ldrb_reg(5, 3, 4));
    const u32 page_cbnz = static_cast<u32>(m_code_buf.size());
    emit(0);

    const u32 inline_idx = static_cast<u32>(m_code_buf.size());
    emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, lo_off >> 4));
    if (width > 16) emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, hi_off >> 4));
    const u8 base = m_linear_base_pinned ? REG_LINEAR_BASE : REG_SCRATCH3;
    if (!m_linear_base_pinned) {
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_JIT_CTX,
                               JIT_CTX_OFF_LINEAR_BASE >> 3));
    }
    emit(arm64::add_x_uxtw(REG_SCRATCH3, base, 9));
    // Release: fence, then the guest store. The LDR Qs above read CpuState.
    emit_vector_tso_fence();
    if (width > 16) emit(arm64::stp_q_off(0, 1, REG_SCRATCH3, 0));
    else emit(arm64::str_q_uoff(0, REG_SCRATCH3, 0));
    emit_mark_write_watch(9, last);
    const u32 b_done = static_cast<u32>(m_code_buf.size());
    emit(0);

    const u32 helper_idx = static_cast<u32>(m_code_buf.size());
    emit_vec_store_span_helper(9, lo_off, hi_off, width, instr_rip);
    const u32 done_idx = static_cast<u32>(m_code_buf.size());

    auto disp = [](u32 from, u32 to) {
        return static_cast<i32>(to - from) * 4;
    };
    m_code_buf[cross_cbnz] = arm64::cbnz_w(6, disp(cross_cbnz, helper_idx));
    m_code_buf[page_cbnz] = arm64::cbnz_w(5, disp(page_cbnz, helper_idx));
    m_code_buf[b_done] = arm64::b_imm(disp(b_done, done_idx));
}

void JitCompiler::emit_sse_store_mem128_fast(u8 src_xmm, u8 guest_addr_reg,
                                             u64 instr_rip) {
    const u32 off_lo = xmm_u64_off(src_xmm, 0);
    const u32 off_hi = xmm_u64_off(src_xmm, 1);
    // Linear same-page and region-bounds checks use SUBS. compile_sse
    // then keeps incoming NZCV for `CMP; MOVAPS [mem]; Jcc` (Intel SDM
    // Vol. 2 MOVAPS / Jcc). Spill/fill so that JAE does not fuse from
    // the page-check C flag.
    const bool hold = should_hold_nzcv();
    if (hold) emit_nzcv_spill();
    // A bulk memcpy fills memory through this path, so the store ring is
    // blind to exactly the copies it exists to explain unless it is
    // recorded here too. Width 16 marks it as the vector store.
    if (m_store_ring_stores &&
        fits_ldr_x_offset(off_lo) && fits_ldr_x_offset(off_hi)) {
        emit(arm64::sub_x_imm(31, 31, 64));
        emit(arm64::stp_x_off(guest_addr_reg, 9, 31, 0));
        emit(arm64::stp_x_off(10, 11, 31, 2));
        emit(arm64::ldr_x_uoff(10, REG_CPU_STATE, off_lo >> 3));
        emit(arm64::ldr_x_uoff(11, REG_CPU_STATE, off_hi >> 3));
        emit(arm64::ldr_x_uoff(9, 31, 0));
        emit_store_ring_record_vec(9);
        emit(arm64::ldr_x_uoff(9, 31, 1));
        emit(arm64::ldp_x_off(10, 11, 31, 2));
        emit(arm64::add_x_imm(31, 31, 64));
    }
    // Long-mode guest base: the store carries the same write-watch and SMC
    // obligations as the scalar linear64 store, which emit_linear64_store128
    // enforces before it stores inline.
    if (m_block_is_64bit && m_cfg.linear64 && m_cfg.linear64_vec128 && m_linear_base_pinned &&
        fits_ldr_q_offset(off_lo)) {
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, off_lo >> 4));
        emit_linear64_store128(0, guest_addr_reg);
        if (hold) emit_nzcv_fill();
        return;
    }
    if (m_block_is_64bit || (m_cfg.linear_jit && m_memory.is_linear()) ||
        !fits_ldr_x_offset(off_lo) || !fits_ldr_x_offset(off_hi)) {
        emit_vec_store_span(guest_addr_reg, off_lo, 0, 16, instr_rip);
        if (hold) emit_nzcv_fill();
        return;
    }

    // The write-region caches: writable, code-free regions only. The read
    // caches this used to probe include the block's own code region, whose
    // pages may be read-only or hold live translations.
    emit(arm64::ldr_x_uoff(10, REG_CPU_STATE, off_lo >> 3));
    emit(arm64::ldr_x_uoff(11, REG_CPU_STATE, off_hi >> 3));
    emit(arm64::mov_w(9, guest_addr_reg));
    u32 blo_idx[2], bhs_idx[2], done_b_idx[2], probe_idx[2];
    for (int r = 0; r < 2; ++r) {
        probe_idx[r] = static_cast<u32>(m_code_buf.size());
        emit(arm64::ldr_w_uoff(3, REG_JIT_CTX,
                               (r == 0 ? JIT_CTX_OFF_WREGION_BASE
                                       : JIT_CTX_OFF_WREGION2_BASE) >> 2));
        emit(arm64::subs_w(31, 9, 3));
        blo_idx[r] = static_cast<u32>(m_code_buf.size());
        emit(0);
        emit(arm64::ldr_w_uoff(4, REG_JIT_CTX,
                               (r == 0 ? JIT_CTX_OFF_WREGION_END
                                       : JIT_CTX_OFF_WREGION2_END) >> 2));
        // 64-bit compare of the LAST byte: a store running past the 4 GiB
        // boundary misses instead of passing a wrapped 32-bit test.
        emit(arm64::add_x_imm(8, 9, 15));
        emit(arm64::subs_x(31, 8, 4));
        bhs_idx[r] = static_cast<u32>(m_code_buf.size());
        emit(0);
        emit(arm64::sub_w(4, 9, 3));
        emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                               (r == 0 ? JIT_CTX_OFF_WREGION_HOST
                                       : JIT_CTX_OFF_WREGION2_HOST) >> 3));
        // Release, one fence for the 128-bit pair: before both guest stores.
        emit_vector_tso_fence();
        emit(arm64::str_x_reg(10, 3, 4));
        emit(arm64::add_x_imm(8, 4, 8));
        emit(arm64::str_x_reg(11, 3, 8));
        done_b_idx[r] = static_cast<u32>(m_code_buf.size());
        emit(0);
    }
    const u32 slow_idx = static_cast<u32>(m_code_buf.size());
    emit_vec_store_span_helper(9, off_lo, 0, 16, instr_rip);
    const u32 done_idx = static_cast<u32>(m_code_buf.size());
    if (hold) emit_nzcv_fill();
    for (int r = 0; r < 2; ++r) {
        const u32 miss = r == 0 ? probe_idx[1] : slow_idx;
        m_code_buf[blo_idx[r]] = arm64::b_cond(
            arm64::CC, static_cast<i32>(miss - blo_idx[r]) * 4);
        m_code_buf[bhs_idx[r]] = arm64::b_cond(
            arm64::CS, static_cast<i32>(miss - bhs_idx[r]) * 4);
        m_code_buf[done_b_idx[r]] = arm64::b_imm(
            static_cast<i32>(done_idx - done_b_idx[r]) * 4);
    }
}

// The end-of-block tail every vector lowering reaches by `break`. Shared by
// compile_sse and compile_avx_unique since the split; open-coded once when
// they were one function.
void JitCompiler::finish_vector_block(u64 next_rip, bool is_last) {
    if (!is_last) return;
    emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                           jit_iters_offset() >> 2));
    emit_update_rip(next_rip);
    emit_epilogue(JitExecResult::Continue);
}

bool JitCompiler::emit_xmm_qword_store(u8 xmm, unsigned half,
                                       const DecodedInstruction& instr,
                                       const Operand& dst, u64 next_rip) {
    // MOVSD/MOVLPS/MOVLPD/MOVHPS/MOVHPD m64, xmm store ONE quadword (Intel
    // SDM Vol. 2). As one 64-bit guest store a straddle into an unmapped or
    // read-only page faults before writing anything, as on x86; as two
    // 32-bit stores it wrote the low half first. The value sits in
    // REG_CALL_TMP (callee-saved) across the guard and its slow-path BLR,
    // exactly as MOVQ m64, xmm does.
    const u32 off = xmm_u64_off(xmm, static_cast<u8>(half));
    if (!fits_ldr_x_offset(off)) return false;
    emit(arm64::ldr_x_uoff(REG_CALL_TMP, REG_CPU_STATE, off >> 3));
    emit_compute_ea(REG_SCRATCH4, dst.mem, next_rip);
    emit_m128_align_check_if_required(REG_SCRATCH4, instr, dst);
    emit_guest_store64(REG_CALL_TMP, REG_SCRATCH4);
    return true;
}

} // namespace vitreo::cpu::jit
