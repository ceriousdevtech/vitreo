#pragma once

// Helpers shared by the Tier-0 emitter translation units.
//
// jit_compiler.cpp was one 8,896-line file, so anything it needed could be a
// file-local static. Splitting the memory lowering out (ADR-0006, step 3) left
// exactly one such helper with callers on both sides. It lives here rather
// than being duplicated -- duplicating an emitter helper is the defect class
// that ADR and its ratchet exist to stop.

#include <cstdio>
#include <cstdlib>

#include "vitreo/types.hpp"
#include "core/cpu/decode/decoder.hpp"

namespace vitreo::cpu::jit {

/// JitCompiler::set_shared_exit_stubs_for_test: -1 (the environment), 0 or 1.
[[nodiscard]] int shared_exit_stubs_override() noexcept;

// Deferred branch patches (a placeholder word emitted now, its displacement
// known only once the stub or join it targets is laid down) go through
// JitCompiler::cold_branch_disp, which declines the whole block to the
// interpreter when the displacement is past the branch's field. The arm64::
// encoders themselves refuse an out-of-range displacement too (they stop
// emission rather than mask), so cold_branch_disp is what turns a block too
// large for one of its branches into a clean decline instead of a fatal stop.

/// The byte displacement from word `from` to word `to` when it fits a branch's
/// imm`imm_bits` word field (26 for B/BL, 19 for B.cond/CBZ/CBNZ, 14 for
/// TBZ/TBNZ). Otherwise `exceeded` is set and 0 is returned, so the caller's
/// placeholder still encodes while its block is being declined.
inline i32 deferred_branch_disp(usize from, usize to, unsigned imm_bits,
                                bool& exceeded) noexcept {
    const i64 words = static_cast<i64>(to) - static_cast<i64>(from);
    const i64 limit = static_cast<i64>(1) << (imm_bits - 1);
    if (words < -limit || words >= limit) {
        exceeded = true;
        return 0;
    }
    return static_cast<i32>(words) * 4;
}

/// Operand-shape predicates shared by the x64 lowering translation units.
///
/// These lived in an anonymous namespace in ops64.cpp, which was fine while
/// that file was one 6,185-line unit. Splitting the shift/rotate family out
/// gave them a second caller, so they move here rather than being copied --
/// the same rule cold_branch_disp follows above, and for the same reason.
inline bool is_mem(const Operand& o) noexcept {
    return !is_register_operand(o.type) && !is_immediate_operand(o.type);
}
inline bool is_imm(const Operand& o) noexcept {
    return is_immediate_operand(o.type);
}

}  // namespace vitreo::cpu::jit
