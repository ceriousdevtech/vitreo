// SPDX-License-Identifier: LGPL-2.1-only
// PAGE_GUARD one-shot traps. Armed pages are host PROT_NONE; a fault clears
// the armed bit and restores the base host protection. The signal/JIT path
// then raises STATUS_GUARD_PAGE_VIOLATION (0x80000001). VirtualQuery reports
// PAGE_GUARD only while the page is still armed.

#pragma once

#include "vitreo/types.hpp"
#include "core/memory/write_watch_traps.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

namespace vitreo::guard_page {

inline constexpr u64 kGuestPage = 4096;
inline constexpr u32 kStatusGuardPageViolation = 0x80000001u;

inline usize host_page_size() noexcept {
    static const usize s = static_cast<usize>(::getpagesize());
    return s;
}

struct Slot {
    std::atomic<u64> base{0};
    std::atomic<u64> size{0};
    std::atomic<u8*> armed{nullptr};
    /// MemoryProtection bitfield (R=1 W=2 X=4) restored after a trip.
    std::atomic<u32> restore_prot{0};
};

inline constexpr int kMaxSlots = 64;
inline Slot& slot(int i) noexcept {
    static Slot slots[kMaxSlots];
    return slots[i];
}
inline std::mutex& table_mutex() noexcept {
    static std::mutex m;
    return m;
}
inline std::atomic<u32>& armed_count() noexcept {
    static std::atomic<u32> n{0};
    return n;
}

inline bool any_armed() noexcept {
    return armed_count().load(std::memory_order_acquire) != 0;
}

inline bool test_bit(const u8* bits, u64 index) noexcept {
    return (bits[index / 8] & static_cast<u8>(1u << (index % 8))) != 0;
}
inline void set_bit(u8* bits, u64 index) noexcept {
    bits[index / 8] |= static_cast<u8>(1u << (index % 8));
}
inline void clear_bit(u8* bits, u64 index) noexcept {
    bits[index / 8] &= static_cast<u8>(~(1u << (index % 8)));
}

inline int host_prot_flags(u32 restore_prot) noexcept {
    int prot = PROT_NONE;
    if (restore_prot & 1u) prot |= PROT_READ;
    if (restore_prot & 2u) prot |= PROT_WRITE;
    if (restore_prot & 4u) prot |= PROT_EXEC;
    return prot;
}

inline void mprotect_guest_page(u64 guest_page, int prot) noexcept {
    const uintptr_t base = write_watch::host_base();
    if (base == 0) {
        // Unit tests / non-linear setups: VirtualMemoryManager::protect_region
        // owns the host mapping; we only track the armed bit.
        return;
    }
    const usize hps = host_page_size();
    auto* host = reinterpret_cast<void*>(base + guest_page);
    const uintptr_t u = reinterpret_cast<uintptr_t>(host);
    const uintptr_t aligned = u & ~(static_cast<uintptr_t>(hps) - 1);
    (void)::mprotect(reinterpret_cast<void*>(aligned), hps, prot);
}

inline Slot* find_slot(u64 addr) noexcept {
    for (int i = 0; i < kMaxSlots; ++i) {
        const u64 base = slot(i).base.load(std::memory_order_acquire);
        if (base == 0) continue;
        const u64 size = slot(i).size.load(std::memory_order_relaxed);
        if (addr >= base && addr - base < size) return &slot(i);
    }
    return nullptr;
}

inline void register_region(u64 base, u64 size, u8* armed,
                            u32 restore_prot) noexcept {
    std::lock_guard<std::mutex> lock(table_mutex());
    int empty = -1;
    for (int i = 0; i < kMaxSlots; ++i) {
        if (slot(i).base.load(std::memory_order_relaxed) == base) {
            slot(i).size.store(size, std::memory_order_relaxed);
            slot(i).armed.store(armed, std::memory_order_release);
            slot(i).restore_prot.store(restore_prot, std::memory_order_relaxed);
            return;
        }
        if (empty < 0 && slot(i).base.load(std::memory_order_relaxed) == 0) {
            empty = i;
        }
    }
    if (empty < 0) return;
    slot(empty).size.store(size, std::memory_order_relaxed);
    slot(empty).armed.store(armed, std::memory_order_release);
    slot(empty).restore_prot.store(restore_prot, std::memory_order_relaxed);
    slot(empty).base.store(base, std::memory_order_release);
}

inline void unregister_region(u64 base) noexcept {
    std::lock_guard<std::mutex> lock(table_mutex());
    for (int i = 0; i < kMaxSlots; ++i) {
        if (slot(i).base.load(std::memory_order_relaxed) != base) continue;
        u8* bits = slot(i).armed.load(std::memory_order_acquire);
        const u64 size = slot(i).size.load(std::memory_order_relaxed);
        if (bits && size) {
            const u64 pages = size / kGuestPage;
            for (u64 p = 0; p < pages; ++p) {
                if (test_bit(bits, p)) {
                    armed_count().fetch_sub(1, std::memory_order_acq_rel);
                    clear_bit(bits, p);
                }
            }
        }
        slot(i).armed.store(nullptr, std::memory_order_release);
        slot(i).size.store(0, std::memory_order_relaxed);
        slot(i).base.store(0, std::memory_order_release);
        return;
    }
}

[[nodiscard]] inline bool is_armed(GuestAddr addr) noexcept {
    if (!any_armed()) return false;
    Slot* s = find_slot(static_cast<u64>(addr));
    if (!s) return false;
    u8* bits = s->armed.load(std::memory_order_acquire);
    if (!bits) return false;
    const u64 base = s->base.load(std::memory_order_relaxed);
    return test_bit(bits, (static_cast<u64>(addr) - base) / kGuestPage);
}

/// Arm guest pages [begin, end) within allocation @p alloc_base.
inline void arm_range(u64 alloc_base, u64 alloc_size, u64 begin, u64 end,
                      u8* armed_bits, u32 restore_prot) noexcept {
    if (!armed_bits || begin >= end) return;
    register_region(alloc_base, alloc_size, armed_bits, restore_prot);
    for (u64 page = begin & ~(kGuestPage - 1); page < end; page += kGuestPage) {
        const u64 index = (page - alloc_base) / kGuestPage;
        if (!test_bit(armed_bits, index)) {
            set_bit(armed_bits, index);
            armed_count().fetch_add(1, std::memory_order_acq_rel);
        }
        mprotect_guest_page(page, PROT_NONE);
    }
}

/// Disarm without a fault (VirtualProtect clearing PAGE_GUARD).
inline void disarm_range(u64 alloc_base, u64 begin, u64 end, u8* armed_bits,
                         u32 restore_prot) noexcept {
    if (!armed_bits || begin >= end) return;
    for (u64 page = begin & ~(kGuestPage - 1); page < end; page += kGuestPage) {
        const u64 index = (page - alloc_base) / kGuestPage;
        if (test_bit(armed_bits, index)) {
            clear_bit(armed_bits, index);
            armed_count().fetch_sub(1, std::memory_order_acq_rel);
        }
        mprotect_guest_page(page, host_prot_flags(restore_prot));
    }
}

/// Async-signal-safe: clear armed bit and restore host protection.
/// Returns true when @p addr was an armed guard page.
inline bool handle_fault(GuestAddr addr) noexcept {
    if (!any_armed()) return false;
    const u64 a = static_cast<u64>(addr);
    Slot* s = find_slot(a);
    if (!s) return false;
    u8* bits = s->armed.load(std::memory_order_acquire);
    if (!bits) return false;
    const u64 base = s->base.load(std::memory_order_relaxed);
    const u64 index = (a - base) / kGuestPage;
    if (!test_bit(bits, index)) return false;
    clear_bit(bits, index);
    armed_count().fetch_sub(1, std::memory_order_acq_rel);
    const u64 page = a & ~(kGuestPage - 1);
    mprotect_guest_page(
        page, host_prot_flags(s->restore_prot.load(std::memory_order_relaxed)));
    return true;
}

/// Forget every registered region without touching its armed bitmap. The
/// table holds RAW pointers into the registering VirtualMemoryManager's
/// AllocInfo; only VirtualFree unregisters. A manager destroyed with a live
/// PAGE_GUARD region (every unit test that arms one and exits) leaves a slot
/// aimed at freed heap, and the next VirtualFree of the same base -- addresses
/// are deterministic, so a later test hits it -- clears bits in that freed
/// block: silent heap corruption that traps inside malloc tests later.
inline void reset_all() noexcept {
    std::lock_guard<std::mutex> lock(table_mutex());
    for (int i = 0; i < kMaxSlots; ++i) {
        slot(i).base.store(0, std::memory_order_release);
        slot(i).armed.store(nullptr, std::memory_order_relaxed);
        slot(i).size.store(0, std::memory_order_relaxed);
        slot(i).restore_prot.store(0, std::memory_order_relaxed);
    }
    armed_count().store(0, std::memory_order_release);
}

}  // namespace vitreo::guard_page
