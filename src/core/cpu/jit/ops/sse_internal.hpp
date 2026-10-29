#pragma once

// Private to the ops/sse*.cpp translation units. compile_sse was split by
// opcode family, one file per family (ADR-0006 step 3); this header holds
// what more than one of those files needs. It includes the include set the
// single ops/sse.cpp had, so name lookup in every part is what it was.
//
// "In this file" in the comments below now means every ops/sse*.cpp.

#include "../jit_helpers.hpp"
#include "core/math/host_fenv.hpp"
#include "core/cpu/decode/vex256_lane_imm.hpp"
#include "core/cpu/sse_float_semantics.hpp"
#include "../../interpreter/ops/exec_ops.hpp"

#include <cstdlib>
#include <limits>

#include <optional>
#include <bit>

// The single choke point for every jit_sse_op helper call in this file.
//
// The helper reads or writes guest memory for every memory-operand form, so
// the call as a whole IS a guest access and has to carry x86-TSO ordering the
// same way the inline 128-bit paths do. ARM64 has no 128-bit acquire/release
// form, so the ordering is a fence, and since the call site cannot tell a
// load form from a store form it takes both positions: fence before (release
// — nothing sinks past a store the helper performs) and fence after (acquire
// — nothing hoists above a load it performed). Register-operand forms touch
// no guest memory and merely pay the fence; emit_vector_tso_fence() is a
// no-op unless vector ordering is on, so that costs nothing by default.
//
// DMB writes no register and no NZCV, so this is transparent to the
// surrounding NZCV spill/fill and branch-fixup bookkeeping.
#define EMIT_SSE_OP_CALL() \
    do { \
        emit_vector_tso_fence(); \
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_SSE_OP >> 3)); \
        emit_semantic_helper_blr(); \
        emit_vector_tso_fence(); \
    } while (0)

// The same contract for the SSE helpers reached through an absolute function
// pointer rather than the jit_ctx sse_op slot (jit_round_op, jit_pcmpxstrx,
// jit_sse41_op, jit_fxsave_fxrstor). Each has a memory-operand form that
// reads or writes guest memory, and — exactly as above — the call site cannot
// tell a load form from a store form, so the fence takes both positions.
//
// The trailing fence sits before the caller's emit_exit_if_pending_raise() on
// purpose: a helper that raised still performed its guest accesses, and the
// fence has to be on that path too, not only on the fall-through.
//
// This does NOT wrap jit_require_align16. That helper takes the address by
// value, checks alignment and may raise; it touches no guest memory, so
// fencing it would order nothing.
#define EMIT_SSE_MEM_HELPER_CALL(FN) \
    do { \
        emit_vector_tso_fence(); \
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(&FN))); \
        emit_semantic_helper_blr(); \
        emit_vector_tso_fence(); \
    } while (0)

namespace vitreo::cpu::jit {

// Defined in ops/sse_vex256.cpp. Callers: compile_sse's VEX bridge
// (ops/sse.cpp) and compile_vex256_lane_split (ops/sse_vex256.cpp).
[[nodiscard]] bool vex256_lane_split_allowed(Opcode op) noexcept;
bool vex256_move_requires_align32(Opcode op);

// Defined in ops/sse_vex_direct.cpp. True for a VEX form whose family
// lowering implements the whole VEX contract (SRC1 from vvvv, VEX.256,
// YMM[255:128] zeroing) and must reach it with prefixes.vex still set.
[[nodiscard]] bool sse_vex_lowered_directly(const DecodedInstruction& instr) noexcept;

} // namespace vitreo::cpu::jit
