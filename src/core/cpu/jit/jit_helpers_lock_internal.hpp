// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_lock_internal.hpp
/// @brief jit_lock_raise_av, shared by the jit_helpers_lock*.cpp translation
/// units.
///
/// PRIVATE: included only by the jit_helpers_lock*.cpp files, never by
/// jit_helpers.hpp. jit_lock_raise_av was a file-local helper (anonymous
/// namespace) in jit_helpers_lock.cpp. It keeps internal linkage here as a
/// `static inline` function, so each includer gets its own copy and no
/// external symbol is created.
///
/// Split out of jit_helpers_lock.cpp.

#pragma once

#include "jit_helpers.hpp"
#include "../seh/seh32.hpp"

namespace vitreo::cpu::jit {

// Intel SDM Vol. 2 LOCK / Vol. 3 paging: a LOCK RMW to an unmapped or
// non-writable address raises #PF/#GP. Mapping that to STATUS_ACCESS_VIOLATION
// matches the ordinary JIT load/store helpers. A silent return leaves GPRs
// unchanged (ZF from the previous insn, addend still in the XADD register) so
// the guest continues as if the RMW completed — Qt QMutex unlock then
// dereferences a null QMutexPrivate at [this+0xc].
static inline void jit_lock_raise_av(CpuState* cpu, u64 addr, bool is_write) noexcept {
    if (cpu == nullptr || cpu->pending_raise.active) return;
    auto& pr = cpu->pending_raise;
    pr.active = true;
    pr.from_helper = true;
    pr.code = STATUS_ACCESS_VIOLATION;
    pr.flags = 0;
    pr.num_args = 2;
    pr.args[0] = is_write ? 1u : 0u;
    pr.args[1] = addr;
}

} // namespace vitreo::cpu::jit
