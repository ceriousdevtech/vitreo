// SPDX-License-Identifier: LGPL-2.1-only

/// @file x64_segments.cpp
/// @brief LDT segment allocation for the native x86_64 backend.
///
/// See x64_segments.hpp for why the LDT is the mechanism rather than
/// wrgsbase or thread_set_state.

#include "core/cpu/x64/x64_segments.hpp"

#include <architecture/i386/table.h>
#include <i386/user_ldt.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>

namespace vitreo::cpu::x64 {

namespace {

std::mutex        g_ldt_mutex;
std::atomic<usize> g_allocated{0};

// Compatibility-mode selectors are allocated once and shared: every 32-bit
// guest thread enters through the same flat segments, and LDT entries are a
// finite per-process resource.
u16  g_compat_code_sel = 0;
u16  g_compat_data_sel = 0;
bool g_compat_ready    = false;

/// LDT index -> selector: index<<3, TI=1 (LDT, not GDT), RPL=3 (user).
constexpr u16 selector_for_index(int index) noexcept {
    return static_cast<u16>((index << 3) | 0x7);
}

/// Fill a flat data descriptor: 4GB via page granularity, user privilege.
void init_flat_data_desc(union ldt_entry* d, u32 base) {
    std::memset(d, 0, sizeof(*d));
    d->data.base00   = static_cast<unsigned short>(base & 0xFFFF);
    d->data.base16   = static_cast<unsigned char>((base >> 16) & 0xFF);
    d->data.base24   = static_cast<unsigned char>((base >> 24) & 0xFF);
    d->data.limit00  = 0xFFFF;
    d->data.limit16  = 0xF;
    d->data.type     = DESC_DATA_WRITE;   // readable + writable
    d->data.dpl      = 3;
    d->data.present  = 1;
    d->data.stksz    = DESC_DATA_32B;
    d->data.granular = DESC_GRAN_PAGE;    // limit counts 4K pages -> 4GB
}

/// Fill a flat 32-bit code descriptor. opsz = D bit, which is what makes the
/// segment 32-bit rather than 16-bit; an LDT code descriptor always has L=0,
/// so this is a compatibility-mode segment by construction.
void init_flat_code32_desc(union ldt_entry* d) {
    std::memset(d, 0, sizeof(*d));
    d->code.limit00  = 0xFFFF;
    d->code.limit16  = 0xF;
    d->code.type     = DESC_CODE_READ;    // executable + readable
    d->code.dpl      = 3;
    d->code.present  = 1;
    d->code.opsz     = DESC_CODE_32B;
    d->code.granular = DESC_GRAN_PAGE;
}

} // namespace

VResult<u16> SegmentTable::allocate_data_segment(u64 base) {
    // A descriptor base is 32 bits. Silently truncating would produce a
    // segment pointing at unrelated memory and a TEB read that returns
    // plausible garbage, so refuse instead.
    if (base >= kSegmentBaseLimit) {
        return VResult<u16>::err(Error::InvalidArgument);
    }

    union ldt_entry desc;
    init_flat_data_desc(&desc, static_cast<u32>(base));

    std::lock_guard lk(g_ldt_mutex);
    const int index = i386_set_ldt(LDT_AUTO_ALLOC, &desc, 1);
    if (index < 0) {
        return VResult<u16>::err(Error::NotSupported);
    }
    g_allocated.fetch_add(1, std::memory_order_relaxed);
    return VResult<u16>::ok(selector_for_index(index));
}

VoidResult SegmentTable::allocate_compat32_pair(u16* out_code_sel,
                                                u16* out_data_sel) {
    if (out_code_sel == nullptr || out_data_sel == nullptr) {
        return VoidResult::err(Error::InvalidArgument);
    }

    std::lock_guard lk(g_ldt_mutex);
    if (g_compat_ready) {
        *out_code_sel = g_compat_code_sel;
        *out_data_sel = g_compat_data_sel;
        return VoidResult::ok();
    }

    // Allocated as one pair so the selectors are adjacent and a single failure
    // cannot leave a half-installed pair behind.
    union ldt_entry desc[2];
    init_flat_code32_desc(&desc[0]);
    init_flat_data_desc(&desc[1], 0);

    const int index = i386_set_ldt(LDT_AUTO_ALLOC, desc, 2);
    if (index < 0) {
        return VoidResult::err(Error::NotSupported);
    }

    g_compat_code_sel = selector_for_index(index);
    g_compat_data_sel = selector_for_index(index + 1);
    g_compat_ready    = true;
    g_allocated.fetch_add(2, std::memory_order_relaxed);

    *out_code_sel = g_compat_code_sel;
    *out_data_sel = g_compat_data_sel;
    return VoidResult::ok();
}

usize SegmentTable::allocated_count() noexcept {
    return g_allocated.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Segment register access
// ---------------------------------------------------------------------------

u16 current_gs() noexcept {
    u16 v;
    __asm__ volatile("movw %%gs, %0" : "=r"(v));
    return v;
}

u16 current_ds() noexcept {
    u16 v;
    __asm__ volatile("movw %%ds, %0" : "=r"(v));
    return v;
}

u16 current_es() noexcept {
    u16 v;
    __asm__ volatile("movw %%es, %0" : "=r"(v));
    return v;
}

u16 current_fs() noexcept {
    u16 v;
    __asm__ volatile("movw %%fs, %0" : "=r"(v));
    return v;
}

void set_gs(u16 selector) noexcept {
    __asm__ volatile("movw %0, %%gs" :: "r"(selector) : "memory");
}

void set_ds(u16 selector) noexcept {
    __asm__ volatile("movw %0, %%ds" :: "r"(selector) : "memory");
}

void set_es(u16 selector) noexcept {
    __asm__ volatile("movw %0, %%es" :: "r"(selector) : "memory");
}

void set_fs(u16 selector) noexcept {
    __asm__ volatile("movw %0, %%fs" :: "r"(selector) : "memory");
}

void enter_guest_gs(u16 guest_gs) noexcept {
    // No save, because there is nothing restorable to save: the host base
    // lives in IA32_GS_BASE and no user-mode mechanism can write it back.
    // See the header.
    set_gs(guest_gs);
}

void enter_guest_fs(u16 guest_fs) noexcept {
    set_fs(guest_fs);
}

} // namespace vitreo::cpu::x64
