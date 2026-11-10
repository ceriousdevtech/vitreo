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

/// jit_raise_memory_av for a semantic helper that has CpuState and
/// GuestMemory but no JitContext (jit_sse_op, jit_fpu_op): a failed
/// @p width-byte guest access at @p addr becomes STATUS_ACCESS_VIOLATION with
/// [0] = direction and [1] = the first inaccessible byte (0xFFFF...FFFF read
/// for a non-canonical address), in cpu->pending_raise; first raise wins. The
/// call site drains it with emit_exit_if_pending_raise, so the faulting
/// instruction commits nothing and the block stops there.
void jit_raise_guest_av(CpuState* cpu, GuestMemory* mem, GuestAddr addr,
                        usize width, bool is_write) noexcept;

/// The destination check every CMPXCHG helper (m8/m16/m32/m64, CMPXCHG8B,
/// CMPXCHG16B; LOCK or not) makes before it reads, compares or writes. When
/// the guest may not write all @p width bytes at @p addr it raises the
/// access violation Windows reports -- [0] = 1 (WRITE) even for an
/// uncommitted or NOACCESS page and whether or not the compare would match,
/// [1] = the first byte the guest may not write
/// (tests/oracle_data/cmpxchg_fault_probe.expected) -- and returns true; the
/// helper must then return with registers, flags and memory untouched. Also
/// true when a raise is already pending.
/// CMPXCHG16B (plain or LOCK) with a misaligned m128: #GP(0), which Windows
/// reports as STATUS_ACCESS_VIOLATION with [0] = 0 and [1] =
/// 0xFFFFFFFFFFFFFFFF -- a #GP has no fault address
/// (tests/oracle_data/cmpxchg16b_align_probe.expected). First raise wins.
void jit_raise_cmpxchg16b_misaligned(CpuState* cpu) noexcept;

[[nodiscard]] bool jit_cmpxchg_dest_faults(CpuState* cpu, GuestMemory* mem,
                                           GuestAddr addr,
                                           usize width) noexcept;

} // namespace vitreo::cpu::jit
