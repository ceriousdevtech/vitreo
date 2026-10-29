// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_cold_exits.cpp
/// @brief Deferred and cold exit stubs
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

void JitCompiler::queue_mid_block_jcc_exit(int fused_arm, u8 x86_cc,
                                           u64 target) {
    ColdJccExit exit;
    exit.target = target;

    const bool try_carried = fused_arm < 0 && can_try_carried_jcc(x86_cc);
    const int carried_arm =
        try_carried ? fused_arm_cond(NzcvState::Sub, x86_cc, KindGuarded{}) : -1;

    if (fused_arm >= 0) {
        // Fused path emits nothing before the B.cond, so the publisher's
        // operand registers are still intact when the stub is entered. Move
        // its skipped store there instead of into the hot fall-through body.
        if (m_incoming_stub_store.valid) {
            exit.stub_store = true;
            exit.stub_op = m_incoming_stub_store.op;
            exit.stub_src = m_incoming_stub_store.src;
            exit.stub_dst = m_incoming_stub_store.dst;
            exit.stub_wide = m_incoming_stub_store.wide;
        }
        exit.branch_pos = m_code_buf.size();
        exit.arm_cond = static_cast<u8>(fused_arm);
        exit.fused = true;
        exit.carry = m_flags.incoming_nzcv;
        emit(0);  // B.cond taken-stub
        m_flags.nzcv = m_flags.incoming_nzcv;
        diagnostics::perf_note_jcc_emit_fused();
    } else if (try_carried && carried_arm >= 0) {
        // The carried and helper paths evaluate the condition before
        // branching, clobbering the publisher's operand registers. There is
        // nothing left for a stub to store, so pay for it here — correct on
        // both edges, just without the hot-path saving.
        if (m_incoming_stub_store.valid) {
            emit_lazy_flag_store_raw(m_incoming_stub_store.op,
                                     m_incoming_stub_store.src,
                                     m_incoming_stub_store.dst,
                                     m_incoming_stub_store.wide);
            note_elided_record_republished();
        }
        emit(arm64::ldrb_uoff(1, REG_CPU_STATE, nzcv_kind_offset()));
        const usize cbz_pos = m_code_buf.size();
        emit(0);  // CBZ W1, .slow
        exit.branch_pos = m_code_buf.size();
        exit.arm_cond = static_cast<u8>(carried_arm);
        exit.fused = true;
        exit.carry = NzcvState::Carried;
        emit(0);  // B.cond taken-stub
        m_flags.nzcv = NzcvState::Carried;
        const usize skip_pos = m_code_buf.size();
        emit(0);  // B .continue
        const usize slow = m_code_buf.size();
        m_code_buf[cbz_pos] = arm64::cbz_w(
            1, static_cast<i32>(slow - cbz_pos) * 4);
        emit_eval_lazy_condition(x86_cc);
        exit.helper_branch_pos = m_code_buf.size();
        emit(0);  // CBNZ W0, taken-stub
        const usize cont = m_code_buf.size();
        m_code_buf[skip_pos] = arm64::b_imm(
            static_cast<i32>(cont - skip_pos) * 4);
        diagnostics::perf_note_jcc_emit_carried();
    } else {
        if (m_incoming_stub_store.valid) {
            emit_lazy_flag_store_raw(m_incoming_stub_store.op,
                                     m_incoming_stub_store.src,
                                     m_incoming_stub_store.dst,
                                     m_incoming_stub_store.wide);
            note_elided_record_republished();
        }
        emit_eval_lazy_condition(x86_cc);
        exit.branch_pos = m_code_buf.size();
        exit.fused = false;
        diagnostics::perf_note_jcc_emit_helper();
        exit.carry = NzcvState::None;
        emit(0);  // CBNZ W0, taken-stub
    }
    m_cold_jcc_exits.push_back(exit);
}

bool JitCompiler::is_forward_in_block_target(
    GuestAddr target, GuestAddr next_rip) const noexcept {
    if (!m_block_is_64bit || target <= next_rip) return false;
    for (u32 i = 0; i < m_block_insn_rip_count; ++i) {
        if (m_block_insn_rips[i] == target) return true;
    }
    return false;
}

bool JitCompiler::find_in_block_loop_pos(u64 target, u32& arm_pos) const {
    if (target == m_block_start_addr) {
        arm_pos = m_body_start_pos;
        return true;
    }
    // Arbitrary x64 loop headers need a canonical NZCV/carry join before a
    // direct backedge can safely enter them. Until that metadata exists, keep
    // long-mode mid-block targets on the dispatcher path.
    if (m_block_is_64bit) return false;
    for (u32 i = 0; i < m_insn_pos_count; ++i) {
        if (m_insn_rips[i] == target) {
            arm_pos = m_insn_arm_pos[i];
            return true;
        }
    }
    return false;
}

void JitCompiler::queue_forward_in_block_jcc(u8 arm_cond,
                                             GuestAddr target) {
    const usize branch_pos = m_code_buf.size();
    emit(0); // B.cond target, patched at the target instruction boundary.
    m_forward_jcc_fixups.push_back(ForwardJccFixup{
        branch_pos, target, arm_cond, m_flags.incoming_nzcv,
        m_flags.incoming_carry_source});
}

void JitCompiler::emit_flush_raise_exit_stub() {
    AttrScope attr_scope(*this, EmitAttribution::Raise);
    const bool have_restore = std::any_of(
        m_raise_restore_nzcv_branch_pos.begin(),
        m_raise_restore_nzcv_branch_pos.end(),
        [](const auto& branches) { return !branches.empty(); });
    // A lazy site's stub is emitted later and branches to the entry its
    // kind names (cold_lazy_write_records), so that entry exists even when
    // no branch in the block targets it yet.
    const bool have_lazy =
        m_raise_lazy_plain ||
        std::any_of(m_raise_lazy_restore.begin(), m_raise_lazy_restore.end(),
                    [](bool b) { return b; });
    if (m_raise_branch_pos.empty() && !have_restore &&
        m_direct_raise_branch_pos.empty() && !have_lazy) return;

    std::vector<usize> restore_to_shared;
    for (u8 kind = 0; kind < m_raise_restore_nzcv_branch_pos.size(); ++kind) {
        auto& branches = m_raise_restore_nzcv_branch_pos[kind];
        if (branches.empty() && !m_raise_lazy_restore[kind]) continue;
        const usize restore_dest = m_code_buf.size();
        m_raise_entry_restore_pos[kind] = restore_dest;
        for (const usize pos : branches) {
            m_code_buf[pos] = arm64::cbnz_w(
                16, cold_branch_disp(pos, restore_dest, 19,
                                     "raise NZCV restore exit"));
        }
        branches.clear();
        emit(arm64::ldr_x_uoff(11, 31, 1));
        emit(arm64::msr_nzcv(11));
        if (kind != 0) {
            emit(arm64::movz_w(4, kind));
            emit(arm64::strb_uoff(4, REG_CPU_STATE, nzcv_kind_offset()));
        }
        restore_to_shared.push_back(m_code_buf.size());
        emit(0); // B .shared_raise
    }
    const usize dest = m_code_buf.size();
    m_raise_entry_plain_pos = dest;
    for (const usize pos : m_raise_branch_pos) {
        m_code_buf[pos] = arm64::cbnz_w(
            16, cold_branch_disp(pos, dest, 19, "raise exit"));
    }
    m_raise_branch_pos.clear();
    for (const usize pos : restore_to_shared) {
        m_code_buf[pos] = arm64::b_imm(cold_branch_disp(
            pos, dest, 26, "raise NZCV shared exit"));
    }
    for (const usize pos : m_direct_raise_branch_pos) {
        m_code_buf[pos] = arm64::b_imm(
            cold_branch_disp(pos, dest, 26, "direct raise exit"));
    }
    m_direct_raise_branch_pos.clear();
    // Body, emitted exactly once per block. RIP is already correct in CpuState
    // (the originating exit or checked-memory cold stub published it), and
    // any site whose authoritative NZCV was spilled joined through the
    // restore prefix above. store_pins stays here so the process tail can
    // ignore host register assignment.
    // Exception dispatch observes integer EFLAGS at the faulting instruction.
    // Preserve a carried lazy NZCV record before returning to the dispatcher.
    // A standard-pin x64 block stores its pins in the process raise tail.
    const bool raise_tail = dispatch_tail_applies() &&
                            materialize_dispatch_tail(kTailRaise) != nullptr;
    if (!raise_tail) emit_store_pins();
    // A block that elided flag records under recovery plans names itself, so
    // execute() can rebuild the record a SOFTWARE raise would otherwise leave
    // stale (the hardware-fault handler rebuilds its own). X17 is free here.
    // Likewise a block with an instruction that moves RSP before its access
    // (JitFlagRecovery::rsp_undo): execute() undoes the move.
    if (std::any_of(m_stencil_flag_recovery.begin(),
                    m_stencil_flag_recovery.end(),
                    [](const StencilFlagRecovery& p) { return p.active(); }) ||
        std::any_of(m_insn_rsp_undo.begin(),
                    m_insn_rsp_undo.begin() + m_insn_pos_count,
                    [](i8 u) { return u != 0; })) {
        emit(arm64::adr_x(17, 0));
        emit(arm64::str_x_uoff(17, REG_JIT_CTX,
                               JIT_CTX_OFF_RAISE_BLOCK_PC >> 3));
    }
    if (raise_tail && emit_dispatch_tail(kTailRaise)) return;
    if (m_block_is_64bit && m_cfg.linear64) {
        if (materialize_process_raise_exit() != nullptr) {
            emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_RAISE_EXIT >> 3));
            emit(0xD61F0200u);  // BR X16
            return;
        }
    }
    emit_persist_live_nzcv();
    emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                           jit_iters_offset() >> 2));
    emit_exit_reason(JitExitReason::EpilogueRaise);  // DIAGNOSTIC
    if (m_cfg.native_callret_enabled && m_block_is_64bit) {
        emit_discard_native_call_frames();
    }
    emit(0xF84107FEu);  // LDR X30, [SP], #16
    emit(arm64::movz_w(0, static_cast<u16>(JitExecResult::Continue)));
    emit(arm64::ret());
}

void JitCompiler::emit_flush_cold_ud2_stub() {
    AttrScope attr_scope(*this, EmitAttribution::Raise);
    if (m_cold_ud2_exits.empty()) return;

    // Keep each guarded island to one ARM word in the hot instruction stream.
    // Site-specific RIP materialization lives in cold prefixes, each of which
    // then joins the one shared flag/exception body below.
    std::vector<usize> shared_body_branches;
    shared_body_branches.reserve(m_cold_ud2_exits.size());
    for (const ColdUd2Exit& exit : m_cold_ud2_exits) {
        const usize prefix = m_code_buf.size();
        m_code_buf[exit.branch_pos] = arm64::b_imm(cold_branch_disp(
            exit.branch_pos, prefix, 26, "native UD2 prefix"));
        emit_update_rip(exit.rip);
        shared_body_branches.push_back(m_code_buf.size());
        emit(0);  // B .shared_ud2 -- patched after all site prefixes
    }
    m_cold_ud2_exits.clear();

    const usize dest = m_code_buf.size();
    for (const usize pos : shared_body_branches) {
        m_code_buf[pos] = arm64::b_imm(
            cold_branch_disp(pos, dest, 26, "shared native UD2 body"));
    }

    // The x64 exception dispatcher snapshots flags.rflags directly.  UD2 is
    // flag-neutral, so canonicalize the predecessor's lazy flag record before
    // publishing the exception.  emit_helper_blr() is required: it preserves
    // caller-saved pinned guest registers around the AAPCS64 helper call.
    emit(arm64::mov_x(0, REG_CPU_STATE));
    emit(arm64::ldr_x_uoff(
        16, REG_JIT_CTX,
        static_cast<u32>(offsetof(JitContext, materialize_flags)) >> 3));
    emit_helper_blr();

    // Match jit_delegate_apply_result's first-exception-wins rule.  A pending
    // exception still exits through the common body without being replaced.
    emit_exit_if_pending_raise();

    using Pending = CpuState::PendingRaiseException;
    constexpr u32 base = static_cast<u32>(offsetof(CpuState, pending_raise));
    constexpr u32 active_off = base + offsetof(Pending, active);
    constexpr u32 helper_off = base + offsetof(Pending, from_helper);
    constexpr u32 code_off = base + offsetof(Pending, code);
    constexpr u32 flags_off = base + offsetof(Pending, flags);
    constexpr u32 nargs_off = base + offsetof(Pending, num_args);
    static_assert(active_off < 4096 && helper_off < 4096);
    static_assert((code_off & 3) == 0 && (code_off >> 2) < 4096);
    static_assert((flags_off & 3) == 0 && (flags_off >> 2) < 4096);
    static_assert((nargs_off & 3) == 0 && (nargs_off >> 2) < 4096);

    emit_mov_imm32(REG_SCRATCH3, 0xC000001Du);  // STATUS_ILLEGAL_INSTRUCTION
    emit(arm64::str_w_uoff(REG_SCRATCH3, REG_CPU_STATE, code_off >> 2));
    emit(arm64::str_w_uoff(31, REG_CPU_STATE, flags_off >> 2));
    emit(arm64::str_w_uoff(31, REG_CPU_STATE, nargs_off >> 2));
    emit(arm64::movz_w(REG_SCRATCH3, 1));
    emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, helper_off));
    // Publish active last so no observer can see a partially initialized raise.
    emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, active_off));

    m_direct_raise_branch_pos.push_back(m_code_buf.size());
    emit(0);  // B .raise -- patched by emit_flush_raise_exit_stub()
}

i32 JitCompiler::cold_branch_disp(usize from, usize to, unsigned imm_bits,
                                  const char* what) {
    bool exceeded = false;
    const i32 disp = deferred_branch_disp(from, to, imm_bits, exceeded);
    const i64 words = static_cast<i64>(to) - static_cast<i64>(from);
    // TEST-ONLY (VITREO_JIT_TEST_BRANCH_REACH_WORDS, read per compile): a
    // smaller reach, so a test can drive an ordinary block into the decline.
    if (!exceeded && m_test_reach_words > 0 &&
        (words >= m_test_reach_words || words < -m_test_reach_words)) {
        exceeded = true;
    }
    if (!exceeded) return disp;
    // Shared code materialized mid-compile has no block to decline, and a
    // test run asks for any decline to stop the process so CI sees it.
    if (m_branch_reach_fatal || m_test_reach_fatal) {
        std::fprintf(stderr,
                     "[jit] FATAL: %s displacement %lld words exceeds the "
                     "imm%u branch field%s\n",
                     what, static_cast<long long>(words), imm_bits,
                     m_branch_reach_fatal ? " (shared helper)"
                                          : " (VITREO_JIT_TEST_BRANCH_REACH_FATAL)");
        std::abort();
    }
    // A block whose code outgrows one of its branches is a size the JIT does
    // not lay out, not a guest error: finalize_code declines it and the
    // interpreter runs it. The first branch that overflowed names the cause.
    if (!m_branch_reach_exceeded) {
        m_reach_what = what;
        m_reach_words = words;
        m_reach_bits = imm_bits;
    }
    m_branch_reach_exceeded = true;
    return 0;
}

void JitCompiler::emit_flush_stack_proof_exit_stub() {
    AttrScope attr_scope(*this, EmitAttribution::Raise);
    if (m_stack_proof_branch_pos.empty() &&
        m_stack_proof_mixed_branch_pos.empty()) return;
    const usize dest = m_code_buf.size();
    for (const usize pos : m_stack_proof_branch_pos) {
        m_code_buf[pos] = arm64::cbnz_x(
            8, cold_branch_disp(pos, dest, 19, "stack proof exit"));
    }
    // The mixed-page test sits before instruction zero and this stub after
    // the whole hot body, so the distance is the block's size: TBNZ's imm14
    // reaches only 8191 words, which a 192-instruction block can exceed. Out
    // of its reach, the same word becomes CBNZ X3 (imm19), which deopts on ANY
    // check-byte bit instead of the mixed-protection bit alone. That is always
    // correct -- the deopt runs the whole block in the interpreter, which
    // applies exact guest permissions -- merely conservative for a stack page
    // that also holds code or a write watch. Hot-path length is unchanged.
    for (const usize pos : m_stack_proof_mixed_branch_pos) {
        m_code_buf[pos] = arm64::tbnz_or_cbnz_x(
            3, kLinearCheckMixedProtBit,
            cold_branch_disp(pos, dest, 19, "stack proof mixed exit"));
    }
    m_stack_proof_branch_pos.clear();
    m_stack_proof_mixed_branch_pos.clear();

    // The proof is before the first guest instruction and is NZCV-neutral.
    // Preserve an incoming linked predecessor's lazy flag carrier, publish
    // the rejected instruction, and report that no JIT iteration ran.
    emit_persist_live_nzcv();
    emit(arm64::str_w_uoff(31, REG_CPU_STATE, jit_iters_offset() >> 2));
    emit_update_rip(m_block_start_addr);
    emit_epilogue(JitExecResult::Unsupported, false);
}

void JitCompiler::defer_cold_arm(std::vector<ColdArmEntry> entries,
                                 std::function<void()> body,
                                 bool semantic_helper) {
    if (m_flushing_cold_arms || m_hot_end_pos != 0) {
        // Already in the cold region: the arm stays where it is, skipped by
        // the fast path. Its helper call records its own effects.
        const usize skip = m_code_buf.size();
        emit(0);  // B .resume
        const usize at = m_code_buf.size();
        for (const ColdArmEntry& e : entries) {
            m_code_buf[e.pos] = e.encode(static_cast<i32>(at - e.pos) *
                                         static_cast<i32>(sizeof(u32)));
        }
        body();
        m_code_buf[skip] = arm64::b_imm(cold_branch_disp(
            skip, m_code_buf.size(), 26, "inline cold arm skip"));
        return;
    }
    DeferredColdArm arm;
    arm.entries = std::move(entries);
    arm.resume_pos = m_code_buf.size();
    arm.body = std::move(body);
    arm.pins_live = m_pins_live;
    arm.linear_base_pinned = m_linear_base_pinned;
    arm.current_original_vex = m_current_original_vex;
    arm.nzcv_hold_depth = m_flags.nzcv_hold_depth;
    arm.xmm_cache_live = m_xmm_cache;
    if (m_insn_pos_count != 0) arm.owner = m_insn_pos_count - 1;
    m_deferred_cold_arms.push_back(std::move(arm));
    m_flags.helper_blr_this_insn = true;
    if (semantic_helper) m_flags.semantic_helper_this_insn = true;
    m_esp_frame_live = false;
}

bool JitCompiler::owning_insn(usize pos, u32& insn) const noexcept {
    // A lazy stub has one site, owned by its instruction (cold_lazy_emit
    // parks that RIP in m_insn_rips[0]).
    if (m_cold_lazy_late) {
        insn = 0;
        return true;
    }
    for (u32 i = 0; i < m_insn_pos_count; ++i) {
        if (pos >= m_insn_arm_pos[i] && pos < m_insn_arm_end[i]) {
            insn = i;
            return true;
        }
    }
    for (const ColdArmRange& r : m_cold_arm_ranges) {
        if (pos >= r.begin && pos < r.end) {
            insn = r.owner;
            return true;
        }
    }
    return false;
}

void JitCompiler::emit_flush_deferred_cold_arms() {
    AttrScope attr_scope(*this, EmitAttribution::ColdArms);
    if (m_deferred_cold_arms.empty()) return;
    const bool saved_pins_live = m_pins_live;
    const bool saved_linear_base = m_linear_base_pinned;
    const bool saved_original_vex = m_current_original_vex;
    const u32 saved_hold_depth = m_flags.nzcv_hold_depth;
    const bool saved_blr = m_flags.helper_blr_this_insn;
    const bool saved_semantic = m_flags.semantic_helper_this_insn;
    const bool saved_esp = m_esp_frame_live;
    const ConstEa saved_const_ea = m_const_ea;
    const u64 saved_rip_value = m_rip_reg_value;
    const usize saved_rip_pos = m_rip_reg_pos;
    const usize saved_rip_start = m_rip_reg_start;
    m_flushing_cold_arms = true;
    u32 ordinal = 0;
    for (DeferredColdArm& arm : m_deferred_cold_arms) {
        const usize at = m_code_buf.size();
        for (const ColdArmEntry& e : arm.entries) {
            m_code_buf[e.pos] = e.encode(static_cast<i32>(at - e.pos) *
                                         static_cast<i32>(sizeof(u32)));
        }
        emit_cold_entry_counter(ordinal++);
        m_pins_live = arm.pins_live;
        m_linear_base_pinned = arm.linear_base_pinned;
        m_current_original_vex = arm.current_original_vex;
        m_flags.nzcv_hold_depth = arm.nzcv_hold_depth;
        m_const_ea.valid = false;
        m_rip_reg_pos = static_cast<usize>(-1);
        arm.body();
        // The arm's helper call clobbered V16-V31; the fast path it resumes
        // still relies on the XMM cache entries live at the deferral.
        emit_xmm_cache_reload(arm.xmm_cache_live);
        const usize back = m_code_buf.size();
        emit(arm64::b_imm(cold_branch_disp(back, arm.resume_pos, 26,
                                           "deferred cold arm resume")));
        if (arm.owner != ~0u) {
            m_cold_arm_ranges.push_back({at, m_code_buf.size(), arm.owner});
        }
    }
    m_deferred_cold_arms.clear();
    m_flushing_cold_arms = false;
    m_pins_live = saved_pins_live;
    m_linear_base_pinned = saved_linear_base;
    m_current_original_vex = saved_original_vex;
    m_flags.nzcv_hold_depth = saved_hold_depth;
    m_flags.helper_blr_this_insn = saved_blr;
    m_flags.semantic_helper_this_insn = saved_semantic;
    m_esp_frame_live = saved_esp;
    m_const_ea = saved_const_ea;
    m_rip_reg_value = saved_rip_value;
    m_rip_reg_pos = saved_rip_pos;
    m_rip_reg_start = saved_rip_start;
}

void JitCompiler::emit_flush_cold_jcc_exits() {
    using P = ColdTailFootprint;
    usize mark = m_code_buf.size();
    const auto note = [&](P::Piece piece) {
        note_cold_tail_words(piece, m_code_buf.size() - mark);
        mark = m_code_buf.size();
    };
    note_cold_tail_words(P::Blocks, 1);
    // Counter ordinals: deferred arms 0.., then memory stubs.
    m_cold_mem_ordinal_base = static_cast<u32>(m_deferred_cold_arms.size());
    if (m_residency_variant) {
        emit_flush_residency_hot_pieces();
        mark = m_code_buf.size();
    }
    emit_flush_stack_proof_exit_stub();
    note(P::MidJccExits);
    // Before the stubs below: an arm's EA or helper call may queue an align
    // check or a raise branch they place and patch.
    emit_flush_deferred_cold_arms();
    note(P::DeferredArms);
    // Memory slow paths first: each one emits its own raise check, so they
    // must be laid down before the shared raise stub is placed and patched.
    // (It notes its own pieces.)
    emit_flush_cold_mem_stubs();
    mark = m_code_buf.size();
    emit_flush_cold_linear_store_stubs();
    note(P::LinearStores);
    emit_flush_cold_ud2_stub();
    note(P::Ud2);
    emit_flush_cold_align_checks();
    note(P::AlignChecks);
    emit_flush_raise_exit_stub();
    note(P::RaiseExit);
    emit_flush_mid_jcc_exits();
    note(P::MidJccExits);
}

void JitCompiler::emit_flush_mid_jcc_exits() {
    AttrScope attr_scope(*this, EmitAttribution::ExitOther);
    for (const ColdJccExit& exit : m_cold_jcc_exits) {
        const usize dest = m_code_buf.size();
        const i32 off =
            cold_branch_disp(exit.branch_pos, dest, 19, "mid-block Jcc exit");
        if (exit.fused) {
            m_code_buf[exit.branch_pos] = arm64::b_cond(exit.arm_cond, off);
        } else {
            m_code_buf[exit.branch_pos] = arm64::cbnz_w(0, off);
        }
        if (exit.helper_branch_pos != static_cast<usize>(-1)) {
            const i32 hoff = cold_branch_disp(exit.helper_branch_pos, dest, 19,
                                              "mid-block Jcc helper exit");
            m_code_buf[exit.helper_branch_pos] = arm64::cbnz_w(0, hoff);
        }
        if (exit.stub_store) {
            emit_lazy_flag_store_raw(exit.stub_op, exit.stub_src,
                                     exit.stub_dst, exit.stub_wide);
        }

        // Every forward taken edge may be patched directly to the resolved
        // target's linked entry. Until patched, or after any SMC / arena
        // generation bump, the guard falls through to the unchanged RIP
        // publication and L1 lookup below. Forward-only keeps the linked hop
        // budget on every possible cycle.
        //
        // NOT gated on the block's other slots. These exits are flushed after
        // the terminal tail, and an `m_emit_links.empty()` gate (left from the
        // single-slot design) silently took the slot away from every mid-block
        // exit of a block whose TERMINAL carries one: a trace ending in a JMP
        // rel or Jcc lost them once those classes went default-on, and
        // enabling CALL rel links took them from every CALL-ending trace too.
        // JitExecTest.MidBlockJccKeepsItsSlotBesideATerminalSlot.
        usize direct_gen_miss = static_cast<usize>(-1);
        if (!m_stack_entry_load_proof &&
            exit.target > m_block_start_addr &&
            link_edge_enabled(LinkEdge::MidJcc) &&
            s_jit_link_enabled.load(std::memory_order_acquire)) {
            if (exit.carry != NzcvState::Carried) {
                emit_store_nzcv_kind(exit.carry);
            }
            constexpr u32 gen_off =
                static_cast<u32>(offsetof(JitContext, link_generation));
            static_assert((gen_off & 3) == 0);
            static_assert((gen_off >> 2) < 4096);
            emit(arm64::ldr_w_uoff(17, REG_JIT_CTX, gen_off >> 2));

            const u32 gen_offset =
                static_cast<u32>(m_code_buf.size() * sizeof(u32));
            emit(arm64::movz_w(3, 0));
            emit(arm64::eor_w(3, 3, 17));
            direct_gen_miss = m_code_buf.size();
            emit(0);  // CBNZ W3, generic fallback

            const u32 patch_offset =
                static_cast<u32>(m_code_buf.size() * sizeof(u32));
            emit(0);  // Atomically patched B target, initially B fallback
            m_emit_links.push_back(
                JitCompileMemo::DirectLinkMeta{patch_offset, gen_offset, exit.target});
            if (stats::report_enabled()) {
                stats::record_link_slot(static_cast<u32>(LinkEdge::MidJcc));
            }
        }

        const usize generic_fallback = m_code_buf.size();
        if (direct_gen_miss != static_cast<usize>(-1)) {
            m_code_buf[direct_gen_miss] = arm64::cbnz_w(
                3, cold_branch_disp(direct_gen_miss, generic_fallback, 19,
                                    "direct Jcc generation fallback"));
            const usize direct_branch =
                m_emit_links.back().patch_offset / sizeof(u32);
            m_code_buf[direct_branch] = arm64::b_imm(
                cold_branch_disp(direct_branch, generic_fallback, 26,
                                 "direct Jcc initial fallback"));
        }
        emit_update_rip(exit.target);
        // DIAGNOSTIC: does the re-emitted deferred lazy-flag store above break
        // the linked hop's carried-NZCV contract?
        // A linked hop out of a MID-BLOCK Jcc must not claim live PSTATE.
        //
        // `exit.carry` is the compile-time NZCV view at the Jcc, and passing it
        // here tells the successor's Jcc-first path that PSTATE still holds
        // those flags, so it may branch on them natively instead of
        // rematerialising from the architectural lazy-flag record. That
        // promise does not survive this edge: the cold stub re-emits any
        // deferred lazy-flag store, publishes the RIP and runs the L1 probe
        // before the hop lands, and the successor then evaluates its condition
        // against flags that are no longer the ones the Jcc produced.
        //
        // Mid-block Jcc exits exist only because x64 trace formation absorbs
        // forward Jccs (VITREO_JIT_TRACES64); before traces every Jcc was
        // terminal and this edge could not occur, which is why the contract
        // was never reconciled. Symptom: a JavaScript engine's libm pow
        // returned `x` for every general-path input, so `2 ** 32` evaluated
        // to 2 and every guest that hit it died in its script parser.
        // Guarded by
        // JitParity64Test.MidBlockJccLinkedHopDoesNotCarryNzcv.
        //
        // Publishing the kind costs one store on a cold exit and lets the
        // successor take the architectural path, which is the semantic
        // reference. VITREO_JIT_MIDJCC_CARRY=1 restores the old behaviour for
        // bisection; it is not a correct configuration.
        static const bool midjcc_carry =
            environment_flag_enabled("VITREO_JIT_MIDJCC_CARRY", false);
        emit_tail_link_or_exit(exit.target,
                               midjcc_carry ? exit.carry : NzcvState::None,
                               LinkEdge::None);
    }
    m_cold_jcc_exits.clear();
}

} // namespace vitreo::cpu::jit
