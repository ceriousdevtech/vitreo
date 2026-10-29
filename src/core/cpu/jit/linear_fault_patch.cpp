// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Site backpatching for the recoverable-fault handler (linear_fault.cpp):
// the lean-guard, range-variant, marker-free crossing-variant and two-word
// TSO site rewrites, and the naked preservation trampoline the handler
// redirects a faulting thread to. All of it runs in normal context, after
// the signal returns, never in the handler.
//
// vitreo_jit_patch_tso_site and the trampoline must stay in this one
// translation unit: the trampoline's `bl _vitreo_jit_patch_tso_site`
// names, by symbol, a function that no header declares and that is
// defined in this file's anonymous namespace.
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

namespace {

std::atomic<u64> g_tso_backpatches{0};
std::atomic<u64> g_range_backpatches{0};
std::atomic<u64> g_lean_backpatches{0};

u32 plain_access(u32 ordered, bool load) noexcept {
    const u8 t = static_cast<u8>(ordered & 31u);
    const u8 n = static_cast<u8>((ordered >> 5) & 31u);
    switch (ordered >> 30) {
        case 0: return load ? arm64::ldrb_uoff(t, n, 0)
                            : arm64::strb_uoff(t, n, 0);
        case 1: return load ? arm64::ldrh_uoff(t, n, 0)
                            : arm64::strh_uoff(t, n, 0);
        case 2: return load ? arm64::ldr_w_uoff(t, n, 0)
                            : arm64::str_w_uoff(t, n, 0);
        default: return load ? arm64::ldr_x_uoff(t, n, 0)
                             : arm64::str_x_uoff(t, n, 0);
    }
}

// VITREO_JIT_VIEW_PATCH_LOG=<file>: one line per rewritten lean guard,
// naming its guest instruction and the guest address the enforcing view
// refused (@p fault, the host address the handler saw, 0 if unknown). The
// patched sites keep the checked path's cost, so their hot weight -- these
// rips against the sampled block weights -- is the gate the JIT view is
// judged by. Normal context (called from the patch trampoline).
void log_lean_patch(const u32* guard, uintptr_t fault) noexcept {
    const char* path = std::getenv("VITREO_JIT_VIEW_PATCH_LOG");
    if (path == nullptr || path[0] == '\0') return;
    JitFaultMetadata meta{};
    const bool have = jit_fault_metadata_for_pc(
                          reinterpret_cast<uintptr_t>(guard), meta) &&
                      meta.has_guest_rip;
    i64 ea = 0;
    bool have_ea = false;
    for (int i = 0; fault != 0 && i < kMaxWindows; ++i) {
        u8* const b = g_views[i].base.load(std::memory_order_acquire);
        if (b == nullptr) continue;
        const usize size = g_views[i].size.load(std::memory_order_acquire);
        const uintptr_t base = reinterpret_cast<uintptr_t>(b);
        if (fault + kLinearJitViewGuardBytes >= base &&
            fault < base + size + kLinearJitViewGuardBytes) {
            ea = static_cast<i64>(fault - base);
            have_ea = true;
            break;
        }
    }
    // Why the view refused: its protection of the page against the host
    // view's (NONE/RW: a page with an uncommitted, guard or NOACCESS part;
    // R/RW: code, a write watch or an R/RW mixed page; RW/RW: a crossing or
    // the window edge). -1 = not known.
    const auto prot_at = [](uintptr_t addr) -> int {
        mach_vm_address_t a = addr;
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64,
                           reinterpret_cast<vm_region_info_t>(&info), &count,
                           &object) != KERN_SUCCESS || a > addr) {
            return -1;
        }
        return info.protection & (VM_PROT_READ | VM_PROT_WRITE);
    };
    int view_prot = -1;
    int host_prot = -1;
    if (have_ea && ea >= 0) {
        view_prot = prot_at(fault);
        for (int i = 0; i < kMaxWindows; ++i) {
            u8* const b = g_windows[i].base.load(std::memory_order_acquire);
            if (b != nullptr && static_cast<usize>(ea) <
                                    g_windows[i].size.load(std::memory_order_acquire)) {
                host_prot = prot_at(reinterpret_cast<uintptr_t>(b) +
                                    static_cast<uintptr_t>(ea));
                break;
            }
        }
    }
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lock(log_mutex);
    if (FILE* f = std::fopen(path, "a")) {
        std::fprintf(f, "lean-patch guard=%p rip=0x%llx ea=0x%llx view=%d host=%d pid=%d\n",
                     static_cast<const void*>(guard),
                     have ? static_cast<unsigned long long>(meta.guest_rip) : 0ull,
                     have_ea ? static_cast<unsigned long long>(ea) : 0ull,
                     view_prot, host_prot, static_cast<int>(::getpid()));
        std::fclose(f);
    }
}

extern "C" void vitreo_jit_patch_tso_site(u32* site, uintptr_t fault) noexcept {
    constexpr u32 kDmbIsh = 0xD5033BBFu;
    // JIT-view lean site: its guard -> B to its checked variant. One aligned
    // word; a concurrent executor runs either the old guard (and takes this
    // same fault at the access) or the branch. A guard already rewritten no
    // longer matches lean_guard, so a second patcher does nothing.
    const u32 head_word =
        std::atomic_ref<u32>{site[0]}.load(std::memory_order_acquire);
    if (uintptr_t variant = 0;
        lean_guard(reinterpret_cast<uintptr_t>(site), variant)) {
        std::atomic_ref<u32> word{site[0]};
        // The word lean_guard validated: a rival patcher's B fails the CAS.
        u32 expected = head_word;
        const i64 displacement =
            static_cast<i64>(variant) - reinterpret_cast<i64>(site);
        pthread_jit_write_protect_np(0);
        const bool completed = word.compare_exchange_strong(
            expected, arm64::b_imm(static_cast<i32>(displacement)),
            std::memory_order_acq_rel);
        sys_icache_invalidate(site, 4);
        pthread_jit_write_protect_np(1);
        if (completed) {
            log_lean_patch(site, fault);
            const u64 n =
                g_lean_backpatches.fetch_add(1, std::memory_order_relaxed) + 1;
            // The patched-site count is the one cost the JIT view keeps (a
            // patched site pays the checked path); log it at powers of two.
            // Normal context: this runs from the trampoline, not the handler.
            if ((n & (n - 1)) == 0) {
                std::fprintf(stderr,
                             "[jit-view] %llu lean sites patched (latest %p)\n",
                             static_cast<unsigned long long>(n),
                             static_cast<void*>(site));
            }
        }
        return;
    }
    // Range-by-fault site: its unmasked LSR -> B to the range variant. One
    // aligned word, as for the crossing variant below.
    if (uintptr_t variant = 0;
        range_variant_site(reinterpret_cast<uintptr_t>(site), variant)) {
        std::atomic_ref<u32> word{site[0]};
        u32 expected = word.load(std::memory_order_acquire);
        const i64 displacement =
            static_cast<i64>(variant) - reinterpret_cast<i64>(site);
        pthread_jit_write_protect_np(0);
        const bool completed = word.compare_exchange_strong(
            expected, arm64::b_imm(static_cast<i32>(displacement)),
            std::memory_order_acq_rel);
        sys_icache_invalidate(site, 4);
        pthread_jit_write_protect_np(1);
        if (completed) {
            g_range_backpatches.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    // Marker-free site: one word, ordered access -> B to its crossing variant.
    // A single aligned word store, so a concurrent executor sees either the
    // ordered access (and takes this same fault) or the branch.
    if (uintptr_t variant = 0; crossing_variant_site(
            reinterpret_cast<uintptr_t>(site), variant)) {
        std::atomic_ref<u32> word{site[0]};
        u32 expected = word.load(std::memory_order_acquire);
        const i64 displacement =
            static_cast<i64>(variant) - reinterpret_cast<i64>(site);
        pthread_jit_write_protect_np(0);
        const bool completed = word.compare_exchange_strong(
            expected, arm64::b_imm(static_cast<i32>(displacement)),
            std::memory_order_acq_rel);
        sys_icache_invalidate(site, 4);
        pthread_jit_write_protect_np(1);
        if (completed) {
            g_tso_backpatches.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    std::atomic_ref<u32> first{site[0]};
    std::atomic_ref<u32> second{site[1]};
    const u32 a = first.load(std::memory_order_acquire);
    const u32 b = second.load(std::memory_order_acquire);
    const u32 family_a = a & 0x3FFFFC00u;
    const u32 family_b = b & 0x3FFFFC00u;
    // Accept the half-patched neighbour too (marker already -> DMB): the
    // faulting thread may be the SECOND one through the window, and finishing
    // the patch here beats re-faulting until the first thread completes. Both
    // CASes below are idempotent against a concurrent patcher.
    const bool load = (b == kTsoPatchMarker || b == kDmbIsh) &&
        (family_a == 0x38BFC000u || family_a == 0x08DFFC00u);
    const bool store = (a == kTsoPatchMarker || a == kDmbIsh) &&
        family_b == 0x089FFC00u;
    if (!load && !store) return;  // another thread already completed it

    pthread_jit_write_protect_np(0);
    bool completed = false;
    if (load) {
        u32 expected = kTsoPatchMarker;
        second.compare_exchange_strong(expected, kDmbIsh,
                                       std::memory_order_acq_rel);
        sys_icache_invalidate(&site[1], 4);
        expected = a;
        completed = first.compare_exchange_strong(
            expected, plain_access(a, true), std::memory_order_acq_rel);
        sys_icache_invalidate(&site[0], 4);
    } else {
        u32 expected = kTsoPatchMarker;
        first.compare_exchange_strong(expected, kDmbIsh,
                                      std::memory_order_acq_rel);
        sys_icache_invalidate(&site[0], 4);
        expected = b;
        completed = second.compare_exchange_strong(
            expected, plain_access(b, false), std::memory_order_acq_rel);
        sys_icache_invalidate(&site[1], 4);
    }
    pthread_jit_write_protect_np(1);
    if (completed) {
        g_tso_backpatches.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

extern "C" __attribute__((naked)) void vitreo_jit_tso_patch_trampoline() {
    __asm__ volatile(
        "sub sp, sp, #672\n"
        "stp x0, x1, [sp, #0]\n"   "stp x2, x3, [sp, #16]\n"
        "stp x4, x5, [sp, #32]\n"  "stp x6, x7, [sp, #48]\n"
        "stp x8, x9, [sp, #64]\n"  "stp x10, x11, [sp, #80]\n"
        "stp x12, x13, [sp, #96]\n" "stp x14, x15, [sp, #112]\n"
        "stp x16, x17, [sp, #128]\n" "str x18, [sp, #144]\n"
        "mrs x9, nzcv\n" "str x9, [sp, #152]\n"
        "stp q0, q1, [sp, #160]\n" "stp q2, q3, [sp, #192]\n"
        "stp q4, q5, [sp, #224]\n" "stp q6, q7, [sp, #256]\n"
        "stp q8, q9, [sp, #288]\n" "stp q10, q11, [sp, #320]\n"
        "stp q12, q13, [sp, #352]\n" "stp q14, q15, [sp, #384]\n"
        "stp q16, q17, [sp, #416]\n" "stp q18, q19, [sp, #448]\n"
        "stp q20, q21, [sp, #480]\n" "stp q22, q23, [sp, #512]\n"
        "stp q24, q25, [sp, #544]\n" "stp q26, q27, [sp, #576]\n"
        "stp q28, q29, [sp, #608]\n" "stp q30, q31, [sp, #640]\n"
        "ldr x0, [sp, #672]\n"
        "ldr x1, [sp, #680]\n"
        "bl _vitreo_jit_patch_tso_site\n"
        "ldp q0, q1, [sp, #160]\n" "ldp q2, q3, [sp, #192]\n"
        "ldp q4, q5, [sp, #224]\n" "ldp q6, q7, [sp, #256]\n"
        "ldp q8, q9, [sp, #288]\n" "ldp q10, q11, [sp, #320]\n"
        "ldp q12, q13, [sp, #352]\n" "ldp q14, q15, [sp, #384]\n"
        "ldp q16, q17, [sp, #416]\n" "ldp q18, q19, [sp, #448]\n"
        "ldp q20, q21, [sp, #480]\n" "ldp q22, q23, [sp, #512]\n"
        "ldp q24, q25, [sp, #544]\n" "ldp q26, q27, [sp, #576]\n"
        "ldp q28, q29, [sp, #608]\n" "ldp q30, q31, [sp, #640]\n"
        "ldp x0, x1, [sp, #0]\n" "ldp x2, x3, [sp, #16]\n"
        "ldp x4, x5, [sp, #32]\n" "ldp x6, x7, [sp, #48]\n"
        "ldp x10, x11, [sp, #80]\n" "ldp x12, x13, [sp, #96]\n"
        "ldp x14, x15, [sp, #112]\n" "ldp x16, x17, [sp, #128]\n"
        "ldr x18, [sp, #144]\n" "ldr x9, [sp, #152]\n"
        "msr nzcv, x9\n" "ldp x8, x9, [sp, #64]\n"
        "add sp, sp, #672\n"
        "ldr x30, [sp], #16\n"
        "ret\n");
}

u64 tso_backpatch_count() noexcept {
    return g_tso_backpatches.load(std::memory_order_relaxed);
}

u64 range_backpatch_count() noexcept {
    return g_range_backpatches.load(std::memory_order_relaxed);
}

u64 lean_site_patch_count() noexcept {
    return g_lean_backpatches.load(std::memory_order_relaxed);
}

} // namespace vitreo::cpu::jit
