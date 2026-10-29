// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — the native LOCK read-modify-write emitter: emit_native_lock_rmw
// and join_native_lock, called by ops/locked_rmw{32,64}.cpp, the XCHG
// r/m32 arm in ops/memory_ops_xchg.cpp and ops/misc64.cpp. Split out of
// ops/memory_ops.cpp as pure code motion.

#include "../jit_helpers.hpp"
#include "core/cpu/lock_prefix.hpp"

#include <atomic>

namespace vitreo::cpu::jit {

// ============================================================================
// Native LOCK read-modify-write
// ============================================================================
//
// Every LOCK RMW used to be a helper call: spill the pins, BLR, resolve the
// host pointer through the TLB (a shared lock on a miss), take the
// LockPrefixGuard, do one host atomic, materialise all six flags, return and
// reload. On a 64-bit title frame the jit_lock_* helpers were 4.5% of the guest
// thread and 3.8% of all CPU -- refcounts, the task graph, spinlocks.
//
// The fast path is the same host atomic, done in place:
//
//   * guest-base window only, with hardware protection (the linear64 store
//     path's model): the EA must be naturally aligned (a split lock is x86's
//     bus-locked case and stays on the helper's non-atomic fallback), inside
//     the reservation, and its host page's check byte clear -- no mixed
//     protection, no translated code, no write watch. A read-only page is
//     enforced by the host page itself: the LSE store faults, and
//     linear_fault turns it into the guest's write AV.
//   * the reader side of the LockPrefixGuard protocol (lock_prefix.hpp):
//     STLR slot:=1, LDAR writers, and a writer in flight stands down to the
//     helper. STLR followed by LDAR is RCsc on AArch64, which is the
//     store->load ordering the protocol's seq_cst fence provides.
//   * one FEAT_LSE AL atomic (LDADDAL/LDCLRAL/LDEORAL/LDSETAL/SWPAL/CASAL):
//     acquire+release, x86 LOCK's full ordering.
//   * the form's lazy flag record, published exactly as the unlocked form
//     publishes it, so both paths leave the flags in CpuState memory.
//
// VITREO_JIT_NATIVE_LOCK=0 keeps every LOCK form on its helper (bisection).
namespace {
bool native_lock_knob() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("VITREO_JIT_NATIVE_LOCK");
        return v == nullptr || v[0] == '\0' || v[0] != '0';
    }();
    return on;
}
}  // namespace

bool JitCompiler::emit_native_lock_rmw(NativeLockSite& site,
                                       NativeLockKind kind, int width,
                                       const MemoryOperand& mem, u64 next_rip,
                                       const Operand* src) {
    site = NativeLockSite{};
    if (!native_lock_knob() || lock_prefix_force_exclusive()) return false;
    if (!m_block_is_64bit || !m_cfg.linear64 || !m_linear_base_pinned ||
        !m_cfg.linear64_hw_protect) {
        return false;
    }
    if (width != 4 && width != 8) return false;
    // A deferred (omitted) record would be re-emitted on a later Jcc edge from
    // THIS path's registers, which the helper path does not fill. LOCK forms
    // are never omitted today; decline rather than rely on it.
    if (m_omit_lazy_current) return false;
    const bool wide = width == 8;
    const u32 size = wide ? 3u : 2u;

    // INC/DEC keep the incoming CF, which may still be a lazy record. Capture
    // it first: the capture may call a helper, and nothing below may be live
    // across that.
    if (kind == NativeLockKind::Inc || kind == NativeLockKind::Dec) {
        emit_lazy_cf_to_call_tmp();
    }

    site.active = true;
    emit_compute_ea64(9, mem, next_rip);
    const auto branch_to_slow = [&](u8 reg, bool branch_wide) {
        site.to_slow.push_back({m_code_buf.size(), reg, branch_wide});
        emit(0);
    };
    // Natural alignment: an LSE atomic requires it, and x86's misaligned
    // (split) LOCK is the helper's non-atomic fallback.
    emit(arm64::ubfx_x(8, 9, 0, static_cast<u8>(wide ? 3 : 2)));
    branch_to_slow(8, true);
    // Inside the reservation. Aligned, so the operand cannot leave the host
    // page or the window.
    emit(arm64::lsr_x_imm(8, 9, m_cfg.linear64_address_bits));
    branch_to_slow(8, true);
    // Any reason the host page needs software handling -- mixed protection,
    // translated code, a write watch -- is a set bit in this byte, which is
    // exactly what a direct store asks (emit_linear64_store).
    emit(arm64::ubfx_x(4, 9, static_cast<u8>(kLinearHostPageShift),
                       static_cast<u8>(m_cfg.linear64_address_bits -
                                       kLinearHostPageShift)));
    emit(arm64::ldrb_reg(3, emit_linear64_check_map_base(3), 4));
    branch_to_slow(3, false);

    // Reader side of the LockPrefixGuard protocol. execute() publishes the
    // thread's slot before any block runs; a context without one (a path that
    // never went through execute()) simply takes the helper.
    emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                           JIT_CTX_OFF_LOCK_READER_ACTIVE >> 3));
    const usize null_slot_branch = m_code_buf.size();
    emit(0);  // CBZ X16, slow
    emit(arm64::movz_w(17, 1));
    emit(arm64::stlr_w(17, 16));
    emit(arm64::ldr_x_uoff(17, REG_JIT_CTX,
                           JIT_CTX_OFF_LOCK_PREFIX_WRITERS >> 3));
    emit(arm64::ldar_w(17, 17));
    site.to_standdown = m_code_buf.size();
    emit(0);  // CBNZ W17, standdown
    emit(arm64::add_x(17, REG_LINEAR_BASE, 9));

    // Operands. X4 = the source as the instruction states it; X0 = the old
    // memory value the atomic returns (CMPXCHG: the expected value going in).
    constexpr u8 kOld = REG_SCRATCH0;
    constexpr u8 kRs = REG_SCRATCH1;
    constexpr u8 kResult = REG_SCRATCH2;
    constexpr u8 kSrc = REG_SCRATCH4;
    const auto load_src = [&] {
        if (src == nullptr) return;
        if (is_immediate_operand(src->type)) {
            if (wide) emit_mov_imm64(kSrc, src->imm);
            else emit_mov_imm32(kSrc, static_cast<u32>(src->imm));
        } else if (wide) {
            emit_load_gpr64(kSrc, src->reg);
        } else {
            emit_load_gpr32(kSrc, src->reg);
        }
    };
    const auto lse = [&](arm64::LseOp op, u8 rs) {
        emit(arm64::lse_al(op, size, rs, kOld, 17));
    };
    switch (kind) {
    case NativeLockKind::Add:
    case NativeLockKind::Xadd:
        load_src();
        lse(arm64::LseOp::Add, kSrc);
        break;
    case NativeLockKind::Sub:
        load_src();
        emit(wide ? arm64::sub_x(kRs, 31, kSrc) : arm64::sub_w(kRs, 31, kSrc));
        lse(arm64::LseOp::Add, kRs);
        break;
    case NativeLockKind::Or:
        load_src();
        lse(arm64::LseOp::Set, kSrc);
        break;
    case NativeLockKind::And:
        load_src();
        emit(wide ? arm64::mvn_x(kRs, kSrc) : arm64::mvn_w(kRs, kSrc));
        lse(arm64::LseOp::Clr, kRs);
        break;
    case NativeLockKind::Xor:
        load_src();
        lse(arm64::LseOp::Eor, kSrc);
        break;
    case NativeLockKind::Inc:
        emit(arm64::movz_w(kSrc, 1));
        lse(arm64::LseOp::Add, kSrc);
        break;
    case NativeLockKind::Dec:
        if (wide) emit_mov_imm64(kSrc, -1);
        else emit_mov_imm32(kSrc, 0xFFFFFFFFu);
        lse(arm64::LseOp::Add, kSrc);
        break;
    case NativeLockKind::Xchg:
        load_src();
        lse(arm64::LseOp::Swp, kSrc);
        break;
    case NativeLockKind::Cmpxchg:
        load_src();  // the value stored on a match
        if (wide) emit_load_gpr64(kOld, static_cast<u8>(RegIndex::RAX));
        else emit_load_gpr32(kOld, static_cast<u8>(RegIndex::RAX));
        emit(wide ? arm64::mov_x(kResult, kOld) : arm64::mov_w(kResult, kOld));
        emit(arm64::casal(size, kOld, kSrc, 17));
        break;
    }
    emit(arm64::stlr_w(31, 16));  // slot.active = 0 (release)

    // Result, flags and register write-back, as the unlocked forms publish
    // them (ops64.cpp): 32-bit through emit_set_lazy_flags, 64-bit through
    // emit_set_lazy_flags64, INC/DEC through their CF-preserving records.
    using F = CpuState::FlagOp;
    const auto publish = [&](F op32, F op64, u8 s, u8 d) {
        if (wide) emit_set_lazy_flags64(op64, s, d);
        else emit_set_lazy_flags(op32, s, d);
    };
    switch (kind) {
    case NativeLockKind::Add:
    case NativeLockKind::Xadd:
        emit(wide ? arm64::add_x(kResult, kOld, kSrc)
                  : arm64::add_w(kResult, kOld, kSrc));
        publish(F::Add32, F::Add64, kSrc, kResult);
        if (kind == NativeLockKind::Xadd) {
            if (wide) emit_store_gpr64(kOld, src->reg);
            else emit_store_gpr32(kOld, src->reg);
        }
        break;
    case NativeLockKind::Sub:
        emit(wide ? arm64::sub_x(kResult, kOld, kSrc)
                  : arm64::sub_w(kResult, kOld, kSrc));
        publish(F::Sub32, F::Sub64, kSrc, kResult);
        break;
    case NativeLockKind::Or:
        emit(wide ? arm64::orr_x(kResult, kOld, kSrc)
                  : arm64::orr_w(kResult, kOld, kSrc));
        publish(F::Or32, F::Or64, kSrc, kResult);
        break;
    case NativeLockKind::And:
        emit(wide ? arm64::and_x(kResult, kOld, kSrc)
                  : arm64::and_w(kResult, kOld, kSrc));
        publish(F::And32, F::And64, kSrc, kResult);
        break;
    case NativeLockKind::Xor:
        emit(wide ? arm64::eor_x(kResult, kOld, kSrc)
                  : arm64::eor_w(kResult, kOld, kSrc));
        publish(F::Xor32, F::Xor64, kSrc, kResult);
        break;
    case NativeLockKind::Inc:
    case NativeLockKind::Dec: {
        const bool inc = kind == NativeLockKind::Inc;
        emit(wide ? arm64::add_x(kResult, kOld, kSrc)
                  : arm64::add_w(kResult, kOld, kSrc));
        if (wide) {
            // INC_RM64 / DEC_RM64's record: flag_src = CF << 32.
            emit(arm64::lsl_x_imm(kRs, REG_CALL_TMP, 32));
            emit_set_lazy_flags64(inc ? F::Inc64 : F::Dec64, kRs, kResult);
        } else {
            emit_set_lazy_flags_preserve_cf(inc ? F::Inc32 : F::Dec32, kOld,
                                            kResult, REG_CALL_TMP);
        }
        break;
    }
    case NativeLockKind::Xchg:
        if (wide) emit_store_gpr64(kOld, src->reg);
        else emit_store_gpr32(kOld, src->reg);
        break;
    case NativeLockKind::Cmpxchg: {
        // Flags are CMP accumulator, destination (Intel SDM Vol. 2A,
        // CMPXCHG): kResult still holds the accumulator.
        emit(wide ? arm64::sub_x(kRs, kResult, kOld)
                  : arm64::sub_w(kRs, kResult, kOld));
        publish(F::Sub32, F::Sub64, kOld, kRs);
        // The accumulator is written only on a mismatch, so a 32-bit match
        // leaves RAX[63:32] alone; a 64-bit match writes back the same value.
        if (wide) {
            emit_store_gpr64(kOld, static_cast<u8>(RegIndex::RAX));
        } else {
            const usize skip = m_code_buf.size();
            emit(0);  // CBZ Wdiff, after the write
            emit_store_gpr32(kOld, static_cast<u8>(RegIndex::RAX));
            m_code_buf[skip] = arm64::cbz_w(
                kRs, static_cast<i32>(m_code_buf.size() - skip) * 4);
        }
        // Host NZCV is the same CMP, for the CAS loop's Jcc; the helper arm
        // rebuilds it from RFLAGS (emit_nzcv_sub_from_rflags), and the caller
        // claims NzcvState::Sub after the join.
        emit(wide ? arm64::subs_x(31, kResult, kOld)
                  : arm64::subs_w(31, kResult, kOld));
        break;
    }
    }
    // The carry-source hint names how the NEXT instruction may read CF out of
    // this record without checking its kind byte. The helper path leaves a
    // materialised record instead, so no hint may survive the join.
    m_flags.pending_carry_source = CarrySource::Unknown;

    site.to_done = m_code_buf.size();
    emit(0);  // B done
    // A fallback writer is in flight: stand down (clear the slot) and take
    // the helper, whose LockPrefixGuard queues behind it.
    const usize standdown = m_code_buf.size();
    emit(arm64::stlr_w(31, 16));
    m_code_buf[site.to_standdown] = arm64::cbnz_w(
        17, static_cast<i32>(standdown - site.to_standdown) * 4);
    const usize slow = m_code_buf.size();
    m_code_buf[null_slot_branch] = arm64::cbz_x(
        16, static_cast<i32>(slow - null_slot_branch) * 4);
    for (const auto& b : site.to_slow) {
        const i32 disp = static_cast<i32>(slow - b.pos) * 4;
        m_code_buf[b.pos] =
            b.wide ? arm64::cbnz_x(b.reg, disp) : arm64::cbnz_w(b.reg, disp);
    }
    return true;
}

void JitCompiler::join_native_lock(NativeLockSite& site) {
    if (!site.active) return;
    m_code_buf[site.to_done] =
        arm64::b_imm(static_cast<i32>(m_code_buf.size() - site.to_done) * 4);
    site.active = false;
}

} // namespace vitreo::cpu::jit
