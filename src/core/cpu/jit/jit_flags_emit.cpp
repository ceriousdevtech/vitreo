// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_flags_emit.cpp
/// @brief Flag publication and carried-NZCV state
///
/// Split out of jit_compiler.cpp by pure code motion; see that file for
/// the JIT register convention.

#include "jit_crash_breadcrumbs.hpp"

#include "jit_compiler.hpp"
#include "jit_emit_internal.hpp"
#include "jit_symbol_map.hpp"
#include "jit_split_layout.hpp"
#include "ir/jit_ir.hpp"
#include "stencils.hpp"
#include "jit_coverage.hpp"
#include "jit_stats.hpp"
#include "core/cpu/decode/guest_mapped_reader.hpp"
#include "flag_liveness.hpp"
#include "jit_helpers.hpp"
#include "idiom_detector.hpp"
#include "linear_fault.hpp"
#include "hot_block_dump.hpp"
#include "../../memory/jit_region_registry.hpp"
#include "core/cpu/lock_prefix.hpp"
#include "../../memory/mem_watch.hpp"
#include "core/memory/write_watch_traps.hpp"
#include "core/math/host_fenv.hpp"
#include "core/base/perf_window.hpp"
#include "core/cpu/decode/opcodes.hpp"  // inlinable perf_window_active_fast()
#include <vitreo/diagnostics.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>
#include <malloc/malloc.h>

namespace vitreo::cpu::jit {

using namespace jit_detail;

int JitCompiler::fused_arm_cond(NzcvState state, u8 x86_cc,
                                KindGuarded) const noexcept {
    if (state == NzcvState::None || x86_cc > 0xF) return -1;
    if (state == NzcvState::FpCmp) {
        // NZCV from FCMP after COMISS-family EFLAGS (see NzcvState::FpCmp):
        // less N=1 V=0, equal Z=1, greater N=Z=V=0, unordered V=1 N=0.
        //   B   CF=1 (less|unordered)          -> LT (N!=V)
        //   NB  CF=0 (greater|equal)           -> GE (N==V)
        //   BE  CF|ZF (not greater)            -> LE (Z|N!=V)
        //   A   !CF&!ZF (greater)              -> GT (!Z&N==V)
        //   P   PF=1 (unordered)               -> VS
        //   NP  PF=0                           -> VC
        // E/NE need Z|V, and the signed/sign/overflow conditions read the
        // cleared SF/OF; none is one ARM condition, so they stay lazy.
        switch (x86_cc) {
        case 0x2: return arm64::LT;
        case 0x3: return arm64::GE;
        case 0x6: return arm64::LE;
        case 0x7: return arm64::GT;
        case 0xA: return arm64::VS;
        case 0xB: return arm64::VC;
        default:  return -1;
        }
    }
    if (state == NzcvState::Carried) {
        // Runtime kind is Sub/And/Add/IncDec. Only conditions that share
        // one ARM mapping across those writers may B.cond.
        if (x86_cc == 0x2 || x86_cc == 0x3 || x86_cc == 0x6 || x86_cc == 0x7)
            return -1;
        if (x86_cc == 0xA || x86_cc == 0xB)
            return -1;
    }
    if (state == NzcvState::NzOnly) {
        // TST result,result after a nonzero-count shift: N and Z are the
        // result's SF/ZF, but ARM C/V were cleared while x86 CF (the
        // shifted-out bit) and OF (defined for count==1 only, Intel SDM
        // Vol. 2 SAL/SAR/SHL/SHR) live solely in the lazy record. Every
        // condition touching CF or OF must stay on the helper.
        if (x86_cc != 0x4 && x86_cc != 0x5 && x86_cc != 0x8 && x86_cc != 0x9)
            return -1;
    }
    // x86 cc (index) -> ARM64 condition, for NZCV from a SUBS (CMP/SUB).
    // Parity (P=0xA, NP=0xB) has no NZCV equivalent -> -1.
    static constexpr int kMap[16] = {
        arm64::VS, arm64::VC,  // 0 O,  1 NO
        arm64::CC, arm64::CS,  // 2 B,  3 NB   (x86 CF = !ARM64 C)
        arm64::EQ, arm64::NE,  // 4 Z,  5 NZ
        arm64::LS, arm64::HI,  // 6 BE, 7 A
        arm64::MI, arm64::PL,  // 8 S,  9 NS
        -1,        -1,         // A P,  B NP   (no NZCV parity)
        arm64::LT, arm64::GE,  // C L,  D GE
        arm64::LE, arm64::GT,  // E LE, F G
    };
    int ac = kMap[x86_cc];
    if (ac < 0) return -1;
    if (state == NzcvState::And) {
        // ANDS clears C and V. x86 TEST also clears CF/OF, so the SIGNED and
        // zero/sign conditions still match (V==OF==0). But the UNSIGNED
        // conditions (B/NB/BE/A) test x86 CF whereas ARM64 CC/CS/LS/HI test
        // C — with C forced 0 they invert, so those must not fuse.
        if (x86_cc == 0x2 || x86_cc == 0x3 || x86_cc == 0x6 || x86_cc == 0x7)
            return -1;
    }
    if (state == NzcvState::Add) {
        // ADDS leaves x86 CF == ARM64 C (carry-out, NOT inverted like SUB).
        // The kMap above is derived for SUBS, so the unsigned conditions need
        // fixups: B/NB map to CS/CC (opposite of the subtract map), and BE/A
        // (CF|ZF, !CF&!ZF) have no single-flag ARM equivalent for the add
        // carry sense, so they must not fuse. Signed and zero/sign conditions
        // (V==OF, Z, N) are identical to the subtract map.
        if (x86_cc == 0x2) return arm64::CS;   // B  (CF)
        if (x86_cc == 0x3) return arm64::CC;   // NB (!CF)
        if (x86_cc == 0x6 || x86_cc == 0x7)    // BE, A
            return -1;
    }
    if (state == NzcvState::IncDec) {
        // Intel SDM Vol. 2 INC/DEC: OF/SF/ZF/AF/PF update, CF is unchanged.
        // ADDS/SUBS #1 leave Z/N/V correct for those, but ARM C is not x86 CF.
        if (x86_cc == 0x2 || x86_cc == 0x3 || x86_cc == 0x6 || x86_cc == 0x7)
            return -1;
    }
    if (state == NzcvState::Sub8) {
        // Zero-extended 8-bit SUBS: Z and C match x86 ZF/CF (borrow is the
        // same at 8 and 32 bits when the high 24 bits are zero). N/V are
        // 32-bit and do not match SF/OF, so signed/overflow Jcc stay lazy.
        if (x86_cc <= 0x1 || x86_cc >= 0x8)
            return -1;
    }
    return ac;
}

void JitCompiler::emit_clear_cf_overlay() {
    // The two one-byte overlays are intentionally adjacent and halfword
    // aligned in CpuState. A full x86 flag writer owns both CF and OF, so one
    // STRH clears the pair exactly as the old two STRBs did.
    static_assert(offsetof(CpuState, of_overlay) ==
                  offsetof(CpuState, cf_overlay) + 1);
    static_assert((offsetof(CpuState, cf_overlay) & 1u) == 0);
    emit(arm64::strh_uoff(31, REG_CPU_STATE, cf_overlay_offset() >> 1));
}

void JitCompiler::emit_set_lazy_flags(CpuState::FlagOp op,
                                       u8 src_arm_reg,
                                       u8 dst_arm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::Flags);
    // Within-block liveness: this full-flag writer's result is dead. Leave
    // any prior pending FlagOp intact — do NOT write FlagOp::None.
    //
    // Record what was skipped: if the very next instruction is a Jcc, its
    // taken edge leaves the block with these flags live and the store has to
    // happen on that edge. queue_mid_block_jcc_exit either moves it into the
    // cold stub (fused path) or emits it inline (every other path).
    if (m_omit_lazy_current) {
        // The record is dead, the overlay is not — see
        // m_omit_lazy_keep_overlay.
        if (m_omit_lazy_keep_overlay) emit_clear_cf_overlay();
        m_pending_stub_store.valid = true;
        m_pending_stub_store.op = op;
        m_pending_stub_store.src = src_arm_reg;
        m_pending_stub_store.dst = dst_arm_reg;
        return;
    }
    emit_lazy_flag_store_raw(op, src_arm_reg, dst_arm_reg);
}

// The skipped store's replay (queue_mid_block_jcc_exit) reads the operands
// back from the ARM registers the publisher named, which is sound only while
// nothing between the publisher and the Jcc can call out. A memory-
// destination writer (ADD/SUB/AND/OR/XOR/ADC/SBB/INC/DEC/NEG, the shifts,
// CMPXCHG ... [mem]) defers its record and THEN stores its result, and the
// store's slow path -- a cold stub's shared helper for an EA above the linear
// window or on a mixed page, the store ring's call -- marshals into X0-X2 and
// calls C++. The Jcc then published garbage: a browser renderer took
// `add qword [rdi+0x880], -8; je` with a nonzero sum as ZF=1 and hit its
// ud2 (2026-10-22). So the store publishes the record first, on both of its
// paths, exactly as the Jcc's non-fused paths do when they clobber.
void JitCompiler::publish_pending_stub_store() {
    if (!m_pending_stub_store.valid) return;
    emit_lazy_flag_store_raw(m_pending_stub_store.op, m_pending_stub_store.src,
                             m_pending_stub_store.dst, m_pending_stub_store.wide);
    m_pending_stub_store = PendingStubStore{};
    note_elided_record_published(m_current_insn_index);
}

void JitCompiler::emit_fcvt_scalar_by_mxcsr(u32 fcvt_op_base) {
    // ARM rmode occupies bits 20:19 of the FCVT encoding:
    //   00 = nearest, 01 = +inf, 10 = -inf, 11 = zero
    // x86 MXCSR.RC (bits 14:13):
    //   00 = nearest, 01 = -inf, 10 = +inf, 11 = zero
    // so the two directed modes are swapped between the encodings.
    constexpr u32 kRmodeShift = 19;
    const u32 op_nearest = fcvt_op_base;                       // RC 00
    const u32 op_minus   = fcvt_op_base | (2u << kRmodeShift); // RC 01
    const u32 op_plus    = fcvt_op_base | (1u << kRmodeShift); // RC 10
    const u32 op_zero    = fcvt_op_base | (3u << kRmodeShift); // RC 11

    const u32 mxcsr_off = static_cast<u32>(
        offsetof(CpuState, sse) + offsetof(SseState, mxcsr));
    emit(arm64::ldr_w_uoff(REG_SCRATCH3, REG_CPU_STATE, mxcsr_off >> 2));
    emit(arm64::lsr_w_imm(REG_SCRATCH3, REG_SCRATCH3, 13));
    emit(arm64::movz_w(REG_SCRATCH4, 3));
    emit(arm64::and_w(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));

    // Round-to-nearest is overwhelmingly the common case, so it is the
    // fall-through; the other three cost a compare each.
    const usize to_nearest = m_code_buf.size();
    emit(0);  // CBZ -> nearest
    emit(arm64::cmp_w_imm(REG_SCRATCH3, 1));
    const usize to_minus = m_code_buf.size();
    emit(0);  // B.EQ -> minus
    emit(arm64::cmp_w_imm(REG_SCRATCH3, 2));
    const usize to_plus = m_code_buf.size();
    emit(0);  // B.EQ -> plus

    emit(op_zero);
    const usize done_from_zero = m_code_buf.size();
    emit(0);

    const usize plus_pos = m_code_buf.size();
    emit(op_plus);
    const usize done_from_plus = m_code_buf.size();
    emit(0);

    const usize minus_pos = m_code_buf.size();
    emit(op_minus);
    const usize done_from_minus = m_code_buf.size();
    emit(0);

    const usize nearest_pos = m_code_buf.size();
    emit(op_nearest);

    const usize done = m_code_buf.size();
    m_code_buf[to_nearest] = arm64::cbz_w(
        REG_SCRATCH3, static_cast<i32>(nearest_pos - to_nearest) * 4);
    m_code_buf[to_minus] = arm64::b_cond(
        arm64::EQ, static_cast<i32>(minus_pos - to_minus) * 4);
    m_code_buf[to_plus] = arm64::b_cond(
        arm64::EQ, static_cast<i32>(plus_pos - to_plus) * 4);
    m_code_buf[done_from_zero] = arm64::b_imm(
        static_cast<i32>(done - done_from_zero) * 4);
    m_code_buf[done_from_plus] = arm64::b_imm(
        static_cast<i32>(done - done_from_plus) * 4);
    m_code_buf[done_from_minus] = arm64::b_imm(
        static_cast<i32>(done - done_from_minus) * 4);
}

void JitCompiler::emit_bmi_result_flags(u8 result_reg, u8 cf_reg, bool w64) {
    const u32 rf_off = cpu_rflags_offset();
    emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
    emit_mov_imm32(REG_SCRATCH4, 0x8C1u);  // OF | SF | ZF | CF
    emit(arm64::bic_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));

    // ZF := (result == 0)
    const usize nz_pos = m_code_buf.size();
    emit(0);
    emit_mov_imm32(REG_SCRATCH4, 0x40u);
    emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
    const usize after_zf = m_code_buf.size();
    m_code_buf[nz_pos] = w64
        ? arm64::cbnz_x(result_reg, static_cast<i32>(after_zf - nz_pos) * 4)
        : arm64::cbnz_w(result_reg, static_cast<i32>(after_zf - nz_pos) * 4);

    // SF := result's sign bit, moved into EFLAGS bit 7.
    emit(w64 ? arm64::lsr_x_imm(REG_SCRATCH4, result_reg, 63)
             : arm64::lsr_w_imm(REG_SCRATCH4, result_reg, 31));
    emit(arm64::orr_x_lsl(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4, 7));

    // CF := cf_reg (already 0 or 1). OF stays clear from the BIC above.
    emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, cf_reg));
    emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));

    emit_mov_imm32(REG_SCRATCH4, static_cast<u32>(CpuState::FlagOp::None));
    emit(arm64::strb_uoff(REG_SCRATCH4, REG_CPU_STATE, flag_op_offset()));
    note_lazy_kind_publish(CpuState::FlagOp::None);
    emit_clear_cf_overlay();
}

void JitCompiler::emit_nzcv_sub_from_rflags() {
    // RFLAGS bits (Intel SDM Vol. 1 3.4.3): CF 0, ZF 6, SF 7, OF 11. NZCV is
    // bits 31:28 (N, Z, C, V), and SUBS sets C to NOT borrow, so C is NOT CF.
    const u32 rf_off = cpu_rflags_offset();
    emit(arm64::ldr_x_uoff(2, REG_CPU_STATE, rf_off / 8));
    emit(arm64::ubfx_w(3, 2, 6, 2));        // (SF << 1) | ZF
    emit(arm64::lsl_w_imm(3, 3, 30));       // N = SF, Z = ZF
    emit(arm64::ubfx_w(4, 2, 0, 1));        // CF
    emit(arm64::sub_w_imm(4, 4, 1));        // bit 0 = NOT CF
    emit(arm64::bfi_w(3, 4, 29, 1));        // C
    emit(arm64::ubfx_w(4, 2, 11, 1));       // OF
    emit(arm64::bfi_w(3, 4, 28, 1));        // V
    emit(arm64::msr_nzcv(3));
}

void JitCompiler::emit_lazy_flag_store_raw(CpuState::FlagOp op,
                                            u8 src_arm_reg,
                                            u8 dst_arm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::Flags);
    emit_lazy_flag_store_raw(op, src_arm_reg, dst_arm_reg, /*wide=*/false);
}

void JitCompiler::emit_lazy_flag_store_raw(CpuState::FlagOp op,
                                            u8 src_arm_reg,
                                            u8 dst_arm_reg,
                                            bool wide) {
    AttrScope attr_scope(*this, EmitAttribution::Flags);
    // Publishing a lazy-flag record is what a block boundary costs, and the
    // cost is the STORES. An unsound probe that omits every live-out record
    // (VITREO_JIT_OMIT_ALL) measures -32% on sc_blocksize bsz1, and
    // a since-removed probe that added redundant stores here priced one store
    // at ~9.9% of bsz1, linearly to four (jit-refactor/FLAG-PUBLICATION.md
    // §3). Four stores therefore account for the whole of the -32%, and the
    // instructions around them are free. So this emitter minimises STORES,
    // not instructions: the record goes out in TWO, where it used to take
    // four.
    //
    // Store 1: the aligned word {pad, flag_op, cf_overlay, of_overlay}. A full
    // x86 flag writer owns CF and OF, so clearing both overlays is exactly
    // what the old separate STRH did -- it is now free, riding in the same
    // word as the op byte. `nzcv_kind` is deliberately NOT in this word (see
    // CpuState): clearing it here would disable the carried-NZCV fast path at
    // every publication.
    static_assert(offsetof(CpuState, flag_op) ==
                  offsetof(CpuState, flag_word_pad) + 1);
    static_assert(offsetof(CpuState, cf_overlay) ==
                  offsetof(CpuState, flag_word_pad) + 2);
    static_assert(offsetof(CpuState, of_overlay) ==
                  offsetof(CpuState, flag_word_pad) + 3);
    static_assert((offsetof(CpuState, flag_word_pad) & 3u) == 0);
    const u32 word_off = flag_word_offset();
    emit_mov_imm32(REG_SCRATCH3, flag_word_value(op));
    emit(arm64::str_w_uoff(REG_SCRATCH3, REG_CPU_STATE, word_off >> 2));
    note_lazy_kind_publish(op);

    // Store 2: flag_src and flag_dst as one STP. They are adjacent 8-aligned
    // u64s, and CpuState places the block low enough that STP's imm7 (signed,
    // scaled by 8, so below offset 504) reaches them -- which is why that
    // block sits ahead of fpu/sse/avx rather than after them.
    //
    // A 64-bit publisher's operands are already full X values, so they are
    // stored straight from their own registers; the two `MOV Xd, Xs` this used
    // to emit were copies into a scratch that was then immediately spilled.
    // A 32-bit publisher still needs its W-move, because the 32-bit FlagOp
    // contract requires the high half zeroed and only a W-form write
    // guarantees that.
    static_assert(offsetof(CpuState, flag_dst) ==
                  offsetof(CpuState, flag_src) + sizeof(u64),
                  "the STP below publishes flag_src and flag_dst as one pair");
    const u32 src_off = flag_src_offset();
    const u32 dst_off = flag_dst_offset();
    const i32 pair_imm = static_cast<i32>(src_off / sizeof(u64));
    const bool pair_ok = (src_off % sizeof(u64)) == 0 &&
                         pair_imm >= -64 && pair_imm <= 63;

    if (pair_ok && wide) {
        emit(arm64::stp_x_off(src_arm_reg, dst_arm_reg, REG_CPU_STATE,
                              pair_imm));
        return;
    }
    // Long-mode 32-bit publishers already W-wrote the destination (SDM Vol. 1
    // §3.4.1.1), so flag_dst is an X register with bits 63:32 clear. Zero-
    // extend the source into the scratch that just held the flag word — still
    // X3, so kLazyFlagStoreClobbers is unchanged — and STP like the 64-bit
    // path. 8/16-bit FlagOps keep the PE32 FMOV/INS/STR Q sequence: their
    // payload is only the low 8/16 bits, and UXTW would publish stale 31:8.
    const bool op32 =
        op == CpuState::FlagOp::Add32 || op == CpuState::FlagOp::Sub32 ||
        op == CpuState::FlagOp::And32 || op == CpuState::FlagOp::Or32 ||
        op == CpuState::FlagOp::Xor32 || op == CpuState::FlagOp::Inc32 ||
        op == CpuState::FlagOp::Dec32 || op == CpuState::FlagOp::Neg32;
    if (!wide && m_block_is_64bit && pair_ok && op32) {
        emit(arm64::mov_w(REG_SCRATCH3, src_arm_reg));
        emit(arm64::stp_x_off(REG_SCRATCH3, dst_arm_reg, REG_CPU_STATE,
                              pair_imm));
        return;
    }

    // 32-bit: STP needs both operands live after a W-move zero-extend, which
    // wants a second scratch. REG_SCRATCH4 is not free -- shifts.cpp parks
    // CF/OF there across the publication (see kLazyFlagStoreClobbers). Use a
    // vector scratch instead: FMOV S31, Wn zeroes the whole Q register and
    // writes the low 32 bits; INS V31.S[2], Wm places dst at the start of the
    // second u64 with the high half still zero; one STR Q publishes both.
    // V31 is never a live guest XMM pin (XMM state lives in CpuState).
    //
    // Measured: the third store is ~10% of bsz1
    // (jit-refactor/FLAG-PUBLICATION.md §3). This brings 32-bit publication
    // to two stores, matching x64.
    if (!wide && fits_ldr_q_offset(src_off)) {
        constexpr u8 kFlagPairVec = 31;
        emit(arm64::fmov_s_from_w(kFlagPairVec, src_arm_reg));
        emit(arm64::ins_v_gpr(kFlagPairVec, 2, dst_arm_reg, arm64::VS_S));
        emit(arm64::str_q_uoff(kFlagPairVec, REG_CPU_STATE, src_off >> 4));
        return;
    }

    // Fallback: one-scratch, two-store form (wide without pair, or Q miss).
    if (fits_ldr_x_offset(src_off)) {
        if (wide) {
            emit(arm64::mov_x(REG_SCRATCH3, src_arm_reg));
        } else {
            emit(0x2A0003E0u | (static_cast<u32>(src_arm_reg) << 16)
                             | static_cast<u32>(REG_SCRATCH3));
        }
        emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, src_off >> 3));
    }

    if (fits_ldr_x_offset(dst_off)) {
        if (wide) {
            emit(arm64::mov_x(REG_SCRATCH3, dst_arm_reg));
        } else {
            emit(0x2A0003E0u | (static_cast<u32>(dst_arm_reg) << 16)
                             | static_cast<u32>(REG_SCRATCH3));
        }
        emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, dst_off >> 3));
    }
}

void JitCompiler::emit_set_lazy_flags_preserve_cf(CpuState::FlagOp op,
                                                   u8 src_arm_reg,
                                                   u8 dst_arm_reg,
                                                   u8 cf_arm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::Flags);
    // Same single-word publication as emit_lazy_flag_store_raw: this is also
    // a full flag writer, so it owns CF and OF and clearing both overlays
    // rides in the op byte's word. The operand pair cannot use an STP here --
    // flag_src is not a plain zero-extend but (CF << 32) | u32(src), so it
    // needs the scratch anyway.
    emit_mov_imm32(REG_SCRATCH3, flag_word_value(op));
    emit(arm64::str_w_uoff(REG_SCRATCH3, REG_CPU_STATE,
                           flag_word_offset() >> 2));
    note_lazy_kind_publish(op);

    u32 src_off = flag_src_offset();
    if (fits_ldr_x_offset(src_off)) {
        // X3 = (u64(CF) << 32) | u32(original).
        emit(0xD3607C00u | (static_cast<u32>(cf_arm_reg) << 5)
                         | static_cast<u32>(REG_SCRATCH3));
        emit(0xAA000000u | (static_cast<u32>(src_arm_reg) << 16)
                         | (static_cast<u32>(REG_SCRATCH3) << 5)
                         | static_cast<u32>(REG_SCRATCH3));
        emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, src_off >> 3));
    }

    u32 dst_off = flag_dst_offset();
    if (fits_ldr_x_offset(dst_off)) {
        emit(0x2A0003E0u | (static_cast<u32>(dst_arm_reg) << 16)
                         | static_cast<u32>(REG_SCRATCH3));
        emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, dst_off >> 3));
    }
}

void JitCompiler::emit_set_shift_lazy_flags(CpuState::FlagOp op,
                                             u8 original_arm_reg,
                                             u8 count_arm_reg,
                                             u8 result_arm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::Flags);
    // Within-block liveness: this proven-nonzero shift's lazy flags are dead
    // before a later full-flag writer. Leave any prior pending FlagOp intact.
    if (m_omit_lazy_current) return;

    // flag_op
    u32 op_off = flag_op_offset();
    emit_mov_imm32(REG_SCRATCH3, static_cast<u32>(op));
    emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, op_off));
    emit_clear_cf_overlay();

    // flag_src = original (zero-extended 32→64)
    u32 src_off = flag_src_offset();
    if (fits_ldr_x_offset(src_off)) {
        emit(0x2A0003E0u | (static_cast<u32>(original_arm_reg) << 16)
                         | static_cast<u32>(REG_SCRATCH3));
        emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, src_off >> 3));
    }

    // flag_dst = (count << 32) | result
    u32 dst_off = flag_dst_offset();
    if (fits_ldr_x_offset(dst_off)) {
        // MOV W3, W_count (zero-extends to X3)
        emit(0x2A0003E0u | (static_cast<u32>(count_arm_reg) << 16)
                         | static_cast<u32>(REG_SCRATCH3));
        // LSL X3, X3, #32
        emit(0xD3607C00u | (static_cast<u32>(REG_SCRATCH3) << 5)
                         | static_cast<u32>(REG_SCRATCH3));
        // ORR X3, X3, X_result (zero-extended from W)
        emit(0xAA000000u | (static_cast<u32>(result_arm_reg) << 16)
                         | (static_cast<u32>(REG_SCRATCH3) << 5)
                         | static_cast<u32>(REG_SCRATCH3));
        emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, dst_off >> 3));
    }
}

void JitCompiler::emit_materialize_flags() {
    AttrScope attr_scope(*this, EmitAttribution::Flags);
    // Jcc exits to dispatcher which materializes flags from lazy state.
}

// Inline two-region store fast path.
//
// Unlike the load path, this uses the wregion/wregion2 slots, which are only
// ever published for regions that are writable AND hold no compiled code (see
// JitContext). That is what lets the emitted code store directly without a
// permission test and without calling notify_executable_write() — both were
// already established when the region was published. Any address not covered
// falls through to jit_mem_write32(), which does the full probe, watch check,
// SMC notification and access-violation raise, and remains the reference.
// SMC-guarded linear store of `width` bytes. Shared by store8/16/32/64: a
// write onto a page that currently holds compiled code routes through the
// width's write helper (driving notify_executable_write). Data pages —
// including a heap sitting between two images — store inline at
// linear_base+addr.
// W9=addr, W10=value are the dedicated memory scratch regs, so loading
// linear_base into X3 can't clobber a caller register.
bool JitCompiler::should_hold_nzcv() const noexcept {
    return m_flags.hold_nzcv || m_flags.nzcv != NzcvState::None
        || m_flags.incoming_nzcv != NzcvState::None
        || m_flags.pred_nzcv_live;
}

void JitCompiler::emit_nzcv_spill() {
    AttrScope attr_scope(*this, EmitAttribution::Flags);
    // Prologue is STR X30, [SP, #-16]! — [SP+0] is LR, [SP+8] is unused
    // padding. Stash NZCV there so a BLR helper can clobber X11 and a
    // pending-raise epilogue (LDR X30, [SP], #16) still pops the real LR.
    // Do not SUB SP: that would make the epilogue pop this slot as LR.
    if (m_flags.nzcv_hold_depth++ > 0) {
        return;
    }
    emit(arm64::mrs_nzcv(11));
    emit(arm64::str_x_uoff(11, 31, 1));
}

void JitCompiler::emit_nzcv_fill() {
    AttrScope attr_scope(*this, EmitAttribution::Flags);
    if (m_flags.nzcv_hold_depth == 0) {
        return;
    }
    if (--m_flags.nzcv_hold_depth > 0) {
        return;
    }
    emit(arm64::ldr_x_uoff(11, 31, 1));
    emit(arm64::msr_nzcv(11));
}

u8 JitCompiler::nzcv_kind_byte(NzcvState state) noexcept {
    switch (state) {
    case NzcvState::Sub: return 1;
    case NzcvState::And: return 2;
    case NzcvState::Add: return 3;
    case NzcvState::IncDec: return 4;
    // NzOnly stays 0 on purpose: the runtime kind-byte consumers (carried
    // Jcc, emit_cset_for_lazy_kind) assume a full Sub/And/Add/IncDec NZCV,
    // which a shift's TST does not provide.
    default: return 0;
    }
}

void JitCompiler::emit_store_nzcv_kind(NzcvState state) {
    AttrScope attr_scope(*this, EmitAttribution::Flags);
    const u8 kind = nzcv_kind_byte(state);
    if (kind == 0) {
        emit(arm64::strb_uoff(31, REG_CPU_STATE, nzcv_kind_offset()));
        return;
    }
    emit(arm64::movz_w(4, kind));
    emit(arm64::strb_uoff(4, REG_CPU_STATE, nzcv_kind_offset()));
}

void JitCompiler::emit_nzcv_spill_if_kind() {
    emit(arm64::ldrb_uoff(4, REG_CPU_STATE, nzcv_kind_offset()));
    const usize cbz = m_code_buf.size();
    emit(0);
    emit_nzcv_spill();
    m_code_buf[cbz] = arm64::cbz_w(
        4, static_cast<i32>(m_code_buf.size() - cbz) * 4);
}

void JitCompiler::emit_nzcv_fill_if_kind() {
    emit(arm64::ldrb_uoff(4, REG_CPU_STATE, nzcv_kind_offset()));
    const usize cbz = m_code_buf.size();
    emit(0);
    emit_nzcv_fill();
    m_code_buf[cbz] = arm64::cbz_w(
        4, static_cast<i32>(m_code_buf.size() - cbz) * 4);
}

bool JitCompiler::jcc_fuses_from_carried_nzcv(u8 x86_cc) const noexcept {
    return fused_arm_cond(NzcvState::Sub, x86_cc, KindGuarded{}) >= 0
        && fused_arm_cond(NzcvState::And, x86_cc, KindGuarded{}) >= 0
        && fused_arm_cond(NzcvState::Add, x86_cc, KindGuarded{}) >= 0;
}

bool JitCompiler::can_try_carried_jcc(u8 x86_cc) const noexcept {
    // Jcc-first blocks use incoming None and a runtime nzcv_kind from the
    // predecessor. Sub8 (and any writer whose kind byte is 0) publishes
    // zero-extended SUBS NZCV that is only safe for unsigned Jcc — a stale
    // nonzero kind must not let signed Jcc fuse that NZCV (ContoursCreate
    // `cmp byte [mem], 0; jge` after a prior CMP32).
    if (!m_flags.pred_nzcv_live || !jcc_fuses_from_carried_nzcv(x86_cc)) {
        return false;
    }
    if (m_flags.incoming_nzcv == NzcvState::None) {
        return true;
    }
    return nzcv_kind_byte(m_flags.incoming_nzcv) != 0;
}

} // namespace vitreo::cpu::jit
