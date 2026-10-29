// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_internal.hpp
/// @brief Helpers shared by the jit_helpers*.cpp translation units.
///
/// PRIVATE to src/core/cpu/jit (not installed, not part of jit_helpers.hpp).
/// jit_helpers.cpp was one 4,197-line file, so both functions below could be
/// file-local statics. Splitting it left each with callers on both sides of
/// a cut, so they are PROMOTED to external linkage and declared here once,
/// rather than duplicated (a duplicated static would be two definitions
/// free to drift apart):
///   - jit_raise_memory_av: defined in jit_helpers_memory_fault.cpp, called
///     by every checked accessor in jit_helpers_memory.cpp. Fault path only.
///   - jit_publish_mem_watch_cpu: defined in jit_helpers_memory.cpp, which
///     can still inline it at its own call sites, and also called by
///     jit_fpu_op in jit_helpers_x87_op.cpp.

#pragma once

#include "jit_helpers.hpp"

namespace vitreo::cpu::jit {

void jit_raise_memory_av(JitContext* ctx, GuestAddr addr, usize width,
                         bool is_write);
void jit_publish_mem_watch_cpu(CpuState* cpu);

} // namespace vitreo::cpu::jit
