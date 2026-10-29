// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_lazy_condition.cpp
/// @brief Lazy-condition evaluation
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

// VITREO_JIT_DENSITY_BASELINE=1: bisection knob that restores the pre-density
// lowering -- the full lazy-condition walk for every Jcc/SETcc/CMOVcc and the
// full pin spill around the cond helper. Same binary, so an A/B isolates the
// codegen change from everything else.
static bool density_baseline_enabled() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("VITREO_JIT_DENSITY_BASELINE");
        return v != nullptr && *v != '\0' && *v != '0';
    }();
    return on;
}

void JitCompiler::emit_cond_helper(u8 cond) {
    note_lazy_record_read();
    emit(arm64::mov_x(0, REG_CPU_STATE));
    emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                            (JIT_CTX_OFF_COND_FNS + cond * 8u) >> 3));
    // jit_cond_* read flag_op / rflags / the overlays and may materialize
    // into rflags; they never read or write CpuState::gpr and never re-enter
    // guest code, so they meet the contract of the light marshal: the
    // callee-saved pins survive the call untouched and only the two
    // caller-saved pins need spilling. (See emit_memory_helper_blr.)
    if (density_baseline_enabled()) {
        emit_helper_blr();
    } else {
        emit_memory_helper_blr();
    }
    // A materialize rewrites the record's kind byte (to None), so the
    // compile-time prediction of it is stale from here.
    m_flags.helper_blr_this_insn = true;
}

void JitCompiler::emit_eval_from_rflags(u8 cond) {
    AttrScope attr_scope(*this, EmitAttribution::Flags);
    note_lazy_record_read();
    emit(arm64::ldr_w_uoff(2, REG_CPU_STATE, cpu_rflags_offset() >> 2));
    emit(arm64::movz_w(1, 1));
    auto bit1 = [&](u8 dst, u8 bit) {
        if (bit == 0) {
            emit(arm64::and_w(dst, 2, 1));
        } else {
            emit(arm64::lsr_w_imm(dst, 2, bit));
            emit(arm64::and_w(dst, dst, 1));
        }
    };
    switch (cond & 0xF) {
    case 0x0: bit1(0, 11); break;
    case 0x1: bit1(0, 11); emit(arm64::eor_w(0, 0, 1)); break;
    case 0x2: bit1(0, 0); break;
    case 0x3: bit1(0, 0); emit(arm64::eor_w(0, 0, 1)); break;
    case 0x4: bit1(0, 6); break;
    case 0x5: bit1(0, 6); emit(arm64::eor_w(0, 0, 1)); break;
    case 0x6:
        bit1(0, 0);
        bit1(3, 6);
        emit(arm64::orr_w(0, 0, 3));
        break;
    case 0x7:
        bit1(0, 0);
        bit1(3, 6);
        emit(arm64::orr_w(0, 0, 3));
        emit(arm64::eor_w(0, 0, 1));
        break;
    case 0x8: bit1(0, 7); break;
    case 0x9: bit1(0, 7); emit(arm64::eor_w(0, 0, 1)); break;
    case 0xA: bit1(0, 2); break;
    case 0xB: bit1(0, 2); emit(arm64::eor_w(0, 0, 1)); break;
    case 0xC:
        bit1(0, 7);
        bit1(3, 11);
        emit(arm64::eor_w(0, 0, 3));
        break;
    case 0xD:
        bit1(0, 7);
        bit1(3, 11);
        emit(arm64::eor_w(0, 0, 3));
        emit(arm64::eor_w(0, 0, 1));
        break;
    case 0xE:
        bit1(0, 7);
        bit1(3, 11);
        emit(arm64::eor_w(0, 0, 3));
        bit1(3, 6);
        emit(arm64::orr_w(0, 0, 3));
        break;
    default:
        bit1(0, 7);
        bit1(3, 11);
        emit(arm64::eor_w(0, 0, 3));
        bit1(3, 6);
        emit(arm64::orr_w(0, 0, 3));
        emit(arm64::eor_w(0, 0, 1));
        break;
    }
}

void JitCompiler::emit_cset_for_lazy_kind(NzcvState kind, u8 cond) {
    AttrScope attr_scope(*this, EmitAttribution::Flags);
    const int ac = fused_arm_cond(kind, cond);
    if (ac >= 0) {
        emit(arm64::cset_w(0, static_cast<u8>(ac)));
        return;
    }
    if (kind == NzcvState::And) {
        if (cond == 0x2) {
            emit(arm64::movz_w(0, 0));
            return;
        }
        if (cond == 0x3) {
            emit(arm64::movz_w(0, 1));
            return;
        }
        if (cond == 0x6) {
            emit(arm64::cset_w(0, arm64::EQ));
            return;
        }
        if (cond == 0x7) {
            emit(arm64::cset_w(0, arm64::NE));
            return;
        }
    }
    if (kind == NzcvState::Add && (cond == 0x6 || cond == 0x7)) {
        emit(arm64::cset_w(0, arm64::CS));
        emit(arm64::cset_w(1, arm64::EQ));
        emit(arm64::orr_w(0, 0, 1));
        if (cond == 0x7) {
            emit(arm64::movz_w(1, 1));
            emit(arm64::eor_w(0, 0, 1));
        }
        return;
    }
    emit_cond_helper(cond);
}

void JitCompiler::note_lazy_kind_publish(CpuState::FlagOp op) noexcept {
    m_flags.lazy_kind_published = static_cast<int>(op);
    if (m_flags.lazy_kind_publish_count < 255) ++m_flags.lazy_kind_publish_count;
}

// One arm of the lazy-condition walk, chosen at compile time.
//
// The generic walk below tests the record's kind byte against every kind it
// can evaluate inline (~120 ARM instructions per Jcc/SETcc/CMOVcc), even
// though the compiler usually just emitted the publisher and knows the byte.
// This emits only the arm the walk would select for the predicted kind, and
// keeps the walk's two runtime gates in front of it:
//
//   * the CF/OF overlay gate (CLC/STC/CMC/BT install an overlay over a live
//     record; the helper is the only reader that honours it), and
//   * the kind byte compare, so a prediction the tracker got wrong -- a
//     publisher it does not know about, a deferred record store -- takes the
//     helper instead of the wrong arm.
//
// Either gate failing lands in emit_cond_helper, the semantic reference, so
// the result is identical to the generic walk for every runtime byte value:
// arm(k) when byte == k, the helper otherwise. A predicted kind the walk has
// no inline arm for goes to the helper directly, which is where the walk
// ended for it anyway.
bool JitCompiler::emit_eval_lazy_condition_predicted(u8 cc) {
    if (m_flags.static_lazy_kind == kLazyKindUnknown) return false;
    if (density_baseline_enabled()) return false;
    if (!fits_ldr_w_offset(cpu_rflags_offset()) ||
        !fits_ldr_w_offset(flag_src_offset()) ||
        !fits_ldr_x_offset(flag_src_offset()) ||
        !fits_ldr_x_offset(flag_dst_offset())) {
        return false;
    }
    using FlagOp = CpuState::FlagOp;
    const auto kind = static_cast<FlagOp>(m_flags.static_lazy_kind);
    const int sub_ac = fused_arm_cond(NzcvState::Sub, cc, KindGuarded{});
    const int and_ac = fused_arm_cond(NzcvState::And, cc, KindGuarded{});
    const int add_ac = fused_arm_cond(NzcvState::Add, cc, KindGuarded{});
    const bool and_ok =
        and_ac >= 0 || cc == 0x2 || cc == 0x3 || cc == 0x6 || cc == 0x7;
    const bool add_ok = add_ac >= 0 || cc == 0x6 || cc == 0x7;
    const bool inc_ok = fused_arm_cond(NzcvState::IncDec, cc, KindGuarded{}) >= 0;
    // Narrow CMP: a zero-extended SUBS gives x86 ZF and CF but not SF/OF, so
    // Sub8 admits exactly the unsigned and zero conditions (cc 0x2-0x7).
    const int sub_narrow_ac =
        fused_arm_cond(NzcvState::Sub8, cc, KindGuarded{});

    // Same eligibility as the generic walk: an arm exists only when
    // emit_cset_for_lazy_kind can express `cc` for that NZCV shape.
    bool have_arm = false;
    switch (kind) {
    case FlagOp::None:
        have_arm = true;
        break;
    case FlagOp::Sub32: case FlagOp::Sub64:
    case FlagOp::Neg32: case FlagOp::Neg64:
        have_arm = sub_ac >= 0;
        break;
    case FlagOp::Sub8: case FlagOp::Sub16:
        have_arm = sub_narrow_ac >= 0;
        break;
    case FlagOp::And32: case FlagOp::Or32: case FlagOp::Xor32:
    case FlagOp::And64: case FlagOp::Or64: case FlagOp::Xor64:
    case FlagOp::And8:  case FlagOp::Or8:  case FlagOp::Xor8:
    case FlagOp::And16: case FlagOp::Or16: case FlagOp::Xor16:
        have_arm = and_ok;
        break;
    case FlagOp::Add32: case FlagOp::Add64:
        have_arm = add_ok;
        break;
    case FlagOp::Inc32: case FlagOp::Dec32:
    case FlagOp::Inc64: case FlagOp::Dec64:
        have_arm = inc_ok;
        break;
    default:
        break;
    }
    if (!have_arm) {
        emit_cond_helper(cc);
        return true;
    }

    emit(arm64::ldrb_uoff(1, REG_CPU_STATE, flag_op_offset()));
    static_assert(offsetof(CpuState, of_overlay) ==
                  offsetof(CpuState, cf_overlay) + 1);
    emit(arm64::ldrh_uoff(2, REG_CPU_STATE, cf_overlay_offset() >> 1));
    const usize overlay_cbnz = m_code_buf.size();
    emit(0);  // CBNZ W2, .helper
    usize kind_branch = 0;
    if (kind == FlagOp::None) {
        kind_branch = m_code_buf.size();
        emit(0);  // CBNZ W1, .helper
        emit_eval_from_rflags(cc);
    } else {
        emit(arm64::cmp_w_imm(1, static_cast<u32>(kind)));
        kind_branch = m_code_buf.size();
        emit(0);  // B.NE .helper
        const bool x = (kind == FlagOp::Sub64 || kind == FlagOp::Neg64 ||
                        kind == FlagOp::And64 || kind == FlagOp::Or64 ||
                        kind == FlagOp::Xor64 || kind == FlagOp::Add64 ||
                        kind == FlagOp::Inc64 || kind == FlagOp::Dec64);
        auto ld_src = [&](u8 r) {
            emit(x ? arm64::ldr_x_uoff(r, REG_CPU_STATE, flag_src_offset() >> 3)
                   : arm64::ldr_w_uoff(r, REG_CPU_STATE, flag_src_offset() >> 2));
        };
        auto ld_dst = [&](u8 r) {
            emit(x ? arm64::ldr_x_uoff(r, REG_CPU_STATE, flag_dst_offset() >> 3)
                   : arm64::ldr_w_uoff(r, REG_CPU_STATE, flag_dst_offset() >> 2));
        };
        switch (kind) {
        case FlagOp::Sub32: case FlagOp::Sub64:
            ld_src(2); ld_dst(3);
            emit(x ? arm64::add_x(4, 3, 2) : arm64::add_w(4, 3, 2));
            emit(x ? arm64::subs_x(31, 4, 2) : arm64::subs_w(31, 4, 2));
            emit_cset_for_lazy_kind(NzcvState::Sub, cc);
            break;
        case FlagOp::Sub8: case FlagOp::Sub16: {
            // A narrow CMP records `dst` already masked to its width and
            // `src` as the subtrahend, so the original left operand is
            // (dst + src) truncated to that width -- which is exactly what
            // CpuState's Sub8/Sub16 CF reconstruction computes. Truncating
            // before the SUBS is the whole point: without it the borrow case
            // carries +2^w into the recomputed operand and ARM C comes out
            // inverted.
            //
            // Reached because a dense switch's bounds check is `CMP BYTE
            // [mem], imm ; JA`, and Sub8 had no arm here at all: the walk
            // fell to `default`, declared no arm, and emitted a bare cond
            // helper. Profiling tools/stress bsz1 put 24.5% of ALL JIT time
            // on that one call.
            ld_src(2); ld_dst(3);
            emit(arm64::add_w(4, 3, 2));
            emit(kind == FlagOp::Sub8 ? arm64::uxtb_w(4, 4)
                                      : arm64::uxth_w(4, 4));
            emit(arm64::subs_w(31, 4, 2));
            emit_cset_for_lazy_kind(NzcvState::Sub8, cc);
            break;
        }
        case FlagOp::Neg32: case FlagOp::Neg64:
            ld_src(2);
            emit(x ? arm64::subs_x(31, 31, 2) : arm64::subs_w(31, 31, 2));
            emit_cset_for_lazy_kind(NzcvState::Sub, cc);
            break;
        case FlagOp::And32: case FlagOp::Or32: case FlagOp::Xor32:
        case FlagOp::And64: case FlagOp::Or64: case FlagOp::Xor64:
            ld_dst(3);
            emit(x ? arm64::ands_x(31, 3, 3) : arm64::ands_w(31, 3, 3));
            emit_cset_for_lazy_kind(NzcvState::And, cc);
            break;
        case FlagOp::And8: case FlagOp::Or8: case FlagOp::Xor8:
            ld_dst(3);
            emit(arm64::sxtb_w(3, 3));
            emit(arm64::ands_w(31, 3, 3));
            emit_cset_for_lazy_kind(NzcvState::And, cc);
            break;
        case FlagOp::And16: case FlagOp::Or16: case FlagOp::Xor16:
            ld_dst(3);
            emit(arm64::sxth_w(3, 3));
            emit(arm64::ands_w(31, 3, 3));
            emit_cset_for_lazy_kind(NzcvState::And, cc);
            break;
        case FlagOp::Add32: case FlagOp::Add64:
            ld_src(2); ld_dst(3);
            emit(x ? arm64::sub_x(4, 3, 2) : arm64::sub_w(4, 3, 2));
            emit(x ? arm64::adds_x(31, 4, 2) : arm64::adds_w(31, 4, 2));
            emit_cset_for_lazy_kind(NzcvState::Add, cc);
            break;
        case FlagOp::Inc32: case FlagOp::Inc64:
            ld_dst(3);
            emit(x ? arm64::sub_x_imm(4, 3, 1) : arm64::sub_w_imm(4, 3, 1));
            emit(x ? arm64::adds_x_imm(31, 4, 1) : arm64::adds_w_imm(31, 4, 1));
            emit_cset_for_lazy_kind(NzcvState::IncDec, cc);
            break;
        case FlagOp::Dec32: case FlagOp::Dec64:
            ld_dst(3);
            emit(x ? arm64::add_x_imm(4, 3, 1) : arm64::add_w_imm(4, 3, 1));
            emit(x ? arm64::subs_x_imm(31, 4, 1) : arm64::subs_w_imm(31, 4, 1));
            emit_cset_for_lazy_kind(NzcvState::IncDec, cc);
            break;
        default:
            break;
        }
    }
    // The helper -- the semantic reference when the record's kind is not the
    // predicted one or an overlay is set -- runs out of line, rejoining with
    // the condition in W0 exactly as the predicted arm leaves it.
    const bool none_kind = kind == FlagOp::None;
    std::vector<ColdArmEntry> entries;
    entries.push_back({overlay_cbnz,
                       [](i32 disp) { return arm64::cbnz_w(2, disp); }});
    entries.push_back({kind_branch, [none_kind](i32 disp) {
                           return none_kind ? arm64::cbnz_w(1, disp)
                                            : arm64::b_cond(arm64::NE, disp);
                       }});
    defer_cold_arm(std::move(entries), [this, cc]() { emit_cond_helper(cc); },
                   /*semantic_helper=*/false);
    return true;
}

void JitCompiler::emit_eval_lazy_condition(u8 cond) {
    AttrScope attr_scope(*this, EmitAttribution::Flags);
    note_lazy_record_read();
    if (emit_eval_lazy_condition_predicted(static_cast<u8>(cond & 0xF))) {
        return;
    }
    static_assert(static_cast<u8>(CpuState::FlagOp::None) == 0);
    static_assert(static_cast<u8>(CpuState::FlagOp::Add32) == 3);
    static_assert(static_cast<u8>(CpuState::FlagOp::Add64) == 4);
    static_assert(static_cast<u8>(CpuState::FlagOp::Sub32) == 7);
    static_assert(static_cast<u8>(CpuState::FlagOp::Sub64) == 8);
    static_assert(static_cast<u8>(CpuState::FlagOp::And32) == 11);
    static_assert(static_cast<u8>(CpuState::FlagOp::And64) == 12);
    static_assert(static_cast<u8>(CpuState::FlagOp::Or32) == 15);
    static_assert(static_cast<u8>(CpuState::FlagOp::Or64) == 16);
    static_assert(static_cast<u8>(CpuState::FlagOp::Xor32) == 19);
    static_assert(static_cast<u8>(CpuState::FlagOp::Xor64) == 20);
    static_assert(static_cast<u8>(CpuState::FlagOp::Inc32) == 23);
    static_assert(static_cast<u8>(CpuState::FlagOp::Inc64) == 24);
    static_assert(static_cast<u8>(CpuState::FlagOp::Dec32) == 27);
    static_assert(static_cast<u8>(CpuState::FlagOp::Dec64) == 28);
    static_assert(static_cast<u8>(CpuState::FlagOp::Neg32) == 46);
    static_assert(static_cast<u8>(CpuState::FlagOp::Neg64) == 47);

    const u8 cc = static_cast<u8>(cond & 0xF);
    const int sub_ac = fused_arm_cond(NzcvState::Sub, cc, KindGuarded{});
    const int and_ac = fused_arm_cond(NzcvState::And, cc, KindGuarded{});
    const int add_ac = fused_arm_cond(NzcvState::Add, cc, KindGuarded{});
    const bool and_ok =
        and_ac >= 0 || cc == 0x2 || cc == 0x3 || cc == 0x6 || cc == 0x7;
    const bool add_ok = add_ac >= 0 || cc == 0x6 || cc == 0x7;
    const bool inc_ok = fused_arm_cond(NzcvState::IncDec, cc, KindGuarded{}) >= 0;

    if (!fits_ldr_w_offset(cpu_rflags_offset()) ||
        !fits_ldr_w_offset(flag_src_offset()) ||
        !fits_ldr_x_offset(flag_src_offset()) ||
        !fits_ldr_x_offset(flag_dst_offset())) {
        emit_cond_helper(cc);
        return;
    }

    auto patch_b = [&](usize pos, usize dest) {
        m_code_buf[pos] = arm64::b_imm(
            static_cast<i32>(dest - pos) * 4);
    };
    auto patch_bne = [&](usize pos, usize dest) {
        m_code_buf[pos] = arm64::b_cond(
            arm64::NE, static_cast<i32>(dest - pos) * 4);
    };
    auto patch_beq = [&](usize pos, usize dest) {
        m_code_buf[pos] = arm64::b_cond(
            arm64::EQ, static_cast<i32>(dest - pos) * 4);
    };
    auto patch_cbnz = [&](usize pos, u8 reg, usize dest) {
        m_code_buf[pos] = arm64::cbnz_w(
            reg, static_cast<i32>(dest - pos) * 4);
    };

    std::vector<usize> to_done;
    usize overlay_cbnz = 0;
    usize not_sub32 = 0;
    usize not_sub64 = 0;
    usize and32_eq = 0;
    usize or32_eq = 0;
    usize xor32_ne = 0;
    usize and64_eq = 0;
    usize or64_eq = 0;
    usize xor64_ne = 0;
    usize not_add32 = 0;
    usize not_add64 = 0;
    usize none_cbnz = 0;

    emit(arm64::ldrb_uoff(1, REG_CPU_STATE, flag_op_offset()));

    // The CF/OF overlay gate must come BEFORE every lazy fast path.
    //
    // It used to sit after the Sub32/And32/Add32 arms, on the reasoning that
    // "CMP/TEST clear the overlay, so a live full-flag FlagOp implies no
    // overlay". That invariant does not hold: CLC/STC/CMC and the BT/ROR
    // family deliberately install an overlay while LEAVING the pending ALU
    // FlagOp live, which is the whole point of the overlay — it lets those
    // partial writers answer CF without forcing a full materialize.
    //
    // With the gate placed late, `ADD; CLC; JB` reached the Add32 arm and
    // recomputed CF from the ADD, branching on the carry the CLC had just
    // cleared. Anything that writes only CF or only OF over a live FlagOp has
    // the same shape: `SUB; CLC; JB`, `AND; STC; JB`, `ADD; BT; JB`.
    //
    // Both overlay bytes are adjacent and aligned, so one LDRH tests the pair.
    // The common no-overlay path is now one load plus a reliably not-taken
    // branch instead of two loads, an OR, and the branch.
    static_assert(offsetof(CpuState, of_overlay) ==
                  offsetof(CpuState, cf_overlay) + 1);
    static_assert((offsetof(CpuState, cf_overlay) & 1u) == 0);
    emit(arm64::ldrh_uoff(2, REG_CPU_STATE, cf_overlay_offset() >> 1));
    overlay_cbnz = m_code_buf.size();
    emit(0);  // CBNZ W2, .helper

    if (sub_ac >= 0) {
        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::Sub32)));
        not_sub32 = m_code_buf.size();
        emit(0);
        emit(arm64::ldr_w_uoff(2, REG_CPU_STATE, flag_src_offset() >> 2));
        emit(arm64::ldr_w_uoff(3, REG_CPU_STATE, flag_dst_offset() >> 2));
        emit(arm64::add_w(4, 3, 2));
        emit(arm64::subs_w(31, 4, 2));
        emit_cset_for_lazy_kind(NzcvState::Sub, cc);
        to_done.push_back(m_code_buf.size());
        emit(0);
        patch_bne(not_sub32, m_code_buf.size());

        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::Sub64)));
        not_sub64 = m_code_buf.size();
        emit(0);
        emit(arm64::ldr_x_uoff(2, REG_CPU_STATE, flag_src_offset() >> 3));
        emit(arm64::ldr_x_uoff(3, REG_CPU_STATE, flag_dst_offset() >> 3));
        emit(arm64::add_x(4, 3, 2));
        emit(arm64::subs_x(31, 4, 2));
        emit_cset_for_lazy_kind(NzcvState::Sub, cc);
        to_done.push_back(m_code_buf.size());
        emit(0);
        patch_bne(not_sub64, m_code_buf.size());
    }

    if (and_ok) {
        // 32-bit TEST/AND first: the load walk is Jcc-first from And32/Sub32.
        // 8/16-bit were ahead of that and cost eight failed CMPs per Jcc.
        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::And32)));
        and32_eq = m_code_buf.size();
        emit(0);
        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::Or32)));
        or32_eq = m_code_buf.size();
        emit(0);
        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::Xor32)));
        xor32_ne = m_code_buf.size();
        emit(0);
        const usize do_and32 = m_code_buf.size();
        patch_beq(and32_eq, do_and32);
        patch_beq(or32_eq, do_and32);
        emit(arm64::ldr_w_uoff(3, REG_CPU_STATE, flag_dst_offset() >> 2));
        emit(arm64::ands_w(31, 3, 3));
        emit_cset_for_lazy_kind(NzcvState::And, cc);
        to_done.push_back(m_code_buf.size());
        emit(0);
        patch_bne(xor32_ne, m_code_buf.size());

        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::And64)));
        and64_eq = m_code_buf.size();
        emit(0);
        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::Or64)));
        or64_eq = m_code_buf.size();
        emit(0);
        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::Xor64)));
        xor64_ne = m_code_buf.size();
        emit(0);
        const usize do_and64 = m_code_buf.size();
        patch_beq(and64_eq, do_and64);
        patch_beq(or64_eq, do_and64);
        emit(arm64::ldr_x_uoff(3, REG_CPU_STATE, flag_dst_offset() >> 3));
        emit(arm64::ands_x(31, 3, 3));
        emit_cset_for_lazy_kind(NzcvState::And, cc);
        to_done.push_back(m_code_buf.size());
        emit(0);
        patch_bne(xor64_ne, m_code_buf.size());

        // 8/16-bit TEST/AND/OR/XOR. SXTB/SXTH so ANDS N is x86 SF.
        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::And8)));
        const usize and8_eq = m_code_buf.size();
        emit(0);
        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::Or8)));
        const usize or8_eq = m_code_buf.size();
        emit(0);
        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::Xor8)));
        const usize xor8_ne = m_code_buf.size();
        emit(0);
        const usize do_and8 = m_code_buf.size();
        patch_beq(and8_eq, do_and8);
        patch_beq(or8_eq, do_and8);
        emit(arm64::ldr_w_uoff(3, REG_CPU_STATE, flag_dst_offset() >> 2));
        emit(arm64::sxtb_w(3, 3));
        emit(arm64::ands_w(31, 3, 3));
        emit_cset_for_lazy_kind(NzcvState::And, cc);
        to_done.push_back(m_code_buf.size());
        emit(0);
        patch_bne(xor8_ne, m_code_buf.size());

        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::And16)));
        const usize and16_eq = m_code_buf.size();
        emit(0);
        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::Or16)));
        const usize or16_eq = m_code_buf.size();
        emit(0);
        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::Xor16)));
        const usize xor16_ne = m_code_buf.size();
        emit(0);
        const usize do_and16 = m_code_buf.size();
        patch_beq(and16_eq, do_and16);
        patch_beq(or16_eq, do_and16);
        emit(arm64::ldr_w_uoff(3, REG_CPU_STATE, flag_dst_offset() >> 2));
        emit(arm64::sxth_w(3, 3));
        emit(arm64::ands_w(31, 3, 3));
        emit_cset_for_lazy_kind(NzcvState::And, cc);
        to_done.push_back(m_code_buf.size());
        emit(0);
        patch_bne(xor16_ne, m_code_buf.size());
    }

    if (add_ok) {
        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::Add32)));
        not_add32 = m_code_buf.size();
        emit(0);
        emit(arm64::ldr_w_uoff(2, REG_CPU_STATE, flag_src_offset() >> 2));
        emit(arm64::ldr_w_uoff(3, REG_CPU_STATE, flag_dst_offset() >> 2));
        emit(arm64::sub_w(4, 3, 2));
        emit(arm64::adds_w(31, 4, 2));
        emit_cset_for_lazy_kind(NzcvState::Add, cc);
        to_done.push_back(m_code_buf.size());
        emit(0);
        patch_bne(not_add32, m_code_buf.size());

        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::Add64)));
        not_add64 = m_code_buf.size();
        emit(0);
        emit(arm64::ldr_x_uoff(2, REG_CPU_STATE, flag_src_offset() >> 3));
        emit(arm64::ldr_x_uoff(3, REG_CPU_STATE, flag_dst_offset() >> 3));
        emit(arm64::sub_x(4, 3, 2));
        emit(arm64::adds_x(31, 4, 2));
        emit_cset_for_lazy_kind(NzcvState::Add, cc);
        to_done.push_back(m_code_buf.size());
        emit(0);
        patch_bne(not_add64, m_code_buf.size());
    }

    if (inc_ok) {
        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::Inc32)));
        const usize not_inc32 = m_code_buf.size();
        emit(0);
        emit(arm64::ldr_w_uoff(3, REG_CPU_STATE, flag_dst_offset() >> 2));
        emit(arm64::sub_w_imm(4, 3, 1));
        emit(arm64::adds_w_imm(31, 4, 1));
        emit_cset_for_lazy_kind(NzcvState::IncDec, cc);
        to_done.push_back(m_code_buf.size());
        emit(0);
        patch_bne(not_inc32, m_code_buf.size());

        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::Dec32)));
        const usize not_dec32 = m_code_buf.size();
        emit(0);
        emit(arm64::ldr_w_uoff(3, REG_CPU_STATE, flag_dst_offset() >> 2));
        emit(arm64::add_w_imm(4, 3, 1));
        emit(arm64::subs_w_imm(31, 4, 1));
        emit_cset_for_lazy_kind(NzcvState::IncDec, cc);
        to_done.push_back(m_code_buf.size());
        emit(0);
        patch_bne(not_dec32, m_code_buf.size());

        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::Inc64)));
        const usize not_inc64 = m_code_buf.size();
        emit(0);
        emit(arm64::ldr_x_uoff(3, REG_CPU_STATE, flag_dst_offset() >> 3));
        emit(arm64::sub_x_imm(4, 3, 1));
        emit(arm64::adds_x_imm(31, 4, 1));
        emit_cset_for_lazy_kind(NzcvState::IncDec, cc);
        to_done.push_back(m_code_buf.size());
        emit(0);
        patch_bne(not_inc64, m_code_buf.size());

        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::Dec64)));
        const usize not_dec64 = m_code_buf.size();
        emit(0);
        emit(arm64::ldr_x_uoff(3, REG_CPU_STATE, flag_dst_offset() >> 3));
        emit(arm64::add_x_imm(4, 3, 1));
        emit(arm64::subs_x_imm(31, 4, 1));
        emit_cset_for_lazy_kind(NzcvState::IncDec, cc);
        to_done.push_back(m_code_buf.size());
        emit(0);
        patch_bne(not_dec64, m_code_buf.size());
    }

    if (sub_ac >= 0) {
        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::Neg32)));
        const usize not_neg32 = m_code_buf.size();
        emit(0);
        emit(arm64::ldr_w_uoff(2, REG_CPU_STATE, flag_src_offset() >> 2));
        emit(arm64::subs_w(31, 31, 2));
        emit_cset_for_lazy_kind(NzcvState::Sub, cc);
        to_done.push_back(m_code_buf.size());
        emit(0);
        patch_bne(not_neg32, m_code_buf.size());

        emit(arm64::cmp_w_imm(1, static_cast<u32>(CpuState::FlagOp::Neg64)));
        const usize not_neg64 = m_code_buf.size();
        emit(0);
        emit(arm64::ldr_x_uoff(2, REG_CPU_STATE, flag_src_offset() >> 3));
        emit(arm64::subs_x(31, 31, 2));
        emit_cset_for_lazy_kind(NzcvState::Sub, cc);
        to_done.push_back(m_code_buf.size());
        emit(0);
        patch_bne(not_neg64, m_code_buf.size());
    }

    none_cbnz = m_code_buf.size();
    emit(0);  // CBNZ W1, .helper
    emit_eval_from_rflags(cc);
    to_done.push_back(m_code_buf.size());
    emit(0);

    const usize helper = m_code_buf.size();
    patch_cbnz(overlay_cbnz, 2, helper);
    patch_cbnz(none_cbnz, 1, helper);
    emit_cond_helper(cc);

    const usize done = m_code_buf.size();
    for (usize pos : to_done) {
        patch_b(pos, done);
    }
}

NzcvState JitCompiler::emit_eval_jcc(u8 cond, int fused_arm) {
    AttrScope attr_scope(*this, EmitAttribution::Flags);
    if (fused_arm >= 0) {
        emit(arm64::cset_w(0, static_cast<u8>(fused_arm)));
        m_flags.nzcv = m_flags.incoming_nzcv;
        return m_flags.incoming_nzcv;
    }
    if (can_try_carried_jcc(cond)) {
        const int ac = fused_arm_cond(NzcvState::Sub, cond, KindGuarded{});
        emit(arm64::ldrb_uoff(1, REG_CPU_STATE, nzcv_kind_offset()));
        const usize cbz = m_code_buf.size();
        emit(0);  // CBZ W1, .lazy
        emit(arm64::cset_w(0, static_cast<u8>(ac)));
        const usize skip = m_code_buf.size();
        emit(0);  // B .have
        const usize lazy = m_code_buf.size();
        m_code_buf[cbz] = arm64::cbz_w(
            1, static_cast<i32>(lazy - cbz) * 4);
        emit_eval_lazy_condition(cond);
        emit_store_nzcv_kind(NzcvState::None);
        const usize have = m_code_buf.size();
        m_code_buf[skip] = arm64::b_imm(
            static_cast<i32>(have - skip) * 4);
        m_flags.nzcv = NzcvState::Carried;
        return NzcvState::Carried;
    }
    emit_eval_lazy_condition(cond);
    return NzcvState::None;
}

} // namespace vitreo::cpu::jit
