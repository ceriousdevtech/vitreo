// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_cold_lazy.cpp
/// @brief Lazy cold memory slow paths (JitCompiler::cold_lazy_enabled).
///
/// Most linear64 memory sites never take their slow path, yet the eager
/// stub (entry, helper call, raise check, the range / crossing / lean
/// variants) was ~60% of an x64 block's bytes. A lazy site instead carries a
/// one-word slot and a four-word record (linear_fault.hpp, ColdLazyRecord).
/// Its first take enters vitreo_jit_cold_lazy_trampoline, which saves the
/// whole register file and calls JitCompiler::cold_lazy_take:
///
///   1. cold_lazy_emit runs the ordinary cold-stub flush for that one site,
///      as if the stub were placed at its final address, so the words are
///      the eager stub's, variants included;
///   2. the words go to the far-branch island of the slot's own chunk, so
///      the stub is reachable by B and dies with the block;
///   3. the stub is published as an interval of the owning instruction (its
///      RIP, pins and flag-recovery descriptor), which is what attributes a
///      fault in it or in the shared helper it calls (X30 is in the stub);
///   4. the slot is CASed to `B stub` and the take resumes at the stub with
///      every register as the hot path left it.
///
/// A take that cannot materialize (island full, chunk retired) leaves the
/// block through its raise exit at the instruction with pending_raise
/// inactive, and execute() interprets that instruction: the state a raise
/// there leaves is exactly the state before the instruction.

#include <set>
#include "jit_compiler.hpp"
#include "jit_code_arena.hpp"
#include "jit_emit_internal.hpp"
#include "jit_helpers.hpp"
#include "linear_fault.hpp"
#include "jit_split_layout.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <libkern/OSCacheControl.h>
#include <pthread.h>

namespace vitreo::cpu::jit {

extern "C" void vitreo_jit_cold_lazy_trampoline();

namespace {

std::atomic<int> g_cold_lazy_test_override{-1};
std::atomic<u64> g_lazy_sites{0};
std::atomic<u64> g_lazy_materialized{0};
std::atomic<u64> g_lazy_materialized_bytes{0};
std::atomic<u64> g_lazy_races{0};
std::atomic<u64> g_lazy_side_exits{0};
std::atomic<u64> g_lazy_fail_retired{0};
std::atomic<u64> g_lazy_fail_island{0};
std::atomic<bool> g_lazy_force_side_exit{false};
std::atomic<bool> g_lazy_force_island_full{false};
std::atomic<u64> g_lazy_mixed_eager{0};
std::atomic<bool> g_lazy_mixed_eager_on{true};

// One materialization at a time: two threads taking the same slot emit it
// once, and the loser resumes at the winner's stub.
std::mutex& cold_lazy_mutex() {
    static auto* m = new std::mutex();
    return *m;
}

// Register save area of vitreo_jit_cold_lazy_trampoline, in u64 words.
constexpr u32 kFrameNzcv = 31;
constexpr u32 kFrameBytes = 784;
static_assert(kFrameBytes % 16 == 0, "AArch64 SP alignment");

[[nodiscard]] constexpr u32 lazy_helper_off(bool is_store, int width) noexcept {
    if (is_store) {
        return width == 1 ? JIT_CTX_OFF_WRITE8
             : width == 2 ? JIT_CTX_OFF_WRITE16
             : width == 4 ? JIT_CTX_OFF_WRITE32
                          : JIT_CTX_OFF_WRITE64;
    }
    return width == 1 ? JIT_CTX_OFF_READ8
         : width == 2 ? JIT_CTX_OFF_READ16
         : width == 4 ? JIT_CTX_OFF_READ32
                      : JIT_CTX_OFF_READ64;
}

[[nodiscard]] bool b_target(const u32* at, u32 w, uintptr_t& to) noexcept {
    if ((w & 0xFC000000u) != 0x14000000u) return false;
    const i64 words = static_cast<i32>(w << 6) >> 6;
    to = reinterpret_cast<uintptr_t>(at) + static_cast<uintptr_t>(words * 4);
    return true;
}

[[nodiscard]] bool b_reaches(uintptr_t from, uintptr_t to) noexcept {
    const i64 bytes = static_cast<i64>(to - from);
    return bytes >= -(i64{1} << 27) && bytes < (i64{1} << 27);
}

// CAS @p word from `B slot` to `B to`, if it still is the former. Caller
// has the thread's MAP_JIT writable.
void retarget(u32* word, uintptr_t slot, uintptr_t to) noexcept {
    const uintptr_t at = reinterpret_cast<uintptr_t>(word);
    if (!b_reaches(at, slot) || !b_reaches(at, to)) return;
    std::atomic_ref<u32> w{*word};
    u32 expected = arm64::b_imm(static_cast<i32>(static_cast<i64>(slot - at)));
    if (w.compare_exchange_strong(
            expected, arm64::b_imm(static_cast<i32>(static_cast<i64>(to - at))),
            std::memory_order_acq_rel)) {
        sys_icache_invalidate(word, 4);
    }
}

}  // namespace

namespace {
// Instruction RIPs whose lazy stub could not be materialized. Ordered, so a
// block's range query is one lower_bound. Grows by at most one entry per
// failing site; read on every compile only once it is non-empty.
std::mutex g_cold_eager_mu;
std::set<GuestAddr> g_cold_eager_rips;
std::atomic<bool> g_cold_eager_any{false};
std::atomic<u64> g_cold_eager_withdrawn{0};
}  // namespace

void JitCompiler::note_cold_eager_rip(GuestAddr rip) {
    std::lock_guard<std::mutex> lock(g_cold_eager_mu);
    g_cold_eager_rips.insert(rip);
    g_cold_eager_any.store(true, std::memory_order_release);
}

bool JitCompiler::cold_eager_requested(GuestAddr start, u32 length) {
    return cold_eager_requested_for(start, length);
}

bool cold_eager_requested_for(GuestAddr start, u32 length) noexcept {
    if (!g_cold_eager_any.load(std::memory_order_acquire) || length == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_cold_eager_mu);
    const auto it = g_cold_eager_rips.lower_bound(start);
    return it != g_cold_eager_rips.end() &&
           *it < start + static_cast<GuestAddr>(length);
}

void JitCompiler::service_cold_eager_pending() {
    std::vector<GuestAddr> pending;
    pending.swap(m_cold_eager_pending);
    if (m_block_cache == nullptr) return;
    std::vector<GuestAddr> starts;
    for (const GuestAddr rip : pending) {
        starts.clear();
        m_block_cache->collect_containing(rip, starts);
        for (const GuestAddr start : starts) {
            if (withdraw_block_now(start)) {
                g_cold_eager_withdrawn.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

bool JitCompiler::cold_lazy_enabled() noexcept {
    const int forced = g_cold_lazy_test_override.load(std::memory_order_relaxed);
    if (forced >= 0) return forced != 0;
    // Default on; VITREO_JIT_COLD_LAZY=0 restores eager slow paths. It was
    // opt-in while an exhausted island side-exited every later take to the
    // interpreter (a launcher: hundreds of millions of times). 2026-10-26,
    // Steam: ~12% of x64 sites take their slow path (heap reservations above
    // the linear window), so the island is sized for that (chunk/8,
    // jit_code_arena.cpp) and a site that still cannot materialize recompiles
    // its block eagerly once (cold_eager_requested_for) instead.
    static const bool lazy = [] {
        const char* v = std::getenv("VITREO_JIT_COLD_LAZY");
        return v == nullptr || (v[0] != '\0' && v[0] != '0');
    }();
    return lazy;
}

void JitCompiler::set_cold_lazy_for_test(int value) noexcept {
    g_cold_lazy_test_override.store(value, std::memory_order_relaxed);
}

ColdLazyStats cold_lazy_stats() noexcept {
    ColdLazyStats s;
    s.sites = g_lazy_sites.load(std::memory_order_relaxed);
    s.materialized = g_lazy_materialized.load(std::memory_order_relaxed);
    s.materialized_bytes =
        g_lazy_materialized_bytes.load(std::memory_order_relaxed);
    s.races = g_lazy_races.load(std::memory_order_relaxed);
    s.side_exits = g_lazy_side_exits.load(std::memory_order_relaxed);
    s.eager_withdrawn = g_cold_eager_withdrawn.load(std::memory_order_relaxed);
    s.mixed_eager = g_lazy_mixed_eager.load(std::memory_order_relaxed);
    return s;
}

void reset_cold_lazy_stats() noexcept {
    g_lazy_sites.store(0, std::memory_order_relaxed);
    g_lazy_materialized.store(0, std::memory_order_relaxed);
    g_lazy_materialized_bytes.store(0, std::memory_order_relaxed);
    g_lazy_races.store(0, std::memory_order_relaxed);
    g_lazy_side_exits.store(0, std::memory_order_relaxed);
}

void set_cold_lazy_force_side_exit_for_test(bool on) noexcept {
    g_lazy_force_side_exit.store(on, std::memory_order_relaxed);
}

void set_cold_lazy_force_island_full_for_test(bool on) noexcept {
    g_lazy_force_island_full.store(on, std::memory_order_relaxed);
}

void set_cold_lazy_mixed_eager_for_test(bool on) noexcept {
    g_lazy_mixed_eager_on.store(on, std::memory_order_relaxed);
}

void reset_cold_eager_for_test() noexcept {
    std::lock_guard<std::mutex> lock(g_cold_eager_mu);
    g_cold_eager_rips.clear();
    g_cold_eager_any.store(false, std::memory_order_release);
}

void JitCompiler::cold_lazy_write_records(u8* base, u32 split_bytes,
                                          u32 cold_delta_bytes) {
    using R = ColdLazyRecord;
    constexpr usize npos = static_cast<usize>(-1);
    const auto placed = [&](usize pos) -> i64 {
        return static_cast<i64>(split_placed_offset(
                   static_cast<u32>(pos * sizeof(u32)), split_bytes,
                   cold_delta_bytes)) /
               static_cast<i64>(sizeof(u32));
    };
    const auto fits = [](i64 words) { return words > -32768 && words < 32768; };
    for (const ColdLazySite& site : m_cold_lazy_sites) {
        const usize raise_pos = site.raise_kind == kColdLazyPlainRaise
                                    ? m_raise_entry_plain_pos
                                    : m_raise_entry_restore_pos[site.raise_kind];
        const i64 slot = placed(site.slot_pos);
        const i64 ret = placed(site.return_pos);
        const i64 raise = raise_pos != npos ? placed(raise_pos) : 0;
        const i64 guard = placed(site.guard_pos) - ret;
        const i64 mixed =
            site.mixed_pos != npos ? placed(site.mixed_pos) - ret : 0;
        // emit_flush_cold_mem_stubs admitted only blocks whose every offset
        // fits (16000 words, one split window); reaching here otherwise is a
        // layout this code does not understand.
        if (raise_pos == npos || !fits(ret - slot) || !fits(raise - slot) ||
            !fits(guard) || (site.mixed_pos != npos && !fits(mixed))) {
            std::fprintf(stderr,
                         "[jit] FATAL: lazy cold record out of range (block "
                         "0x%llx slot=%lld ret=%lld raise=%lld)\n",
                         static_cast<unsigned long long>(m_block_start_addr),
                         static_cast<long long>(slot),
                         static_cast<long long>(ret),
                         static_cast<long long>(raise));
            std::abort();
        }
        u32* const rec = reinterpret_cast<u32*>(base) + slot + 1;
        rec[0] = R::pack(ret - slot, raise - slot);
        rec[1] = R::pack(site.mixed_pos != npos ? mixed : i64{-32768}, guard);
        rec[2] = site.packed;
        rec[3] = site.config;
    }
    g_lazy_sites.fetch_add(m_cold_lazy_sites.size(), std::memory_order_relaxed);
}

bool JitCompiler::cold_lazy_emit(const u32* record, uintptr_t at, GuestAddr rip,
                                 std::vector<u32>& out, usize& entry) {
    using R = ColdLazyRecord;
    constexpr usize npos = static_cast<usize>(-1);
    std::lock_guard<std::mutex> lock(m_compile_mutex);
    const uintptr_t slot = reinterpret_cast<uintptr_t>(record) - sizeof(u32);
    const u32 w0 = record[0];
    const u32 w1 = record[1];
    const u32 w2 = record[2];
    const u32 w3 = record[3];
    const uintptr_t ret = slot + static_cast<uintptr_t>(R::lo(w0) * 4);
    const uintptr_t raise = slot + static_cast<uintptr_t>(R::hi(w0) * 4);
    // Word index of @p abs in a buffer placed at @p at; wraps for code
    // before it, which cold_branch_disp reads back as a signed distance.
    const auto rel = [&](uintptr_t abs) {
        return static_cast<usize>(static_cast<i64>(abs - at) / 4);
    };

    ColdMemStub s{};
    s.width = static_cast<int>(w2 & R::kWidthMask);
    s.is_store = (w2 & R::kStore) != 0;
    s.helper_off = lazy_helper_off(s.is_store, s.width);
    s.addr_arm = static_cast<u8>((w2 >> R::kAddrShift) & 31u);
    s.store_arm = static_cast<u8>((w2 >> R::kStoreRegShift) & 31u);
    s.dst_arm = static_cast<u8>((w2 >> R::kDstShift) & 31u);
    s.single_upper_branch = true;
    s.range_proven = true;
    s.above_branch_pos = 0;
    s.return_pos = rel(ret);
    s.mixed_page_branch_pos =
        R::has_mixed(w1)
            ? rel(ret + static_cast<uintptr_t>(R::lo(w1) * 4))
            : npos;
    s.spill_nzcv = (w2 & R::kSpillNzcv) != 0;
    s.restore_nzcv_on_raise = (w2 & R::kRestoreNzcv) != 0;
    s.nzcv_kind_on_raise = static_cast<u8>((w2 >> R::kKindShift) & 7u);
    s.range_variant = (w2 & R::kRangeVariant) != 0;
    s.tso_crossing_variant = (w2 & R::kCrossingVariant) != 0;
    s.lean_variant = (w2 & R::kLeanVariant) != 0;
    s.lean_window_is_locator = (w2 & R::kLeanLocator) != 0;
    s.lean_window_pos = npos;
    s.xmm_cache_live = static_cast<u16>(w3);

    // Everything the flush reads or writes, saved: this compiler may be
    // anywhere between two compiles of its own.
    std::vector<u32> saved_code;
    saved_code.swap(m_code_buf);
    std::vector<ColdMemStub> saved_stubs;
    saved_stubs.swap(m_cold_mem_stubs);
    std::vector<usize> saved_raise;
    saved_raise.swap(m_raise_branch_pos);
    std::array<std::vector<usize>, 5> saved_restore;
    for (usize k = 0; k < saved_restore.size(); ++k) {
        saved_restore[k].swap(m_raise_restore_nzcv_branch_pos[k]);
    }
    std::vector<usize> saved_direct;
    saved_direct.swap(m_direct_raise_branch_pos);
    std::vector<LeanVariantRange> saved_lean;
    saved_lean.swap(m_lean_variant_ranges);
    std::vector<ColdLazySite> saved_lazy;
    saved_lazy.swap(m_cold_lazy_sites);
    const JitConfig saved_cfg = m_cfg;
    const bool saved_pins = m_pins_live;
    const bool saved_64 = m_block_is_64bit;
    const bool saved_linear_base = m_linear_base_pinned;
    const bool saved_esp = m_esp_frame_live;
    const LazyFlagState saved_flags = m_flags;
    const u64 saved_rip0 = m_insn_rips[0];
    const bool saved_exceeded = m_branch_reach_exceeded;
    const char* const saved_what = m_reach_what;
    const i64 saved_words = m_reach_words;
    const unsigned saved_bits = m_reach_bits;
    const bool saved_attr_on = m_attr_on;
    const u8 saved_attr_cat = m_attr_cat;
    const usize saved_attr_mark = m_attr_mark;

    m_cfg.linear64 = true;
    m_cfg.no_jitcache = false;
    m_cfg.tso_mode = static_cast<TsoMode>((w3 >> 16) & 3u);
    m_cfg.linear64_address_bits = static_cast<u8>((w3 >> 18) & 63u);
    m_cfg.linear64_jit_view = (w2 & R::kJitView) != 0;
    m_pins_live = (w2 & R::kPinsLive) != 0;
    m_block_is_64bit = true;
    m_linear_base_pinned = (w2 & R::kLinearBasePinned) != 0;
    m_flags.nzcv_hold_depth = 0;
    m_insn_rips[0] = rip;
    m_branch_reach_exceeded = false;
    m_attr_on = false;

    m_cold_lazy_late = true;
    ++m_ts_foreign_emission;
    m_cold_mem_stubs.push_back(s);
    emit_flush_cold_mem_stubs();
    --m_ts_foreign_emission;
    m_cold_lazy_late = false;

    bool ok = !m_branch_reach_exceeded;
    // The site's raise check joins the block's raise entry the record names
    // -- the one emit_site_tail chose at compile time -- through a veneer.
    std::vector<usize> raise_checks = m_raise_branch_pos;
    for (const auto& v : m_raise_restore_nzcv_branch_pos) {
        raise_checks.insert(raise_checks.end(), v.begin(), v.end());
    }
    if (!raise_checks.empty() || !m_direct_raise_branch_pos.empty()) {
        const usize veneer = m_code_buf.size();
        const uintptr_t from = at + veneer * sizeof(u32);
        if (!b_reaches(from, raise)) ok = false;
        emit(arm64::b_imm(static_cast<i32>(static_cast<i64>(raise - from))));
        for (const usize p : raise_checks) {
            m_code_buf[p] = arm64::cbnz_w(
                16, static_cast<i32>((veneer - p) * sizeof(u32)));
        }
        for (const usize p : m_direct_raise_branch_pos) {
            m_code_buf[p] = arm64::b_imm(
                static_cast<i32>((veneer - p) * sizeof(u32)));
        }
    }
    entry = s.lean_variant
                ? 0
                : (s.range_variant ? kRangeVariantWords : 0u) +
                      (s.tso_crossing_variant ? tso_crossing_variant_words() : 0u);
    if (entry >= m_code_buf.size()) ok = false;
    // A zero word is a placeholder the flush left for a patch it skipped in
    // late mode; running it would be UDF.
    if (const auto hole = std::find(m_code_buf.begin(), m_code_buf.end(), 0u);
        ok && hole != m_code_buf.end()) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "[jit] lazy cold stub for rip 0x%llx has an unpatched "
                         "word at +%td; not materialized\n",
                         static_cast<unsigned long long>(rip),
                         (hole - m_code_buf.begin()) * 4);
        }
        ok = false;
    }
    out = std::move(m_code_buf);

    m_code_buf = std::move(saved_code);
    m_cold_mem_stubs = std::move(saved_stubs);
    m_raise_branch_pos = std::move(saved_raise);
    for (usize k = 0; k < saved_restore.size(); ++k) {
        m_raise_restore_nzcv_branch_pos[k] = std::move(saved_restore[k]);
    }
    m_direct_raise_branch_pos = std::move(saved_direct);
    m_lean_variant_ranges = std::move(saved_lean);
    m_cold_lazy_sites = std::move(saved_lazy);
    m_cfg = saved_cfg;
    m_pins_live = saved_pins;
    m_block_is_64bit = saved_64;
    m_linear_base_pinned = saved_linear_base;
    m_esp_frame_live = saved_esp;
    m_flags = saved_flags;
    m_insn_rips[0] = saved_rip0;
    m_branch_reach_exceeded = saved_exceeded;
    m_reach_what = saved_what;
    m_reach_words = saved_words;
    m_reach_bits = saved_bits;
    m_attr_on = saved_attr_on;
    m_attr_cat = saved_attr_cat;
    m_attr_mark = saved_attr_mark;
    return ok;
}

uintptr_t JitCompiler::cold_lazy_take(const u32* record, u64* frame) {
    using R = ColdLazyRecord;
    u32* const slot = const_cast<u32*>(record) - 1;
    const uintptr_t sl = reinterpret_cast<uintptr_t>(slot);
    std::atomic_ref<u32> slot_word{*slot};
    uintptr_t to = 0;
    // A thread that fetched the BL before another thread's patch landed.
    if (b_target(slot, slot_word.load(std::memory_order_acquire), to)) return to;

    const u32 w0 = record[0];
    const u32 w1 = record[1];
    const u32 w2 = record[2];
    const uintptr_t ret = sl + static_cast<uintptr_t>(R::lo(w0) * 4);
    const uintptr_t raise = sl + static_cast<uintptr_t>(R::hi(w0) * 4);
    const uintptr_t guard = ret + static_cast<uintptr_t>(R::hi(w1) * 4);
    JitFaultMetadata md{};
    if (!jit_fault_metadata_for_pc(guard, md) || !md.has_guest_rip) {
        // The block's map is published before its code is reachable, and
        // the slot exists only for a site its instruction's interval names.
        std::fprintf(stderr,
                     "[jit] FATAL: lazy cold slot %p has no owning instruction "
                     "(guard %p)\n",
                     static_cast<void*>(slot), reinterpret_cast<void*>(guard));
        std::abort();
    }

    std::lock_guard<std::mutex> lock(cold_lazy_mutex());
    if (b_target(slot, slot_word.load(std::memory_order_acquire), to)) {
        g_lazy_races.fetch_add(1, std::memory_order_relaxed);
        return to;
    }

    usize entry = 0;
    std::vector<u32> words;
    void* stub = nullptr;
    // A take whose access is an ordinary in-window access to a MIXED host
    // page (a 16 KiB page whose 4 KiB guest pages differ in protection) is
    // served by the stub's inline variant, without the helper -- and the
    // same data page keeps sending that site there. Materialized in the
    // chunk's far island it costs two far branches per access (a sort
    // kernel whose array shares a host page with a PE section: +20-30% per
    // iteration), so such a block is also recompiled with its slow paths
    // eager and adjacent: this take is served by the materialized stub, and
    // the block is swapped at this thread's next dispatch. Takes bound for
    // the helper (outside the window, a denied page, code or watched pages)
    // only materialize: the helper dwarfs the branch.
    const bool mixed_access = g_lazy_mixed_eager_on.load(std::memory_order_relaxed) && [&] {
        const u8 addr_reg = static_cast<u8>((w2 >> R::kAddrShift) & 0x1Fu);
        if (addr_reg >= 31) return false;
        const u64 ea = frame[addr_reg];
        const u8* map = m_memory.linear_check_page_map();
        const usize count = m_memory.linear_check_page_count();
        const usize index = static_cast<usize>(ea >> kLinearHostPageShift);
        if (map == nullptr || index >= count) return false;
        const u8 bits = __atomic_load_n(&map[index], __ATOMIC_RELAXED);
        const bool store = (w2 & R::kStore) != 0;
        const u8 needs_helper = static_cast<u8>(kLinearCheckHoldsCode |
                                                kLinearCheckWatched);
        return (bits & (kLinearCheckMixedProt | kLinearCheckMixedRead)) != 0 &&
               (!store || (bits & needs_helper) == 0);
    }();
    if (mixed_access && !g_lazy_force_side_exit.load(std::memory_order_relaxed) &&
        JitCodeArena::arena_live_generation(slot) != 0) {
        g_lazy_mixed_eager.fetch_add(1, std::memory_order_relaxed);
        note_cold_eager_rip(md.guest_rip);
        m_cold_eager_pending.push_back(md.guest_rip);
    }
    if (!g_lazy_force_side_exit.load(std::memory_order_relaxed) &&
        cold_lazy_emit(record, sl, md.guest_rip, words, entry)) {
        const usize bytes = words.size() * sizeof(u32);
        const u64 generation = JitCodeArena::arena_live_generation(slot);
        if (generation != 0) {
            stub = g_lazy_force_island_full.load(std::memory_order_relaxed)
                       ? nullptr
                       : JitCodeArena::alloc_far_branch(slot, generation, bytes);
            if (stub == nullptr) g_lazy_fail_island.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_lazy_fail_retired.fetch_add(1, std::memory_order_relaxed);
        }
        if (stub != nullptr) {
            // Every branch out of the stub is PC-relative: emit again for
            // the address it got. The length cannot change.
            std::vector<u32> placed;
            usize placed_entry = 0;
            if (cold_lazy_emit(record, reinterpret_cast<uintptr_t>(stub),
                               md.guest_rip, placed, placed_entry) &&
                placed.size() == words.size() && placed_entry == entry) {
                words.swap(placed);
            } else {
                stub = nullptr;  // its island bytes stay unused
            }
        }
    }
    if (stub != nullptr) {
        const usize bytes = words.size() * sizeof(u32);
        pthread_jit_write_protect_np(0);
        std::memcpy(stub, words.data(), bytes);
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(stub, bytes);
        // The whole stub belongs to the site's instruction: a fault in a
        // variant's access, or in the shared helper it called (attributed
        // through X30), is that instruction's, with its descriptor.
        const JitRipSite site{0, static_cast<u32>(bytes), md.guest_rip,
                              md.flags};
        if (!publish_jit_rip_block(reinterpret_cast<uintptr_t>(stub),
                                   static_cast<u32>(bytes), &site, 1,
                                   md.pins)) {
            stub = nullptr;
        }
    }
    if (stub == nullptr) {
        // A live chunk whose island cannot take the stub will not take it on
        // the next attempt either. Rather than leave this instruction to the
        // interpreter on every take, recompile the block holding it with an
        // eager slow path (withdrawn at this thread's next dispatch; other
        // threads withdraw theirs on their own first failed take).
        if (!g_lazy_force_side_exit.load(std::memory_order_relaxed) &&
            JitCodeArena::arena_live_generation(slot) != 0) {
            note_cold_eager_rip(md.guest_rip);
            m_cold_eager_pending.push_back(md.guest_rip);
        }
        // Leave at the instruction, as a raise would; execute() runs it.
        m_jit_ctx.cold_side_exit = true;
        m_jit_ctx.cold_side_exit_rip = md.guest_rip;
        auto* cpu = reinterpret_cast<CpuState*>(frame[19]);
        cpu->rip = md.guest_rip;
        if ((w2 & R::kSpillNzcv) != 0) {
            // The restore entry reads the guest NZCV the stub would have
            // spilled at its entry.
            u64* const block_sp = frame + kFrameBytes / sizeof(u64);
            block_sp[1] = frame[kFrameNzcv];
        }
        const u64 n =
            g_lazy_side_exits.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((n & (n - 1)) == 0) {
            std::fprintf(stderr,
                         "[jit] lazy cold stub not materialized (%llu so far; "
                         "island full %llu, chunk retired %llu): rip "
                         "0x%llx runs in the interpreter\n",
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(g_lazy_fail_island.load()),
                         static_cast<unsigned long long>(g_lazy_fail_retired.load()),
                         static_cast<unsigned long long>(md.guest_rip));
        }
        return raise;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(stub);
    const uintptr_t target = base + entry * sizeof(u32);
    pthread_jit_write_protect_np(0);
    u32 expected = slot_word.load(std::memory_order_acquire);
    slot_word.compare_exchange_strong(
        expected, arm64::b_imm(static_cast<i32>(static_cast<i64>(target - sl))),
        std::memory_order_acq_rel);
    sys_icache_invalidate(slot, 4);
    // A site the fault handler already sent to the slot (its variant was not
    // laid out yet) now goes straight to the variant.
    if ((w2 & R::kLeanVariant) == 0 && R::has_mixed(w1)) {
        const uintptr_t mixed = ret + static_cast<uintptr_t>(R::lo(w1) * 4);
        if ((w2 & R::kRangeVariant) != 0) {
            retarget(reinterpret_cast<u32*>(mixed - 8), sl, base);
        }
        if ((w2 & R::kCrossingVariant) != 0) {
            const uintptr_t crossing =
                base + ((w2 & R::kRangeVariant) != 0 ? kRangeVariantWords : 0u) *
                           sizeof(u32);
            retarget(reinterpret_cast<u32*>(mixed + 8), sl, crossing);
        }
    }
    pthread_jit_write_protect_np(1);
    g_lazy_materialized.fetch_add(1, std::memory_order_relaxed);
    g_lazy_materialized_bytes.fetch_add(words.size() * sizeof(u32),
                                        std::memory_order_relaxed);
    return target;
}

void JitCompiler::finish_cold_side_exit(CpuState& cpu) {
    const GuestAddr rip = m_jit_ctx.cold_side_exit_rip;
    m_jit_ctx.cold_side_exit = false;
    m_jit_ctx.cold_side_exit_rip = 0;
    if (cpu.pending_raise.active) return;
    jit_interpret_step(&cpu, &m_memory, rip);
}

extern "C" __attribute__((used)) uintptr_t vitreo_jit_cold_lazy_take(
        const u32* record, u64* frame, JitContext* ctx) {
    return ctx->inchain_owner->cold_lazy_take(record, frame);
}

// Entered by BR from a block's lazy entry with X30 = the record (the slot's
// BL), X16 dead and every other register live guest or block state. Saves
// all of it, NZCV and the FP control/status registers included, and resumes
// at the address cold_lazy_take returns with all of it restored.
extern "C" __attribute__((naked)) void vitreo_jit_cold_lazy_trampoline() {
    __asm__ volatile(
        "sub sp, sp, #784\n"
        "stp x0, x1, [sp, #0]\n"     "stp x2, x3, [sp, #16]\n"
        "stp x4, x5, [sp, #32]\n"    "stp x6, x7, [sp, #48]\n"
        "stp x8, x9, [sp, #64]\n"    "stp x10, x11, [sp, #80]\n"
        "stp x12, x13, [sp, #96]\n"  "stp x14, x15, [sp, #112]\n"
        "stp x16, x17, [sp, #128]\n" "stp x18, x19, [sp, #144]\n"
        "stp x20, x21, [sp, #160]\n" "stp x22, x23, [sp, #176]\n"
        "stp x24, x25, [sp, #192]\n" "stp x26, x27, [sp, #208]\n"
        "stp x28, x29, [sp, #224]\n" "str x30, [sp, #240]\n"
        "mrs x9, nzcv\n" "str x9, [sp, #248]\n"
        "mrs x9, fpsr\n" "str x9, [sp, #256]\n"
        "mrs x9, fpcr\n" "str x9, [sp, #264]\n"
        "stp q0, q1, [sp, #272]\n"   "stp q2, q3, [sp, #304]\n"
        "stp q4, q5, [sp, #336]\n"   "stp q6, q7, [sp, #368]\n"
        "stp q8, q9, [sp, #400]\n"   "stp q10, q11, [sp, #432]\n"
        "stp q12, q13, [sp, #464]\n" "stp q14, q15, [sp, #496]\n"
        "stp q16, q17, [sp, #528]\n" "stp q18, q19, [sp, #560]\n"
        "stp q20, q21, [sp, #592]\n" "stp q22, q23, [sp, #624]\n"
        "stp q24, q25, [sp, #656]\n" "stp q26, q27, [sp, #688]\n"
        "stp q28, q29, [sp, #720]\n" "stp q30, q31, [sp, #752]\n"
        "mov x0, x30\n"
        "mov x1, sp\n"
        "mov x2, x20\n"
        "bl _vitreo_jit_cold_lazy_take\n"
        "str x0, [sp, #128]\n"
        "ldp q0, q1, [sp, #272]\n"   "ldp q2, q3, [sp, #304]\n"
        "ldp q4, q5, [sp, #336]\n"   "ldp q6, q7, [sp, #368]\n"
        "ldp q8, q9, [sp, #400]\n"   "ldp q10, q11, [sp, #432]\n"
        "ldp q12, q13, [sp, #464]\n" "ldp q14, q15, [sp, #496]\n"
        "ldp q16, q17, [sp, #528]\n" "ldp q18, q19, [sp, #560]\n"
        "ldp q20, q21, [sp, #592]\n" "ldp q22, q23, [sp, #624]\n"
        "ldp q24, q25, [sp, #656]\n" "ldp q26, q27, [sp, #688]\n"
        "ldp q28, q29, [sp, #720]\n" "ldp q30, q31, [sp, #752]\n"
        "ldr x9, [sp, #264]\n" "msr fpcr, x9\n"
        "ldr x9, [sp, #256]\n" "msr fpsr, x9\n"
        "ldr x9, [sp, #248]\n" "msr nzcv, x9\n"
        "ldp x0, x1, [sp, #0]\n"     "ldp x2, x3, [sp, #16]\n"
        "ldp x4, x5, [sp, #32]\n"    "ldp x6, x7, [sp, #48]\n"
        "ldp x8, x9, [sp, #64]\n"    "ldp x10, x11, [sp, #80]\n"
        "ldp x12, x13, [sp, #96]\n"  "ldp x14, x15, [sp, #112]\n"
        "ldp x16, x17, [sp, #128]\n" "ldp x18, x19, [sp, #144]\n"
        "ldp x20, x21, [sp, #160]\n" "ldp x22, x23, [sp, #176]\n"
        "ldp x24, x25, [sp, #192]\n" "ldp x26, x27, [sp, #208]\n"
        "ldp x28, x29, [sp, #224]\n" "ldr x30, [sp, #240]\n"
        "add sp, sp, #784\n"
        "br x16\n");
}

void* cold_lazy_trampoline_address() noexcept {
    return reinterpret_cast<void*>(&vitreo_jit_cold_lazy_trampoline);
}

}  // namespace vitreo::cpu::jit
