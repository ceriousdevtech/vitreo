// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_frontend.cpp
/// @brief Decoded x86 -> canonical IR. See `ir_frontend.hpp` for the scope of
///        this increment and why each excluded family is excluded.
///
/// ## The flag-record operand contract
///
/// A `DefFlags` node names a `CpuState::FlagState::FlagOp`, and that record has
/// two payload words whose meaning is per-op. This frontend fixes the mapping
/// once, here, so the backend never has to guess:
///
///   `DefFlags` operand 0 -> `flag_src`,  operand 1 -> `flag_dst`.
///
/// That is the same order `FlagState::materialize` reads (flag_state.hpp): for
/// `Add32` it reconstructs the original destination as `flag_dst - flag_src`,
/// for `Sub32` as `flag_dst + flag_src`, and for `Neg32` it computes
/// `0 - flag_src` against `flag_dst`. Getting this backwards produces flags
/// that are wrong only for asymmetric operands, which parity tests built from
/// symmetric ones would not catch — so the mapping is asserted by name in the
/// unit tests rather than left to the reader.
///
/// The logic ops (`And32`/`Or32`/`Xor32`) read only `flag_dst`; operand 0 is
/// still supplied so every ALU node has one shape.
///
/// ## Why every accepted op publishes a record
///
/// `FlagRepr::InCpuStateRecord` is the architecturally authoritative home, and
/// it is correct at every point including a fault. Choosing a cheaper
/// representation — proving the definition dead, or keeping it in host NZCV for
/// a nearby consumer — is milestone M3's flag-demand analysis, and it is a
/// *pass over this output*, not a frontend decision. The frontend's job is to
/// state the semantics; deciding what need not be computed is the middle-end's.
///
/// ## Layout
///
/// This file keeps `build_block` and `decline_reason_name`. The admission
/// tables are in `ir_frontend_forms.cpp`, the 32-bit frontend in
/// `ir_frontend32.cpp`, the long-mode frontend in `ir_frontend64*.cpp`, and
/// the coverage telemetry in `ir_frontend_stats.cpp`.

#include "ir_frontend.hpp"
#include "ir_frontend_internal.hpp"

#include "ir_passes.hpp"

#include "core/cpu/decode/decoder.hpp"

#include <string>

namespace vitreo::cpu::jit::ir {

const char* decline_reason_name(DeclineReason reason) noexcept {
    switch (reason) {
        case DeclineReason::None:                return "none";
        case DeclineReason::NoInstructions:      return "no-instructions";
        case DeclineReason::Bitness64:           return "bitness-64";
        case DeclineReason::BlockTooLong:        return "block-too-long";
        case DeclineReason::UnsupportedPrefix:   return "unsupported-prefix";
        case DeclineReason::UnsupportedOpcode:   return "unsupported-opcode";
        case DeclineReason::UnsupportedOperand:  return "unsupported-operand";
        case DeclineReason::UnsupportedWidth:    return "unsupported-width";
        case DeclineReason::VerifierRejected:    return "verifier-rejected";
        case DeclineReason::BackendUnsupported:  return "backend-unsupported";
        case DeclineReason::BackendRegisterPressure:
                                                 return "backend-reg-pressure";
        case DeclineReason::BranchReach:         return "branch-reach";
    }
    return "?";
}

BuildResult build_block(const DecodedInstruction* insns, const u64* rips,
                        u32 count, u64 entry_rip, bool is_64bit,
                        bool recovery_available, IrBlock& out,
                        std::string* verifier_why, u16 recovery_gprs) {
    out.clear();
    out.entry_rip = entry_rip;
    out.is_64bit = is_64bit;
    out.recovery_gprs = recovery_gprs;

    BuildResult result{};
    // `site` names the check that declined, because a join or a noncanonical
    // target blames an instruction that is itself supported.
    const auto give_up = [&](DeclineReason reason, u32 index,
                             const char* site = "form") {
        result.ok = false;
        result.reason = reason;
        result.insn_index = index;
        result.blocking_opcode =
            (index < count && insns != nullptr)
                ? static_cast<u32>(insns[index].opcode) : 0u;
        stats::record(result);
        if (stats::per_block_log()) {
            std::fprintf(stderr,
                "[ir-decline] block=0x%llx insn=0x%llx %s %s opcode=%s\n",
                static_cast<unsigned long long>(entry_rip),
                static_cast<unsigned long long>(
                    index < count && rips != nullptr ? rips[index] : 0),
                decline_reason_name(reason), site,
                index < count && insns != nullptr
                    ? opcode_name(insns[index].opcode) : "-");
        }
        return result;
    };

    if (insns == nullptr || rips == nullptr || count == 0)
        return give_up(DeclineReason::NoInstructions, kNoInsnIndex);
    if (count > kMaxBlockInsns)
        return give_up(DeclineReason::BlockTooLong, kNoInsnIndex);

    // Census of what the guest actually executes, taken BEFORE any decline so
    // the denominator is every block Tier 1 was asked about rather than every
    // block it accepted. Answers the M9 demand question — how much SSE is
    // there to migrate — without depending on the first-blocker histogram,
    // which cannot see an SSE instruction in a block that declined earlier for
    // another reason.
    stats::note_block_census(insns, count);

    IrBuilder builder(out);
    // The most recent flag definition, so a Jcc can name the value it tests.
    // A running variable rather than a search because that is exactly the
    // architectural fact: x86 has ONE flags register, and the last writer owns
    // it. When block input values arrive (M2), this starts at the block's
    // incoming flags value instead of "none".
    ValueId live_flags = kInvalidValue;
    for (u32 i = 0; i < count; ++i) {
        builder.set_rip(rips[i]);
        const u64 next_rip = rips[i] + insns[i].length;
        const usize before = out.insts.size();
        const InsnVerdict verdict = is_64bit
            ? build_long_mode_insn(builder, insns[i], next_rip, live_flags)
            : build_insn(builder, insns[i], next_rip, live_flags);
        if (!verdict.ok) return give_up(verdict.reason, i);
        for (usize j = before; j < out.insts.size(); ++j) {
            const Inst& in = out.insts[j];
            if (in.op == Op::DefFlags) live_flags = in.result;
            // Intel SDM Vol. 2 JMP/CALL/Jcc (64-bit mode): a non-canonical
            // target is #GP(0) AT the branch with nothing committed
            // (tools/oracle_probe/noncanonical_branch_probe.expected). Every
            // target here is a translation-time constant, so such a block is
            // left to the interpreter, which raises it exactly; the decline
            // names the branch itself.
            if (is_64bit && (in.op == Op::Jcc || in.op == Op::Jmp ||
                             in.op == Op::CondExit)) {
                const u64 t = in.branch.target;
                if (static_cast<u64>(static_cast<i64>(t << 16) >> 16) != t) {
                    return give_up(DeclineReason::UnsupportedOperand, i,
                                   "noncanonical-target");
                }
            }
        }
    }

    // A long-mode trace continues past its conditional branches along the
    // fall-through (block_decoder.cpp), so every Jcc but the last is a side
    // exit: taken leaves the block, not-taken runs on. The frontend built
    // each from the live flags at its position, which is the operand a
    // CondExit requires. A 32-bit block keeps its rule that a Jcc ends it.
    //
    // A branch whose target is ANOTHER INSTRUCTION OF THIS BLOCK is a join
    // inside the trace, which the stencil path compiles in place. This IR has
    // no control-flow merge: values and flags defined between the branch and
    // its target would be undefined on the taken edge. Leaving to the target
    // instead is correct but puts a block exit on every taken edge; on
    // always-taken skips (jit_bench many_blocks, submem_jcc) that measured
    // 1.3-1.65x slower than the in-place join, so such a block declines.
    if (is_64bit) {
        for (usize j = 0; j + 1 < out.insts.size(); ++j) {
            if (out.insts[j].op != Op::Jcc) continue;
            const u64 target = out.insts[j].branch.target;
            for (u32 k = 0; k < count; ++k) {
                if (rips[k] == target) {
                    return give_up(DeclineReason::UnsupportedOperand, k,
                                   "in-block-join");
                }
            }
            out.insts[j].op = Op::CondExit;
        }
    }

    // A terminal Jcc may be followed by NOPs, which lower to nothing. Its
    // not-taken edge then runs through them, so it leaves at the block's end.
    if (!out.insts.empty() && out.insts.back().op == Op::Jcc)
        out.insts.back().branch.fallthrough = rips[count - 1] + insns[count - 1].length;

    // Verify structural and encoded fault-state obligations after every pass.
    // Verification does not prove arbitrary semantic equivalence; differential
    // execution tests remain necessary for transformations such as read reuse.
    PassStats pass_stats = run_passes(out, recovery_available);
    if (stats::enabled()) {
        measure_cross_instruction_opportunity(out, pass_stats);
        stats::note_pass_stats(pass_stats);
    }

    // The verifier is a correctness boundary, not a debug convenience: a block
    // that fails it is never emitted, and the failure is counted apart from
    // the coverage gaps so a frontend bug cannot hide as "not supported yet".
    if (!verify(out, verifier_why))
        return give_up(DeclineReason::VerifierRejected, kNoInsnIndex, "verifier");

    result.ok = true;
    result.reason = DeclineReason::None;
    result.insn_index = kNoInsnIndex;
    // NOT recorded here. Whether this block is really accepted depends on the
    // backend too, and only the caller knows that; `ir_attempt_block` records
    // the final verdict. Counting an accept here would report coverage the
    // runtime does not have whenever the frontend runs ahead of the emitter.
    return result;
}

}  // namespace vitreo::cpu::jit::ir
