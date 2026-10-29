// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file linear_jit_view.cpp
/// @brief The JIT view: a second, protection-ENFORCING mapping of the linear
///        reservation's pages, used only by translated code.
///
/// See GuestMemory::enable_linear_jit_view() for the contract. Three rules
/// carry all of the correctness:
///
///  1. ALIASING. The two views must map the same pages. mach_vm_remap with
///     copy=FALSE makes them share VM objects, but anything that later
///     replaces the host view's backing (a MAP_FIXED drop on MEM_RELEASE, a
///     remap of shared memory) leaves the JIT view on the OLD pages, so each
///     such site re-aliases. `linear_jit_view_coherent` checks it.
///  2. ENFORCEMENT. A host page's JIT protection never grants what any guest
///     page inside it denies, and never grants WRITE while a store there owes
///     a helper (translated code, write watch).
///  3. FAIL CLOSED. Every decision is serialized with its mprotect under
///     m_jit_view_mutex, and when the host ever refuses a re-alias the whole
///     view is POISONED to PROT_NONE: translated code then faults on every
///     access and takes the checked host-view path, which is slow but exact.

#include "guest_memory.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>

namespace vitreo {

namespace {

constexpr usize kHostPage = usize{1} << kLinearHostPageShift;
constexpr int kProtRW = PROT_READ | PROT_WRITE;

/// Most-restrictive protection shared by a MIXED host page's four guest pages,
/// from its subpage byte (bit k: guest page k readable; bit 4+k: writable).
/// Writable-without-readable has no host form, so it is NONE like no-read.
[[nodiscard]] int mixed_meet(u8 subpage) noexcept {
    constexpr u8 kAllRead = 0x0F;
    constexpr u8 kAllWrite = 0xF0;
    if ((subpage & kAllRead) != kAllRead) return PROT_NONE;
    return (subpage & kAllWrite) == kAllWrite ? kProtRW : PROT_READ;
}

/// The host-view VM entry holding @p addr: its protection (PROT_READ/WRITE
/// bits only) and its end address. False when the kernel reports nothing.
bool host_entry(mach_vm_address_t addr, int& prot, mach_vm_address_t& end) {
    mach_vm_address_t start = addr;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &start, &size, VM_REGION_BASIC_INFO_64,
                       reinterpret_cast<vm_region_info_t>(&info), &count,
                       &object) != KERN_SUCCESS) {
        return false;
    }
    if (start > addr) {
        // A hole: nothing maps addr, so nothing may be granted there.
        prot = PROT_NONE;
        end = start;
        return true;
    }
    prot = info.protection & (VM_PROT_READ | VM_PROT_WRITE);
    end = start + size;
    return true;
}

/// The VM object and object offset backing @p addr, for the coherence check.
struct BackingAt {
    u32 object_id;
    u64 offset;     // object offset of addr itself
    u64 entry_end;  // end of the VM entry holding addr
};

bool backing_at(mach_vm_address_t addr, BackingAt& out) {
    mach_vm_address_t start = addr;
    mach_vm_size_t size = 0;
    natural_t depth = 0;
    vm_region_submap_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    if (mach_vm_region_recurse(mach_task_self(), &start, &size, &depth,
                               reinterpret_cast<vm_region_recurse_info_t>(&info),
                               &count) != KERN_SUCCESS ||
        start > addr) {
        return false;
    }
    out.object_id = info.object_id;
    out.offset = info.offset + (addr - start);
    out.entry_end = start + size;
    return true;
}

bool jit_view_verify_enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("VITREO_JIT_VIEW_VERIFY");
        return v != nullptr && v[0] == '1';
    }();
    return enabled;
}

}  // namespace

bool GuestMemory::enable_linear_jit_view() {
    std::unique_lock lk(m_regions_mutex);
    if (m_linear_jit_view != nullptr) return false;
    if (m_linear_base == nullptr || m_linear_identity) return false;
    if (m_linear_size == 0 || m_linear_size > kLinearJitViewMaxWindow ||
        (m_linear_size & (kHostPage - 1)) != 0) {
        return false;
    }
    // The view mirrors hardware enforcement and decides mixed pages from the
    // subpage map; without either it would have nothing exact to mirror.
    if (!m_linear_hw_protect || m_linear_check_map == nullptr ||
        m_linear_subpage_map == nullptr) {
        return false;
    }
    if (!regions_.empty()) return false;

    const usize total = m_linear_size + 2 * kLinearJitViewGuardBytes;
    void* raw = mmap(nullptr, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);
    if (raw == MAP_FAILED) return false;
    u8* const view = static_cast<u8*>(raw) + kLinearJitViewGuardBytes;
    const auto fail = [&] {
        munmap(raw, total);
        m_linear_jit_view = nullptr;
        return false;
    };

    {
        std::lock_guard view_lock(m_jit_view_mutex);
        m_linear_jit_view = view;
        m_linear_jit_view_poisoned = false;
        linear_jit_view_realias_locked(0, m_linear_size);
        if (m_linear_jit_view_poisoned) return fail();

        // Prove the alias before anything relies on it, on a host page no
        // guest can own (Windows never maps the low 64 KiB) -- once as
        // aliased now, once after the MAP_FIXED drop + re-alias that MEM_RELEASE
        // performs, since that is the path a broken kernel contract would
        // silently corrupt.
        volatile u64* const h = reinterpret_cast<volatile u64*>(m_linear_base);
        volatile u64* const j = reinterpret_cast<volatile u64*>(view);
        bool proven = true;
        for (const u64 magic : {0x5649'5452'554D'4A56ull, 0x4A49'5456'4945'5721ull}) {
            if (mprotect(m_linear_base, kHostPage, kProtRW) != 0 ||
                mprotect(view, kHostPage, PROT_READ) != 0) {
                proven = false;
                break;
            }
            *h = magic;
            proven = proven && *j == magic;
            *h = 0;
            void* dropped = mmap(m_linear_base, kHostPage, PROT_NONE,
                                 MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (dropped != m_linear_base) {
                proven = false;
                break;
            }
            linear_jit_view_realias_locked(0, kHostPage);
            if (!proven || m_linear_jit_view_poisoned) break;
        }
        (void)mprotect(m_linear_base, kHostPage, PROT_NONE);
        if (!proven || m_linear_jit_view_poisoned) {
            std::fprintf(stderr,
                         "[jit-view] the two views do not alias; JIT view off\n");
            return fail();
        }
    }
    std::fprintf(stderr, "[jit-view] JIT view %p..%p aliases the reservation\n",
                 static_cast<void*>(view),
                 static_cast<void*>(view + m_linear_size));
    return true;
}

void GuestMemory::linear_jit_view_realias_locked(usize lo, usize hi) {
    if (m_linear_jit_view == nullptr || lo >= hi) return;
    mach_vm_address_t target =
        reinterpret_cast<mach_vm_address_t>(m_linear_jit_view + lo);
    const mach_vm_address_t expected = target;
    vm_prot_t cur = VM_PROT_NONE;
    vm_prot_t max = VM_PROT_NONE;
    const kern_return_t kr = mach_vm_remap(
        mach_task_self(), &target, hi - lo, 0,
        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
        reinterpret_cast<mach_vm_address_t>(m_linear_base + lo),
        /*copy=*/FALSE, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS || target != expected) {
        // The JIT view may now map pages the host view no longer has. Nothing
        // short of denying every access through it is safe.
        std::fprintf(stderr,
                     "[jit-view] re-alias failed guest=0x%zx-0x%zx kr=%d; "
                     "JIT view POISONED (every access takes the checked path)\n",
                     lo, hi, static_cast<int>(kr));
        m_linear_jit_view_poisoned = true;
        (void)mprotect(m_linear_jit_view, m_linear_size, PROT_NONE);
        return;
    }
    // The new entry inherits the host view's current protection; the caller
    // decides the real one.
    linear_jit_view_protect_locked(lo, hi, PROT_NONE);
}

void GuestMemory::linear_jit_view_protect_locked(usize lo, usize hi, int prot) {
    if (m_linear_jit_view == nullptr || lo >= hi) return;
    if (m_linear_jit_view_poisoned) prot = PROT_NONE;
    if (mprotect(m_linear_jit_view + lo, hi - lo, prot) != 0) {
        // A protection the view could not take may be MORE permissive than
        // the decision; deny everything instead.
        std::fprintf(stderr,
                     "[jit-view] mprotect failed guest=0x%zx-0x%zx prot=%d "
                     "errno=%d; JIT view POISONED\n",
                     lo, hi, prot, errno);
        m_linear_jit_view_poisoned = true;
        (void)mprotect(m_linear_jit_view, m_linear_size, PROT_NONE);
    }
}

int GuestMemory::linear_jit_view_final_prot(usize index, int meet) const noexcept {
    const u8 byte = __atomic_load_n(&m_linear_check_map[index], __ATOMIC_ACQUIRE);
    if ((byte & (kLinearCheckHoldsCode | kLinearCheckWatched)) != 0) {
        meet &= ~PROT_WRITE;
    }
    return meet;
}

void GuestMemory::linear_jit_view_refresh(usize lo, usize hi) {
    if (m_linear_jit_view == nullptr) return;
    std::lock_guard view_lock(m_jit_view_mutex);
    linear_jit_view_refresh_locked(lo, hi);
}

void GuestMemory::linear_jit_view_refresh_locked(usize lo, usize hi) {
    if (m_linear_jit_view == nullptr) return;
    lo &= ~(kHostPage - 1);
    hi = std::min((hi + kHostPage - 1) & ~(kHostPage - 1), m_linear_size);
    usize run_lo = lo;
    int run_prot = -1;
    const auto flush = [&](usize upto) {
        if (run_prot >= 0) linear_jit_view_protect_locked(run_lo, upto, run_prot);
    };
    for (usize page = lo; page < hi;) {
        int base = PROT_NONE;
        mach_vm_address_t end_abs = 0;
        usize entry_hi = hi;
        if (host_entry(reinterpret_cast<mach_vm_address_t>(m_linear_base + page),
                       base, end_abs)) {
            const usize end = static_cast<usize>(
                end_abs - reinterpret_cast<mach_vm_address_t>(m_linear_base));
            entry_hi = std::min(hi, std::max(end, page + kHostPage));
        } else {
            base = PROT_NONE;  // unknown: deny
        }
        for (; page < entry_hi; page += kHostPage) {
            const usize index = page >> kLinearHostPageShift;
            const u8 byte =
                __atomic_load_n(&m_linear_check_map[index], __ATOMIC_ACQUIRE);
            // A mixed page's host-view protection is the most PERMISSIVE of
            // its guest pages; the subpage map has the exact ones.
            const int meet = (byte & kLinearCheckMixedProt) != 0
                ? mixed_meet(__atomic_load_n(&m_linear_subpage_map[index],
                                             __ATOMIC_ACQUIRE))
                : base;
            const int prot = linear_jit_view_final_prot(index, meet);
            if (prot != run_prot) {
                flush(page);
                run_lo = page;
                run_prot = prot;
            }
        }
    }
    flush(hi);
}

bool GuestMemory::linear_jit_view_coherent(GuestAddr lo, GuestAddr hi) const {
    if (m_linear_jit_view == nullptr) return true;
    usize x = static_cast<usize>(lo) & ~(kHostPage - 1);
    const usize end = std::min(static_cast<usize>(hi), m_linear_size);
    const auto host0 = reinterpret_cast<mach_vm_address_t>(m_linear_base);
    const auto view0 = reinterpret_cast<mach_vm_address_t>(m_linear_jit_view);
    while (x < end) {
        BackingAt h{};
        BackingAt j{};
        if (!backing_at(host0 + x, h) || !backing_at(view0 + x, j)) return false;
        // Two entries with no object yet would fault in DIFFERENT pages.
        if (h.object_id == 0 || h.object_id != j.object_id ||
            h.offset != j.offset) {
            return false;
        }
        x = std::min(static_cast<usize>(h.entry_end - host0),
                     static_cast<usize>(j.entry_end - view0));
    }
    return true;
}

bool GuestMemory::linear_jit_view_verify_after_reconcile(usize lo, usize hi) const {
    if (m_linear_jit_view == nullptr || !jit_view_verify_enabled()) return true;
    if (linear_jit_view_coherent(static_cast<GuestAddr>(lo),
                                 static_cast<GuestAddr>(hi))) {
        return true;
    }
    std::fprintf(stderr, "[jit-view] INCOHERENT guest=0x%zx-0x%zx\n", lo, hi);
    std::abort();
}

void GuestMemory::linear_jit_view_teardown() noexcept {
    if (m_linear_jit_view == nullptr) return;
    munmap(m_linear_jit_view - kLinearJitViewGuardBytes,
           m_linear_size + 2 * kLinearJitViewGuardBytes);
    m_linear_jit_view = nullptr;
}

}  // namespace vitreo
