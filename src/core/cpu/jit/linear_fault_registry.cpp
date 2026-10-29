// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// The tables the recoverable-fault handler (linear_fault.cpp) reads
// lock-free, and their mutex-guarded writers: the registered reservation
// windows and JIT views, the executable JIT code ranges, the shared
// memory-helper range, and LinearReservationRegistration. g_windows,
// g_views, g_shared_helper_lo/hi and g_jit_ranges, which
// linear_fault_internal.hpp declares, are defined here, once.
// jit_code_range_contains, their lock-free reader, stays beside the
// handler.
//
// Split out of linear_fault.cpp as pure code motion: every line below the
// includes is verbatim from that file.

#include <type_traits>
#include <new>
#include "linear_fault.hpp"
#include "linear_fault_internal.hpp"
#include "jit_compiler.hpp"
#include "jit_helpers.hpp"
#include "stencils.hpp"

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/write_watch_traps.hpp"
#include "core/memory/guard_page_traps.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <pthread.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <unistd.h>
#include <mach/mach_vm.h>
#include <mach/mach.h>
#include <mach/vm_param.h>

namespace vitreo::cpu::jit {

Window g_windows[kMaxWindows];
// Registered JIT views (register_linear_jit_view): base = the view's alias of
// guest address 0, size = the reservation's window. Guarded by
// g_windows_mutex for writers, read lock-free by the handler.
Window g_views[kMaxWindows];

// note_shared_mem_helper_range. Read lock-free in the handler.
std::atomic<uintptr_t> g_shared_helper_lo{0};
std::atomic<uintptr_t> g_shared_helper_hi{0};

namespace {

std::mutex g_windows_mutex;
JitCodeRangeSnapshot g_jit_range_snapshots[kMaxJitCodeRanges + 1];
usize g_jit_range_snapshot_count = 0;
std::mutex g_jit_ranges_mutex;

} // namespace

std::atomic<const JitCodeRangeSnapshot*> g_jit_ranges{
    &g_jit_range_snapshots[0]};
static_assert(decltype(g_jit_ranges)::is_always_lock_free,
              "signal-handler range snapshot must be lock-free");

void note_shared_mem_helper_range(uintptr_t lo, uintptr_t hi) noexcept {
    g_shared_helper_hi.store(hi, std::memory_order_release);
    g_shared_helper_lo.store(lo, std::memory_order_release);
}

bool note_jit_code_range(uintptr_t lo, uintptr_t hi) {
    if (lo >= hi) return false;
    std::lock_guard<std::mutex> lk(g_jit_ranges_mutex);
    const JitCodeRangeSnapshot* current =
        g_jit_ranges.load(std::memory_order_relaxed);
    for (usize i = 0; i < current->count; ++i) {
        if (current->ranges[i].lo == lo && current->ranges[i].hi == hi) {
            return true;
        }
    }
    if (current->count >= kMaxJitCodeRanges ||
        g_jit_range_snapshot_count >= kMaxJitCodeRanges) {
        std::fprintf(stderr, "[linear-fault] WARNING: no free JIT range slot\n");
        return false;
    }
    JitCodeRangeSnapshot& next =
        g_jit_range_snapshots[++g_jit_range_snapshot_count];
    next = *current;
    next.ranges[next.count++] = JitCodeRange{lo, hi};
    g_jit_ranges.store(&next, std::memory_order_release);
    return true;
}

bool forget_jit_code_range(uintptr_t lo, uintptr_t hi) {
    if (lo >= hi) return false;
    std::lock_guard<std::mutex> lk(g_jit_ranges_mutex);
    const JitCodeRangeSnapshot* current =
        g_jit_ranges.load(std::memory_order_relaxed);
    usize remove_index = current->count;
    for (usize i = 0; i < current->count; ++i) {
        if (current->ranges[i].lo == lo && current->ranges[i].hi == hi) {
            remove_index = i;
            break;
        }
    }
    if (remove_index == current->count) return false;
    if (g_jit_range_snapshot_count >= kMaxJitCodeRanges) {
        std::fprintf(stderr,
                     "[linear-fault] WARNING: no free JIT range snapshot\n");
        return false;
    }
    JitCodeRangeSnapshot& next =
        g_jit_range_snapshots[++g_jit_range_snapshot_count];
    next.count = 0;
    for (usize i = 0; i < current->count; ++i) {
        if (i != remove_index) {
            next.ranges[next.count++] = current->ranges[i];
        }
    }
    g_jit_ranges.store(&next, std::memory_order_release);
    return true;
}

bool register_linear_reservation(u8* base, usize size) {
    if (base == nullptr || size == 0 || !g_installed.load(std::memory_order_acquire) ||
        g_trampoline.load(std::memory_order_acquire) == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lk(g_windows_mutex);
    for (int i = 0; i < kMaxWindows; ++i) {
        if (g_windows[i].base.load(std::memory_order_relaxed) == nullptr) {
            g_windows[i].size.store(size, std::memory_order_release);
            g_windows[i].base.store(base, std::memory_order_release);
            return true;
        }
    }
    std::fprintf(stderr, "[linear-fault] WARNING: no free reservation slot\n");
    return false;
}

void unregister_linear_reservation(u8* base) {
    std::lock_guard<std::mutex> lk(g_windows_mutex);
    for (int i = 0; i < kMaxWindows; ++i) {
        if (g_windows[i].base.load(std::memory_order_relaxed) == base) {
            g_windows[i].base.store(nullptr, std::memory_order_release);
            g_windows[i].size.store(0, std::memory_order_release);
            return;
        }
    }
}

bool register_linear_jit_view(u8* view, usize window) {
    if (view == nullptr || window == 0 ||
        !g_installed.load(std::memory_order_acquire) ||
        g_trampoline.load(std::memory_order_acquire) == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lk(g_windows_mutex);
    for (int i = 0; i < kMaxWindows; ++i) {
        if (g_views[i].base.load(std::memory_order_relaxed) == nullptr) {
            g_views[i].size.store(window, std::memory_order_release);
            g_views[i].base.store(view, std::memory_order_release);
            return true;
        }
    }
    std::fprintf(stderr, "[linear-fault] WARNING: no free JIT view slot\n");
    return false;
}

void unregister_linear_jit_view(u8* view) {
    std::lock_guard<std::mutex> lk(g_windows_mutex);
    for (int i = 0; i < kMaxWindows; ++i) {
        if (g_views[i].base.load(std::memory_order_relaxed) == view) {
            g_views[i].base.store(nullptr, std::memory_order_release);
            g_views[i].size.store(0, std::memory_order_release);
            return;
        }
    }
}

bool linear_jit_view_registered(u8* view, usize window) {
    if (view == nullptr || window == 0 ||
        !g_installed.load(std::memory_order_acquire)) {
        return false;
    }
    for (int i = 0; i < kMaxWindows; ++i) {
        if (g_views[i].base.load(std::memory_order_acquire) == view &&
            g_views[i].size.load(std::memory_order_acquire) == window) {
            return true;
        }
    }
    return false;
}

bool linear_reservation_registered(u8* base, usize size) {
    if (base == nullptr || size == 0 || !g_installed.load(std::memory_order_acquire) ||
        g_trampoline.load(std::memory_order_acquire) == nullptr) {
        return false;
    }
    for (int i = 0; i < kMaxWindows; ++i) {
        if (g_windows[i].base.load(std::memory_order_acquire) == base &&
            g_windows[i].size.load(std::memory_order_acquire) == size) {
            return true;
        }
    }
    return false;
}

LinearReservationRegistration::~LinearReservationRegistration() {
    reset();
}

bool LinearReservationRegistration::acquire(u8* base, usize size) {
    reset();
    if (!install_linear_fault_handler() ||
        !register_linear_reservation(base, size)) {
        return false;
    }
    m_base = base;
    return true;
}

bool LinearReservationRegistration::acquire_jit_view(u8* view, usize window) {
    if (m_base == nullptr || m_view != nullptr ||
        !register_linear_jit_view(view, window)) {
        return false;
    }
    m_view = view;
    return true;
}

void LinearReservationRegistration::reset() noexcept {
    if (m_view != nullptr) {
        unregister_linear_jit_view(m_view);
        m_view = nullptr;
    }
    if (m_base != nullptr) {
        unregister_linear_reservation(m_base);
        m_base = nullptr;
    }
}

} // namespace vitreo::cpu::jit
