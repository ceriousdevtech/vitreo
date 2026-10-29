// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_memory.cpp
/// @brief Checked guest memory read/write helpers called from JIT code.
///
/// jit_mem_read8/16/32/64 and jit_mem_write8/16/32/64/span are the out-of-line
/// path an emitted access takes when it misses the inline translation. They
/// refresh the region caches the emitted code reads, publish write regions
/// back into the inline-store tables, and feed the per-call diagnostics.
///
/// Split out of jit_helpers.cpp. Closed in both directions, checked with
/// tools/architecture/plan_extraction.py before moving; the cut is open only
/// by the two promotions declared in jit_helpers_internal.hpp.

#include "jit_helpers.hpp"
#include "jit_helpers_internal.hpp"
#include "jit_helpers_mem_slow_stats.hpp"
#include "core/base/perf_window.hpp"
#include "linear_fault.hpp"
#include "core/memory/mem_watch.hpp"
#include "core/memory/write_watch_traps.hpp"

#include <vitreo/diagnostics.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace vitreo::cpu::jit {

// Helper: update region2 cache in JitContext after a slow-path resolve.
static inline void jit_update_region2(JitContext* ctx, GuestAddr addr) {
    auto info = ctx->memory->get_region_info_cached(addr);
    if (info.host_ptr) {
        ctx->region2_base = info.base;
        ctx->region2_end = info.end;
        ctx->region2_host_base = info.host_ptr;
    }
}

// Reaching a memory helper means the emitted access missed the direct-mapped
// translation cache. VITREO_JIT_XREGION_DIAG=1 reports the miss count and the
// regions responsible, which is how a working set that no longer fits shows
// itself: a healthy long-mode workload misses only once per granule touched.
static bool xregion_diag_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("VITREO_JIT_XREGION_DIAG");
        return v != nullptr && *v != '\0' && *v != '0';
    }();
    return on;
}

static void xregion_diag_record(JitContext* ctx, GuestAddr addr,
                               const GuestMemory::AccessInfo& info) {
    struct Bucket { u64 base, end, count; };
    static thread_local Bucket buckets[12];
    static thread_local u64 calls;
    static thread_local u64 empty, collision, covered;

    if (ctx->x64_rslots) {
        const auto& slot = ctx->x64_rslots[x64_slot_index(addr)];
        if (!slot.host) ++empty;
        else if (addr >= slot.base && addr < slot.end) ++covered;
        else ++collision;
    }

    ++calls;
    for (auto& b : buckets) {
        if (b.count != 0 && b.base == info.region_base) { ++b.count; break; }
        if (b.count == 0) { b = {info.region_base, info.region_end, 1}; break; }
    }

    if ((calls % 1000000u) != 0) return;
    std::fprintf(stderr, "[xregion] pid=%d misses=%llu by region:\n",
                 (int)getpid(), (unsigned long long)calls);
    std::fprintf(stderr, "[xregion-cache] pid=%d empty=%llu collision=%llu covered=%llu latest=%llx-%llx addr=%llx\n",
        (int)getpid(), (unsigned long long)empty, (unsigned long long)collision,
        (unsigned long long)covered, (unsigned long long)info.region_base,
        (unsigned long long)info.region_end, (unsigned long long)addr);
    for (const auto& b : buckets) {
        if (b.count == 0) continue;
        std::fprintf(stderr, "[xregion]   %012llx-%012llx  %llu\n",
                     (unsigned long long)b.base, (unsigned long long)b.end,
                     (unsigned long long)b.count);
    }
}

// Fused probe: a single TLB lookup yields both the host pointer for the
// access and the region info needed to refresh region2_*.  This halves the
// number of thread_local probes (each one is a _tlv_get_addr call on
// Darwin) compared to read_uXX() followed by get_region_info_cached().
static inline void jit_refresh_region2_from(JitContext* ctx, GuestAddr addr,
    const GuestMemory::AccessInfo& info) {
    if (xregion_diag_enabled()) xregion_diag_record(ctx, addr, info);
    // Index the entry by the granule that was accessed, but store the whole
    // region it resolved to: any later access anywhere in that region hits as
    // long as it maps to this entry.
    if (info.region_host_base && info.readable && ctx->x64_rslots) {
        X64MemSlot& slot = ctx->x64_rslots[x64_slot_index(addr)];
        slot.base = info.region_base;
        slot.end = info.region_end;
        slot.host = info.region_host_base;
    }
    // These legacy inline caches hold 32-bit bounds. Do not truncate a PE32+
    // mapping into them and accidentally alias an unrelated low address.
    if (info.region_host_base && info.region_end < (1ull << 32)) {
        ctx->region2_base = info.region_base;
        ctx->region2_end = info.region_end;
        ctx->region2_host_base = info.region_host_base;
    }
}

static inline void jit_refresh_read16_from(
    JitContext* ctx, const GuestMemory::AccessInfo& info) {
    if (!info.region_host_base) return;
    if (ctx->x64_read16_region_base == info.region_base &&
        ctx->x64_read16_region_end == info.region_end) {
        return;
    }
    ctx->x64_read16_region2_base = ctx->x64_read16_region_base;
    ctx->x64_read16_region2_end = ctx->x64_read16_region_end;
    ctx->x64_read16_region2_host_base =
        ctx->x64_read16_region_host_base;
    ctx->x64_read16_region_base = info.region_base;
    ctx->x64_read16_region_end = info.region_end;
    ctx->x64_read16_region_host_base = info.region_host_base;
}

static inline void jit_record_read16_diag(
    JitContext* ctx, GuestAddr addr, const GuestMemory::AccessInfo& info) {
    if (!ctx->read16_diag_enabled) return;

    ++ctx->read16_diag_calls;
    if (addr < (1ull << 32) && info.region_end <= (1ull << 32)) {
        ++ctx->read16_diag_low_calls;
    } else {
        ++ctx->read16_diag_high_calls;
    }
    if (!ctx->cpu) {
        ++ctx->read16_diag_unknown_mode_calls;
    } else if (ctx->cpu->is_64bit()) {
        ++ctx->read16_diag_x64_calls;
    } else {
        ++ctx->read16_diag_x86_calls;
    }

    if (ctx->cpu) {
        const u64 rip = ctx->cpu->rip;
        const usize start = static_cast<usize>(
            ((rip >> 2) ^ (rip >> 32)) &
            (ctx->read16_diag_rips.size() - 1));
        bool recorded = false;
        for (usize probe = 0; probe < ctx->read16_diag_rips.size(); ++probe) {
            auto& slot = ctx->read16_diag_rips[
                (start + probe) & (ctx->read16_diag_rips.size() - 1)];
            if (slot.calls == 0) {
                slot.rip = rip;
                slot.calls = 1;
                recorded = true;
                break;
            }
            if (slot.rip == rip) {
                ++slot.calls;
                recorded = true;
                break;
            }
        }
        if (!recorded) ++ctx->read16_diag_rip_collisions;
    }

    if (!info.region_host_base) return;
    const u64 mixed = (info.region_base >> 12) ^ (info.region_base >> 32);
    const usize start = static_cast<usize>(
        mixed & (ctx->read16_diag_regions.size() - 1));
    for (usize probe = 0; probe < ctx->read16_diag_regions.size(); ++probe) {
        auto& slot = ctx->read16_diag_regions[
            (start + probe) & (ctx->read16_diag_regions.size() - 1)];
        if (slot.calls == 0) {
            slot.base = info.region_base;
            slot.end = info.region_end;
            slot.calls = 1;
            ++ctx->read16_diag_distinct_regions;
            return;
        }
        if (slot.base == info.region_base && slot.end == info.region_end) {
            ++slot.calls;
            return;
        }
    }
    ++ctx->read16_diag_region_collisions;
}

// Publish the just-written region into the inline-store fast path, but only
// when it satisfies the invariant the emitted stores rely on: writable, and
// free of compiled code (so no notify_executable_write() is owed).
//
// Called only from the write helpers, i.e. only after a store already missed
// the inline path — so the cost is paid once per region, then amortized over
// every subsequent store into it. This is what turns a decompression inner
// loop's output buffer into inline stores after its first write.
//
// Protection is region-uniform in GuestMemory (probe_access reads a single
// region protection, and protect_region applies whole-region), so `writable`
// here describes the entire published range, not just the accessed byte.
static inline void jit_publish_wregion2(JitContext* ctx,
                                        const GuestMemory::AccessInfo& info) {
    if (!info.region_host_base || !info.writable ||
        info.region_end >= (1ull << 32))
        return;
    // A live MEM_WRITE_WATCH region must never be published into an inline
    // store table: the emitted store writes host memory with no hook, so
    // GetWriteWatch would report the page clean -- and a guest GC frees live
    // objects on a clean lie. Over-report, never under-report. Keeping the
    // region on the helper path costs a call per store into that one region
    // and gets an exact, 64-bit-clean record_store().
    // While any region is watched, keep every store on record_store(). This
    // process-wide refusal is deliberate: a slot published for an unrelated
    // region could otherwise survive a later MEM_WRITE_WATCH allocation and
    // let linked JIT code bypass the tracker entirely.
    if (write_watch::any_active()) {
        return;
    }
    if (ctx->memory && ctx->memory->jit_code_pages_overlap(
            info.region_base, info.region_end - info.region_base)) {
        return;
    }
    ctx->wregion2_base = info.region_base;
    ctx->wregion2_end = info.region_end;
    ctx->wregion2_host_base = info.region_host_base;
}

// Long-mode counterpart of jit_publish_wregion2, without the 32-bit bound so a
// PE32+ decompression output buffer or heap block qualifies.
//
// `executable` is rejected in addition to the block-cache code-freeness test:
// an executable mapping can acquire compiled code between this publish and a
// later store, and only the helper path owes notify_executable_write(). A live
// memory watch also keeps every store on the helper path so watched writes stay
// observable.
static inline void jit_publish_x64_wregion(
    JitContext* ctx, GuestAddr addr, const GuestMemory::AccessInfo& info) {
    if (xregion_diag_enabled()) {
        static thread_local u64 wcalls;
        if ((++wcalls % 1000000u) == 0) {
            std::fprintf(stderr, "[xregion-w] pid=%d store misses=%llu\n",
                         (int)getpid(), (unsigned long long)wcalls);
        }
    }
    // RMW lowering may read and then write through one published slot, so a
    // write slot proves both permissions. Plain stores to write-only mappings
    // simply remain on the checked helper path.
    if (!info.region_host_base || !info.readable || !info.writable ||
        info.executable) return;
    if (vitreo::mem_watch_enabled()) return;
    // A live MEM_WRITE_WATCH region must never be published into an inline
    // store table: the emitted store writes host memory with no hook, so
    // GetWriteWatch would report the page clean -- and a guest GC frees live
    // objects on a clean lie. Over-report, never under-report. Keeping the
    // region on the helper path costs a call per store into that one region
    // and gets an exact, 64-bit-clean record_store().
    if (write_watch::any_active()) {
        return;
    }
    if (ctx->memory && ctx->memory->jit_code_pages_overlap(
            info.region_base, info.region_end - info.region_base)) {
        return;
    }
    if (!ctx->x64_wslots) return;
    X64MemSlot& slot = ctx->x64_wslots[x64_slot_index(addr)];
    slot.base = info.region_base;
    slot.end = info.region_end;
    slot.host = info.region_host_base;
}

// A cached translation makes its guest byte range executable from the JIT's
// point of view even when the backing Windows mapping is still writable. The
// guarded store path reaches these helpers specifically so writes overlapping
// cached code can retire that translation before it is dispatched again.
static inline bool jit_write_requires_code_notification(
    JitContext* ctx, const GuestMemory::AccessInfo& info,
    GuestAddr addr, usize size) {
    (void)info;
    // Executable permission alone does not make bytes translated code.
    // Journal only stores that overlap a live cached block. Code-generation
    // changes retract published store regions at block entry, so a later
    // translation cannot make an earlier code-free fast path stale.
    return ctx->memory && ctx->memory->jit_code_pages_overlap(addr, size);
}

// Watched writes from JIT-compiled code carry no call context on their own, and
// the interpreter-only VITREO_CODE_WATCH cannot see a JIT block at all. Dumping
// the stack at the hit makes the writing call chain recoverable offline, which
// is what distinguishes "this global was torn down by its owner" from "an
// unrelated path clobbered it".
static void jit_mem_watch(JitContext* ctx, GuestAddr addr, usize size, u64 val,
                          const char* who) {
    if (!vitreo::mem_watch_enabled() && !vitreo::store_ring_enabled()) return;
    bool hit = false;
    for (u64 w : vitreo::mem_watch_addrs()) {
        if (addr <= w && w < addr + size) { hit = true; break; }
    }
    vitreo::mem_watch_check(addr, size, val, who);
    if (!hit || ctx == nullptr || ctx->cpu == nullptr) return;

    auto* c = ctx->cpu;
    std::fprintf(stderr, "[watch]   rip=0x%llx rsp=0x%llx\n",
                 (unsigned long long)c->rip,
                 (unsigned long long)c->gpr.get64(4));
    GuestAddr rsp = static_cast<GuestAddr>(c->gpr.get64(4));
    auto sd = ctx->memory->probe_access(rsp, 256);
    if (!sd.host_ptr || !sd.readable) return;
    const u64* q = reinterpret_cast<const u64*>(sd.host_ptr);
    for (int row = 0; row < 4; ++row) {
        std::fprintf(stderr, "[watch]   stack @ rsp+0x%02x:", row * 64);
        for (int i = 0; i < 8; ++i)
            std::fprintf(stderr, " %016llx",
                         (unsigned long long)q[row * 8 + i]);
        std::fprintf(stderr, "\n");
    }
}

/// Read-side twin of `jit_note_helper_write`.
///
/// The x64 load guard (`emit_linear64_load`) leaves the inline path for four
/// distinct reasons, and they call for different fixes:
///
///   * the EA is outside the reservation;
///   * the access STRADDLES a 16 KiB host page -- routed to the helper as
///     "rare", which is exactly the assumption the store side's halfword probe
///     also made and which measured at 91.7% there;
///   * the page's mixed-protection byte is set;
///   * the permission guard rejected it.
///
/// Only the straddle is derivable here without re-deriving the guard, so that
/// is what is recorded: `page_cross` says whether this access spans a host
/// page, which distinguishes hypothesis two from the rest on its own.
inline void jit_note_helper_read(JitContext* ctx, GuestAddr addr,
                                 int width, uintptr_t ret_pc) noexcept {
    if (mem_slow_stats::g_enabled) mem_slow_stats::note(ctx, addr, width, false);
    if (!diagnostics::perf_window_active_fast()) return;
    diagnostics::perf_note_helper_read(addr, width);
    const bool page_cross =
        ((addr ^ (addr + static_cast<GuestAddr>(width) - 1)) >>
         vitreo::kLinearHostPageShift) != 0;
    diagnostics::perf_note_helper_read_cross(page_cross);
    if (ctx != nullptr && ctx->memory != nullptr) {
        if (const u8* cm = ctx->memory->linear_check_page_map()) {
            const usize i =
                static_cast<usize>(addr) >> vitreo::kLinearHostPageShift;
            if (i < ctx->memory->linear_check_page_count()) {
                diagnostics::perf_note_helper_read_reason(
                    __atomic_load_n(&cm[i], __ATOMIC_RELAXED));
            }
        }
    }
    if (diagnostics::perf_helper_write_site_sample_due()) {
        JitFaultMetadata md;
        const bool in_block = jit_fault_metadata_for_pc(ret_pc, md);
        (void)in_block;
        diagnostics::perf_note_helper_read_site(
            md.has_guest_rip ? md.guest_rip : 0);
    }
}

extern "C" u32 jit_mem_read32(JitContext* ctx, GuestAddr addr) {
    jit_revalidate_memory_caches(ctx);
    jit_note_helper_read(
        ctx, addr, 4,
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    auto info = ctx->memory->probe_access(addr, sizeof(u32));
    if (!info.host_ptr || !info.readable) {
        const auto result = ctx->memory->read_u32(addr);
        diagnostics::perf_add_helper_mem_fast(false, result.is_err());
        if (result.is_ok()) return result.value();
        jit_raise_memory_av(ctx, addr, sizeof(u32), /*is_write=*/false);
        return 0u;
    }
    diagnostics::perf_add_helper_mem_fast(false, false);
    u32 value;
    std::memcpy(&value, info.host_ptr, sizeof(u32));
    jit_refresh_region2_from(ctx, addr, info);
    return value;
}

void jit_publish_mem_watch_cpu(CpuState* cpu) {
    if (cpu == nullptr || (!vitreo::mem_watch_enabled() && !vitreo::store_ring_enabled())) return;
    vitreo::mem_watch_cur_rip() = cpu->rip;
    auto& r = vitreo::mem_watch_cur_registers();
    r.eax = cpu->gpr.get32(static_cast<u8>(RegIndex::RAX));
    r.ecx = cpu->gpr.get32(static_cast<u8>(RegIndex::RCX));
    r.edx = cpu->gpr.get32(static_cast<u8>(RegIndex::RDX));
    r.ebx = cpu->gpr.get32(static_cast<u8>(RegIndex::RBX));
    r.esp = static_cast<u32>(cpu->sp());
    r.ebp = cpu->gpr.get32(static_cast<u8>(RegIndex::RBP));
    r.esi = cpu->gpr.get32(static_cast<u8>(RegIndex::RSI));
    r.edi = cpu->gpr.get32(static_cast<u8>(RegIndex::RDI));
}

namespace {

/// Perf-window attribution for a store that reached a C++ helper.
///
/// Records three independent things, because the first two left
/// 67.8% of 1.9 G helper writes unexplained -- in range, and with a linear
/// check-map byte of ZERO, which should have stayed inline:
///
///   * the target PAGE  -- says whether the traffic is concentrated;
///   * the check-map REASON bits -- says which guard sent it here;
///   * the calling GUEST INSTRUCTION -- says which emitter produced the call,
///     which is the only one of the three that can explain a zero byte.
///
/// The site is SAMPLED. `jit_guest_rip_for_pc` walks published block metadata,
/// and doing that 1.9 G times would dominate the workload it is measuring;
/// one in every kWriteSiteStride calls keeps the distribution and the cost.
/// `__builtin_return_address(0)` is the arena address just past the BLR in the
/// emitting block, which is what the metadata is keyed on.
/// @param ret_pc the CALLER's return address, captured by the helper itself.
///        Taking `__builtin_return_address(0)` in here instead reports this
///        function's own caller -- i.e. `jit_mem_write32` -- whenever the
///        compiler declines to inline it, which is what a first attempt did:
///        every sample resolved to `jit_mem_write32 +0x64`.
inline void jit_note_helper_write(JitContext* ctx, GuestAddr addr,
                                  int width, uintptr_t ret_pc) noexcept {
    // Long-mode cold entries publish the precise guest RIP for checked
    // faults; reuse it for store attribution. Shared helper machine code is
    // outside any individual instruction interval. Legacy helpers retain
    // their existing block-level attribution.
    if (store_ring_enabled() && ctx != nullptr)
        jit_publish_mem_watch_cpu(ctx->cpu);
    if (mem_slow_stats::g_enabled) mem_slow_stats::note(ctx, addr, width, true);
    if (!diagnostics::perf_window_active_fast()) return;
    diagnostics::perf_note_helper_write_page(addr);
    diagnostics::perf_note_helper_write_width(width);
    if (ctx != nullptr && ctx->memory != nullptr) {
        if (const u8* cm = ctx->memory->linear_check_page_map()) {
            const usize i =
                static_cast<usize>(addr) >> vitreo::kLinearHostPageShift;
            if (i < ctx->memory->linear_check_page_count()) {
                const u8 self = __atomic_load_n(&cm[i], __ATOMIC_RELAXED);
                const u8 next =
                    (i + 1 < ctx->memory->linear_check_page_count())
                        ? __atomic_load_n(&cm[i + 1], __ATOMIC_RELAXED)
                        : u8{0};
                diagnostics::perf_note_helper_write_reason(self, next);
            }
        }
    }
    if (diagnostics::perf_helper_write_site_sample_due()) {
        const auto pc = ret_pc;
        JitFaultMetadata md;
        const bool in_block = jit_fault_metadata_for_pc(pc, md);
        const int kind = md.has_guest_rip ? 0 : (in_block ? 1 : 2);
        diagnostics::perf_note_helper_write_site(
            md.has_guest_rip ? md.guest_rip : 0, static_cast<u64>(pc), kind);
        // A return address in neither the arena nor a published block means
        // the caller is host C++, and then the only useful identity is its
        // SYMBOL. Resolved once per distinct PC (dladdr is far too slow to run
        // on every sample) and printed by the perf window.
        if (kind == 2) diagnostics::perf_note_helper_write_symbol(pc);
    }
}

}  // namespace

extern "C" void jit_mem_write32(JitContext* ctx, GuestAddr addr, u32 val) {
    jit_revalidate_memory_caches(ctx);
    constexpr int kHelperWriteWidth = 4;
    auto info = ctx->memory->probe_access(addr, sizeof(u32));
    if (!info.host_ptr || !info.writable) {
        jit_publish_mem_watch_cpu(ctx->cpu);
        const auto result = ctx->memory->write_u32(addr, static_cast<u32>(val));
        jit_note_helper_write(
        ctx, addr, kHelperWriteWidth,
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    diagnostics::perf_add_helper_mem_fast(true, result.is_err());
        if (result.is_ok()) return;
        jit_raise_memory_av(ctx, addr, sizeof(u32), /*is_write=*/true);
        return;
    }
    jit_note_helper_write(
        ctx, addr, kHelperWriteWidth,
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    diagnostics::perf_add_helper_mem_fast(true, false);
    if (ctx != nullptr) {
        jit_publish_mem_watch_cpu(ctx->cpu);
    }
    if (vitreo::mem_watch_enabled() || vitreo::store_ring_enabled()) {
        vitreo::mem_watch_check(addr, sizeof(u32), val, "jit.u32");
    }
    if (write_watch::any_active()) {
        write_watch::record_store(addr, sizeof(u32));
    }
    std::memcpy(info.host_ptr, &val, sizeof(u32));
    if (jit_write_requires_code_notification(
            ctx, info, addr, sizeof(u32))) {
        ctx->memory->notify_executable_write(addr, sizeof(u32));
    }
    jit_refresh_region2_from(ctx, addr, info);
    jit_publish_wregion2(ctx, info);
    jit_publish_x64_wregion(ctx, addr, info);
}

extern "C" u64 jit_mem_read64(JitContext* ctx, GuestAddr addr) {
    jit_revalidate_memory_caches(ctx);
    jit_note_helper_read(
        ctx, addr, 8,
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    auto info = ctx->memory->probe_access(addr, sizeof(u64));
    if (!info.host_ptr || !info.readable) {
        const auto result = ctx->memory->read_u64(addr);
        diagnostics::perf_add_helper_mem_fast(false, result.is_err());
        if (result.is_ok()) return result.value();
        jit_raise_memory_av(ctx, addr, sizeof(u64), /*is_write=*/false);
        return 0u;
    }
    diagnostics::perf_add_helper_mem_fast(false, false);
    u64 value;
    std::memcpy(&value, info.host_ptr, sizeof(u64));
    jit_refresh_region2_from(ctx, addr, info);
    return value;
}

extern "C" void jit_mem_write64(JitContext* ctx, GuestAddr addr, u64 val) {
    jit_revalidate_memory_caches(ctx);
    constexpr int kHelperWriteWidth = 8;
    auto info = ctx->memory->probe_access(addr, sizeof(u64));
    if (!info.host_ptr || !info.writable) {
        jit_publish_mem_watch_cpu(ctx->cpu);
        const auto result = ctx->memory->write_u64(addr, static_cast<u64>(val));
        jit_note_helper_write(
        ctx, addr, kHelperWriteWidth,
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    diagnostics::perf_add_helper_mem_fast(true, result.is_err());
        if (result.is_ok()) return;
        jit_raise_memory_av(ctx, addr, sizeof(u64), /*is_write=*/true);
        return;
    }
    jit_note_helper_write(
        ctx, addr, kHelperWriteWidth,
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    diagnostics::perf_add_helper_mem_fast(true, false);
    if (vitreo::mem_watch_enabled() || vitreo::store_ring_enabled()) {
        jit_mem_watch(ctx, addr, sizeof(u64), val, "jit.u64");
    }
    if (write_watch::any_active()) {
        write_watch::record_store(addr, sizeof(u64));
    }
    std::memcpy(info.host_ptr, &val, sizeof(u64));
    if (jit_write_requires_code_notification(
            ctx, info, addr, sizeof(u64))) {
        ctx->memory->notify_executable_write(addr, sizeof(u64));
    }
    jit_refresh_region2_from(ctx, addr, info);
    jit_publish_x64_wregion(ctx, addr, info);
}

// A 16- or 32-byte guest store taken out of line (the vec128/vec256 cold
// stubs): ONE write of the whole operand, validated before any byte is
// modified (GuestMemory::write_bytes). x86 performs none of a faulting store
// (Intel SDM Vol. 3A 6.15, #PF / #GP), so the stubs must not split it into
// 8-byte helper writes, which performed the pieces before a denied one.
// @p lo holds bytes 0-15 of the value, @p hi bytes 16-31 (32-byte only).
extern "C" void jit_mem_write_span(JitContext* ctx, GuestAddr addr,
                                   const u8* lo, const u8* hi, u64 size) {
    jit_revalidate_memory_caches(ctx);
    u8 value[32];
    std::memcpy(value, lo, 16);
    if (size > 16) std::memcpy(value + 16, hi, 16);
    jit_publish_mem_watch_cpu(ctx->cpu);
    const auto result =
        ctx->memory->write_bytes(addr, value, static_cast<usize>(size));
    jit_note_helper_write(
        ctx, addr, static_cast<int>(size),
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    if (result.is_err()) {
        jit_raise_memory_av(ctx, addr, static_cast<usize>(size),
                            /*is_write=*/true);
        return;
    }
    // write_bytes notifies an EXECUTABLE page; a page that merely holds a
    // live translation (RW guest JIT code, a translated page made writable)
    // must invalidate it too -- the same rule as jit_mem_write64.
    if (ctx->memory->jit_code_pages_overlap(addr, static_cast<usize>(size))) {
        ctx->memory->notify_executable_write(addr, static_cast<usize>(size));
    }
}

extern "C" u32 jit_mem_read8(JitContext* ctx, GuestAddr addr) {
    jit_revalidate_memory_caches(ctx);
    jit_note_helper_read(
        ctx, addr, 1,
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    auto info = ctx->memory->probe_access(addr, 1);
    if (!info.host_ptr || !info.readable) {
        diagnostics::perf_add_helper_mem_fast(false, true);
        jit_raise_memory_av(ctx, addr, 1, /*is_write=*/false);
        return 0u;
    }
    diagnostics::perf_add_helper_mem_fast(false, false);
    u32 value = static_cast<u32>(*info.host_ptr);
    jit_refresh_region2_from(ctx, addr, info);
    return value;
}

extern "C" void jit_mem_write8(JitContext* ctx, GuestAddr addr, u32 val) {
    jit_revalidate_memory_caches(ctx);
    constexpr int kHelperWriteWidth = 1;
    auto info = ctx->memory->probe_access(addr, 1);
    if (!info.host_ptr || !info.writable) {
        jit_note_helper_write(
        ctx, addr, kHelperWriteWidth,
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    diagnostics::perf_add_helper_mem_fast(true, true);
        jit_raise_memory_av(ctx, addr, 1, /*is_write=*/true);
        return;
    }
    jit_note_helper_write(
        ctx, addr, kHelperWriteWidth,
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    diagnostics::perf_add_helper_mem_fast(true, false);
    if (vitreo::mem_watch_enabled() || vitreo::store_ring_enabled()) {
        vitreo::mem_watch_check(addr, 1, val, "jit.u8");
    }
    if (write_watch::any_active()) {
        write_watch::record_store(addr, 1);
    }
    *info.host_ptr = static_cast<u8>(val);
    if (jit_write_requires_code_notification(ctx, info, addr, 1)) {
        ctx->memory->notify_executable_write(addr, 1);
    }
    jit_refresh_region2_from(ctx, addr, info);
    jit_publish_wregion2(ctx, info);
    jit_publish_x64_wregion(ctx, addr, info);
}

extern "C" u32 jit_mem_read16(JitContext* ctx, GuestAddr addr) {
    jit_revalidate_memory_caches(ctx);
    jit_note_helper_read(
        ctx, addr, 2,
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    auto info = ctx->memory->probe_access(addr, sizeof(u16));
    jit_record_read16_diag(ctx, addr, info);
    if (!info.host_ptr || !info.readable) {
        const auto result = ctx->memory->read_u16(addr);
        diagnostics::perf_add_helper_mem_fast(false, result.is_err());
        if (result.is_ok()) return result.value();
        jit_raise_memory_av(ctx, addr, sizeof(u16), /*is_write=*/false);
        return 0u;
    }
    diagnostics::perf_add_helper_mem_fast(false, false);
    u16 value;
    std::memcpy(&value, info.host_ptr, sizeof(u16));
    jit_refresh_read16_from(ctx, info);
    jit_refresh_region2_from(ctx, addr, info);
    return static_cast<u32>(value);
}

extern "C" void jit_mem_write16(JitContext* ctx, GuestAddr addr, u32 val) {
    jit_revalidate_memory_caches(ctx);
    constexpr int kHelperWriteWidth = 2;
    auto info = ctx->memory->probe_access(addr, sizeof(u16));
    if (!info.host_ptr || !info.writable) {
        jit_publish_mem_watch_cpu(ctx->cpu);
        const auto result = ctx->memory->write_u16(addr, static_cast<u16>(val));
        jit_note_helper_write(
        ctx, addr, kHelperWriteWidth,
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    diagnostics::perf_add_helper_mem_fast(true, result.is_err());
        if (result.is_ok()) return;
        jit_raise_memory_av(ctx, addr, sizeof(u16), /*is_write=*/true);
        return;
    }
    jit_note_helper_write(
        ctx, addr, kHelperWriteWidth,
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    diagnostics::perf_add_helper_mem_fast(true, false);
    u16 v16 = static_cast<u16>(val);
    if (vitreo::mem_watch_enabled() || vitreo::store_ring_enabled()) {
        vitreo::mem_watch_check(addr, sizeof(u16), val, "jit.u16");
    }
    if (write_watch::any_active()) {
        write_watch::record_store(addr, sizeof(u16));
    }
    std::memcpy(info.host_ptr, &v16, sizeof(u16));
    if (jit_write_requires_code_notification(
            ctx, info, addr, sizeof(u16))) {
        ctx->memory->notify_executable_write(addr, sizeof(u16));
    }
    jit_refresh_region2_from(ctx, addr, info);
    jit_publish_wregion2(ctx, info);
    jit_publish_x64_wregion(ctx, addr, info);
}

} // namespace vitreo::cpu::jit
