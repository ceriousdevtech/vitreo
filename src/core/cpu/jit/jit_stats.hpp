// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_stats.hpp
/// @brief Emitted-ARM64 code-quality telemetry: how many ARM instructions each
///        x86 instruction costs, and a raw dump of one block's emitted words.
///
/// Why this exists (.docs/jit-profile-2026-08-22.md): delegation is 0%, flag
/// materialization is ~1 per 8M-iteration run, and `sample` puts 95-100% of
/// guest thread time inside JIT-emitted ARM64. The entire remaining gap to
/// the host x86 runner is therefore *emitted code quality* — and nothing in the tree
/// measured it. `VITREO_JIT_COVERAGE` answers "did we lower it natively?";
/// this answers "how expensively?".
///
/// Two env vars, both off by default and both zero-cost when unset:
///
///   VITREO_JIT_STATS=1          per-opcode and per-block report at exit
///   VITREO_JIT_DISASM=<hex_rip> dump the emitted ARM words for the block
///                               containing that guest RIP (comma-separated
///                               list accepted, up to kMaxDisasmTargets)
///
/// The per-instruction count is exact, not estimated: `JitCompiler` only ever
/// grows `m_code_buf` through `emit()`, one `u32` per ARM instruction, so the
/// delta in `m_code_buf.size()` across one `compile_instruction_impl()` call is
/// precisely the number of ARM instructions that instruction emitted.
///
/// Accounting caveats, stated up front because a measuring instrument that
/// lies is worse than none:
///
///  * **Executions are dispatcher-observed.** A block reached through an inline
///    block link (`stage_b1_inline_block_link.cpp`) never returns to
///    `JitCompiler::execute`, so its hops are not counted — same blind spot as
///    `jit_coverage` and `hot_block_dump`, and it matters most on exactly the
///    tight self-linking loops the bench kernels use. Execution counts are a
///    LOWER bound; the ARM-per-x86 ratios are exact regardless, because they
///    come from compile time. To get exact iteration counts for one block, use
///    `VITREO_COUNT_RIP=0x<addr>` (which does emit a counter, and therefore is
///    not used here).
///  * **Cold Jcc stubs and the prologue/epilogue are block overhead**, not
///    instruction cost: they are emitted by `emit_flush_cold_jcc_exits()` after
///    the instruction loop, so they are reported per block, separately from the
///    per-opcode bodies. A Jcc's true cost is its body plus its share of that
///    tail.
///  * **Recompiles overwrite the composition** for a guest address (SMC can
///    change it) but keep the execution counter, and every compile is counted
///    in the static per-opcode totals.

#pragma once

#include "../decode/opcodes.hpp"

#include <vitreo/types.hpp>

#include <atomic>

namespace vitreo::cpu::jit::stats {

namespace detail {

/// Env-knob caches for the inline gates below. Namespace scope, not
/// function-local statics: on Apple a function-local static costs a
/// `_tlv_get_addr` for its guard, and these are read once per dispatched
/// block and once per compiled instruction. Keeping the gate in the header
/// also removes a cross-TU call from the disabled path, which is the whole
/// point — see jit_coverage.hpp for the same treatment.
extern std::atomic<int> g_enabled;       ///< -1 unknown, 0 off, 1 on
extern std::atomic<int> g_report;        ///< VITREO_JIT_STATS specifically
extern std::atomic<u32> g_disasm_count;  ///< VITREO_JIT_DISASM target count

/// One-time env read. Out of line; never inlined into the fast path.
[[nodiscard]] bool stats_init_slow() noexcept;

}  // namespace detail

/// One compiled x86 instruction's footprint inside the block's code buffer.
/// Populated by `JitCompiler::compile_instruction` and handed to
/// `record_block` / `maybe_dump_block` when the block finishes.
struct InsnSite {
    u64 rip{0};        ///< guest address of the x86 instruction
    u32 opcode{0};     ///< `Opcode` value, kept as u32 to keep this POD-simple
    u32 x86_len{0};    ///< x86 instruction length in bytes
    u32 arm_pos{0};    ///< index of its first word in the block's code buffer
    u32 arm_words{0};  ///< ARM instructions emitted for it (1 word each)
    u8 mem{0};         ///< bit0: has a memory operand; bit1: operand 0 is memory
    u8 native{1};      ///< 0 when it fell through to the interpreter delegate
};

/// True when `VITREO_JIT_STATS` **or** `VITREO_JIT_DISASM` is set: both need
/// the per-instruction emit accounting. Cached in a process-wide atomic; safe
/// on any thread, and one inlined relaxed load on the default-off path.
[[nodiscard]] inline bool enabled() noexcept {
    const int seen = detail::g_enabled.load(std::memory_order_relaxed);
    if (seen >= 0) return seen != 0;
    return detail::stats_init_slow();
}

/// True when `VITREO_JIT_STATS` is set (controls the exit report only).
[[nodiscard]] inline bool report_enabled() noexcept {
    return enabled() && detail::g_report.load(std::memory_order_relaxed) != 0;
}

/// Compile time: one x86 instruction emitted `arm_words` ARM instructions.
/// `native` is false when it fell through to the interpreter delegate.
void record_insn(Opcode op, bool is_64bit, bool native, u32 arm_words) noexcept;

/// Compile time: a block finished. `total_arm_words` is the whole code buffer
/// (prologue + bodies + cold stubs + epilogue); the difference against the sum
/// of `insns[].arm_words` is the block overhead.
void record_block(u64 guest_addr, u32 guest_size, bool is_64bit,
                  u32 total_arm_words, const InsnSite* insns,
                  u32 insn_count) noexcept;

/// Runtime: a compiled block was entered from the dispatcher. Lock-free in the
/// common case (one thread-local last-hit cache); a shared_lock lookup on a
/// block change. See the accounting caveats above.
void record_block_exec(u64 guest_addr) noexcept;

/// Compile time: if any `VITREO_JIT_DISASM` target falls inside
/// [guest_addr, guest_addr + guest_size), print the emitted ARM64 words for
/// this block to stderr, once. `guest_bytes` may be null (x86 bytes then
/// omitted). Never called when disasm is off.
void maybe_dump_block(u64 guest_addr, u32 guest_size, bool is_64bit,
                      const u32* code, u32 code_words, const InsnSite* insns,
                      u32 insn_count, const u8* guest_bytes) noexcept;

/// True when at least one `VITREO_JIT_DISASM` target is configured.
[[nodiscard]] inline bool disasm_enabled() noexcept {
    return enabled() &&
           detail::g_disasm_count.load(std::memory_order_relaxed) != 0;
}

/// Compile time: one direct-link patch slot was emitted for `edge_bit`
/// (a JitCompiler::LinkEdge value). Runtime: `record_link_patch` counts each
/// slot that `try_patch_direct_link` actually turned into a direct branch.
/// A class whose patch count stays far below its slot count is paying the
/// slot's fallthrough words for a branch that never fires.
void record_link_slot(u32 edge_bit) noexcept;
void record_link_patch() noexcept;
/// A block finalize_code declined because a deferred branch could not reach
/// its target (JitCompiler::cold_branch_disp). Counted always -- it happens at
/// compile time only -- and returns this decline's ordinal (1-based), which
/// the caller uses to rate-limit its log line.
u64 record_branch_reach_decline() noexcept;
[[nodiscard]] u64 branch_reach_declines() noexcept;
/// Direct-link sites patched so far in this process. Counted always: patches
/// happen at compile/publish time, never on an executed path.
[[nodiscard]] u64 link_patches_performed() noexcept;

/// Instruction-class attribution of emitted bytes (load, store, ALU+flags,
/// branch, call/ret, SSE, x87, delegate, ...) plus per-block overhead and the
/// overall bytes-per-guest-instruction. Printed by `dump()` ahead of the
/// per-opcode table; folded from every `record_block` call, so it covers all
/// compiled instructions, not just the 40 hottest opcodes.
void dump_class_table() noexcept;

/// Print the report to stderr. Called from the exit reporter; also safe to
/// call by hand. No-op when disabled or when nothing was recorded.
void dump() noexcept;

}  // namespace vitreo::cpu::jit::stats
