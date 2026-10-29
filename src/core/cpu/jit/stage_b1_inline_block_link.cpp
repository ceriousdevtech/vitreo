// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler.cpp
/// @brief Copy-and-patch JIT compiler implementation (CPU-040)
///
/// Compiles decoded x86 BasicBlocks into ARM64 machine code using
/// direct instruction encoding. Uses lazy flag evaluation to avoid
/// computing x86 flags after every ALU instruction.
///
/// Register convention for JIT code:
///   X19 = CpuState*        (callee-saved, pinned)
///   X20 = JitContext*       (callee-saved, pinned — helper fn ptrs + memory)
///   W0-W15 = scratch registers
///   X16-X17 = intra-procedure scratch

#include <cstdint>
#include <cstdio>
#include <cstdlib>
///   X30 = LR (saved/restored in prologue/epilogue for BLR calls)

#include "jit_compiler.hpp"
#include "jit_emit_internal.hpp"
#include "jit_stats.hpp"
#include "stencils.hpp"
#include "jit_helpers.hpp"
#include "jit_split_layout.hpp"
#include "idiom_detector.hpp"
#include "linear_fault.hpp"
#include "../../memory/jit_region_registry.hpp"
#include "../../memory/mem_watch.hpp"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>

/// @file stage_b1_inline_block_link.cpp
/// @brief Stage B1: inline block-link tail, plus a direct patch of the link site.
///
/// Closed cut from jit_compiler.cpp — verified with plan_extraction.py.

namespace vitreo::cpu::jit {

bool JitCompiler::l1_way1_fallthrough_enabled() noexcept {
    static const bool on = environment_flag_enabled("VITREO_JIT_L1_WAY1_FALLTHROUGH", true);
    return on;
}

namespace {

bool can_duplicate_l1_suffix(u32* words, usize count) noexcept {
    // All words stay on the same side, so this validates without modifying
    // them. Reject future outbound PC-relative references (including ADR or
    // literal loads); callers then retain the original branch-to-hit path.
    // The null-code and hop-cap branches are still placeholders and their
    // copies are independently patched to the existing fallback below.
    return JitCompiler::l1_way1_fallthrough_enabled() &&
           relocate_split_branches(words, count, count, 0);
}

constexpr u32 rsb_base_off() noexcept {
    return static_cast<u32>(offsetof(JitContext, rsb));
}
constexpr u32 rsb_top_off() noexcept {
    return static_cast<u32>(offsetof(JitContext, rsb_top));
}
constexpr u32 sticky_rip_off() noexcept {
    return static_cast<u32>(offsetof(JitContext, ret_sticky_rip));
}
constexpr u32 sticky_host_off() noexcept {
    return static_cast<u32>(offsetof(JitContext, ret_sticky_host));
}
constexpr u32 sticky_i_off() noexcept {
    return static_cast<u32>(offsetof(JitContext, ret_sticky_i));
}
constexpr u32 cont_sticky_rip_off() noexcept {
    return static_cast<u32>(offsetof(JitContext, cont_sticky_rip));
}
constexpr u32 cont_sticky_host_off() noexcept {
    return static_cast<u32>(offsetof(JitContext, cont_sticky_host));
}
constexpr u32 cont_sticky_i_off() noexcept {
    return static_cast<u32>(offsetof(JitContext, cont_sticky_i));
}

static_assert((rsb_base_off() & 7) == 0, "JitContext::rsb must be 8-aligned");
static_assert(sizeof(JitContext::RsbEntry) == 16,
              "RsbEntry must be 16 bytes for idx<<4 addressing");
static_assert((JitContext::kRsbDepth & (JitContext::kRsbDepth - 1)) == 0,
              "kRsbDepth must be a power of two");
static_assert((rsb_top_off() & 3) == 0, "rsb_top must be 4-byte aligned");
static_assert((rsb_top_off() >> 2) < 4096, "rsb_top must fit ldr_w_uoff");
static_assert((sticky_rip_off() & 7) == 0, "ret_sticky_rip must be 8-aligned");
static_assert((sticky_host_off() & 7) == 0, "ret_sticky_host must be 8-aligned");
static_assert((sticky_rip_off() >> 3) < 4096, "ret_sticky_rip[0] must fit ldr_x_uoff");
static_assert(((sticky_rip_off() + 8) >> 3) < 4096,
              "ret_sticky_rip[1] must fit ldr_x_uoff");
static_assert((sticky_host_off() >> 3) < 4096,
              "ret_sticky_host[0] must fit ldr_x_uoff");
static_assert(((sticky_host_off() + 8) >> 3) < 4096,
              "ret_sticky_host[1] must fit ldr_x_uoff");
static_assert((sticky_i_off() & 3) == 0, "ret_sticky_i must be 4-byte aligned");
static_assert((sticky_i_off() >> 2) < 4096, "ret_sticky_i must fit ldr_w_uoff");
static_assert((cont_sticky_rip_off() & 7) == 0,
              "cont_sticky_rip must be 8-aligned");
static_assert((cont_sticky_host_off() & 7) == 0,
              "cont_sticky_host must be 8-aligned");
static_assert((cont_sticky_rip_off() >> 3) < 4096,
              "cont_sticky_rip[0] must fit ldr_x_uoff");
static_assert(((cont_sticky_rip_off() + 8) >> 3) < 4096,
              "cont_sticky_rip[1] must fit ldr_x_uoff");
static_assert((cont_sticky_host_off() >> 3) < 4096,
              "cont_sticky_host[0] must fit ldr_x_uoff");
static_assert(((cont_sticky_host_off() + 8) >> 3) < 4096,
              "cont_sticky_host[1] must fit ldr_x_uoff");
static_assert((cont_sticky_i_off() & 3) == 0,
              "cont_sticky_i must be 4-byte aligned");
static_assert((cont_sticky_i_off() >> 2) < 4096,
              "cont_sticky_i must fit ldr_w_uoff");

// Measurement gate: VITREO_JIT_SHARED_EXITS=0 emits every tail inline again,
// so a single binary can be A/B'd on a real guest without a second build.
// Default ON. Off is a strictly larger, behaviourally identical translation.
static bool shared_exits_enabled() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("VITREO_JIT_SHARED_EXITS");
        return v == nullptr || v[0] != '0';
    }();
    return on;
}

// DIAGNOSTIC gate for the dispatcher exit-reason histogram and the linked-tail
// hit counters. Process-wide and resolved exactly once, so every block in the
// process is instrumented the same way — a block compiled before the perf
// window opens still stores its reason, which a compile-time gate on
// perf_window_active() would silently miss.
//
// OFF (the default) emits not one extra instruction and not one extra byte of
// generated code: the translation is byte-identical to a build without this
// diagnostic. ON costs 2 ARM instructions on a dispatcher-exit path (never on
// a successful linked hop) and 3 ARM instructions per successful RET /
// indirect hop, all flag-neutral.
static bool jit_exit_stats_enabled() noexcept {
    static const bool on = [] {
        if (const char* v = std::getenv("VITREO_JIT_EXIT_STATS")) {
            return v[0] != '0';
        }
        // The histogram is only ever *reported* through the perf window, so
        // follow it by default rather than making the user set two variables.
        const char* w = std::getenv("VITREO_PERF_WINDOW");
        return w != nullptr && w[0] != '\0';
    }();
    return on;
}

} // namespace

void JitCompiler::emit_exit_reason(JitExitReason reason) {
    AttrScope attr_scope(*this, EmitAttribution::ExitDispatch);
    if (!jit_exit_stats_enabled()) return;
    using namespace arm64;
    constexpr u32 off = static_cast<u32>(offsetof(JitContext, exit_reason));
    static_assert((off & 3) == 0, "exit_reason must be 4-byte aligned");
    static_assert((off >> 2) < 4096, "exit_reason must fit str_w_uoff");
    static_assert(static_cast<u32>(JitExitReason::Count) <= 0xFFFFu,
                  "JitExitReason must fit a MOVZ Wd, #imm16");
    // MOVZ + STR are both flag-neutral, so a caller whose fallback carries
    // live guest NZCV into emit_shared_continue_exit(false) is unaffected.
    emit(movz_w(3, static_cast<u16>(reason)));
    emit(str_w_uoff(3, REG_JIT_CTX, off >> 2));
}

void JitCompiler::emit_link_hit_counter(u32 field_off) {
    AttrScope attr_scope(*this, EmitAttribution::ExitDispatch);
    if (!jit_exit_stats_enabled()) return;
    using namespace arm64;
    // Callers pass offsetof(JitContext, <u32 counter>); assert the encoding
    // range dynamically because the offset is a runtime argument here.
    assert((field_off & 3) == 0 && (field_off >> 2) < 4096 &&
           "linked-tail hit counter must fit ldr_w_uoff/str_w_uoff");
    emit(ldr_w_uoff(3, REG_JIT_CTX, field_off >> 2));
    emit(add_w_imm(3, 3, 1));  // ADD, not ADDS: must not disturb PSTATE
    emit(str_w_uoff(3, REG_JIT_CTX, field_off >> 2));
}

void JitCompiler::emit_l1_index(u8 dst, u8 src) {
    using namespace arm64;
    static_assert(BlockCache::l1_index_bits() >= 1 &&
                      BlockCache::l1_index_bits() <= 32,
                  "L1 index ubfx width must be encodable");
    static_assert(BlockCache::l1_index_fold() >= BlockCache::l1_index_bits(),
                  "the fold must not cancel the low bits it disambiguates");
    // Must stay bit-identical to BlockCache::l1_index(). Checked here rather
    // than trusted, on a value whose fold actually changes the result.
    // The emitted value is the SET index; callers scale it by the set size
    // (kL1Ways entries) to reach way 0, and way 1 is one entry further.
    static_assert(BlockCache::l1_set_index(0x1400EDC80ull) ==
                      ((0x1400EDC80ull ^
                        (0x1400EDC80ull >> BlockCache::l1_index_fold())) &
                       ((BlockCache::l1_capacity() / BlockCache::kL1Ways) - 1)),
                  "emitted L1 set index must match BlockCache::l1_set_index");
    static_assert(BlockCache::kL1Ways == 2 &&
                      sizeof(BlockCache::L1Entry) == 32,
                  "the probes below scale the set index by 64 and read way 1 at +32");
    emit(eor_x_lsr(dst, src, src,
                   static_cast<u8>(BlockCache::l1_index_fold())));
    emit(ubfx_x(dst, dst, 0, static_cast<u8>(BlockCache::l1_index_bits())));
}

// ----------------------------------------------------------------------------
// Stage B1: inline block-link tail.
//
// At the end of a basic block whose terminator is a direct unconditional
// JMP REL with a known compile-time target, instead of returning to the
// C++ dispatcher (which would re-do the BlockCache lookup, the BLR back
// into JIT'd code, and the prologue), we emit an inline lookup that:
//
//   1. Loads BlockCache::m_l1[0] from JitContext
//   2. CBZ → fallback if linking disabled (block_cache_l1 == nullptr)
//   3. Loads m_l1[target & (L1-1)] (register-offset; L1 may exceed
//      ldr_x_uoff's 4096-slot imm12)
//   4. CBZ → fallback if slot empty
//   5. Loads slot->start_addr (offset 0) and compares against target
//   6. B.NE → fallback on mismatch
//   7. Loads slot->compiled_link_code
//   8. CBZ → fallback if not yet compiled
//   9. BR Xt — tail-jump past the target's LR save, pin load, and
//      linear-base load. Pins stay in X23–X28 / X12 / X13. Fallback
//      (emit_epilogue) stores them into CpuState before returning.
//
// The fallback emits the standard epilogue and pops the one LR frame owned by
// the whole chain. Successful links neither pop the current frame nor push a
// new one, and they do not store or reload GPR pins.
//
// Safety: BlockCache is per-thread (each thread has its own JitCompiler
// and its own BlockCache).  The L1 array, BasicBlock entries, and the
// arena code are all touched only by the owning thread, so the inline
// load/compare sequence is race-free without any synchronization.  If
// the cache is invalidated (SMC or LRU eviction), the L1 slot becomes
// nullptr or its start_addr no longer matches — the lookup misses and
// the fallback returns to the dispatcher, which handles re-decode.
std::vector<u32> JitCompiler::emit_for_test(TestEmitter which) {
    m_code_buf.clear();
    switch (which) {
    case TestEmitter::LazyFlagStoreNarrow:
        emit_lazy_flag_store_raw(CpuState::FlagOp::Sub32, 5, 6, /*wide=*/false);
        break;
    case TestEmitter::LazyFlagStoreWide:
        emit_lazy_flag_store_raw(CpuState::FlagOp::Sub64, 5, 6, /*wide=*/true);
        break;
    case TestEmitter::SetLazyFlagsPreserveCf:
        emit_set_lazy_flags_preserve_cf(CpuState::FlagOp::Add32, 5, 6, 7);
        break;
    case TestEmitter::ClearCfOverlay:
        emit_clear_cf_overlay();
        break;
    }
    std::vector<u32> out = m_code_buf;
    m_code_buf.clear();
    return out;
}

void JitCompiler::emit_tail_link_or_exit(u64 target, NzcvState carry,
                                        LinkEdge edge) {
    AttrScope attr_scope(*this, EmitAttribution::ExitDispatch);
    if (emit_linked_tail_continue(target, carry, edge)) return;
    emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                           jit_iters_offset() >> 2));
    emit_epilogue(JitExecResult::Continue);
}

bool JitCompiler::emit_linked_tail_continue(u64 target, NzcvState carry,
                                            LinkEdge edge) {
    AttrScope attr_scope(*this, EmitAttribution::ExitDispatch);
    // An entry-authorized load block owns a GuestMemory protection lease for
    // this native entry only. Returning to the dispatcher releases it;
    // carrying it into an arbitrary successor could enter a helper while a
    // writer waits.
    if (m_stack_entry_load_proof) {
        return false;
    }
    if (!s_jit_link_enabled.load(std::memory_order_acquire)) {
        return false;
    }
    // Full 64-bit compare path — works for both 32-bit and 64-bit guest
    // targets (a 64-bit image lives at the default base 0x140000000 and would otherwise
    // always bail to the dispatcher here).
    constexpr u32 cc_off =
        static_cast<u32>(offsetof(BasicBlock, compiled_link_code));
    static_assert((cc_off & 7) == 0,
                  "BasicBlock::compiled_link_code must be 8-byte aligned");
    static_assert(cc_off < 32768,
                  "BasicBlock::compiled_link_code offset must fit ldr_x_uoff");
    static_assert(offsetof(BasicBlock, start_addr) == 0,
                  "emit_linked_tail_continue assumes start_addr at offset 0");
    static_assert(sizeof(BasicBlock::start_addr) == 8,
                  "emit_linked_tail_continue assumes 64-bit start_addr");
    // The index formula itself is asserted in emit_l1_index(), which is the
    // only thing that emits it.

    using namespace arm64;
    // Pins stay in registers across a successful hop. emit_epilogue on
    // the fallback path below stores them before the dispatcher reads
    // CpuState.

    // emit_update_rip() leaves X0 holding `target` and, for the usual
    // adjacent tail, has just published the same value to CpuState. A linked
    // successor does not need that publication: its precise-fault side table
    // identifies its own guest instruction, and every eventual dispatcher
    // exit publishes the live X0 below. This is the constant-target analogue
    // of emit_linked_tail_reg(), which already keeps RIP register-only on a
    // successful indirect/RET hop.
    //
    // Keep this as an exact one-word peephole. If a caller emits anything
    // between emit_update_rip() and the tail, or the RIP store shape changes,
    // leave the architectural publication untouched and merely perform the
    // harmless fallback store again.
    constexpr u32 state_rip_off = static_cast<u32>(offsetof(CpuState, rip));
    const u32 publish_rip = str_x_uoff(
        REG_SCRATCH0, REG_CPU_STATE, state_rip_off >> 3);
    if (m_rip_reg_pos == m_code_buf.size() && !m_code_buf.empty() &&
        m_rip_reg_value == target && m_code_buf.back() == publish_rip) {
        m_code_buf.pop_back();
        // The live-X0 proof remains valid at the new end of the buffer.
        m_rip_reg_pos = m_code_buf.size();
    }

    // CALL/JMP rel with a fixed thunk target: never probe L1 — same as
    // emit_linked_tail_reg's runtime range check, but decided at emit time.
    // Decided before anything is emitted: this path never runs the probe, so
    // the predecessor's flags are still live in PSTATE and the shared
    // live-PSTATE continue exit publishes them directly.
    const u64 thunk_lo = m_jit_ctx.thunk_region_lo;
    const u64 thunk_hi = m_jit_ctx.thunk_region_hi;
    if (thunk_hi > thunk_lo && target >= thunk_lo && target < thunk_hi) {
        if (carry != NzcvState::Carried) {
            emit_store_nzcv_kind(carry);
        }
        emit(str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                        jit_iters_offset() >> 2));
        emit(publish_rip);
        emit_exit_reason(JitExitReason::ConstTailThunkTarget);  // DIAGNOSTIC
        // L3: offered to the in-chain exit, whose slot-map test decides
        // whether this thunk is an in-chain export.
        emit_shared_api_call_exit();
        return true;
    }

    // ---- Backedge safepoint (phase 1) ----------------------------------
    // The hop budget exists so a linked chain still returns to execute()
    // for the 16 ms GUI pump and so JitCodeArena::ArenaReadLease holds stay bounded. It
    // is NOT a throughput cap. Keep it only on guest-BACKWARD edges, i.e.
    // edges whose target does not increase the guest start RIP.
    //
    // Safety: at run time the L1 probe only hops when the resolved block's
    // start_addr == `target`, so a taken hop moves the "current block start
    // RIP" from m_block_start_addr to exactly `target`. A chain built solely
    // from forward edges therefore has a STRICTLY INCREASING sequence of
    // block start RIPs, so it can never revisit a block and can never be a
    // cycle. Every cycle in guest-RIP space contains at least one edge with
    // target <= source start, so every unbounded chain still hits a counted
    // edge. Indirect/RET tails (emit_linked_tail_reg) keep their budget
    // unconditionally, which covers cycles closed through a return.
    const bool backward_edge = (target <= m_block_start_addr);

    // Direct-edge patch slot, one per constant FORWARD edge of an enabled
    // class. Ordering matches the original Jcc-only slot exactly: the nzcv
    // kind is published before the slot when the flags do not stay live in
    // PSTATE, and guest RIP is NOT stored on the patched hop — the L1
    // fallthrough below still stores it, unchanged.
    //
    // Enabling every class at once (JMP rel, CALL rel, both terminal Jcc legs,
    // fall-throughs, plus backward edges) measured cmp_jcc 3.8x -> 1.8x and
    // stopped presentation: a launcher kept its processes, windows and draws
    // but presented zero frames, where it had reached present_1000 before
    // (5be713ad). The
    // classes are therefore gated individually by VITREO_JIT_LINK_EDGES and
    // re-landed one at a time against the presents column, so a regression
    // names the edge class that caused it instead of the mechanism.
    //
    // The nzcv-kind fold below must happen ONLY when a slot is really emitted:
    // folding `carry` to Carried also selects a different shared tail stub, so
    // a disabled class has to leave this function byte-identical to today.
    // A backward edge may now carry a slot too, when the direction is enabled.
    // emit_direct_link_slot puts the same counted hop budget inside the slot
    // that the backward stub below carries, so a patched backward chain is
    // still bounded and still returns to execute() for the GUI pump. The two
    // budgets can both fire on a fallthrough hop; the only effect is returning
    // a little sooner than the cap, which is the safe direction.
    // VITREO_JIT_PROBE_CALLREL_RANGE=<lo>:<hi> (hex guest addresses):
    // **MEASUREMENT PROBE.** Admit CALL rel slots only for source blocks that
    // start inside [lo, hi), to bisect which code's linked calls a behaviour
    // depends on. The range comes from the environment; nothing is named here.
    static const auto callrel_range = []() -> std::pair<u64, u64> {
        const char* env = std::getenv("VITREO_JIT_PROBE_CALLREL_RANGE");
        if (env == nullptr || env[0] == '\0') return {0, ~u64{0}};
        char* end = nullptr;
        const u64 lo = std::strtoull(env, &end, 16);
        const u64 hi = (end != nullptr && *end == ':')
                           ? std::strtoull(end + 1, nullptr, 16) : ~u64{0};
        return {lo, hi};
    }();
    const bool callrel_admitted =
        edge != LinkEdge::CallRel ||
        (m_block_start_addr >= callrel_range.first &&
         m_block_start_addr < callrel_range.second);
    const bool emits_slot = edge != LinkEdge::None && link_edge_enabled(edge) &&
                            callrel_admitted &&
                            (!backward_edge || backward_link_enabled()) &&
                            s_jit_link_enabled.load(std::memory_order_acquire);
    if (emits_slot) {
        // The patched hop does not read X0 either: a linked entry starts with
        // the successor's own code, which never assumes X0 (the RET/indirect
        // tails enter it with X0 = whatever the probe left). Only this tail's
        // fallthrough reads the target from X0, so emit_update_rip's
        // materialization moves behind the slot: the code below rebuilds X0
        // when nothing left it there. VITREO_JIT_PROBE_POISON_LINK_X0 audits
        // the "never assumes X0" half (jit_compiler.cpp, block prologue);
        // VITREO_JIT_TAIL_X0_BEFORE_SLOT=1 restores the old order.
        static const bool keep_x0_before_slot =
            std::getenv("VITREO_JIT_TAIL_X0_BEFORE_SLOT") != nullptr;
        if (!keep_x0_before_slot && m_rip_reg_pos == m_code_buf.size() &&
            m_rip_reg_value == target &&
            m_rip_reg_start != static_cast<usize>(-1) &&
            m_rip_reg_start <= m_code_buf.size()) {
            m_code_buf.resize(m_rip_reg_start);
            m_rip_reg_pos = static_cast<usize>(-1);
            m_rip_reg_start = static_cast<usize>(-1);
        }
        if (carry != NzcvState::Carried) {
            emit_store_nzcv_kind(carry);
            carry = NzcvState::Carried;  // stub must not store the kind again
        }
        emit_direct_link_slot(target, edge);
    }

    // The whole tail is out of line in a per-block stub keyed by
    // (carry, backward_edge). The stub reads the target out of REG_SCRATCH0,
    // which emit_update_rip has just left holding exactly `target` at every
    // call site, so the site itself is one branch.
    //
    // NZCV: nothing between this branch and the stub body executes, and the
    // branch is unconditional, so the predecessor's PSTATE reaches the stub
    // exactly as it reached the old inline sequence.
    //
    // The register handoff is only taken when emit_update_rip left exactly
    // this target in REG_SCRATCH0 and nothing has been emitted since. Any
    // other caller shape rematerializes the target here, so no site can
    // inherit a stale X0.
    if (m_rip_reg_pos != m_code_buf.size() || m_rip_reg_value != target) {
        emit_mov_imm64(REG_SCRATCH0, static_cast<i64>(target));
    }

    if (!shared_exits_enabled()) {
        emit_cont_tail_stub_body(carry, backward_edge);
        return true;
    }

    for (ContTailStub& stub : m_cont_tail_stubs) {
        if (stub.carry == carry && stub.backward == backward_edge) {
            stub.sites.push_back(m_code_buf.size());
            emit(0);
            return true;
        }
    }
    m_cont_tail_stubs.push_back(ContTailStub{carry, backward_edge, {}});
    m_cont_tail_stubs.back().sites.push_back(m_code_buf.size());
    emit(0);
    return true;
}

bool JitCompiler::emit_linked_call_continue(u64 target, u64 next_rip) {
    AttrScope attr_scope(*this, EmitAttribution::ExitLink);
    // Native guest CALL: a patchable direct BL into the callee's landing pad
    // (emit_native_call_landing_pad), so the callee's RET can return with a
    // real RET to the continuation right after it -- predicted by the host
    // return stack instead of an L1 probe and an indirect BR. Caller
    // contract: the guest CALL has retired (return RIP pushed, RSP updated).
    if (!m_cfg.native_callret_enabled || !m_block_is_64bit ||
        m_stack_entry_load_proof ||
        !s_jit_link_enabled.load(std::memory_order_acquire) ||
        !link_edge_enabled(LinkEdge::CallRel)) {
        return false;
    }
    const u64 thunk_lo = m_jit_ctx.thunk_region_lo;
    const u64 thunk_hi = m_jit_ctx.thunk_region_hi;
    if (thunk_hi > thunk_lo && target >= thunk_lo && target < thunk_hi) {
        return false;
    }
    const bool backward_edge = (target <= m_block_start_addr);
    if (backward_edge && !backward_link_enabled()) return false;

    using namespace arm64;
    // A callee's linked entry reads its incoming flags from the lazy record,
    // exactly as on the ordinary CALL tail.
    emit_store_nzcv_kind(NzcvState::None);

    // Same guard as emit_direct_link_slot: the patched BL is valid only for
    // the link generation it was written under, and a backward call pays the
    // counted hop budget (it can close a cycle; a forward call cannot).
    constexpr u32 gen_off =
        static_cast<u32>(offsetof(JitContext, link_generation));
    static_assert((gen_off & 3) == 0 && (gen_off >> 2) < 4096);
    emit(ldr_w_uoff(17, REG_JIT_CTX, gen_off >> 2));
    const u32 gen_offset = static_cast<u32>(m_code_buf.size() * sizeof(u32));
    emit(movz_w(3, 0));
    emit(eor_w(3, 3, 17));
    std::vector<usize> to_fallback;
    to_fallback.push_back(m_code_buf.size());
    emit(0);  // CBNZ W3, .fallback
    usize hop_cap_pos = static_cast<usize>(-1);
    if (backward_edge) {
        static_assert(kMaxLinkedHops == (1u << 12),
                      "hop-cap UBFX #12 must match the limit");
        emit(add_w_imm(REG_LOOP_CTR, REG_LOOP_CTR, 1));
        emit(kHopCapTestWord);  // UBFX W4, W22, #12, #1
        hop_cap_pos = m_code_buf.size();
        emit(0);  // CBNZ W4, .fallback
    }
    // The landing pad records the guest return RIP from X21.
    emit_mov_imm64(REG_CALL_TMP, static_cast<i64>(next_rip));
    // Patched to BL <callee call entry>. Until then it is a BL to the
    // fallback -- the BL opcode is what tells try_patch_direct_link that this
    // site wants the callee's landing pad rather than its linked entry.
    const usize call_pos = m_code_buf.size();
    emit(0);
    m_emit_links.push_back(JitCompileMemo::DirectLinkMeta{
        static_cast<u32>(call_pos * sizeof(u32)), gen_offset, target});
    if (stats::report_enabled()) {
        stats::record_link_slot(static_cast<u32>(LinkEdge::CallRel));
    }

    // A native RET from the callee lands here, with the callee's pins and the
    // flag state its RET tail publishes. The callee may have changed any XMM.
    m_vregs_clobbered = true;
    emit_update_rip(next_rip);
    const bool continuation = emit_linked_tail_continue(
        next_rip, NzcvState::Carried, LinkEdge::CallRel);
    assert(continuation && "native CALL continuation must retain linked tail");
    (void)continuation;

    // Unpatched, stale-generation or out-of-budget: the CALL has already
    // retired, so continue to the target on the ordinary path.
    const usize fallback = m_code_buf.size();
    for (const usize pos : to_fallback) {
        m_code_buf[pos] = cbnz_w(3, cold_branch_disp(
            pos, fallback, 19, "native CALL generation fallback"));
    }
    if (hop_cap_pos != static_cast<usize>(-1)) {
        m_code_buf[hop_cap_pos] = cbnz_w(4, cold_branch_disp(
            hop_cap_pos, fallback, 19, "native CALL hop budget"));
    }
    m_code_buf[call_pos] = bl_imm(cold_branch_disp(
        call_pos, fallback, 26, "native CALL initial fallback"));
    emit_update_rip(target);
    emit_tail_link_or_exit(target, NzcvState::Carried, LinkEdge::None);
    return true;
}

// Body of one shared linked-tail stub. Entered with the guest target in
// REG_SCRATCH0 (emit_update_rip left it there) and the predecessor's NZCV
// still live in PSTATE.
void JitCompiler::emit_cont_tail_stub_body(NzcvState carry, bool backward) {
    AttrScope attr_scope(*this, EmitAttribution::ExitProbe);
    using namespace arm64;

    constexpr u32 cc_off =
        static_cast<u32>(offsetof(BasicBlock, compiled_link_code));
    constexpr u32 state_rip_off = static_cast<u32>(offsetof(CpuState, rip));

    if (carry != NzcvState::Carried) {
        emit_store_nzcv_kind(carry);
    }
    if (shared_link_probe_enabled() &&
        emit_shared_link_probe(backward ? kProbeL1Counted : kProbeL1)) {
        emit_shared_continue_exit(false);
        return;
    }

    // Nothing in this tail clobbers NZCV any more. The probe was already
    // flag-neutral (the start_addr compare is EOR+CBNZ, not SUBS+B.NE), and
    // the hop budget below now uses a flag-neutral EOR+CBZ instead of
    // CMP+B.HS. So a predecessor's live NZCV flows across the hop untouched
    // and the spill/fill/persist trio is gone -- which was 4 instructions and
    // 2 memory accesses on EVERY backward (i.e. every loop) hop.
    constexpr bool clobbers_nzcv = false;
    if (clobbers_nzcv) {
        emit_nzcv_spill_if_kind();
    }

    // 1. LDR X17, [X20, #JIT_CTX_OFF_L1_BASE] — &m_l1[0], or 0 if disabled
    emit(ldr_x_uoff(17, REG_JIT_CTX, JIT_CTX_OFF_L1_BASE >> 3));
    const usize cbz_l1_pos = m_code_buf.size();
    emit(0);

    // 2. X17 = &m_l1[l1_index(target)], computed from the live target
    //    register rather than a per-site immediate. One shifted ADD scales
    //    the index by sizeof(L1Entry); a register offset also lets L1 grow
    //    past ldr_x_uoff's 4095-element imm12.
    // See emit_l1_index() for the formula and its agreement assert.
    static_assert(sizeof(BlockCache::L1Entry) == 32,
                  "cont-tail stub scales the L1 set index by 64");
    emit_l1_index(3, REG_SCRATCH0);
    emit(add_x_lsl(17, 17, 3, 6));        // X17 = &set (way 0)

    // 3. ONE load for both halves of the slot.
    //
    //    This is the change the whole per-transition cost hangs on. The slot
    //    used to hold a BasicBlock*, so the probe was a THREE-DEEP dependent
    //    load chain (ctx -> base, base -> slot, slot -> tag, slot -> code),
    //    and `sc_blocksize` measured the result at ~49 cycles per transition
    //    on x64. Inlining tag and entry pointer into the slot makes it
    //    base -> LDP: one load, both halves.
    //
    //    An empty slot has tag 0, and guest address 0 is never executable, so
    //    the tag compare subsumes the old "slot is null" test — one branch
    //    fewer on every hop. EOR/CBNZ rather than SUBS/B.NE so a predecessor's
    //    live NZCV still survives the probe untouched.
    static_assert(offsetof(BlockCache::L1Entry, tag) == 0 &&
                      offsetof(BlockCache::L1Entry, link_code) == 8,
                  "cont-tail stub LDPs tag/link_code as one pair");
        emit(ldp_x_off(3, 16, 17, 0));       // X3 = tag, X16 = link_code (way 0)
    emit(eor_x(3, 3, REG_SCRATCH0));
    // A way-0 hit -- the common case -- falls straight through to the BR.
    // Way 1 is probed out of line after it, so a hop costs no taken branch
    // beyond the BR itself (the front end pays for every taken branch).
    const usize w0_miss = m_code_buf.size();
    emit(0);                              // CBNZ X3 -> .way1
    usize bne_pos = static_cast<usize>(-1);
    const usize hit_pos = m_code_buf.size();
    const usize cbz_cc_pos = m_code_buf.size();
    emit(0);

    (void)cc_off;

    usize bhs_pos = static_cast<usize>(-1);
    if (backward) {
        static_assert(kMaxLinkedHops == (1u << 12),
                      "hop-cap UBFX #12 must match the limit");
        // ADD W22 + UBFX + CBNZ — NOT CMP + B.HS. None of them writes or
        // reads NZCV, so the budget no longer clobbers it and the
        // spill/fill/persist trio below disappears from every backward hop.
        //
        // This is the same sequence emit_linked_tail_reg uses on the
        // indirect/RET tail.
        emit(add_w_imm(REG_LOOP_CTR, REG_LOOP_CTR, 1));
        emit(kHopCapTestWord);  // UBFX W4, W22, #12, #1
        bhs_pos = m_code_buf.size();
        emit(0);  // CBNZ W4, .fallback
    }

    if (clobbers_nzcv) {
        emit_nzcv_fill_if_kind();
    }
    // 5. BR X16 — tail-jump immediately after target's native prologue
    emit(br(16));
    const usize hit_end = m_code_buf.size();

    // .way1: way 0 missed. Probe way 1 at +32, leaving X17 on it so the
    // layout load reads the way that matched, and rejoin the hit path. A
    // way-1 hit is still a hop, so this stays with the hot lines.
    m_code_buf[w0_miss] =
        cbnz_x(3, static_cast<i32>(m_code_buf.size() - w0_miss) * 4);
    emit(add_x_imm(17, 17, 32));
    emit(ldp_x_off(3, 16, 17, 0));       // X3 = tag, X16 = link_code (way 1)
    emit(eor_x(3, 3, REG_SCRATCH0));
    bne_pos = m_code_buf.size();
    emit(0);                              // CBNZ X3 -> miss
    usize duplicate_hit = static_cast<usize>(-1);
    if (can_duplicate_l1_suffix(m_code_buf.data() + hit_pos, hit_end - hit_pos)) {
        duplicate_hit = m_code_buf.size();
        for (usize i = hit_pos; i < hit_end; ++i) emit(m_code_buf[i]);
    } else {
        emit(b_imm((static_cast<i32>(hit_pos) -
                    static_cast<i32>(m_code_buf.size())) * 4));  // B .hit
    }

    // ---- DIAGNOSTIC: per-cause landing pads --------------------------------
    // Each probe failure gets its own three-word pad (MOVZ reason, STR, B
    // .fallback) so the perf-window histogram can tell an L1 conflict miss
    // apart from "target not compiled yet" and from the hop cap. The pads sit
    // between the hit path and .fallback and are reached only by a branch
    // that used to jump straight to .fallback, so they cost NOTHING on a
    // successful hop and 3 ARM instructions on an exit.
    //
    // With jit_exit_stats_enabled() false, no pad is emitted, `patch_off_to()`
    // sends every site straight to .fallback, and this stub is byte-identical
    // to the uninstrumented one.
    const bool exit_stats = jit_exit_stats_enabled();
    std::vector<usize> pad_to_fallback;
    auto make_pad = [&](JitExitReason reason) -> usize {
        if (!exit_stats) return static_cast<usize>(-1);
        const usize pos = m_code_buf.size();
        emit_exit_reason(reason);
        pad_to_fallback.push_back(m_code_buf.size());
        emit(0);  // B .fallback -- patched below
        return pos;
    };
    const usize pad_l1_null  = make_pad(JitExitReason::ConstTailLinkDisabled);
    // ConstTailL1Empty no longer has a site of its own: an empty slot has
    // tag 0 and now fails the address compare like any other miss, so it is
    // counted as ConstTailL1AddrMismatch.
    const usize pad_addr     = make_pad(JitExitReason::ConstTailL1AddrMismatch);
    const usize pad_no_code  = make_pad(JitExitReason::ConstTailNoCompiledCode);
    const usize pad_hop_cap  = (bhs_pos != static_cast<usize>(-1))
                                   ? make_pad(JitExitReason::ConstTailHopCap)
                                   : static_cast<usize>(-1);
    // ------------------------------------------------------------------------

    // .fallback:
    const usize fallback_pos = m_code_buf.size();
    for (const usize pos : pad_to_fallback) {
        m_code_buf[pos] =
            b_imm(static_cast<i32>(fallback_pos - pos) * 4);
    }
    auto patch_off = [&](usize from) -> i32 {
        return static_cast<i32>(fallback_pos - from) * 4;
    };
    // Destination for one miss site: its own reason pad when instrumented,
    // otherwise .fallback exactly as before.
    auto patch_off_to = [&](usize from, usize pad) -> i32 {
        if (pad == static_cast<usize>(-1)) return patch_off(from);
        return static_cast<i32>(pad - from) * 4;
    };
    // Every branch here is a CBZ/CBNZ/B.cond imm19 (+-1 MiB) reaching
    // forward to a .fallback at most ~30 words away, so none can overflow
    // its field (and the encoders refuse, rather than mask, if one did).
    assert(patch_off(cbz_l1_pos) > 0 && patch_off(cbz_l1_pos) < (1 << 20)
           && "cont-tail fallback branch out of imm19 range");
    m_code_buf[cbz_l1_pos]    = cbz_x(17, patch_off_to(cbz_l1_pos, pad_l1_null));
    m_code_buf[bne_pos]       = cbnz_x(3, patch_off_to(bne_pos, pad_addr));
    m_code_buf[cbz_cc_pos]    = cbz_x(16, patch_off_to(cbz_cc_pos, pad_no_code));
    if (bhs_pos != static_cast<usize>(-1)) {
        m_code_buf[bhs_pos]   = cbnz_w(4, patch_off_to(bhs_pos, pad_hop_cap));
    }
    if (duplicate_hit != static_cast<usize>(-1)) {
        const usize no_code = duplicate_hit + cbz_cc_pos - hit_pos;
        m_code_buf[no_code] = cbz_x(16, patch_off_to(no_code, pad_no_code));
        if (bhs_pos != static_cast<usize>(-1)) {
            const usize cap = duplicate_hit + bhs_pos - hit_pos;
            m_code_buf[cap] = cbnz_w(4, patch_off_to(cap, pad_hop_cap));
        }
    }

    // .fallback: publish the retiring block's iteration count and NZCV, keep
    // kind, then pop the chain frame. Successful links deliberately skip the
    // CpuState store; the linked target starts its own count in REG_LOOP_CTR.
    emit(str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, state_rip_off >> 3));
    emit(str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                    jit_iters_offset() >> 2));
    emit_shared_continue_exit(clobbers_nzcv);
}

void JitCompiler::emit_flush_cont_tail_stubs() {
    AttrScope attr_scope(*this, EmitAttribution::ExitProbe);
    for (ContTailStub& stub : m_cont_tail_stubs) {
        if (stub.sites.empty()) continue;
        const bool emitted = stub.emitted_pos != static_cast<usize>(-1);
        const usize dest = emitted ? stub.emitted_pos : m_code_buf.size();
        for (const usize pos : stub.sites) {
            const i64 words =
                static_cast<i64>(dest) - static_cast<i64>(pos);
            // B is imm26 (+-128 MiB): out of reach, the block is declined.
            const i32 disp = cold_branch_disp(pos, dest, 26, "cont-tail stub");
            // The last site often sits right before its stub. A branch to
            // the next word is still a taken branch for the front end.
            m_code_buf[pos] = words == 1 ? arm64::nop() : arm64::b_imm(disp);
        }
        stub.sites.clear();
        if (!emitted) {
            stub.emitted_pos = dest;
            emit_cont_tail_stub_body(stub.carry, stub.backward);
        }
    }
}

// ---------------------------------------------------------------------------
// Shared per-block "continue" exit stub.
//
// See the declaration comment in jit_compiler.hpp. The body reads only X19
// (CpuState), the pins, and the block's fixed [SP,#8] NZCV spill slot, so it
// is identical for every tail in the block that selects the same persist
// mode. Sites emit a placeholder that emit_flush_continue_exit_stubs()
// rewrites into a B.
// ---------------------------------------------------------------------------
void JitCompiler::emit_shared_continue_exit(bool spilled) {
    AttrScope attr_scope(*this, EmitAttribution::ExitDispatch);
    if (!shared_exits_enabled()) {
        if (spilled) {
            emit_persist_spilled_nzcv();
        } else {
            emit_persist_live_nzcv();
        }
        emit_epilogue(JitExecResult::Continue, false);
        return;
    }
    m_continue_exit_pos[spilled ? 1u : 0u].push_back(m_code_buf.size());
    emit(0);
}

void JitCompiler::emit_flush_continue_exit_stubs() {
    AttrScope attr_scope(*this, EmitAttribution::ExitDispatch);
    for (u32 v = 0; v < kContinueExitVariants; ++v) {
        std::vector<usize>& sites = m_continue_exit_pos[v];
        if (sites.empty()) continue;
        const bool emitted = m_continue_exit_body[v] != static_cast<usize>(-1);
        const usize dest = emitted ? m_continue_exit_body[v] : m_code_buf.size();
        for (const usize pos : sites) {
            const i64 words =
                static_cast<i64>(dest) - static_cast<i64>(pos);
            // B is imm26 (+-128 MiB): out of reach, the block is declined.
            const i32 disp = cold_branch_disp(pos, dest, 26, "continue-exit stub");
            m_code_buf[pos] = words == 1 ? arm64::nop() : arm64::b_imm(disp);
        }
        sites.clear();
        if (emitted) continue;
        m_continue_exit_body[v] = dest;
        if (emit_dispatch_tail(v == 1u ? kTailContinueSpilled
                                       : kTailContinueLive)) {
            continue;
        }
        if (v == 1u) {
            emit_persist_spilled_nzcv();
        } else {
            emit_persist_live_nzcv();
        }
        emit_epilogue(JitExecResult::Continue, false);
    }
}

void JitCompiler::emit_rsb_push(u64 return_rip) {
    AttrScope attr_scope(*this, EmitAttribution::ExitProbe);
    if (!s_jit_link_enabled.load(std::memory_order_acquire)) {
        return;
    }
    constexpr u32 cc_off =
        static_cast<u32>(offsetof(BasicBlock, compiled_link_code));
    constexpr u32 rsb_off = rsb_base_off();
    constexpr u32 top_off = rsb_top_off();
    constexpr u8 kIdxBits = 4;  // log2(kRsbDepth)
    static_assert(JitContext::kRsbDepth == (1u << kIdxBits),
                  "kIdxBits must match kRsbDepth");

    using namespace arm64;

    if (shared_rsb_push_enabled() && materialize_rsb_push() != nullptr) {
        emit_mov_imm64(2, static_cast<i64>(return_rip));
        emit(ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_RSB_PUSH >> 3));
        emit(0xD63F0200u);  // BLR X16
        return;
    }

    // Inline L1 lookup of return_rip → host in X17 (0 on miss).
    emit(ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_L1_BASE >> 3));
    const usize cbz_l1_pos = m_code_buf.size();
    emit(0);  // CBZ X16 → .miss

        emit_mov_imm64(17, static_cast<i64>(return_rip));
    emit_l1_index(3, 17);
    emit(add_x_lsl(2, 16, 3, 6));     // X2 = &set (way 0)
    // Way 0.
    emit(ldr_x_uoff(3, 2, 24 >> 3));  // X3 = way0.block
    const usize w0_null = m_code_buf.size();
    emit(0);  // CBZ X3 -> .way1
    emit(ldr_x_uoff(16, 3, 0));       // start_addr
    emit(subs_x(31, 16, 17));
    const usize w0_hit = m_code_buf.size();
    emit(0);  // B.EQ -> .hit
    // Way 1; its misses go to .miss.
    const usize way1_pos = m_code_buf.size();
    emit(ldr_x_uoff(3, 2, 56 >> 3));  // X3 = way1.block
    const usize cbz_block_pos = m_code_buf.size();
    emit(0);  // CBZ X3 → .miss
    emit(ldr_x_uoff(16, 3, 0));  // start_addr
    emit(subs_x(31, 16, 17));
    const usize bne_pos = m_code_buf.size();
    emit(0);  // B.NE → .miss
    m_code_buf[w0_null] = cbz_x(3, static_cast<i32>(way1_pos - w0_null) * 4);
    m_code_buf[w0_hit] = b_cond(EQ, static_cast<i32>(m_code_buf.size() - w0_hit) * 4);

    emit(ldr_x_uoff(17, 3, cc_off >> 3));  // compiled_link_code (may be null)
    const usize b_store_pos = m_code_buf.size();
    emit(0);  // B .store

    // .miss: host_link = nullptr
    const usize miss_pos = m_code_buf.size();
    emit_mov_imm64(17, 0);

    // .store: push {return_rip, X17} at rsb[top & 15], then top++
    const usize store_pos = m_code_buf.size();
    m_code_buf[cbz_l1_pos] = cbz_x(16, static_cast<i32>(miss_pos - cbz_l1_pos) * 4);
    m_code_buf[cbz_block_pos] =
        cbz_x(3, static_cast<i32>(miss_pos - cbz_block_pos) * 4);
    m_code_buf[bne_pos] =
        b_cond(NE, static_cast<i32>(miss_pos - bne_pos) * 4);
    m_code_buf[b_store_pos] =
        b_imm(static_cast<i32>(store_pos - b_store_pos) * 4);

    emit(ldr_w_uoff(3, REG_JIT_CTX, top_off >> 2));
    emit(ubfx_x(4, 3, 0, kIdxBits));
    emit(lsl_x_imm(4, 4, 4));  // byte offset = idx * 16

    if (rsb_off <= 4095) {
        emit(add_x_imm(16, REG_JIT_CTX, rsb_off));
    } else {
        // Large offsetof: materialize in X16 then add ctx (W2 scratch).
        emit_mov_imm32(2, rsb_off);
        emit(add_x(16, REG_JIT_CTX, 2));
    }
    emit(add_x(16, 16, 4));  // X16 = &rsb[idx]

    emit_mov_imm64(2, static_cast<i64>(return_rip));
    emit(str_x_uoff(2, 16, 0));   // guest_rip
    emit(str_x_uoff(17, 16, 1));  // host_link at +8

    emit(add_w_imm(3, 3, 1));
    emit(str_w_uoff(3, REG_JIT_CTX, top_off >> 2));
}

bool JitCompiler::emit_linked_tail_ret(u8 target_reg, NzcvState carry) {
    AttrScope attr_scope(*this, EmitAttribution::ExitProbe);
    // Diagnostic/correctness gate for the return-stack-buffer fast path.
    // Keep ordinary JIT compilation and other linked exits active.
    if (const char* value = std::getenv("VITREO_JIT_LINK_RET");
        value != nullptr && value[0] == '0' && value[1] == '\0') {
        return false;
    }
    return emit_linked_tail_reg(target_reg, carry, true);
}

bool JitCompiler::emit_linked_tail_reg(u8 target_reg, NzcvState carry,
                                      bool try_rsb_first, bool native_ret,
                                      bool api_call_tail) {
    AttrScope attr_scope(*this, EmitAttribution::ExitProbe);
    if (!try_rsb_first) {
        if (const char* value = std::getenv("VITREO_JIT_LINK_INDIRECT");
            value != nullptr && value[0] == '0' && value[1] == '\0') {
            return false;
        }
    }
    if (!s_jit_link_enabled.load(std::memory_order_acquire)) {
        return false;
    }
    // target_reg is live through the lookup. X16/X17/W3/W4/W5 are scratch.
    if (target_reg == 16 || target_reg == 17 || target_reg == 3
        || target_reg == 4 || target_reg == 5) {
        return false;
    }
    constexpr u32 cc_off =
        static_cast<u32>(offsetof(BasicBlock, compiled_link_code));
    static_assert((cc_off & 7) == 0,
                  "BasicBlock::compiled_link_code must be 8-byte aligned");
    static_assert(cc_off < 32768,
                  "BasicBlock::compiled_link_code offset must fit ldr_x_uoff");
    static_assert(offsetof(BasicBlock, start_addr) == 0,
                  "emit_linked_tail_reg assumes start_addr at offset 0");
    // Index-formula asserts live in emit_l1_index().
    constexpr u32 state_rip_off = static_cast<u32>(offsetof(CpuState, rip));
    static_assert((state_rip_off & 7) == 0,
                  "CpuState::rip must be 8-byte aligned");
    static_assert((state_rip_off >> 3) < 4096,
                  "CpuState::rip must fit STR X unsigned offset");

    using namespace arm64;
    if (carry != NzcvState::Carried) {
        emit_store_nzcv_kind(carry);
    }
    // A RET whose native continuation frame matches returns from here with
    // exactly the flag state this tail would hand its L1 target.
    if (native_ret) emit_native_ret_check(target_reg);

    if (shared_link_probe_enabled()) {
        const u8 kind = try_rsb_first ? kProbeRet : kProbeL1Counted;
        if (materialize_link_probe(kind) != nullptr) {
            if (target_reg != REG_SCRATCH0) {
                emit(mov_x(REG_SCRATCH0, target_reg));
            }
            emit_shared_link_probe(kind);
            if (api_call_tail) {
                emit_shared_api_call_exit();
            } else {
                emit_shared_continue_exit(false);
            }
            return true;
        }
    }

    constexpr u32 rsb_off = rsb_base_off();
    constexpr u32 top_off = rsb_top_off();
    constexpr u32 sticky_rip = sticky_rip_off();
    constexpr u32 sticky_host = sticky_host_off();
    constexpr u32 sticky_i = sticky_i_off();
    constexpr u8 kIdxBits = 4;
    // Every lookup below validates the exact guest RIP before using a host
    // link. Thunks are not compiled and therefore miss naturally. Keeping a
    // separate range check here used two flag-setting comparisons on every
    // indirect edge, forcing RET to spill and refill live x86 flags even on a
    // warm RSB/sticky hit. Equality probes and the hop-limit check are kept
    // flag-neutral instead, so the predecessor's PSTATE can flow unchanged
    // into the linked target.

    // Optional sticky → RSB prefix. Shared .hop_and_br keeps a single NZCV
    // fill for depth accounting (sticky/RSB/L1 hits are alternate paths).
    usize sticky0_bne = 0;
    usize sticky0_cbz = 0;
    usize sticky0_b_hop = 0;
    usize sticky1_bne = 0;
    usize sticky1_cbz = 0;
    usize sticky1_b_hop = 0;
    usize rsb_b_l1_empty = 0;
    usize rsb_b_l1_null = 0;
    usize rsb_bne_l1 = 0;
    usize rsb_b_hop = 0;
    bool rsb_emitted = false;

    if (try_rsb_first) {
        rsb_emitted = true;

        // 2-deep sticky ring: probe slot0 then slot1 before RSB.
        emit(ldr_x_uoff(17, REG_JIT_CTX, sticky_rip >> 3));
        emit(ldr_x_uoff(16, REG_JIT_CTX, sticky_host >> 3));
        emit(eor_x(17, target_reg, 17));
        sticky0_bne = m_code_buf.size();
        emit(0);  // CBNZ X17 → .sticky1
        sticky0_cbz = m_code_buf.size();
        emit(0);  // CBZ X16 → .sticky1
        // DIAGNOSTIC: ret_sticky_*[0] supplied the host link.
        emit_link_hit_counter(
            static_cast<u32>(offsetof(JitContext, ret_sticky0_hits)));
        sticky0_b_hop = m_code_buf.size();
        emit(0);  // B .hop_and_br (host already in X16)

        // .sticky1:
        const usize sticky1_pos = m_code_buf.size();
        m_code_buf[sticky0_bne] =
            cbnz_x(17, static_cast<i32>(sticky1_pos - sticky0_bne) * 4);
        m_code_buf[sticky0_cbz] =
            cbz_x(16, static_cast<i32>(sticky1_pos - sticky0_cbz) * 4);

        emit(ldr_x_uoff(17, REG_JIT_CTX, (sticky_rip + 8) >> 3));
        emit(ldr_x_uoff(16, REG_JIT_CTX, (sticky_host + 8) >> 3));
        emit(eor_x(17, target_reg, 17));
        sticky1_bne = m_code_buf.size();
        emit(0);  // CBNZ X17 → .rsb_or_l1
        sticky1_cbz = m_code_buf.size();
        emit(0);  // CBZ X16 → .rsb_or_l1
        // DIAGNOSTIC: ret_sticky_*[1] supplied the host link.
        emit_link_hit_counter(
            static_cast<u32>(offsetof(JitContext, ret_sticky1_hits)));
        sticky1_b_hop = m_code_buf.size();
        emit(0);  // B .hop_and_br

        // .rsb_or_l1:
        const usize rsb_or_l1_pos = m_code_buf.size();
        m_code_buf[sticky1_bne] =
            cbnz_x(17, static_cast<i32>(rsb_or_l1_pos - sticky1_bne) * 4);
        m_code_buf[sticky1_cbz] =
            cbz_x(16, static_cast<i32>(rsb_or_l1_pos - sticky1_cbz) * 4);

        emit(ldr_w_uoff(3, REG_JIT_CTX, top_off >> 2));
        rsb_b_l1_empty = m_code_buf.size();
        emit(0);  // CBZ W3 → .l1 (empty)

        emit(sub_w_imm(3, 3, 1));
        emit(str_w_uoff(3, REG_JIT_CTX, top_off >> 2));

        emit(ubfx_x(4, 3, 0, kIdxBits));
        emit(lsl_x_imm(4, 4, 4));
        if (rsb_off <= 4095) {
            emit(add_x_imm(16, REG_JIT_CTX, rsb_off));
        } else {
            emit_mov_imm32(17, rsb_off);
            emit(add_x(16, REG_JIT_CTX, 17));
        }
        emit(add_x(16, 16, 4));  // &rsb[idx]

        emit(ldr_x_uoff(17, 16, 0));  // guest_rip
        emit(eor_x(17, 17, target_reg));
        rsb_bne_l1 = m_code_buf.size();
        emit(0);  // CBNZ X17 → .l1 (top already popped)

        emit(ldr_x_uoff(16, 16, 1));  // host_link
        rsb_b_l1_null = m_code_buf.size();
        emit(0);  // CBZ X16 → .l1 (already popped; do not clear RSB)

        // DIAGNOSTIC: the software RSB supplied the host link.
        emit_link_hit_counter(
            static_cast<u32>(offsetof(JitContext, ret_rsb_hits)));
        rsb_b_hop = m_code_buf.size();
        emit(0);  // B .hop_and_br
    }

    // .l1:
    const usize l1_pos = m_code_buf.size();
    if (rsb_emitted) {
        m_code_buf[rsb_b_l1_empty] =
            cbz_w(3, static_cast<i32>(l1_pos - rsb_b_l1_empty) * 4);
        m_code_buf[rsb_bne_l1] =
            cbnz_x(17, static_cast<i32>(l1_pos - rsb_bne_l1) * 4);
        m_code_buf[rsb_b_l1_null] =
            cbz_x(16, static_cast<i32>(l1_pos - rsb_b_l1_null) * 4);
    }

    emit(ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_L1_BASE >> 3));
    const usize cbz_l1_pos = m_code_buf.size();
    emit(0);

    // index = rip & (L1-1); slot = l1[index]
    emit_l1_index(3, target_reg);
    // X17 = &m_l1[idx]; one LDP then gets both the tag and the entry pointer.
    // This is the indirect/RET dispatch tail — the OTHER hot path, and the one
    // a switch-dispatch loop takes on its jump table. Same reasoning as
    // emit_cont_tail_stub_body(): the slot used to be a BasicBlock*, so this
    // was three dependent loads deep.
    static_assert(sizeof(BlockCache::L1Entry) == 32,
                  "reg tail scales the L1 set index by 64");
    emit(add_x_lsl(17, 16, 3, 6));    // X17 = &set (way 0)

    // An empty slot has tag 0 and fails the compare like any other miss, so
    // the old "slot is null" branch is gone.
    emit(ldp_x_off(3, 16, 17, 0));   // X3 = tag, X16 = link_code (way 0)
    emit(eor_x(3, 3, target_reg));
    // Way-0 hits fall through; way 1 is probed out of line after the BR (see
    // emit_cont_tail_stub_body).
    const usize w0_miss = m_code_buf.size();
    emit(0);                          // CBNZ X3 -> .way1
    usize bne_pos = static_cast<usize>(-1);

    const usize hit_pos = m_code_buf.size();
    const usize cbz_cc_pos = m_code_buf.size();
    emit(0);

    (void)cc_off;

    // DIAGNOSTIC: only the L1 path falls through to here; the sticky/RSB hits
    // branch to .hop_and_br below and so skip this counter.
    emit_link_hit_counter(static_cast<u32>(
        try_rsb_first ? offsetof(JitContext, ret_l1_hits)
                      : offsetof(JitContext, ind_l1_hits)));

    // .hop_and_br: host in X16 (sticky/RSB hit branch here; L1 falls through)
    const usize hop_pos = m_code_buf.size();
    if (rsb_emitted) {
        m_code_buf[sticky0_b_hop] =
            b_imm(static_cast<i32>(hop_pos - sticky0_b_hop) * 4);
        m_code_buf[sticky1_b_hop] =
            b_imm(static_cast<i32>(hop_pos - sticky1_b_hop) * 4);
        m_code_buf[rsb_b_hop] =
            b_imm(static_cast<i32>(hop_pos - rsb_b_hop) * 4);
    }

    static_assert(kMaxLinkedHops == (1u << 12),
                  "hop-cap UBFX #12 must match the limit");
    // ADD W22 + UBFX + CBNZ are flag-neutral, so the guest's carried NZCV can
    // flow into the linked target; the count stays in a register, so a hop
    // pays no JitContext load/store for it.
    emit(add_w_imm(REG_LOOP_CTR, REG_LOOP_CTR, 1));
    emit(kHopCapTestWord);  // UBFX W4, W22, #12, #1
    const usize bhs_pos = m_code_buf.size();
    emit(0);

    // MRU-write sticky ring on every successful RET hop (not JMP/CALL).
    if (try_rsb_first) {
        emit(ldr_w_uoff(3, REG_JIT_CTX, sticky_i >> 2));
        emit(ubfx_x(4, 3, 0, 1));       // idx = i & 1
        emit(add_w_imm(3, 3, 1));
        emit(str_w_uoff(3, REG_JIT_CTX, sticky_i >> 2));
        emit(lsl_x_imm(4, 4, 3));       // byte offset = idx * 8
        if (sticky_rip <= 4095) {
            emit(add_x_imm(17, REG_JIT_CTX, sticky_rip));
        } else {
            emit_mov_imm32(3, sticky_rip);
            emit(add_x(17, REG_JIT_CTX, 3));
        }
        emit(str_x_reg(target_reg, 17, 4));
        if (sticky_host <= 4095) {
            emit(add_x_imm(17, REG_JIT_CTX, sticky_host));
        } else {
            emit_mov_imm32(3, sticky_host);
            emit(add_x(17, REG_JIT_CTX, 3));
        }
        emit(str_x_reg(16, 17, 4));
    }
    emit(br(16));
    const usize hit_end = m_code_buf.size();

    // .way1: probe way 1 at +32, leaving X17 on the matching way. A way-1
    // hit is still a hop, so this stays with the hot lines.
    m_code_buf[w0_miss] =
        cbnz_x(3, static_cast<i32>(m_code_buf.size() - w0_miss) * 4);
    emit(add_x_imm(17, 17, 32));
    emit(ldp_x_off(3, 16, 17, 0));   // way 1
    emit(eor_x(3, 3, target_reg));
    bne_pos = m_code_buf.size();
    emit(0);                          // CBNZ X3 -> miss
    usize duplicate_hit = static_cast<usize>(-1);
    if (can_duplicate_l1_suffix(m_code_buf.data() + hit_pos, hit_end - hit_pos)) {
        duplicate_hit = m_code_buf.size();
        for (usize i = hit_pos; i < hit_end; ++i) emit(m_code_buf[i]);
    } else {
        emit(b_imm((static_cast<i32>(hit_pos) -
                    static_cast<i32>(m_code_buf.size())) * 4));  // B .hit
    }

    // ---- DIAGNOSTIC: per-cause landing pads --------------------------------
    // Same shape as emit_cont_tail_stub_body(). Reaching any of these on a
    // RET tail means the sticky ring, the RSB and L1 ALL failed, which is
    // exactly the return-predictor miss the report needs.
    const bool exit_stats = jit_exit_stats_enabled();
    std::vector<usize> pad_to_fallback;
    auto make_pad = [&](JitExitReason reason) -> usize {
        if (!exit_stats) return static_cast<usize>(-1);
        const usize pos = m_code_buf.size();
        emit_exit_reason(reason);
        pad_to_fallback.push_back(m_code_buf.size());
        emit(0);  // B .fallback -- patched below
        return pos;
    };
    const usize pad_l1_null = make_pad(
        try_rsb_first ? JitExitReason::RetTailLinkDisabled
                      : JitExitReason::IndTailLinkDisabled);
    // *L1Empty no longer has a site of its own: an empty slot has tag 0 and
    // now fails the address compare like any other miss.
    const usize pad_addr = make_pad(
        try_rsb_first ? JitExitReason::RetTailL1AddrMismatch
                      : JitExitReason::IndTailL1AddrMismatch);
    const usize pad_no_code = make_pad(
        try_rsb_first ? JitExitReason::RetTailNoCompiledCode
                      : JitExitReason::IndTailNoCompiledCode);
    const usize pad_hop_cap = make_pad(
        try_rsb_first ? JitExitReason::RetTailHopCap
                      : JitExitReason::IndTailHopCap);
    // ------------------------------------------------------------------------

    const usize fallback_pos = m_code_buf.size();
    for (const usize pos : pad_to_fallback) {
        m_code_buf[pos] =
            b_imm(static_cast<i32>(fallback_pos - pos) * 4);
    }
    auto patch_off_to = [&](usize from, usize pad) -> i32 {
        const usize dest = (pad != static_cast<usize>(-1)) ? pad : fallback_pos;
        return static_cast<i32>(dest - from) * 4;
    };
    m_code_buf[cbz_l1_pos]    = cbz_x(16, patch_off_to(cbz_l1_pos, pad_l1_null));
    m_code_buf[bne_pos]       = cbnz_x(3, patch_off_to(bne_pos, pad_addr));
    m_code_buf[cbz_cc_pos]    = cbz_x(16, patch_off_to(cbz_cc_pos, pad_no_code));
    m_code_buf[bhs_pos]       = cbnz_w(4, patch_off_to(bhs_pos, pad_hop_cap));
    if (duplicate_hit != static_cast<usize>(-1)) {
        const usize no_code = duplicate_hit + cbz_cc_pos - hit_pos;
        const usize cap = duplicate_hit + bhs_pos - hit_pos;
        m_code_buf[no_code] = cbz_x(16, patch_off_to(no_code, pad_no_code));
        m_code_buf[cap] = cbnz_w(4, patch_off_to(cap, pad_hop_cap));
    }

    // Successful links keep the dynamic target live in a register and never
    // expose CpuState::rip or jit_iters. Publish both only when the probe
    // actually returns to the dispatcher; the linked target publishes its
    // own RIP before any faulting operation.
    emit(str_x_uoff(target_reg, REG_CPU_STATE, state_rip_off >> 3));
    emit(str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                    jit_iters_offset() >> 2));
    // An indirect CALL/JMP misses L1 on every API call (a thunk is never a
    // block): offer the target to the in-chain thunk exit. Anything else
    // leaves through the plain continue exit, unchanged.
    if (api_call_tail) {
        emit_shared_api_call_exit();
    } else {
        emit_shared_continue_exit(false);
    }
    return true;
}

// ---------------------------------------------------------------------------
// L3: the in-chain API-call exit.
//
// Entered by a B from an API-call tail whose L1 probe missed (or from a
// constant CALL/JMP to a thunk), with CpuState::rip and jit_iters published
// and the predecessor's NZCV live in PSTATE. That is the same state the
// shared continue exit is entered with, and every path out of the body that
// does not resume in translated code is that continue exit, so a target
// that is not an in-chain export (any guest code, a kernel32 thunk, the CFG
// dispatch) costs the 12-word slot test on top of today's dispatcher round
// trip, and only an in-chain export calls out.
//
// One body per block, in two parts. The HOT part is what every miss runs:
//
//   .test:   slot = (rip - thunk_lo) >> 4; leave via .plain unless
//            slot < map.count and map.eligible[slot] (JitContext::
//            inchain_slot_map, the registry's per-slot InChainClass bit).
//            SUB/LSR/LDR/CBZ only: flag-neutral, PSTATE untouched.
//            B .call                            -- to the cold part
//   .plain:  B shared continue exit (live-PSTATE persist + epilogue)
//
// The COLD part (after m_hot_end_pos, moved out of line under split
// placement) is what only an in-chain export runs:
//
//   .call:   persist live NZCV; store pins     -- CpuState is the whole guest
//            LDR X16, [X20, #inchain_trampoline]; BLR X16
//            ... returns here ONLY to resume, with X0 = the successor's link
//            entry; every other outcome leaves execute() from the
//            trampoline itself, without touching arena code
//            MOV X16, X0
//            reload pins, X14 (linear base), X15 (check map) from CpuState /
//            JitContext -- the call clobbered X0-X18 and changed RAX/RSP
//            restore NZCV from CpuState exactly as execute()'s asm does
//            counted hop (ADD W22; UBFX; CBNZ .hop) -- an API-calling loop
//            still returns to the dispatcher for the pump and suspension
//            BR X16
//   .hop:    STR W22 -> jit_iters; B .plain (pins and NZCV equal CpuState)
//
// The two parts are joined by plain B (imm26), which the split relocator
// re-targets; neither part has a PC-relative word of any other form.
// The only V registers the block keeps are the XMM register cache, which is
// write-through and empty at every block entry, so the C++ call's V-register
// clobbers lose nothing. W22 (the hop count), X19-X21 and X23-X28 are
// callee-saved across the call; X29 is saved by the trampoline.
// ---------------------------------------------------------------------------
void JitCompiler::emit_shared_api_call_exit() {
    // An entry-authorized load block holds a GuestMemory protection lease
    // for this native entry only (execute() takes and drops it): a host
    // call made under it could wait on a writer that waits on the lease.
    if (!inchain_thunks_codegen() || m_jit_ctx.inchain_trampoline == nullptr ||
        m_stack_entry_load_proof) {
        emit_shared_continue_exit(false);
        return;
    }
    if (!shared_exits_enabled()) {
        // Inline copy (VITREO_JIT_SHARED_EXITS=0): both parts in place.
        usize to_cold = 0;
        usize plain = 0;
        emit_api_call_exit_hot(to_cold, plain);
        patch_api_call_b(to_cold, m_code_buf.size());
        emit_api_call_exit_cold(plain);
        return;
    }
    m_api_exit_sites.push_back(m_code_buf.size());
    emit(0);  // B .api_exit -- patched by emit_flush_api_call_exit_stubs
}

void JitCompiler::patch_api_call_b(usize from, usize to) {
    const i64 words = static_cast<i64>(to) - static_cast<i64>(from);
    // B is imm26 (+-128 MiB): out of reach, the block is declined to the
    // interpreter (cold_branch_disp), like every other deferred branch.
    const i32 disp = cold_branch_disp(from, to, 26, "api-call exit");
    // The body's site often sits right before the hot part. A branch to the
    // next word is still a taken branch for the front end.
    m_code_buf[from] = words == 1 ? arm64::nop() : arm64::b_imm(disp);
}

void JitCompiler::emit_api_call_exit_hot(usize& to_cold, usize& plain) {
    using namespace arm64;
    // Shared slot test: .plain must be ONE word (the B to the continue exit)
    // so the test's eligible return, X30 + 4, is the B .call below it.
    if (shared_exits_enabled() && shared_dispatch_tail_enabled() &&
        materialize_dispatch_tail(kTailApiTest) != nullptr) {
        emit(adr_x(30, 3 * 4));  // .plain
        emit(ldr_x_uoff(16, REG_JIT_CTX,
                        (JIT_CTX_OFF_DISPATCH_TAIL >> 3) + kTailApiTest));
        emit(br(16));
        plain = m_code_buf.size();
        emit_shared_continue_exit(false);
        assert(m_code_buf.size() == plain + 1);
        to_cold = m_code_buf.size();
        emit(0);  // B .call (cold part) -- patched when the cold part is placed
        return;
    }
    emit_api_test_inline(to_cold, plain);
    emit_shared_continue_exit(false);
}

void JitCompiler::emit_api_test_body() {
    using namespace arm64;
    usize to_eligible = 0;
    usize plain = 0;
    emit_api_test_inline(to_eligible, plain);
    // .plain, not eligible: back to the site's .plain.
    emit(br(30));
    // Eligible: the site's B .call, one word past .plain.
    const usize eligible = m_code_buf.size();
    m_code_buf[to_eligible] = b_imm(static_cast<i32>(eligible - to_eligible) * 4);
    emit(add_x_imm(30, 30, 4));
    emit(br(30));
}

void JitCompiler::emit_api_test_inline(usize& to_cold, usize& plain) {
    using namespace arm64;
    constexpr u32 rip_off = static_cast<u32>(offsetof(CpuState, rip));
    constexpr u32 lo_off =
        static_cast<u32>(offsetof(JitContext, thunk_region_lo));
    constexpr u32 map_off =
        static_cast<u32>(offsetof(JitContext, inchain_slot_map));
    static_assert(rip_off % 8 == 0 && rip_off / 8 < 4096);
    static_assert(lo_off % 8 == 0 && lo_off / 8 < 4096);
    static_assert(map_off % 8 == 0 && map_off / 8 < 4096);
    static_assert(kInChainSlotShift == 4,
                  "the slot index LSR below assumes 16-byte thunk spacing");
    // .test: slot = (rip - thunk_lo) >> 4; leave unless slot < map.count and
    // map[slot] != 0. A rip below thunk_lo wraps to an index far past any
    // count, so one unsigned compare covers both ends.
    emit(ldr_x_uoff(16, REG_CPU_STATE, rip_off >> 3));
    emit(ldr_x_uoff(17, REG_JIT_CTX, lo_off >> 3));
    emit(sub_x(16, 16, 17));
    emit(lsr_x_imm(16, 16, kInChainSlotShift));  // X16 = slot index
    emit(ldr_x_uoff(17, REG_JIT_CTX, map_off >> 3));
    emit(ldr_x_uoff(3, 17, 0));                    // X3 = map.count
    emit(sub_x(3, 16, 3));
    emit(lsr_x_imm(3, 3, 63));                     // 1 iff slot < count
    const usize past_count = m_code_buf.size();
    emit(0);  // CBZ X3, .plain
    emit(add_x_imm(17, 17, 8));
    emit(ldrb_reg(3, 17, 16));                     // W3 = map.eligible[slot]
    const usize not_eligible = m_code_buf.size();
    emit(0);  // CBZ W3, .plain
    to_cold = m_code_buf.size();
    emit(0);  // B .call (cold part) -- patched when the cold part is placed
    // .plain
    plain = m_code_buf.size();
    // Both are CBZ (imm19, +-1 MiB) forward by a few words.
    assert(plain - past_count < (1u << 18) && "api-call exit branch range");
    m_code_buf[past_count] =
        cbz_x(3, static_cast<i32>(plain - past_count) * 4);
    m_code_buf[not_eligible] =
        cbz_w(3, static_cast<i32>(plain - not_eligible) * 4);
}

void JitCompiler::emit_api_call_exit_cold(usize plain) {
    using namespace arm64;
    constexpr u32 tramp_off =
        static_cast<u32>(offsetof(JitContext, inchain_trampoline));
    constexpr u32 kind_off = static_cast<u32>(offsetof(CpuState, nzcv_kind));
    constexpr u32 bits_off = static_cast<u32>(offsetof(CpuState, nzcv_bits));
    static_assert(tramp_off % 8 == 0 && tramp_off / 8 < 4096,
                  "inchain_trampoline must fit LDR X unsigned offset");
    static_assert(kind_off < 4096, "nzcv_kind must fit LDRB unsigned offset");
    static_assert(bits_off % 4 == 0 && bits_off / 4 < 4096);
    static_assert(kMaxLinkedHops == (1u << 12),
                  "hop-cap UBFX #12 must match the limit");
    // .call: the export is in-chain; hand the crossing to the runtime.
    emit_persist_live_nzcv();
    emit_store_pins();
    emit(ldr_x_uoff(16, REG_JIT_CTX, tramp_off >> 3));
    emit(0xD63F0200u);  // BLR X16
    emit(mov_x(16, 0));
    m_vregs_clobbered = true;
    emit_load_pins();
    if (m_linear_base_pinned) emit_load_linear_base();
    if (m_block_is_64bit && m_cfg.linear64 && m_linear_base_pinned) {
        emit_load_linear64_check_map();
    }
    // NZCV, exactly as execute() restores it before entering a block: a
    // constant thunk tail can hand over a live kind (a Tier-0 fused Jcc leg,
    // x86 JMP rel, a block-cap fallthrough), and a Jcc-first successor then
    // branches on PSTATE.
    emit(ldrb_uoff(3, REG_CPU_STATE, kind_off));
    const usize no_kind = m_code_buf.size();
    emit(0);  // CBZ W3, .nzcv_done
    emit(ldr_w_uoff(3, REG_CPU_STATE, bits_off >> 2));
    emit(msr_nzcv(3));
    m_code_buf[no_kind] =
        cbz_w(3, static_cast<i32>(m_code_buf.size() - no_kind) * 4);
    // Counted hop: ADD W22 + UBFX + CBNZ are flag-neutral.
    emit(add_w_imm(REG_LOOP_CTR, REG_LOOP_CTR, 1));
    emit(kHopCapTestWord);  // UBFX W4, W22, #12, #1
    const usize hop_cap = m_code_buf.size();
    emit(0);  // CBNZ W4, .hop
    emit(br(16));
    // .hop
    m_code_buf[hop_cap] =
        cbnz_w(4, static_cast<i32>(m_code_buf.size() - hop_cap) * 4);
    emit(str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, jit_iters_offset() >> 2));
    const usize back = m_code_buf.size();
    emit(0);  // B .plain (hot part)
    patch_api_call_b(back, plain);
}

void JitCompiler::emit_flush_api_call_exit_stubs() {
    AttrScope attr_scope(*this, EmitAttribution::ExitOther);
    if (m_api_exit_sites.empty()) return;
    if (m_api_exit_hot == static_cast<usize>(-1)) {
        m_api_exit_hot = m_code_buf.size();
        emit_api_call_exit_hot(m_api_exit_to_cold, m_api_exit_plain);
    }
    for (const usize pos : m_api_exit_sites) {
        patch_api_call_b(pos, m_api_exit_hot);
    }
    m_api_exit_sites.clear();
}

void JitCompiler::emit_flush_api_call_exit_cold() {
    AttrScope attr_scope(*this, EmitAttribution::ExitOther);
    if (m_api_exit_hot == static_cast<usize>(-1) || m_api_exit_cold_done) {
        return;
    }
    m_api_exit_cold_done = true;
    patch_api_call_b(m_api_exit_to_cold, m_code_buf.size());
    emit_api_call_exit_cold(m_api_exit_plain);
}

// ---------------------------------------------------------------------------
// Process-wide linked-tail probes and RSB push.
//
// The probe bodies above are identical in every block: they read only the
// target, the running thread's JitContext (X20), W22 and the scratch
// registers. One immortal copy per kind replaces a per-block copy of 15-50
// words. A site keeps everything block-specific: its NZCV-kind store, its
// native-RET check, and its miss continuation (the block's shared continue
// or in-chain API exit). The probe is BLR'd and RETs the link target in X16,
// or zero after publishing RIP and the loop counter exactly as the inline
// fallback does. The hop itself stays a per-site BR X16. X30 is free inside
// a block: the chain's dispatcher LR is on the stack.
//
// The shared probe is opt-in (VITREO_JIT_LINK_PROBE_SHARED=1). It is on
// every linked hop, and the extra BLR/RET and shared branches between a
// call site and the callee's RET cost ~35 cycles per CALL or RET hop on
// call_ret (1.86 s -> 3.0-3.5 s), though an isolated BLR/RET round trip
// costs ~2: the polymorphic RET BR loses the path history that predicted
// it. Only exit-side tails (never on a hop) are shared by default.
// ---------------------------------------------------------------------------
bool JitCompiler::shared_link_probe_enabled() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("VITREO_JIT_LINK_PROBE_SHARED");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    if (jit_exit_stats_enabled()) return false;
    const int forced = shared_exit_stubs_override();
    return forced >= 0 ? forced != 0 : on;
}

// Opt-in (VITREO_JIT_RSB_PUSH_SHARED=1) for the same reason: the BLR/RET on
// every x86 CALL cost x86 call_ret 18% (3.20 s -> 3.78 s).
bool JitCompiler::shared_rsb_push_enabled() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("VITREO_JIT_RSB_PUSH_SHARED");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    const int forced = shared_exit_stubs_override();
    return forced >= 0 ? forced != 0 : on;
}

bool JitCompiler::emit_shared_link_probe(u8 kind) {
    AttrScope attr_scope(*this, EmitAttribution::ExitProbe);
    if (materialize_link_probe(kind) == nullptr) return false;
    using namespace arm64;
    emit(ldr_x_uoff(16, REG_JIT_CTX, (JIT_CTX_OFF_LINK_PROBE >> 3) + kind));
    emit(0xD63F0200u);  // BLR X16
    emit(cbz_x(16, 2 * 4));  // .miss
    emit(br(16));
    return true;
}

void JitCompiler::emit_link_probe_body(u8 kind) {
    using namespace arm64;
    constexpr u8 t = REG_SCRATCH0;
    constexpr u32 state_rip_off = static_cast<u32>(offsetof(CpuState, rip));
    static_assert(offsetof(BlockCache::L1Entry, tag) == 0 &&
                      offsetof(BlockCache::L1Entry, link_code) == 8 &&
                      sizeof(BlockCache::L1Entry) == 32,
                  "link probe LDPs tag/link_code and scales the set by 64");
    static_assert(kMaxLinkedHops == (1u << 12),
                  "hop-cap UBFX #12 must match the limit");
    const bool ret = kind == kProbeRet;
    const bool counted = kind != kProbeL1;

    // Every miss branch, patched to .fallback at the end.
    struct Miss { usize pos; u8 op; u8 reg; };  // op: 0 CBZ X, 1 CBNZ X, 2 CBZ W, 3 CBNZ W
    std::vector<Miss> misses;
    std::vector<usize> to_hop;
    auto local = [&](usize from, usize to) {
        return static_cast<i32>(static_cast<i64>(to) - static_cast<i64>(from)) * 4;
    };

    usize l1_skips[3]{};
    usize l1_skip_n = 0;
    if (ret) {
        constexpr u32 sticky_rip = sticky_rip_off();
        constexpr u32 sticky_host = sticky_host_off();
        constexpr u32 rsb_off = rsb_base_off();
        constexpr u32 top_off = rsb_top_off();
        constexpr u8 kIdxBits = 4;
        static_assert(JitContext::kRsbDepth == (1u << kIdxBits));
        // Sticky ring, slot 0 then slot 1.
        for (u32 slot = 0; slot < 2; ++slot) {
            emit(ldr_x_uoff(17, REG_JIT_CTX, (sticky_rip + slot * 8) >> 3));
            emit(ldr_x_uoff(16, REG_JIT_CTX, (sticky_host + slot * 8) >> 3));
            emit(eor_x(17, t, 17));
            const usize bne = m_code_buf.size();
            emit(0);
            const usize cbz = m_code_buf.size();
            emit(0);
            to_hop.push_back(m_code_buf.size());
            emit(0);  // B .hop
            m_code_buf[bne] = cbnz_x(17, local(bne, m_code_buf.size()));
            m_code_buf[cbz] = cbz_x(16, local(cbz, m_code_buf.size()));
        }
        // Software RSB: pop the top entry, use it only on an exact RIP match.
        emit(ldr_w_uoff(3, REG_JIT_CTX, top_off >> 2));
        l1_skips[l1_skip_n++] = m_code_buf.size();
        emit(0);  // CBZ W3 -> .l1
        emit(sub_w_imm(3, 3, 1));
        emit(str_w_uoff(3, REG_JIT_CTX, top_off >> 2));
        emit(ubfx_x(4, 3, 0, kIdxBits));
        emit(lsl_x_imm(4, 4, 4));
        if (rsb_off <= 4095) {
            emit(add_x_imm(16, REG_JIT_CTX, rsb_off));
        } else {
            emit_mov_imm32(17, rsb_off);
            emit(add_x(16, REG_JIT_CTX, 17));
        }
        emit(add_x(16, 16, 4));
        emit(ldr_x_uoff(17, 16, 0));
        emit(eor_x(17, 17, t));
        l1_skips[l1_skip_n++] = m_code_buf.size();
        emit(0);  // CBNZ X17 -> .l1
        emit(ldr_x_uoff(16, 16, 1));
        l1_skips[l1_skip_n++] = m_code_buf.size();
        emit(0);  // CBZ X16 -> .l1
        to_hop.push_back(m_code_buf.size());
        emit(0);  // B .hop
        const usize l1 = m_code_buf.size();
        m_code_buf[l1_skips[0]] = cbz_w(3, local(l1_skips[0], l1));
        m_code_buf[l1_skips[1]] = cbnz_x(17, local(l1_skips[1], l1));
        m_code_buf[l1_skips[2]] = cbz_x(16, local(l1_skips[2], l1));
    }

    // .l1
    emit(ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_L1_BASE >> 3));
    misses.push_back({m_code_buf.size(), 0, 16});
    emit(0);
    emit_l1_index(3, t);
    emit(add_x_lsl(17, 16, 3, 6));
    emit(ldp_x_off(3, 16, 17, 0));
    emit(eor_x(3, 3, t));
    const usize w0_miss = m_code_buf.size();
    emit(0);  // CBNZ X3 -> .way1
    const usize hit = m_code_buf.size();
    misses.push_back({m_code_buf.size(), 0, 16});
    emit(0);  // CBZ X16 -> .fallback (not compiled)

    // .hop
    const usize hop = m_code_buf.size();
    for (const usize pos : to_hop) m_code_buf[pos] = b_imm(local(pos, hop));
    if (counted) {
        emit(add_w_imm(REG_LOOP_CTR, REG_LOOP_CTR, 1));
        emit(kHopCapTestWord);  // UBFX W4, W22, #12, #1
        misses.push_back({m_code_buf.size(), 3, 4});
        emit(0);
    }
    if (ret) {
        // MRU-write the sticky ring on every successful RET hop.
        constexpr u32 sticky_rip = sticky_rip_off();
        constexpr u32 sticky_host = sticky_host_off();
        constexpr u32 sticky_i = sticky_i_off();
        emit(ldr_w_uoff(3, REG_JIT_CTX, sticky_i >> 2));
        emit(ubfx_x(4, 3, 0, 1));
        emit(add_w_imm(3, 3, 1));
        emit(str_w_uoff(3, REG_JIT_CTX, sticky_i >> 2));
        emit(lsl_x_imm(4, 4, 3));
        if (sticky_rip <= 4095) {
            emit(add_x_imm(17, REG_JIT_CTX, sticky_rip));
        } else {
            emit_mov_imm32(3, sticky_rip);
            emit(add_x(17, REG_JIT_CTX, 3));
        }
        emit(str_x_reg(t, 17, 4));
        if (sticky_host <= 4095) {
            emit(add_x_imm(17, REG_JIT_CTX, sticky_host));
        } else {
            emit_mov_imm32(3, sticky_host);
            emit(add_x(17, REG_JIT_CTX, 3));
        }
        emit(str_x_reg(16, 17, 4));
    }
    emit(arm64::ret());
    const usize hit_end = m_code_buf.size();

    // .way1
    m_code_buf[w0_miss] = cbnz_x(3, local(w0_miss, m_code_buf.size()));
    emit(add_x_imm(17, 17, 32));
    emit(ldp_x_off(3, 16, 17, 0));
    emit(eor_x(3, 3, t));
    misses.push_back({m_code_buf.size(), 1, 3});
    emit(0);
    if (can_duplicate_l1_suffix(m_code_buf.data() + hit, hit_end - hit)) {
        const usize duplicate = m_code_buf.size();
        for (usize i = hit; i < hit_end; ++i) emit(m_code_buf[i]);
        const usize original_misses = misses.size();
        for (usize i = 0; i < original_misses; ++i) {
            const Miss m = misses[i];
            if (m.pos >= hit && m.pos < hit_end)
                misses.push_back({duplicate + m.pos - hit, m.op, m.reg});
        }
    } else {
        emit(b_imm(local(m_code_buf.size(), hit)));
    }

    // .fallback: exactly the inline tails' dispatcher publication.
    const usize fallback = m_code_buf.size();
    for (const Miss& m : misses) {
        const i32 d = local(m.pos, fallback);
        m_code_buf[m.pos] = m.op == 0 ? cbz_x(m.reg, d)
                          : m.op == 1 ? cbnz_x(m.reg, d)
                          : m.op == 2 ? cbz_w(m.reg, d)
                                      : cbnz_w(m.reg, d);
    }
    emit(str_x_uoff(t, REG_CPU_STATE, state_rip_off >> 3));
    emit(str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, jit_iters_offset() >> 2));
    emit(movz_x(16, 0, 0));
    emit(arm64::ret());
}

void JitCompiler::emit_rsb_push_body() {
    using namespace arm64;
    constexpr u32 cc_off =
        static_cast<u32>(offsetof(BasicBlock, compiled_link_code));
    constexpr u32 rsb_off = rsb_base_off();
    constexpr u32 top_off = rsb_top_off();
    constexpr u8 kIdxBits = 4;
    static_assert(JitContext::kRsbDepth == (1u << kIdxBits));
    // X2 = guest return RIP. L1 lookup of it -> host link in X17 (0 on miss),
    // the same probe emit_rsb_push's inline copy makes.
    emit(ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_L1_BASE >> 3));
    const usize cbz_l1 = m_code_buf.size();
    emit(0);  // CBZ X16 -> .miss
    emit_l1_index(3, 2);
    emit(add_x_lsl(17, 16, 3, 6));     // X17 = &set (way 0)
    emit(ldr_x_uoff(3, 17, 24 >> 3));  // way0.block
    const usize w0_null = m_code_buf.size();
    emit(0);  // CBZ X3 -> .way1
    emit(ldr_x_uoff(16, 3, 0));
    emit(eor_x(16, 16, 2));
    const usize w0_hit = m_code_buf.size();
    emit(0);  // CBZ X16 -> .hit
    const usize way1 = m_code_buf.size();
    emit(ldr_x_uoff(3, 17, 56 >> 3));  // way1.block
    const usize w1_null = m_code_buf.size();
    emit(0);  // CBZ X3 -> .miss
    emit(ldr_x_uoff(16, 3, 0));
    emit(eor_x(16, 16, 2));
    const usize w1_ne = m_code_buf.size();
    emit(0);  // CBNZ X16 -> .miss
    const usize hit = m_code_buf.size();
    emit(ldr_x_uoff(17, 3, cc_off >> 3));
    const usize b_store = m_code_buf.size();
    emit(0);  // B .store
    const usize miss = m_code_buf.size();
    emit(movz_x(17, 0, 0));
    const usize store = m_code_buf.size();
    auto d = [](usize from, usize to) {
        return static_cast<i32>(static_cast<i64>(to) - static_cast<i64>(from)) * 4;
    };
    m_code_buf[cbz_l1] = cbz_x(16, d(cbz_l1, miss));
    m_code_buf[w0_null] = cbz_x(3, d(w0_null, way1));
    m_code_buf[w0_hit] = cbz_x(16, d(w0_hit, hit));
    m_code_buf[w1_null] = cbz_x(3, d(w1_null, miss));
    m_code_buf[w1_ne] = cbnz_x(16, d(w1_ne, miss));
    m_code_buf[b_store] = b_imm(d(b_store, store));
    // .store: rsb[top & 15] = {return RIP, host link}; top++.
    emit(ldr_w_uoff(3, REG_JIT_CTX, top_off >> 2));
    emit(ubfx_x(4, 3, 0, kIdxBits));
    emit(lsl_x_imm(4, 4, 4));
    if (rsb_off <= 4095) {
        emit(add_x_imm(16, REG_JIT_CTX, rsb_off));
    } else {
        emit_mov_imm32(16, rsb_off);
        emit(add_x(16, REG_JIT_CTX, 16));
    }
    emit(add_x(16, 16, 4));
    emit(stp_x_off(2, 17, 16, 0));
    emit(add_w_imm(3, 3, 1));
    emit(str_w_uoff(3, REG_JIT_CTX, top_off >> 2));
    emit(ret());
}

} // namespace vitreo::cpu::jit
