#pragma once

// Private to ops/ops64.cpp and the translation units split out of it
// (ADR-0006 step 3): ops/align_checks.cpp and the families compile_64bit
// dispatches to (ops/mov64.cpp ... ops/sse_move64.cpp). It carries the
// include set the single ops/ops64.cpp had, so name lookup in every part
// is what it was, and sx_imm64, the one file-local helper more than one
// family reads. That stays in an anonymous namespace, so its linkage is
// internal exactly as it was in the single translation unit.

#include <cstdlib>
#include "../jit_helpers.hpp"
#include "../jit_emit_internal.hpp"
#include "../../interpreter/ops/exec_ops.hpp"
#include <vitreo/diagnostics.hpp>

#include <atomic>
#include <optional>
#include <limits>

namespace vitreo::cpu::jit {

namespace {

// Sign-extends an i64 operand-immediate. The decoder already produces a
// 64-bit signed immediate for IMM32/IMM8 forms, so a simple cast suffices.
inline i64 sx_imm64(const Operand& o) noexcept {
    return o.imm;
}

} // namespace

}  // namespace vitreo::cpu::jit
