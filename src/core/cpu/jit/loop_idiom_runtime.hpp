// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// WS2 loop-idiom bulk helper, called from a recognised loop block's prologue.

#pragma once

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"

namespace vitreo::cpu::jit {

/// Run a bulk prefix of m whole iterations of the loop described by the six
/// packed words (loop_idiom::pack), m < loop_idiom::max_iters_per_call(w)
/// (loop_idiom_thresholds.hpp), leaving at
/// least the last non-exiting iteration and the exit iteration to the
/// translated body. Writes only guest memory, the induction registers,
/// cpu->jit_iters and cpu->rip (= the loop start). Returns kDeclined (0)
/// when it declined, in which case nothing at all was changed; kRanTail when
/// it ran and the loop's exit iteration lies directly after the bulk (the
/// block continues into its translated body for the tail, with the pins
/// reloaded from CpuState by the helper call); kRanRedispatch when it ran
/// but more bulk may follow (budget or accessible span reached), so the
/// block re-dispatches at the loop head and the next entry bulks again.
inline constexpr u32 kDeclined = 0;
inline constexpr u32 kRanRedispatch = 1;
inline constexpr u32 kRanTail = 2;
extern "C" u32 jit_loop_idiom(CpuState* cpu, GuestMemory* mem, u64 w0, u64 w1,
                              u64 w2, u64 w3, u64 w4, u64 w5);

}  // namespace vitreo::cpu::jit
