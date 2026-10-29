#pragma once

// Private to ops/fpu.cpp and the x87 families split out of its
// compile_fpu_native (ADR-0006): ops/fpu_control.cpp, fpu_stack.cpp,
// fpu_load.cpp, fpu_store.cpp, fpu_arith_reg.cpp, fpu_arith_mem.cpp,
// fpu_arith_int.cpp, fpu_math.cpp and fpu_compare.cpp.
//
// compile_fpu_native was a single 3,575-line function, so every name here
// could be file-local to ops/fpu.cpp. The family split gave each of them
// callers in several translation units, so they are shared here rather
// than copied.

#include "vitreo/types.hpp"
#include "core/cpu/decode/opcodes.hpp"

namespace vitreo::cpu::jit {

// The operand-kind field (bits 28..31) of jit_fpu_op's packed opcode
// argument, and the W register that marks a forwarded D0 value valid.
// Plain constexpr: const at namespace scope already has internal linkage,
// so each unit has its own copy, as when they were file-local.
constexpr u32 kFpuKindNone = 0;
constexpr u32 kFpuKindMem  = 1;
constexpr u32 kFpuKindSti  = 2;
constexpr u32 kFpuKindM32Value = 3;
constexpr u32 kFpuKindI64Value = 4;
constexpr u8 kX87D0Valid = 6;

// VITREO_JIT_DISABLE_X87_D0_FORWARD. Defined in ops/fpu.cpp, so the
// environment is still read exactly once, at static initialization.
extern const bool kDisableX87D0Forward;

// False when VITREO_JIT_DISABLE_NATIVE_FPU_ARITH is set, or when `op` is
// named by VITREO_JIT_FPU_FORCE_HELPER. Defined in ops/fpu.cpp; both
// variables are read once at static initialization, so the compile path
// calls no getenv.
[[nodiscard]] bool native_arith_allowed(Opcode op) noexcept;

}  // namespace vitreo::cpu::jit
