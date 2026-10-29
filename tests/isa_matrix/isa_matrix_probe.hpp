// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file isa_matrix_probe.hpp
/// @brief Compile one instruction form and report how the JIT lowered it.
///
/// This answers the
/// question the ISA audit only approximates: for THIS form, in THIS bitness,
/// did the JIT emit ARM64 that computes the result, call a C++ helper, or
/// delegate to the interpreter?
///
/// It reads the answer from the coverage record of the compile it just
/// performed. It deliberately does not use `VITREO_JIT_NO_DELEGATE`: that knob
/// is process-global and the compiled-block memo is shared, so in a binary
/// running many cases the answer depends on which case compiled those bytes at
/// that address first (see tests/cpu/test_jit_dispatch_gap.cpp:73-90).

#pragma once

#include "core/cpu/jit/isa_form.hpp"
#include "isa_form_enum.hpp"

namespace vitreo::isa_matrix {

using vitreo::cpu::jit::LowerKind;

struct ProbeResult {
    /// False when the JIT declined to produce a block at all (the compiler
    /// returned nullptr). Distinct from a delegate: nothing ran.
    bool compiled{false};
    LowerKind kind{LowerKind::Delegate};
    /// Emitted ARM64 instruction words, for the per-form size budget.
    unsigned arm_words{0};
};

/// Compile `fc` as a single-instruction block and report the lowering.
/// Each call uses a fresh JitCompiler and a fresh guest address so no
/// process-global cache can answer for a previous form.
[[nodiscard]] ProbeResult probe_form(const FormCase& fc);

}  // namespace vitreo::isa_matrix
