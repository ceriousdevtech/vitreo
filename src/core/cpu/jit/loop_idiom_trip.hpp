// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// WS2 closed-form trip solver for a loop's induction exit.

#pragma once

#include "core/cpu/jit/loop_idiom_desc.hpp"

namespace vitreo::cpu::jit::loop_idiom {

struct TripResult {
    enum class Kind : u8 {
        Exit,          ///< the loop leaves at iteration k (1-based)
        NoExitWithin,  ///< iterations 1..k all continue (k <= limit)
    } kind{Kind::NoExitWithin};
    u64 k{0};
};

/// Evaluate `cont` on CMP lhs, rhs at `width` bits (Intel SDM Vol. 2 Jcc:
/// CF = lhs <u rhs, ZF = lhs == rhs, SF != OF <=> lhs <s rhs).
[[nodiscard]] bool eval_cont(Cond cont, u64 lhs, u64 rhs, u32 width) noexcept;

/// Smallest k in [1, limit] at which the loop leaves, where the induction
/// value compared in iteration k is v1 + (k - 1) * step (mod 2^width) and the
/// other operand is `bound`. `ind_is_lhs` says which side of the CMP the
/// induction is on. Equality conditions are solved modularly. Ordered
/// conditions are solved over the prefix of the window in which the value
/// does not wrap in the condition's (signed or unsigned) order, where the
/// predicate is monotone; the result then covers only that prefix
/// (NoExitWithin with k < limit).
[[nodiscard]] TripResult first_induction_exit(Cond cont, bool ind_is_lhs, u64 v1,
                                              i64 step, u32 width, u64 bound,
                                              u64 limit) noexcept;

}  // namespace vitreo::cpu::jit::loop_idiom
