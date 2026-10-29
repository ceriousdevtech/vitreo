// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_tier1_first_compile.hpp
/// @brief Scope in which a block's first compile is Tier 1.
///
/// The IR backend suites compile a block once and assert on the IR's
/// emission. Under tiering (jit_compiler_cold_residency.hpp) a first compile
/// is tier 0, so they pin the threshold to 0 for their duration; the tier-up
/// itself is test_jit_ir_tier.cpp's.

#pragma once

#include "core/cpu/jit/jit_compiler.hpp"

namespace vitreo::cpu::jit::test {

struct IrTier1FirstCompile {
    IrTier1FirstCompile() { JitCompiler::set_ir_tier_threshold_for_test(0); }
    ~IrTier1FirstCompile() { JitCompiler::set_ir_tier_threshold_for_test(-1); }
    IrTier1FirstCompile(const IrTier1FirstCompile&) = delete;
    IrTier1FirstCompile& operator=(const IrTier1FirstCompile&) = delete;
};

}  // namespace vitreo::cpu::jit::test
