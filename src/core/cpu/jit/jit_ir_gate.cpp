// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_ir_gate.cpp
/// @brief Tier-1 optimizing-IR gate
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
#include "jit_compile_internal.hpp"

namespace vitreo::cpu::jit {

using namespace jit_detail;

// Phase 2 optimizing-IR gate. Packed policy layout:
//   bits 0-1  : IrBitness
//   bits 2-31 : IrOps mask
u32 JitCompiler::parse_ir_policy(const char* mode, const char* ops) noexcept {
    // The IR mirrors NzcvState as ir::NzcvKind so the IR header
    // can stay free of any jit_compiler.hpp dependency -- that independence is
    // what lets the IR be unit-tested without a JitCompiler at all. It costs a
    // duplicated enumerator list, and a duplicated list silently mistranslates
    // every flag kind the moment someone reorders or inserts into one of them.
    // That would surface as wrong guest flags far from its cause, which is the
    // worst failure mode this subsystem has.
    //
    // Checked inside a member function body rather than at namespace scope
    // because NzcvState is private, and widening the class's public surface
    // just to assert on it would be the wrong trade.
    static_assert(static_cast<u8>(NzcvState::None) ==
                  static_cast<u8>(ir::NzcvKind::None));
    static_assert(static_cast<u8>(NzcvState::Sub) ==
                  static_cast<u8>(ir::NzcvKind::Sub));
    static_assert(static_cast<u8>(NzcvState::And) ==
                  static_cast<u8>(ir::NzcvKind::And));
    static_assert(static_cast<u8>(NzcvState::Add) ==
                  static_cast<u8>(ir::NzcvKind::Add));
    static_assert(static_cast<u8>(NzcvState::IncDec) ==
                  static_cast<u8>(ir::NzcvKind::IncDec));
    static_assert(static_cast<u8>(NzcvState::Sub8) ==
                  static_cast<u8>(ir::NzcvKind::Sub8));
    static_assert(static_cast<u8>(NzcvState::Carried) ==
                  static_cast<u8>(ir::NzcvKind::Carried));
    static_assert(static_cast<u8>(NzcvState::NzOnly) ==
                  static_cast<u8>(ir::NzcvKind::NzOnly));

    u32 bitness = static_cast<u32>(IrBitness::Off);
    if (mode != nullptr && *mode != '\0') {
        const std::string_view v{mode};
        if (v == "32") {
            bitness = static_cast<u32>(IrBitness::Bits32);
        } else if (v == "64") {
            bitness = static_cast<u32>(IrBitness::Bits64);
        } else if (v == "all" || v == "1") {
            bitness = static_cast<u32>(IrBitness::All);
        }
        // Anything else -- including "off", "0" and any typo -- stays Off.
        // Failing closed matters more than accepting a spelling: an
        // unrecognised value that silently ENABLED the IR would be
        // indistinguishable from the control arm in an A/B, and would be
        // discovered as an unexplained correctness difference rather than as
        // a configuration mistake.
    }
    if (bitness == static_cast<u32>(IrBitness::Off)) {
        return bitness;  // ops are meaningless with the IR off
    }
    // Default to every implemented class, so VITREO_JIT_IR=32 alone is useful;
    // VITREO_JIT_IR_OPS narrows it for bisection.
    u32 op_mask = static_cast<u32>(IrOps::Alu);
    if (ops != nullptr && *ops != '\0') {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(ops, &end, 0);
        if (end != ops && *end == '\0') {
            // Bits 29-31 of the packed word are the memo / store tier bits
            // (ir_key_policy).
            op_mask = static_cast<u32>(parsed) & 0x07FFFFFFu;
        }
    }
    return bitness | (op_mask << 2);
}

u32 JitCompiler::ir_policy() noexcept {
    static const u32 policy = parse_ir_policy(std::getenv("VITREO_JIT_IR"),
                                              std::getenv("VITREO_JIT_IR_OPS"));
    return policy;
}

bool JitCompiler::ir_enabled_for(bool is_64bit) noexcept {
    const u32 bitness = ir_policy() & 0x3u;
    switch (static_cast<IrBitness>(bitness)) {
    case IrBitness::Off:    return false;
    case IrBitness::Bits32: return !is_64bit;
    case IrBitness::Bits64: return is_64bit;
    case IrBitness::All:    return true;
    }
    return false;
}

bool JitCompiler::ir_op_enabled(IrOps op) noexcept {
    return ((ir_policy() >> 2) & static_cast<u32>(op)) != 0;
}

const ir::BuildResult& JitCompiler::ir_build_block(const BasicBlock& block) {
    // `instruction_count` is block METADATA and outlives the decoded array:
    // `publish_compiled` releases `instructions` once no generated code refers
    // to it, leaving a block that still reports its count. Trusting the count
    // alone would walk off the end of a freed (or never-shared) array, so the
    // array's own size is the bound and a disagreement declines.
    const u32 count = block.instruction_count;
    if (block.instructions.data() == nullptr ||
        block.instructions.size() < count) {
        m_ir_result = ir::BuildResult{false, ir::DeclineReason::NoInstructions,
                                      ir::kNoInsnIndex};
        ir::stats::record(m_ir_result);
        return m_ir_result;
    }

    // `DecodedInstruction` carries no address (decoder.hpp:272 -- it is
    // retained per guest instruction for every cached block, and the memory
    // budget there forbids growing it), so guest RIPs are re-derived by
    // accumulating `length` from the block start. Same walk as
    // flag_liveness.cpp:389-393.
    m_ir_rips.clear();
    m_ir_rips.reserve(count);
    u64 rip = block.start_addr;
    for (u32 i = 0; i < count; ++i) {
        m_ir_rips.push_back(rip);
        rip += block.instructions[i].length;
    }

    // A 32-bit block's fixed ABI pins all eight legacy GPRs, and only its
    // linear (hardware-fault) path consults recovery metadata: its region-
    // cache helpers raise without reaching the map. A long-mode block's
    // software raises all leave through the block's raise exit, which names
    // the block for execute()'s rebuild (rebuild_flags_after_software_raise)
    // in either memory model, so there only the per-block pin layout limits
    // which registers a plan may name. With pinning disabled, retain records
    // rather than promise an unavailable recovery.
    u16 recovery_gprs = 0x00FF;
    bool recovery_available = m_cfg.linear_jit && m_memory.is_linear() &&
        !g_env_no_pin;
    if (block.is_64bit) {
        // An unpinned register (R8-R15) is written through to CpuState by
        // the instruction that defines it, so a plan may name it too: both
        // fault paths read it there (JitFlagRecovery::kCpuStateGpr).
        // VITREO_JIT_NO_CPUSTATE_RECOVERY=1 restores the pinned-only rule.
        static const bool no_cpu_state_recovery =
            std::getenv("VITREO_JIT_NO_CPUSTATE_RECOVERY") != nullptr;
        recovery_gprs = 0;
        for (u8 reg = 0; reg < 16; ++reg) {
            if (pinned_gpr64(reg) != 0xFF || !no_cpu_state_recovery)
                recovery_gprs |= u16(1u << reg);
        }
        recovery_available = !g_env_no_pin && recovery_gprs != 0;
    }
    // The verifier's reason is only formatted when the coverage report will
    // print it.
    std::string verifier_why;
    m_ir_result = ir::build_block(block.instructions.data(), m_ir_rips.data(),
                                  count, block.start_addr, block.is_64bit,
                                  recovery_available, m_ir_block,
                                  ir::stats::enabled() ? &verifier_why : nullptr,
                                  recovery_gprs);
    if (m_ir_result.reason == ir::DeclineReason::VerifierRejected) {
        ir::stats::note_verifier_reject(verifier_why.c_str());
    }
    return m_ir_result;
}

bool JitCompiler::ir_gate_open(const BasicBlock& block) const {
    // With `VITREO_JIT_IR` unset the shipping default is Tier-1 IR for every
    // 64-bit block and for linear 32-bit guest-base blocks (fail closed: any
    // decline falls back to the stencil path unchanged). Override with
    // VITREO_JIT_IR=0 (never), 32, 64 or all. Non-linear 32-bit still
    // requires an explicit opt-in.
    //
    // An admitted block is not compiled with the IR at once: its first
    // compile is tier 0 (stencils plus an entry counter) and the IR is paid
    // only after VITREO_JIT_IR_TIER_THRESHOLD entries (default
    // kDefaultIrTierThreshold; 0 or 1 = IR on first compile). See
    // jit_compiler_cold_residency.hpp.
    // Read once, like the policy it selects: getenv takes libc's process-wide
    // lock and every compiling thread passes here.
    static const bool ir_env_set = [] {
        const char* ir_mode = std::getenv("VITREO_JIT_IR");
        return ir_mode != nullptr && ir_mode[0] != '\0';
    }();
    if (ir_env_set) {
        if (!ir_enabled_for(block.is_64bit)) return false;
    } else if (!block.is_64bit) {
        if (!(m_cfg.linear_jit && m_memory.is_linear())) return false;
        // Unset env ⇒ behave as VITREO_JIT_IR=32 for this linear 32-bit block.
    }
    // Unset env ⇒ a 64-bit block behaves as under VITREO_JIT_IR=64.
    // Explicit env still honours VITREO_JIT_IR_OPS; the linear default
    // enables the full implemented op mask (same as VITREO_JIT_IR=32).
    return !ir_env_set || ir_op_enabled(IrOps::Alu);
}

void JitCompiler::ir_attempt_block(const BasicBlock& block) {
    // Gate first, and before touching the block. A tier-0 compile never
    // builds the IR: that cost is what tiering defers.
    if (m_tier_stencil || !ir_gate_open(block)) return;
    if (!ir_build_block(block).ok) return;
    // Feasibility is decided HERE, while declining is still free. Emission
    // itself has no failure path (ADR-0002 / ir_emit.cpp).
    m_ir_emit_active = ir_emit_feasible(m_ir_block);
    // Record the FINAL verdict, not the frontend's. A block whose IR verifies
    // but whose backend cannot lower it is declined, and the coverage report
    // has to say so or it reports coverage the runtime does not have.
    if (!m_ir_emit_active) {
        m_ir_result = ir::BuildResult{false, ir_emit_decline_reason(),
                                      ir::kNoInsnIndex, 0};
    }
    ir::stats::record(m_ir_result);
    if (ir::stats::enabled()) ir_measure_cross_block_flags();
}

void JitCompiler::ir_measure_cross_block_flags() noexcept {
    // ADR-0003 measured 72-75% of Tier 1 flag definitions as "live out of the
    // block", which `eliminate_dead_flags` cannot touch because it has no view
    // past the terminator. This counts how many of those a ONE-INSTRUCTION
    // look into the successor would prove dead, before any of that machinery
    // is built.
    //
    // The test applied to the successor's first instruction is
    // `pending_flags_dead_across`, which already exists for the delegate
    // boundary and requires all three of: it redefines every flag; its class
    // never reads flags; and it touches no memory, so it cannot fault
    // part-way and hand an exception record the stale RFLAGS. Reusing it
    // rather than writing a second predicate is deliberate — a cross-block
    // claim that disagreed with the intra-block one would be a silent
    // divergence between two analyses of the same question.
    const ir::IrBlock& ir_block = m_ir_block;
    if (ir_block.insts.empty()) return;

    // Live-out means: a flag definition with no later definition to kill it.
    // Only the LAST one can be live-out, so find it and confirm nothing
    // follows.
    usize last_def = ir_block.insts.size();
    for (usize i = ir_block.insts.size(); i-- > 0;) {
        if (ir_block.insts[i].op == ir::Op::DefFlags &&
            ir_block.insts[i].flags.repr != ir::FlagRepr::Dead) {
            last_def = i;
            break;
        }
    }
    if (last_def == ir_block.insts.size()) return;

    // The terminator carries the successor addresses. An indirect branch
    // (RET, JMP r/m, CALL r/m) has no compile-time successor and can never be
    // proven; count it apart so the unreachable share is visible rather than
    // being folded into "not killable".
    const ir::Inst& term = ir_block.insts.back();
    u64 targets[2] = {ir::kInvalidRip, ir::kInvalidRip};
    switch (term.op) {
    case ir::Op::Jcc:
        targets[0] = term.branch.target;
        targets[1] = term.branch.fallthrough;
        break;
    case ir::Op::Jmp:
        targets[0] = term.branch.target;
        break;
    default:
        ir::stats::note_liveout(ir::stats::LiveOutVerdict::IndirectSuccessor);
        return;
    }

    // Scan forward from each successor until the flags are settled. One
    // instruction is far too shallow a look — real code opens a block with a
    // MOV or a LEA and reaches its first flag writer several instructions in,
    // so a depth-1 test reports almost everything as unreachable.
    //
    // Two verdicts are tracked, and their difference is the whole reason M5's
    // recovery metadata exists: `fault_blocked` is a successor that WOULD be
    // provable except that a memory access stands before the redefinition,
    // and a fault there would expose the very record this block would have
    // skipped publishing.
    constexpr u32 kScanDepth = 8;
    bool fault_blocked = false;
    for (u64 target : targets) {
        if (target == ir::kInvalidRip) continue;
        GuestMappedByteReader reader(m_memory, target);
        bool settled = false;
        for (u32 depth = 0; depth < kScanDepth && !settled; ++depth) {
            DecodedInstruction insn{};
            if (decode_instruction(reader,
                                   m_block_is_64bit ? DecodeMode::Mode64
                                                    : DecodeMode::Mode32,
                                   insn) != DecodeError::Success) {
                ir::stats::note_liveout(
                    ir::stats::LiveOutVerdict::Undecodable);
                return;
            }
            const FlagEffect effect = classify_flag_effect(insn);
            const bool touches_memory = instruction_touches_memory(insn);

            // A full writer settles it — but only if it cannot fault first.
            // `pending_flags_dead_across` folds both conditions together and
            // is the same predicate the delegate boundary uses.
            if (effect == FlagEffect::FullWriter && !touches_memory) {
                settled = true;
                break;
            }
            // Anything that may fault before the flags are redefined keeps
            // the record observable. Recorded, not abandoned: this is the
            // population `JitFlagRecovery` addresses.
            if (touches_memory) {
                fault_blocked = true;
                continue;
            }
            // A barrier reads the flags, or preserves part of them, or is
            // unrecognised. Either way the record is live and no depth helps.
            if (effect == FlagEffect::Barrier) {
                // Record WHICH opcode stopped the scan. `classify` returns
                // Barrier for anything it does not recognise, so a large
                // successor-reads share could equally mean "the successors
                // genuinely read flags" or "the classifier is blind to a
                // common opcode" — and those call for opposite work. The
                // same distinction the decline histogram needed.
                ir::stats::note_liveout_barrier(static_cast<u16>(insn.opcode));
                ir::stats::note_liveout(
                    ir::stats::LiveOutVerdict::SuccessorReads);
                return;
            }
            // Neutral: keep scanning.
        }
        if (!settled) {
            ir::stats::note_liveout(ir::stats::LiveOutVerdict::ScanExhausted);
            return;
        }
    }
    ir::stats::note_liveout(fault_blocked
                                ? ir::stats::LiveOutVerdict::FaultBlocked
                                : ir::stats::LiveOutVerdict::Killable);
}

} // namespace vitreo::cpu::jit
