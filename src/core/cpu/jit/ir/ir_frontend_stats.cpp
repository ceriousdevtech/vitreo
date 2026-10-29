// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_frontend_stats.cpp
/// @brief Tier-1 frontend coverage telemetry (`VITREO_JIT_IR_STATS`)
///
/// Split out of ir_frontend.cpp by pure code motion. Deliberately does not
/// include ir_frontend_internal.hpp: nothing here uses it.

#include "ir_frontend.hpp"

#include "ir_passes.hpp"

#include "core/cpu/decode/decoder.hpp"

#include <atomic>
#include <algorithm>
#include <vector>
#include <string>
#include <mutex>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vitreo::cpu::jit::ir {

// =============================================================================
// Coverage telemetry
// =============================================================================

namespace stats {
namespace {

constexpr usize kReasonCount =
    static_cast<usize>(DeclineReason::BranchReach) + 1;

std::atomic<int>  g_enabled{-1};   ///< -1 unknown, 0 off, 1 on
std::atomic<u64>  g_accepted{0};
std::atomic<u64>  g_declined[kReasonCount]{};
std::atomic<bool> g_atexit_armed{false};

/// Per-opcode first-blocker counts. One slot per `Opcode`, allocated once and
/// only touched when the report is enabled — a few KB of BSS for the answer to
/// "what should I implement next", which is otherwise guesswork.
std::atomic<u32> g_blocking_opcode[static_cast<usize>(Opcode::_COUNT)]{};

/// Flag-demand outcomes, summed over every block the frontend built.
/// `g_flags_kept_by_fault` is the one that decides an M5 design question:
/// it counts definitions that are dead but must still be published because a
/// may-fault op stands between them and their killer. Recovery metadata is
/// what would unlock exactly that population and nothing else, so its size is
/// the honest measure of whether the metadata is worth building.
std::atomic<u64> g_flags_killed{0};
std::atomic<u64> g_flags_kept_by_fault{0};
std::atomic<u64> g_flags_kept_by_use{0};
std::atomic<u64> g_flags_live_out{0};
std::atomic<u64> g_flags_by_recovery{0};
std::atomic<u64> g_recovery_decline[9]{};
std::atomic<u64> g_cross_insn_values{0};
std::atomic<u64> g_register_reads_reused{0};
std::atomic<u64> g_scalar_constants_reused{0};
std::atomic<u64> g_overlays_killed{0};
std::atomic<u64> g_values_dce{0};
std::atomic<u64> g_blocks_seen{0};
std::atomic<u64> g_blocks_with_xmm{0};
std::atomic<u64> g_blocks_with_mmx{0};
std::atomic<u64> g_insns_seen{0};
std::atomic<u64> g_insns_xmm{0};
std::atomic<u64> g_insns_mmx{0};
std::atomic<u64> g_blocks_with_x87{0};
std::atomic<u64> g_insns_x87{0};
std::atomic<u64> g_prefix_decline[16]{};

/// x87 occupies one contiguous run of the opcode enum, [FNCLEX, FDIVR_M64],
/// plus three enumerators appended later. A range test rather than a switch
/// because `opcodes.hpp` maintains the section markers deliberately (the same
/// leverage `fp_rounding_class` uses), and because a census that misses the
/// arithmetic forms would report a game's FP work as absent.
[[nodiscard]] bool is_x87_opcode(Opcode op) noexcept {
    if (op >= Opcode::FNCLEX && op <= Opcode::FDIVR_M64) return true;
    return op == Opcode::FFREE_STI || op == Opcode::FBLD_M80 ||
           op == Opcode::FBSTP_M80;
}
std::atomic<u64> g_total_leas{0};
std::atomic<u64> g_redundant_leas{0};
std::atomic<u64> g_redundant_leas_cross{0};

/// Live-out verdicts, indexed by `LiveOutVerdict`.
std::atomic<u64> g_liveout[6]{};
std::atomic<u32> g_liveout_barrier[static_cast<usize>(Opcode::_COUNT)]{};

void note_pass_stats_impl(const PassStats& st) noexcept {
    g_scalar_constants_reused.fetch_add(st.scalar_constants_reused, std::memory_order_relaxed);
    g_overlays_killed.fetch_add(st.overlays_killed, std::memory_order_relaxed);
    g_values_dce.fetch_add(st.values_dce, std::memory_order_relaxed);
    g_register_reads_reused.fetch_add(st.register_reads_reused, std::memory_order_relaxed);
    g_flags_killed.fetch_add(st.flags_killed, std::memory_order_relaxed);
    g_flags_kept_by_fault.fetch_add(st.flags_kept_by_fault,
                                    std::memory_order_relaxed);
    g_flags_kept_by_use.fetch_add(st.flags_kept_by_use,
                                  std::memory_order_relaxed);
    g_flags_live_out.fetch_add(st.flags_live_out, std::memory_order_relaxed);
    g_flags_by_recovery.fetch_add(st.flags_killed_by_recovery,
                                  std::memory_order_relaxed);
    for (usize i = 0; i < 9; ++i) {
        g_recovery_decline[i].fetch_add(st.recovery_decline_reason[i],
                                        std::memory_order_relaxed);
    }
    g_cross_insn_values.fetch_add(st.cross_instruction_values,
                                  std::memory_order_relaxed);
    g_total_leas.fetch_add(st.total_leas, std::memory_order_relaxed);
    g_redundant_leas.fetch_add(st.redundant_leas, std::memory_order_relaxed);
    g_redundant_leas_cross.fetch_add(st.redundant_leas_cross_insn,
                                     std::memory_order_relaxed);
}

[[nodiscard]] bool init_slow() noexcept {
    const char* env = std::getenv("VITREO_JIT_IR_STATS");
    const bool on = env != nullptr && *env != '\0' && std::strcmp(env, "0") != 0;
    g_enabled.store(on ? 1 : 0, std::memory_order_relaxed);
    return on;
}

}  // namespace

bool enabled() noexcept {
    const int seen = g_enabled.load(std::memory_order_relaxed);
    if (seen >= 0) return seen != 0;
    return init_slow();
}

bool per_block_log() noexcept {
    static const bool on = [] {
        const char* env = std::getenv("VITREO_JIT_IR_STATS");
        return env != nullptr && std::strcmp(env, "2") == 0;
    }();
    return on;
}

void note_pass_stats(const PassStats& stats) noexcept {
    if (!enabled()) return;
    note_pass_stats_impl(stats);
}

void note_prefix_decline(u32 slot) noexcept {
    if (!enabled()) return;
    if (slot < 16) g_prefix_decline[slot].fetch_add(1,
                                                    std::memory_order_relaxed);
}

void note_block_census(const DecodedInstruction* insns, u32 count) noexcept {
    if (!enabled() || insns == nullptr || count == 0) return;
    bool block_xmm = false;
    bool block_mmx = false;
    bool block_x87 = false;
    u32 xmm = 0;
    u32 mmx = 0;
    u32 x87 = 0;
    for (u32 i = 0; i < count; ++i) {
        bool is_xmm = false;
        bool is_mmx = false;
        for (u8 k = 0; k < insns[i].operand_count; ++k) {
            const OperandType t = insns[i].operands[k].type;
            if (t == OperandType::Reg128 || t == OperandType::Mem128) {
                is_xmm = true;
            } else if (t == OperandType::RegMM) {
                is_mmx = true;
            }
        }
        if (is_xmm) { ++xmm; block_xmm = true; }
        if (is_mmx) { ++mmx; block_mmx = true; }
        if (is_x87_opcode(insns[i].opcode)) { ++x87; block_x87 = true; }
    }
    g_blocks_seen.fetch_add(1, std::memory_order_relaxed);
    g_insns_seen.fetch_add(count, std::memory_order_relaxed);
    if (block_xmm) g_blocks_with_xmm.fetch_add(1, std::memory_order_relaxed);
    if (block_mmx) g_blocks_with_mmx.fetch_add(1, std::memory_order_relaxed);
    g_insns_xmm.fetch_add(xmm, std::memory_order_relaxed);
    g_insns_mmx.fetch_add(mmx, std::memory_order_relaxed);
    if (block_x87) g_blocks_with_x87.fetch_add(1, std::memory_order_relaxed);
    g_insns_x87.fetch_add(x87, std::memory_order_relaxed);
}

void note_liveout_barrier(u16 opcode) noexcept {
    if (!enabled()) return;
    const usize slot = static_cast<usize>(opcode);
    if (slot < static_cast<usize>(Opcode::_COUNT)) {
        g_liveout_barrier[slot].fetch_add(1, std::memory_order_relaxed);
    }
}

void note_liveout(LiveOutVerdict verdict) noexcept {
    if (!enabled()) return;
    const usize slot = static_cast<usize>(verdict);
    if (slot < 6) g_liveout[slot].fetch_add(1, std::memory_order_relaxed);
}

namespace {
std::mutex g_verifier_mutex;
std::map<std::string, u64>* g_verifier_reasons = nullptr;
}  // namespace

void note_verifier_reject(const char* why) noexcept {
    if (!enabled() || why == nullptr) return;
    std::string key;
    key.reserve(96);
    bool in_number = false;
    for (const char* c = why; *c != '\0' && key.size() < 160; ++c) {
        const bool digit = (*c >= '0' && *c <= '9') ||
                           (in_number && ((*c >= 'a' && *c <= 'f') || *c == 'x'));
        if (digit) {
            if (!in_number) key.push_back('#');
            in_number = true;
            continue;
        }
        in_number = false;
        key.push_back(*c);
    }
    std::lock_guard<std::mutex> lock(g_verifier_mutex);
    if (g_verifier_reasons == nullptr) {
        g_verifier_reasons = new std::map<std::string, u64>();
    }
    ++(*g_verifier_reasons)[key];
}

void record(const BuildResult& result) noexcept {
    if (!enabled()) return;
    if (!g_atexit_armed.exchange(true, std::memory_order_relaxed))
        std::atexit(+[] { dump(); });
    if (result.ok) {
        g_accepted.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const usize slot = static_cast<usize>(result.reason);
    if (slot < kReasonCount)
        g_declined[slot].fetch_add(1, std::memory_order_relaxed);
    if (result.blocking_opcode <
        static_cast<u32>(Opcode::_COUNT)) {
        g_blocking_opcode[result.blocking_opcode].fetch_add(
            1, std::memory_order_relaxed);
    }
}

void dump() noexcept { dump_to(stderr); }

void dump_to(std::FILE* out) noexcept {
    if (out == nullptr) return;
    const u64 accepted = g_accepted.load(std::memory_order_relaxed);
    u64 declined = 0;
    for (usize i = 0; i < kReasonCount; ++i)
        declined += g_declined[i].load(std::memory_order_relaxed);
    const u64 total = accepted + declined;
    if (total == 0) return;

    // The histogram is FIRST-BLOCKER, not a census: a block declines at its
    // first unsupported instruction and the rest are never classified, because
    // Tier 1 is all-or-nothing and continuing would cost work for a block
    // already lost. So a reason's share is "how often this was the thing that
    // stopped us", which is the sequencing question, not "how much of the
    // guest looks like this".
    std::fprintf(out,
                 "\n=== Tier 1 IR coverage ===\n"
                 "attempts %llu  accepted %llu (%.2f%%)  declined %llu"
                 "  (reasons are first-blocker, not a census)\n",
                 static_cast<unsigned long long>(total),
                 static_cast<unsigned long long>(accepted),
                 100.0 * static_cast<double>(accepted) /
                     static_cast<double>(total),
                 static_cast<unsigned long long>(declined));
    for (usize i = 1; i < kReasonCount; ++i) {
        const u64 n = g_declined[i].load(std::memory_order_relaxed);
        if (n == 0) continue;
        std::fprintf(out, "  %-20s %llu (%.2f%%)\n",
                     decline_reason_name(static_cast<DeclineReason>(i)),
                     static_cast<unsigned long long>(n),
                     100.0 * static_cast<double>(n) /
                         static_cast<double>(total));
    }

    {
        std::vector<std::pair<std::string, u64>> rows;
        {
            std::lock_guard<std::mutex> lock(g_verifier_mutex);
            if (g_verifier_reasons != nullptr) {
                rows.assign(g_verifier_reasons->begin(),
                            g_verifier_reasons->end());
            }
        }
        std::sort(rows.begin(), rows.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        if (!rows.empty()) std::fprintf(out, "  verifier rejections by rule:\n");
        for (usize i = 0; i < rows.size() && i < 16; ++i) {
            std::fprintf(out, "    %9llu  %s\n",
                         static_cast<unsigned long long>(rows[i].second),
                         rows[i].first.c_str());
        }
    }

    // Flag-demand outcomes. Printed next to coverage because the two answer
    // different questions about the same blocks: coverage says what Tier 1
    // can compile, this says how much of the flag work inside an accepted
    // block it manages to remove.
    {
        const u64 killed = g_flags_killed.load(std::memory_order_relaxed);
        const u64 by_fault = g_flags_kept_by_fault.load(
            std::memory_order_relaxed);
        const u64 by_use = g_flags_kept_by_use.load(std::memory_order_relaxed);
        const u64 live_out = g_flags_live_out.load(std::memory_order_relaxed);
        const u64 seen = killed + by_fault + by_use + live_out;
        if (seen != 0) {
            const auto pct = [&](u64 n) {
                return 100.0 * static_cast<double>(n) /
                       static_cast<double>(seen);
            };
            std::fprintf(out,
                         "  flag definitions %llu: killed %llu (%.1f%%)  "
                         "kept-by-fault %llu (%.1f%%)  kept-by-use %llu "
                         "(%.1f%%)  live-out %llu (%.1f%%)\n",
                         static_cast<unsigned long long>(seen),
                         static_cast<unsigned long long>(killed), pct(killed),
                         static_cast<unsigned long long>(by_fault),
                         pct(by_fault),
                         static_cast<unsigned long long>(by_use), pct(by_use),
                         static_cast<unsigned long long>(live_out),
                         pct(live_out));
            const u64 by_rec =
                g_flags_by_recovery.load(std::memory_order_relaxed);
            std::fprintf(out,
                         "    of which killed BY RECOVERY %llu (%.1f%% of "
                         "all definitions)\n",
                         static_cast<unsigned long long>(by_rec),
                         pct(by_rec));
            bool any = false;
            for (usize i = 1; i < 9; ++i) {
                if (g_recovery_decline[i].load(std::memory_order_relaxed))
                    any = true;
            }
            if (any) {
                std::fprintf(out, "    recovery refused because:\n");
                for (usize i = 1; i < 9; ++i) {
                    const u64 n =
                        g_recovery_decline[i].load(std::memory_order_relaxed);
                    if (n == 0) continue;
                    std::fprintf(out, "      %-26s %llu\n",
                                 recovery_decline_name(
                                     static_cast<RecoveryDecline>(i)),
                                 static_cast<unsigned long long>(n));
                }
            }
        }
    }

    // How much of the live-out population a one-instruction look into the
    // successor would reach. This is the measurement that decides whether
    // cross-block flag liveness is worth its correctness surface.
    {
        const u64 killable = g_liveout[0].load(std::memory_order_relaxed);
        const u64 reads = g_liveout[1].load(std::memory_order_relaxed);
        const u64 fault_blocked = g_liveout[2].load(std::memory_order_relaxed);
        const u64 exhausted = g_liveout[3].load(std::memory_order_relaxed);
        const u64 indirect = g_liveout[4].load(std::memory_order_relaxed);
        const u64 undecodable = g_liveout[5].load(std::memory_order_relaxed);
        const u64 seen = killable + reads + fault_blocked + exhausted +
                         indirect + undecodable;
        if (seen != 0) {
            const auto pct = [&](u64 n) {
                return 100.0 * static_cast<double>(n) /
                       static_cast<double>(seen);
            };
            std::fprintf(out,
                         "  live-out blocks %llu: killable %llu (%.1f%%)  "
                         "fault-blocked %llu (%.1f%%)  successor-reads %llu "
                         "(%.1f%%)  scan-exhausted %llu (%.1f%%)  indirect "
                         "%llu (%.1f%%)  undecodable %llu (%.1f%%)\n",
                         static_cast<unsigned long long>(seen),
                         static_cast<unsigned long long>(killable),
                         pct(killable),
                         static_cast<unsigned long long>(fault_blocked),
                         pct(fault_blocked),
                         static_cast<unsigned long long>(reads), pct(reads),
                         static_cast<unsigned long long>(exhausted),
                         pct(exhausted),
                         static_cast<unsigned long long>(indirect),
                         pct(indirect),
                         static_cast<unsigned long long>(undecodable),
                         pct(undecodable));
        }
    }

    // Which prefix stopped a block. Segment overrides are broken out by
    // register because a flat 32-bit process gives DS/ES/SS a zero base —
    // those are free to support — while FS is the TEB and GS is not.
    {
        static const char* kNames[16] = {
            "lock", "repne", "rep", "operand-size", "address-size", "vex",
            "rex", "seg:ES", "seg:CS", "seg:SS", "seg:DS", "seg:FS", "seg:GS",
            "seg:?", "seg:?", "other"};
        bool any = false;
        for (usize i = 0; i < 16; ++i) {
            if (g_prefix_decline[i].load(std::memory_order_relaxed)) any = true;
        }
        if (any) {
            std::fprintf(out, "  prefix declines by prefix:\n");
            for (usize i = 0; i < 16; ++i) {
                const u64 n =
                    g_prefix_decline[i].load(std::memory_order_relaxed);
                if (n == 0) continue;
                std::fprintf(out, "    %-16s %llu\n", kNames[i],
                             static_cast<unsigned long long>(n));
            }
        }
    }

    // How much SSE/MMX the guest actually runs — the M9 demand question.
    // Blocks, not just instructions: Tier 1 is all-or-nothing per block, so a
    // block with ONE xmm instruction is entirely lost to Tier 0.
    {
        const u64 blocks = g_blocks_seen.load(std::memory_order_relaxed);
        const u64 insns = g_insns_seen.load(std::memory_order_relaxed);
        if (blocks != 0 && insns != 0) {
            const u64 bx = g_blocks_with_xmm.load(std::memory_order_relaxed);
            const u64 bm = g_blocks_with_mmx.load(std::memory_order_relaxed);
            const u64 ix = g_insns_xmm.load(std::memory_order_relaxed);
            const u64 im = g_insns_mmx.load(std::memory_order_relaxed);
            std::fprintf(out,
                         "  SSE/MMX census: %llu blocks, %llu instructions; "
                         "XMM in %llu blocks (%.1f%%) / %llu insns (%.1f%%); "
                         "MMX in %llu blocks (%.1f%%) / %llu insns (%.1f%%); "
                         "x87 in %llu blocks (%.1f%%) / %llu insns (%.1f%%)\n",
                         static_cast<unsigned long long>(blocks),
                         static_cast<unsigned long long>(insns),
                         static_cast<unsigned long long>(bx),
                         100.0 * static_cast<double>(bx) /
                             static_cast<double>(blocks),
                         static_cast<unsigned long long>(ix),
                         100.0 * static_cast<double>(ix) /
                             static_cast<double>(insns),
                         static_cast<unsigned long long>(bm),
                         100.0 * static_cast<double>(bm) /
                             static_cast<double>(blocks),
                         static_cast<unsigned long long>(im),
                         100.0 * static_cast<double>(im) /
                             static_cast<double>(insns),
                         static_cast<unsigned long long>(
                             g_blocks_with_x87.load(std::memory_order_relaxed)),
                         100.0 * static_cast<double>(
                             g_blocks_with_x87.load(std::memory_order_relaxed)) /
                             static_cast<double>(blocks),
                         static_cast<unsigned long long>(
                             g_insns_x87.load(std::memory_order_relaxed)),
                         100.0 * static_cast<double>(
                             g_insns_x87.load(std::memory_order_relaxed)) /
                             static_cast<double>(insns));
        }
    }

    // What cross-instruction residency (M6) and EA commoning (M8) would have
    // to work with. `cross-instruction values` is what an allocator holding
    // values across a guest instruction boundary would hold TODAY;
    // `redundant LEAs` is what an EA-commoning pass would fold, and the
    // cross-instruction subset of those is what would then need holding.
    {
        std::fprintf(out, "  scalar constants reused %llu (VITREO_JIT_IR_PASSES mask 0x10)\n",
                     static_cast<unsigned long long>(g_scalar_constants_reused.load(std::memory_order_relaxed)));
        std::fprintf(out, "  register reads reused %llu (VITREO_JIT_IR_PASSES mask 0x8)\n",
                     static_cast<unsigned long long>(g_register_reads_reused.load(std::memory_order_relaxed)));
        std::fprintf(out, "  dead CF/OF overlays %llu, values swept %llu (VITREO_JIT_IR_PASSES mask 0x20)\n",
                     static_cast<unsigned long long>(g_overlays_killed.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_values_dce.load(std::memory_order_relaxed)));
        const u64 xv = g_cross_insn_values.load(std::memory_order_relaxed);
        const u64 rl = g_redundant_leas.load(std::memory_order_relaxed);
        const u64 rlx = g_redundant_leas_cross.load(std::memory_order_relaxed);
        std::fprintf(out,
                     "  cross-instruction values %llu   LEAs %llu of which redundant "
                     "%llu (%llu across instructions)\n",
                     static_cast<unsigned long long>(xv),
                     static_cast<unsigned long long>(g_total_leas.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(rl),
                     static_cast<unsigned long long>(rlx));
    }

    // Which opcodes stop a successor scan. A barrier that turns out to be a
    // classifier blind spot is a different (and much cheaper) fix than one
    // that genuinely reads flags.
    {
        constexpr usize kTopB = 8;
        usize bb[kTopB]{};
        u32 bn[kTopB]{};
        for (usize op = 1; op < static_cast<usize>(Opcode::_COUNT); ++op) {
            const u32 n =
                g_liveout_barrier[op].load(std::memory_order_relaxed);
            if (n == 0) continue;
            for (usize k = 0; k < kTopB; ++k) {
                if (n > bn[k]) {
                    for (usize m = kTopB - 1; m > k; --m) {
                        bn[m] = bn[m - 1];
                        bb[m] = bb[m - 1];
                    }
                    bn[k] = n;
                    bb[k] = op;
                    break;
                }
            }
        }
        if (bn[0] != 0) {
            std::fprintf(out, "  successor-scan barriers:\n");
            for (usize k = 0; k < kTopB && bn[k] != 0; ++k) {
                const char* nm = opcode_name(static_cast<Opcode>(bb[k]));
                std::fprintf(out, "    %-28s %u\n",
                             (nm && nm[0] != '?') ? nm : "?", bn[k]);
            }
        }
    }

    // The actionable half: WHICH instruction blocked, not which category.
    // 30, not 12: after a tranche of opcodes is implemented the next family's
    // tail sinks below a short list, and "no SSE in the top 12" is not the
    // same claim as "no SSE left worth doing".
    constexpr usize kTop = 30;
    usize best[kTop]{};
    u32 best_n[kTop]{};
    for (usize op = 1; op < static_cast<usize>(Opcode::_COUNT); ++op) {
        const u32 n = g_blocking_opcode[op].load(std::memory_order_relaxed);
        if (n == 0) continue;
        for (usize k = 0; k < kTop; ++k) {
            if (n > best_n[k]) {
                for (usize m = kTop - 1; m > k; --m) {
                    best_n[m] = best_n[m - 1];
                    best[m] = best[m - 1];
                }
                best_n[k] = n;
                best[k] = op;
                break;
            }
        }
    }
    if (best_n[0] != 0) {
        std::fprintf(out, "  top blocking opcodes:\n");
        for (usize k = 0; k < kTop && best_n[k] != 0; ++k) {
            // `opcode_name` returns "?" for enumerators it has no entry for,
            // and on a real guest those "?" rows were the two LARGEST — an
            // unnamed blocker is not an actionable one, so print the raw
            // enumerator alongside. Recovering it by hand from opcodes.hpp is
            // the difference between "something unknown blocks 3664 blocks"
            // and knowing which milestone owns it.
            const char* nm = opcode_name(static_cast<Opcode>(best[k]));
            if (nm != nullptr && nm[0] != '?') {
                std::fprintf(out, "    %-28s %u\n", nm, best_n[k]);
            } else {
                std::fprintf(out, "    opcode#%-21u %u\n",
                             static_cast<unsigned>(best[k]), best_n[k]);
            }
        }
    }
}

void reset() noexcept {
    g_accepted.store(0, std::memory_order_relaxed);
    for (usize i = 0; i < kReasonCount; ++i)
        g_declined[i].store(0, std::memory_order_relaxed);
    for (usize i = 0; i < static_cast<usize>(Opcode::_COUNT); ++i)
        g_blocking_opcode[i].store(0, std::memory_order_relaxed);
}

}  // namespace stats

}  // namespace vitreo::cpu::jit::ir
