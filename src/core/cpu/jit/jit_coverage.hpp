// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_coverage.hpp
/// @brief JIT native-lowering coverage telemetry, split by guest bitness.
///
/// The JIT never fails to compile an instruction: any opcode without a native
/// lowering emits a per-instruction interpreter delegate
/// (`instruction_compilation.cpp`, `jit_interpret_decoded`). Correctness is
/// preserved by construction, so a coverage gap shows up as a *performance*
/// wall rather than a crash — and is therefore invisible without measurement.
///
/// This module answers two questions that the older `VITREO_JIT_FALLBACK_HIST`
/// diagnostic cannot:
///
///   1. **Which bitness needs the lowering?** The old histogram pools 32-bit
///      and 64-bit counts into one table. 64-bit codegen lives in a separate
///      path (`compile_64bit` in `ops/ops64.cpp`) from the 32-bit lowerings, so
///      a pooled count cannot tell you which one to write.
///   2. **What fraction of executed work delegates?** The old histogram reports
///      percentages *of delegates*, with no denominator. "MOVQ is 40%" is
///      unactionable without knowing whether delegates are 1% or 60% of all
///      executed instructions.
///
/// It is also cheap enough to leave on: the runtime path is a pair of relaxed
/// atomic increments into a fixed array, where the older diagnostic took a
/// global `std::mutex` and inserted into an `unordered_map` on every delegated
/// instruction — perturbing the very workload it was measuring.
///
/// Enabled by `VITREO_JIT_COVERAGE=1`. Fully disabled otherwise: every entry
/// point early-returns on a cached flag.

#pragma once

#include "../decode/opcodes.hpp"
#include "isa_form.hpp"

#include <vitreo/types.hpp>

#include <atomic>

namespace vitreo::cpu::jit::coverage {

namespace detail {

/// Tri-state cache of the env knob: -1 unknown, 0 off, 1 on.
///
/// Namespace scope, not a function-local static: on Apple platforms a
/// function-local static costs a `_tlv_get_addr` call for its guard, and this
/// is read once per dispatched block. Declared here so `enabled()` inlines
/// into the caller as a single relaxed load — an out-of-line call in another
/// translation unit was itself a top-20 symbol in a live browser profile even
/// though the knob was off.
extern std::atomic<int> g_coverage_enabled;

/// One-time env read. Out of line and never inlined so the fast path above
/// stays a load-and-branch.
[[nodiscard]] bool coverage_init_slow() noexcept;

}  // namespace detail

/// True when `VITREO_JIT_COVERAGE` is set. Cached; safe on any thread.
[[nodiscard]] inline bool enabled() noexcept {
    const int seen =
        detail::g_coverage_enabled.load(std::memory_order_relaxed);
    if (seen >= 0) return seen != 0;
    return detail::coverage_init_slow();
}

/// Compile-time: one instruction was emitted into a block, keyed by the FORM
/// that was compiled and bucketed three ways (native / helper / delegate).
/// Counts distinct *emitted* instructions, so it measures static coverage —
/// how much of the code we translate, independent of how hot it is.
///
/// Form-keyed because a `case Opcode::X` arm commonly lowers `X` natively in
/// one operand shape and delegates in another; an opcode-keyed counter reports
/// that opcode as covered.
void record_compiled(Opcode op, FormKey form, bool is_64bit,
                     LowerKind kind) noexcept;

/// Query the compile-time bucket counts for one (opcode, form, bitness).
/// Used by the ISA matrix sweep to assert a form was lowered natively without
/// depending on `VITREO_JIT_NO_DELEGATE` (which is process-global and races
/// the block memo — see tests/cpu/test_jit_dispatch_gap.cpp:73-90).
///
/// Unlike every other entry point here this one works with the environment
/// knob off: the sweep enables recording explicitly via `set_enabled`.
[[nodiscard]] u64 compiled_count(Opcode op, FormKey form, bool is_64bit,
                                 LowerKind kind) noexcept;

/// Force recording on or off, independent of `VITREO_JIT_COVERAGE`. Returns
/// the previous state. The matrix sweep turns it on for the duration of one
/// compile and reads `compiled_count`.
bool set_enabled(bool on) noexcept;

/// Drop every counter. The sweep calls this between forms so the counts it
/// reads belong to the compile it just performed.
void reset() noexcept;

/// Runtime: a delegated instruction actually executed. Lock-free. This is the
/// dynamic (execution-weighted) view — the one that ranks which delegated
/// opcodes actually ran.
void record_delegate_exec(Opcode op, bool is_64bit) noexcept;

/// Runtime: a compiled block was entered, contributing `native_insns`
/// natively-executed instructions. Supplies the denominator that turns
/// delegate counts into a percentage of executed work.
///
/// Known undercount: blocks reached through a native inline link (a direct
/// block-to-block jump that never returns to `JitCompiler::execute`) are not
/// counted, and neither is interpreter-only execution outside the JIT. The
/// reported delegate percentage is therefore an **upper bound** on the true
/// delegate share. That is the right direction for a gap list — it will never
/// tell you a gap is smaller than it is — but do not quote it as an absolute.
void record_block_exec(u32 native_insns, bool is_64bit) noexcept;

/// Print the ranked report to stderr. Called at process exit; also safe to call
/// periodically. No-op when disabled or when nothing was recorded.
void dump() noexcept;

}  // namespace vitreo::cpu::jit::coverage
