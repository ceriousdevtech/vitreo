// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include <cstdint>

namespace vitreo::cpu::jit {
// Constant-initialized, trivial storage. A JIT thread touches its record before
// native entry; the fatal signal handler reads that same thread's last entry.
// No other thread may overwrite the record while a crash is being reported.
struct JitCrashBreadcrumbs {
    uint64_t last_block_addr{};
    uint32_t region1_base{};
    uint32_t region1_end{};
    uint32_t region2_base{};
    uint32_t region2_end{};
    uint32_t guest_esp{};
    uint64_t region1_host{};
    uint64_t region2_host{};
};
extern thread_local volatile JitCrashBreadcrumbs jit_crash_breadcrumbs;
} // namespace vitreo::cpu::jit
