// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_direct_link.cpp
/// @brief Direct block linking and patching
///
/// Split out of jit_compiler.cpp by pure code motion; see that file for
/// the JIT register convention.

#include "jit_crash_breadcrumbs.hpp"

#include "jit_compiler.hpp"
#include "jit_emit_internal.hpp"
#include "jit_symbol_map.hpp"
#include "jit_split_layout.hpp"
#include "ir/jit_ir.hpp"
#include "stencils.hpp"
#include "jit_coverage.hpp"
#include "jit_stats.hpp"
#include "core/cpu/decode/guest_mapped_reader.hpp"
#include "flag_liveness.hpp"
#include "jit_helpers.hpp"
#include "idiom_detector.hpp"
#include "linear_fault.hpp"
#include "hot_block_dump.hpp"
#include "../../memory/jit_region_registry.hpp"
#include "core/cpu/lock_prefix.hpp"
#include "../../memory/mem_watch.hpp"
#include "core/memory/write_watch_traps.hpp"
#include "core/math/host_fenv.hpp"
#include "core/base/perf_window.hpp"
#include "core/cpu/decode/opcodes.hpp"  // inlinable perf_window_active_fast()
#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <unordered_map>
#include <vector>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>
#include <malloc/malloc.h>
#include "jit_compile_internal.hpp"

namespace vitreo::cpu::jit {

using namespace jit_detail;

namespace {

// DIAGNOSTIC: direct-link bookkeeping over time. `touch
// /tmp/vitreo-jit-link-stats` before launch and every JIT process prints its
// cumulative counters to stderr every 2 s. A marker file, not an environment
// variable: a store-launched guest does not reliably inherit the environment.
// Off (no marker) it costs one predictable branch per patch walk.
std::atomic<u64> g_patch_walks{0};
std::atomic<u64> g_patch_site_visits{0};
std::atomic<u64> g_patch_skip_current{0};
std::atomic<u64> g_patch_sites_recorded{0};
// Sites a walk could not patch, by reason: pin layout, dead source chunk,
// no entry, and target out of the B/BL imm26 (+-128 MiB) range.
std::array<std::atomic<u64>, 4> g_patch_rejects{};
std::atomic<u64> g_far_veneers{0};
std::array<std::atomic<u64>, 8> g_link_gen_bumps{};
// Precise unlinking (unlink_direct_links_overlapping): calls, sites failed
// closed, and patches that failed their own site closed as possibly stale.
std::atomic<u64> g_unlink_calls{0};
std::atomic<u64> g_unlinked_sites{0};
std::atomic<u64> g_patch_self_poisoned{0};

bool link_stats_on() noexcept {
    static const bool on = ::access("/tmp/vitreo-jit-link-stats", F_OK) == 0;
    return on;
}

void start_link_stats_sampler() noexcept {
    if (!link_stats_on()) return;
    std::thread([] {
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            const double t = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t0).count();
            const JitCompileMemo::CompileMemoStats m =
                JitCompileMemo::compile_memo_stats();
            const auto ld = [](const std::atomic<u64>& a) {
                return static_cast<unsigned long long>(
                    a.load(std::memory_order_relaxed));
            };
            std::fprintf(
                stderr,
                "[jit-link-stats] t=%.0f mask=%u patches=%llu walks=%llu "
                "visits=%llu skip_current=%llu gen=%u retire_seq=%llu "
                "fresh=%llu adopt=%llu memo_hits=%llu smc_purges=%llu "
                "bumps=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
                "sites=%llu heap_mb=%.0f arena_mb=%.0f "
                "rejects=%llu/%llu/%llu/%llu far_veneers=%llu "
                "unlinks=%llu unlinked_sites=%llu self_poisoned=%llu\n",
                t, JitCompiler::link_edge_mask(),
                static_cast<unsigned long long>(stats::link_patches_performed()),
                ld(g_patch_walks), ld(g_patch_site_visits),
                ld(g_patch_skip_current),
                JitCompiler::link_generation_for_test(),
                static_cast<unsigned long long>(
                    JitCodeArena::s_arena_retire_seq.load(
                        std::memory_order_relaxed)),
                static_cast<unsigned long long>(m.fresh_emits),
                static_cast<unsigned long long>(m.cold_adoptions),
                static_cast<unsigned long long>(m.compile_hits),
                static_cast<unsigned long long>(m.smc_purges),
                ld(g_link_gen_bumps[0]), ld(g_link_gen_bumps[1]),
                ld(g_link_gen_bumps[2]), ld(g_link_gen_bumps[3]),
                ld(g_link_gen_bumps[4]), ld(g_link_gen_bumps[5]),
                ld(g_link_gen_bumps[6]), ld(g_link_gen_bumps[7]),
                ld(g_patch_sites_recorded),
                [] {
                    malloc_statistics_t heap{};
                    malloc_zone_statistics(nullptr, &heap);
                    return static_cast<double>(heap.size_in_use) / 1048576.0;
                }(),
                static_cast<double>(JitCodeArena::arena_bytes_used()) /
                    1048576.0,
                ld(g_patch_rejects[0]), ld(g_patch_rejects[1]),
                ld(g_patch_rejects[2]), ld(g_patch_rejects[3]),
                ld(g_far_veneers), ld(g_unlink_calls), ld(g_unlinked_sites),
                ld(g_patch_self_poisoned));
        }
    }).detach();
}
} // namespace

u32 JitCompiler::link_edge_mask() noexcept {
    static const u32 mask = [] {
        const char* env = std::getenv("VITREO_JIT_LINK_EDGES");
        if (env == nullptr || *env == '\0') return kDefaultLinkEdges;
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(env, &end, 0);
        if (end == env || *end != '\0') return kDefaultLinkEdges;
        return static_cast<u32>(parsed);
    }();
    // First use of the mask is the first link decision in this process, so
    // the DIAGNOSTIC sampler (marker-gated, see start_link_stats_sampler)
    // starts here and reports the mask the process really runs with.
    static const bool sampler_started = [] {
        start_link_stats_sampler();
        return true;
    }();
    (void)sampler_started;
    return mask;
}

bool JitCompiler::backward_link_enabled() noexcept {
    static const bool on = [] {
        const char* env = std::getenv("VITREO_JIT_LINK_BACKWARD");
        return env == nullptr || (env[0] != '0' || env[1] != '\0');
    }();
    return on;
}

void JitCompiler::emit_direct_link_slot(u64 target, LinkEdge edge) {
    AttrScope attr_scope(*this, EmitAttribution::ExitLink);
    if (m_stack_entry_load_proof) return;
    if (!s_jit_link_enabled.load(std::memory_order_acquire)) return;
    if (!link_edge_enabled(edge)) return;

    // FORWARD EDGES ONLY, and no hop budget — those two facts are one fact.
    // A patched chain built solely from forward edges has strictly increasing
    // block-start RIPs, so it terminates without a counted budget; every guest
    // cycle contains at least one edge with target <= source start, and that
    // edge keeps the counted L1 probe, which is what returns the chain to
    // execute() for the GUI pump and bounds JitCodeArena::ArenaReadLease holds.
    //
    // Do NOT "fix" this by counting forward hops here as 5be713ad tried:
    // counting forward hops was measured NOT to be what restored presents.
    //
    // The other half of that warning -- that a budget's CMP clobbers NZCV, and
    // this slot is emitted on Carried edges whose whole purpose is to carry
    // live PSTATE into the successor's Jcc -- is no longer true of the budget
    // itself. emit_cont_tail_stub_body replaced its CMP with
    // LDR/ADD/STR/EOR #0xFFF/CBZ, every one of which is flag-neutral, and has
    // used it on the backward L1-probe tail since. So a BACKWARD edge can now
    // carry a slot: it pays the same counted budget the probe pays, in the
    // same five flag-neutral instructions, and skips the ~20-instruction hash
    // probe around it.
    //
    // This matters because a backward edge is the hot one. A loop's closing
    // Jcc is backward by definition, so before this every iteration of every
    // guest loop that crossed a block boundary paid the full probe, no matter
    // which LinkEdge classes were enabled -- which is why the class mask
    // measured flat on tools/stress branchy.
    const bool backward_edge = (target <= m_block_start_addr);
    if (backward_edge && !backward_link_enabled()) return;

    // Until patched — and again after any SMC / arena generation bump — the
    // guard falls through to whatever the caller emits next (RIP publication
    // and the L1 probe tail). Patched steady state: LDR + MOVZ + EOR +
    // CBNZ(not taken) + B target, entering the successor's linked entry with
    // pins and (when carry == Carried) live PSTATE intact.
    //
    // MOVZ deliberately limits direct publication to generations <= 65535.
    // The runtime comparison remains full-width, so after the process crosses
    // that horizon old sites fail closed and new sites stay unpatched. Like
    // the preceding full-u32 design, link generations assume the process does
    // not survive 2^32 invalidation events and wrap back to an old identity.
    //
    // Flag-neutral by construction: EOR and CBNZ neither write nor read NZCV,
    // so nothing in this sequence disturbs a Carried edge.
    constexpr u32 gen_off =
        static_cast<u32>(offsetof(JitContext, link_generation));
    static_assert((gen_off & 3) == 0);
    static_assert((gen_off >> 2) < 4096);
    emit(arm64::ldr_w_uoff(17, REG_JIT_CTX, gen_off >> 2));

    const u32 gen_offset = static_cast<u32>(m_code_buf.size() * sizeof(u32));
    emit(arm64::movz_w(3, 0));
    emit(arm64::eor_w(3, 3, 17));
    const usize gen_miss = m_code_buf.size();
    emit(0);  // CBNZ W3, fallthrough

    // Counted hop budget, backward edges only. Forward-only chains have
    // strictly increasing block-start RIPs and cannot cycle, so they still
    // need no counter; every guest cycle contains at least one backward edge,
    // and this is now where that edge is counted. Same register, same cap as
    // the probe tail, so the two paths cannot disagree about how close a
    // chain is to returning to execute().
    usize hop_cap_pos = static_cast<usize>(-1);
    if (backward_edge) {
        static_assert(kMaxLinkedHops == (1u << 12),
                      "hop-cap UBFX #12 must match the limit");
        emit(arm64::add_w_imm(REG_LOOP_CTR, REG_LOOP_CTR, 1));
        emit(kHopCapTestWord);  // UBFX W4, W22, #12, #1
        hop_cap_pos = m_code_buf.size();
        emit(0);  // CBNZ W4, fallthrough
    }

    const u32 patch_offset = static_cast<u32>(m_code_buf.size() * sizeof(u32));
    emit(0);  // Atomically patched B target, initially B fallthrough

    const usize fallthrough = m_code_buf.size();
    m_code_buf[gen_miss] = arm64::cbnz_w(
        3, cold_branch_disp(gen_miss, fallthrough, 19,
                            "direct-link generation fallback"));
    if (hop_cap_pos != static_cast<usize>(-1)) {
        m_code_buf[hop_cap_pos] = arm64::cbnz_w(
            4, cold_branch_disp(hop_cap_pos, fallthrough, 19,
                                "direct-link hop budget"));
    }
    m_code_buf[patch_offset / sizeof(u32)] = arm64::b_imm(
        cold_branch_disp(patch_offset / sizeof(u32), fallthrough, 26,
                         "direct-link initial fallback"));
    m_emit_links.push_back(JitCompileMemo::DirectLinkMeta{patch_offset, gen_offset, target});
    if (stats::report_enabled()) stats::record_link_slot(static_cast<u32>(edge));
}

void JitCompiler::bump_link_generation(u32 reason) noexcept {
    s_link_generation.fetch_add(1, std::memory_order_acq_rel);
    s_predictor_epoch.fetch_add(1, std::memory_order_acq_rel);
    // Every slot just failed closed; a retargeted B.cond has no guard of its
    // own, so it goes back to its slot before anything else runs here.
    undo_jcc_retargets(nullptr);
    if (link_stats_on()) {
        g_link_gen_bumps[reason & 7].fetch_add(1, std::memory_order_relaxed);
    }
}

namespace {

// ---- Precise unlinking on guest code writes -------------------------------
// Every patched direct-link site, filed under each 4 KiB guest page its
// TARGET translated. A guest write fails closed exactly the sites whose
// target bytes it overlaps (unlink_direct_links_overlapping), where it used
// to bump the process-wide link generation and fail every site closed.
//
// The race this has to close: a writer's observer unlinks BEFORE GuestMemory
// publishes the write event, so a thread that has not consumed the event yet
// may still hold the stale translation in its cache and patch an edge into
// it after the unlink ran. Every unlink therefore also enters a short ring,
// stamped with a sequence number, and a patch checks the ring for an unlink
// after its thread's last sync that overlaps its target, failing its own
// site closed if it finds one (or if the ring wrapped past that sync).
struct LinkedSite {
    u32* gen_site{nullptr};
    u32* branch_site{nullptr};
    u32 branch_word{0};
    u64 source_generation{0};
    GuestAddr target_lo{0};
    GuestAddr target_hi{0};  // exclusive
    const void* target_code{nullptr};
};
struct UnlinkRecord {
    u64 seq{0};
    GuestAddr lo{0};
    GuestAddr hi{0};  // exclusive
};
constexpr usize kUnlinkRing = 512;  // power of two
constexpr GuestAddr kLinkPage = 0x1000;
std::mutex g_linked_mu;
// Page -> records. A flat vector, not a node per site: the registry must not
// add an allocation per linked edge (test_jit_memory_footprint.cpp), and a
// page's bucket is short, so the re-patch dedupe scan is cheap next to the
// patch's own W^X flips and icache flushes.
using LinkedBucket = std::vector<LinkedSite>;
std::unordered_map<GuestAddr, LinkedBucket> g_linked_by_page;
std::array<UnlinkRecord, kUnlinkRing> g_unlink_ring{};
std::atomic<u64> g_unlink_seq{0};  // written under g_linked_mu

[[nodiscard]] bool ranges_overlap(GuestAddr a_lo, GuestAddr a_hi,
                                  GuestAddr b_lo, GuestAddr b_hi) noexcept {
    return a_lo < b_hi && b_lo < a_hi;
}

/// Whether an unlink with sequence > `since` overlapped [lo, hi). A ring
/// that no longer reaches back to `since` answers yes: fail closed.
/// Caller holds g_linked_mu.
[[nodiscard]] bool unlinked_since_locked(u64 since, GuestAddr lo,
                                         GuestAddr hi) noexcept {
    const u64 now = g_unlink_seq.load(std::memory_order_relaxed);
    if (now <= since) return false;
    if (now - since > kUnlinkRing) return true;
    for (u64 s = since + 1; s <= now; ++s) {
        const UnlinkRecord& r = g_unlink_ring[s & (kUnlinkRing - 1)];
        if (ranges_overlap(r.lo, r.hi, lo, hi)) return true;
    }
    return false;
}

/// File `site` under every page of its target. A re-patch of the same site
/// replaces its record; a bucket that doubles drops records whose source
/// code died (retired chunks never write their sites again).
void register_linked_site_locked(const LinkedSite& site) {
    for (GuestAddr page = site.target_lo & ~(kLinkPage - 1);
         page < site.target_hi; page += kLinkPage) {
        LinkedBucket& bucket = g_linked_by_page[page];
        // No per-push dedupe: a page of many small blocks makes that scan
        // quadratic in the page's edges (measured on tools/jit_bench cold).
        // A stale duplicate is harmless -- unlinking checks the site still
        // holds the branch it recorded -- and the amortized sweep at each
        // power-of-two size drops records whose source died or whose site
        // has since been patched elsewhere.
        if (bucket.size() >= 64 && (bucket.size() & (bucket.size() - 1)) == 0) {
            std::erase_if(bucket, [](const LinkedSite& s) {
                return !JitCodeArena::arena_code_live(s.gen_site,
                                                      s.source_generation) ||
                       __atomic_load_n(s.branch_site, __ATOMIC_ACQUIRE) !=
                           s.branch_word;
            });
        }
        bucket.push_back(site);
    }
}

void write_movz_w(u32* site, u8 reg, u16 value) noexcept {
    site[0] = arm64::movz_w(reg, value);
}

// LDR X16, #8 (LDR literal, 64-bit; Arm ARM C6.2.168): the far-branch
// veneer's first word, loading the entry stored two words later.
constexpr u32 kFarBranchLdrX16 = 0x58000000u | (2u << 5) | 16u;

// ---- Mid-block Jcc retarget -------------------------------------------------
// A hot B.cond whose target is a forward slot's guard word -- nothing before
// it: no lazy-flag stub store, no kind store, so the edge carries live NZCV
// (NzcvState::Carried) exactly as the slot's patched B does -- reaches the
// successor's linked entry through four words that, while the slot is
// current, only load, compare and branch. Patching the B.cond itself to that
// entry is the same edge without them. It is valid exactly while the slot
// is: every event that fails the slot closed (a link-generation bump, the
// target's chunk retiring, a residency withdrawal of the target) restores
// the original B.cond, which branches to the guard again.
constexpr u32 kLinkGuardLdr = arm64::ldr_w_uoff(
    17, REG_JIT_CTX, static_cast<u32>(offsetof(JitContext, link_generation)) >> 2);

// B.cond (Arm ARM C6.2.26): 0101 0100 imm19 0 cond; bit 4 set is BC.cond.
// CBZ/CBNZ (C6.2.47/46): sf 011010 op imm19 Rt. Both keep imm19 in bits
// 23:5 and everything else in bits 31:24 and 4:0.
constexpr bool is_b_cond(u32 w) noexcept {
    return (w & 0xFF000010u) == 0x54000000u || (w & 0x7E000000u) == 0x34000000u;
}
constexpr i64 b_cond_bytes(u32 w) noexcept {
    return static_cast<i64>(static_cast<i32>(w << 8) >> 13) * 4;
}
constexpr u32 b_cond_to(u32 w, i64 bytes) noexcept {
    return (w & 0xFF00001Fu) | ((static_cast<u32>(bytes >> 2) & 0x7FFFFu) << 5);
}
constexpr bool b_cond_reaches(i64 bytes) noexcept {
    return (bytes & 3) == 0 && bytes >= -(i64{1} << 20) && bytes < (i64{1} << 20);
}

struct JccRetarget {
    u32* bcond{nullptr};
    u64 source_generation{0};
    u32 original{0};
    const void* target_code{nullptr};  ///< non-null while retargeted
};
/// Keyed by the slot's generation word. A slot has at most a few B.cond
/// words reaching it (the Carried path has one); extras stay unpatched.
struct JccRetargetSlot {
    std::array<JccRetarget, 2> c{};
    u8 n{0};
    bool retargeted() const noexcept {
        for (u8 k = 0; k < n; ++k) {
            if (c[k].target_code != nullptr) return true;
        }
        return false;
    }
};
std::mutex g_retarget_mu;
std::unordered_map<u32*, JccRetargetSlot> g_retarget_slots;
/// Slots by the target code a B.cond was patched to, so withdrawing one
/// target visits only its own edges. A slot whose B.cond moved to another
/// target, or was already undone, may stay in a bucket; undo skips it.
std::unordered_map<const void*, std::vector<u32*>> g_retarget_live;
usize g_retarget_prune_at{4096};
std::atomic<usize> g_retarget_slot_count{0};
std::atomic<u64> g_retarget_candidates{0};
std::atomic<u64> g_retarget_patches{0};
std::atomic<u64> g_retarget_undos{0};

void prune_dead_retargets_locked() {
    for (auto it = g_retarget_slots.begin(); it != g_retarget_slots.end();) {
        const JccRetarget& c = it->second.c[0];
        if (!it->second.retargeted() &&
            !JitCodeArena::arena_code_live(c.bcond, c.source_generation)) {
            it = g_retarget_slots.erase(it);
        } else {
            ++it;
        }
    }
    g_retarget_prune_at = std::max<usize>(4096, 2 * g_retarget_slots.size());
}

} // namespace

void JitCompiler::note_jcc_retarget_candidates(u8* mem, u32 hot_bytes,
                                               u64 chunk_generation) {
    struct Guard { const u32* guard; u32* gen_site; };
    std::array<Guard, 16> guards{};
    usize n_guards = 0;
    for (const JitCompileMemo::DirectLinkMeta& link : m_emit_links) {
        // Forward slot only: LDR; MOVZ (gen); EOR; CBNZ; B. A backward slot
        // (hop budget) or a call slot (BL after a MOV) has another shape.
        if (link.gen_offset < 4 || link.patch_offset != link.gen_offset + 12) continue;
        auto* gen_site = reinterpret_cast<u32*>(mem + link.gen_offset);
        const u32 patch = *reinterpret_cast<const u32*>(mem + link.patch_offset);
        if (gen_site[-1] != kLinkGuardLdr || (patch & 0xFC000000u) != 0x14000000u) continue;
        if (n_guards == guards.size()) break;
        guards[n_guards++] = Guard{gen_site - 1, gen_site};
    }
    if (n_guards == 0) return;
    auto* const words = reinterpret_cast<u32*>(mem);
    std::lock_guard<std::mutex> lock(g_retarget_mu);
    for (u32 i = 0; i < hot_bytes / sizeof(u32); ++i) {
        const u32 w = words[i];
        if (!is_b_cond(w)) continue;
        const u32* reached = words + i + b_cond_bytes(w) / 4;
        for (usize g = 0; g < n_guards; ++g) {
            if (reached != guards[g].guard) continue;
            JccRetargetSlot& slot = g_retarget_slots[guards[g].gen_site];
            if (slot.n != 0 &&
                !JitCodeArena::arena_code_live(slot.c[0].bcond,
                                               slot.c[0].source_generation)) {
                slot = JccRetargetSlot{};  // a dead block's slot at this address
            }
            if (slot.n < slot.c.size()) {
                slot.c[slot.n++] = JccRetarget{words + i, chunk_generation, w, nullptr};
                g_retarget_candidates.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (g_retarget_slots.size() > g_retarget_prune_at) prune_dead_retargets_locked();
    g_retarget_slot_count.store(g_retarget_slots.size(), std::memory_order_release);
}

void JitCompiler::retarget_jcc_into(u32* gen_site, const void* target_code,
                                    void* entry, u32 gen) noexcept {
    if (g_retarget_slot_count.load(std::memory_order_acquire) == 0 ||
        !jcc_retarget_enabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_retarget_mu);
    // A bump between the slot's publication and here already ran (or is
    // waiting on this mutex to run) its undo: never patch a stale slot.
    if (s_link_generation.load(std::memory_order_acquire) != gen) return;
    const auto it = g_retarget_slots.find(gen_site);
    if (it == g_retarget_slots.end()) return;
    JccRetargetSlot& slot = it->second;
    for (u8 k = 0; k < slot.n; ++k) {
        JccRetarget& c = slot.c[k];
        if (!JitCodeArena::arena_code_live(c.bcond, c.source_generation)) continue;
        const i64 bytes = static_cast<i64>(reinterpret_cast<intptr_t>(entry) -
                                           reinterpret_cast<intptr_t>(c.bcond));
        if (!b_cond_reaches(bytes)) continue;
        const u32 want = b_cond_to(c.original, bytes);
        if (c.bcond[0] == want && c.target_code == target_code) continue;
        jit_set_writable(true);
        __atomic_store_n(c.bcond, want, __ATOMIC_RELEASE);
        jit_set_writable(false);
        sys_icache_invalidate(c.bcond, sizeof(u32));
        if (c.target_code != target_code) g_retarget_live[target_code].push_back(gen_site);
        c.target_code = target_code;
        // Retiring the target restores the B.cond, as it poisons the slot.
        JitCodeArena::arena_note_incoming_link(target_code, c.bcond,
                                               c.source_generation, c.bcond,
                                               c.original);
        g_retarget_patches.fetch_add(1, std::memory_order_relaxed);
    }
}

void JitCompiler::undo_jcc_retargets(const void* target_code) noexcept {
    if (g_retarget_slot_count.load(std::memory_order_acquire) == 0) return;
    std::lock_guard<std::mutex> lock(g_retarget_mu);
    const auto undo_slots = [target_code](const std::vector<u32*>& keys) {
        for (u32* key : keys) {
            const auto it = g_retarget_slots.find(key);
            if (it == g_retarget_slots.end()) continue;
            JccRetargetSlot& slot = it->second;
            for (u8 k = 0; k < slot.n; ++k) {
                JccRetarget& c = slot.c[k];
                if (c.target_code == nullptr) continue;
                if (target_code != nullptr && c.target_code != target_code) continue;
                if (JitCodeArena::arena_write_word_if_live(c.bcond, c.source_generation,
                                                           c.original)) {
                    g_retarget_undos.fetch_add(1, std::memory_order_relaxed);
                }
                c.target_code = nullptr;
            }
        }
    };
    if (target_code == nullptr) {
        for (const auto& [target, keys] : g_retarget_live) undo_slots(keys);
        g_retarget_live.clear();
        return;
    }
    const auto bucket = g_retarget_live.find(target_code);
    if (bucket == g_retarget_live.end()) return;
    undo_slots(bucket->second);
    g_retarget_live.erase(bucket);
}

void JitCompiler::unlink_own_direct_sites() noexcept {
    s_predictor_epoch.fetch_add(1, std::memory_order_acq_rel);
    // The B.cond retargets of these slots first: they bypass the guard.
    if (g_retarget_slot_count.load(std::memory_order_acquire) != 0) {
        std::lock_guard<std::mutex> lock(g_retarget_mu);
        for (auto& [target, entry] : m_direct_patch_targets) {
            entry.for_each([&](DirectPatchSite& site) {
                if (site.code == nullptr) return;
                auto* gen_site = reinterpret_cast<u32*>(
                    static_cast<u8*>(site.code) + site.gen_offset);
                const auto it = g_retarget_slots.find(gen_site);
                if (it == g_retarget_slots.end()) return;
                for (u8 k = 0; k < it->second.n; ++k) {
                    JccRetarget& c = it->second.c[k];
                    if (c.target_code == nullptr) continue;
                    if (JitCodeArena::arena_write_word_if_live(
                            c.bcond, c.source_generation, c.original)) {
                        g_retarget_undos.fetch_add(1, std::memory_order_relaxed);
                    }
                    c.target_code = nullptr;
                }
            });
        }
    }
    for (auto& [target, entry] : m_direct_patch_targets) {
        entry.for_each([&](DirectPatchSite& site) {
            if (site.code == nullptr) return;
            (void)JitCodeArena::arena_write_word_if_live(
                reinterpret_cast<u32*>(static_cast<u8*>(site.code) +
                                       site.gen_offset),
                site.arena_generation, arm64::movz_w(3, 0));
        });
    }
}

void JitCompiler::jcc_retarget_stats(ColdResidencyStats& out) noexcept {
    out.retarget_candidates = g_retarget_candidates.load(std::memory_order_relaxed);
    out.retargets = g_retarget_patches.load(std::memory_order_relaxed);
    out.retarget_undos = g_retarget_undos.load(std::memory_order_relaxed);
}

u64 JitCompiler::unlink_sequence() noexcept {
    return g_unlink_seq.load(std::memory_order_acquire);
}

void JitCompiler::unlink_direct_links_overlapping(GuestAddr addr, usize size,
                                                  bool writer) noexcept {
    if (writer) s_predictor_epoch.fetch_add(1, std::memory_order_acq_rel);
    if (size == 0) return;
    const GuestAddr lo = addr;
    const GuestAddr hi = addr + size < addr ? ~GuestAddr{0} : addr + size;
    std::vector<LinkedSite> hit;
    {
        std::lock_guard<std::mutex> lock(g_linked_mu);
        if (writer) {
            const u64 seq = g_unlink_seq.load(std::memory_order_relaxed) + 1;
            g_unlink_ring[seq & (kUnlinkRing - 1)] = UnlinkRecord{seq, lo, hi};
            g_unlink_seq.store(seq, std::memory_order_release);
        }
        if (!g_linked_by_page.empty()) {
            // A target spans at most a few pages and is filed under each of
            // them, so walking the written pages finds every overlapping one.
            for (GuestAddr page = lo & ~(kLinkPage - 1); page < hi;
                 page += kLinkPage) {
                const auto it = g_linked_by_page.find(page);
                if (it != g_linked_by_page.end()) {
                    LinkedBucket& bucket = it->second;
                    for (usize i = 0; i < bucket.size();) {
                        if (ranges_overlap(bucket[i].target_lo,
                                           bucket[i].target_hi, lo, hi)) {
                            hit.push_back(bucket[i]);
                            bucket[i] = bucket.back();
                            bucket.pop_back();
                        } else {
                            ++i;
                        }
                    }
                    if (bucket.empty()) g_linked_by_page.erase(it);
                }
                if (page + kLinkPage < page) break;  // top of the space
            }
        }
    }
    g_unlink_calls.fetch_add(1, std::memory_order_relaxed);
    if (hit.empty()) return;
    // Outside the registry lock: arena_write_word_if_live takes the chunk
    // mutex. Failing a site closed is always safe, so a race with a re-patch
    // can only cost that edge a later re-patch; a site whose branch was
    // re-patched to another target meanwhile was validated against this
    // unlink by its own patcher (the ring), so it is left alone.
    for (const LinkedSite& s : hit) {
        if (__atomic_load_n(s.branch_site, __ATOMIC_ACQUIRE) != s.branch_word) {
            continue;
        }
        if (JitCodeArena::arena_write_word_if_live(s.gen_site, s.source_generation,
                                                   arm64::movz_w(3, 0))) {
            g_unlinked_sites.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // A retargeted mid-block B.cond into one of these targets bypasses the
    // guard entirely; it goes back to its slot now, not at a later bump.
    const void* last = nullptr;
    for (const LinkedSite& s : hit) {
        if (s.target_code == last) continue;
        last = s.target_code;
        undo_jcc_retargets(s.target_code);
    }
}

void JitCompiler::try_patch_direct_link(const CompiledBlock& block,
                                       const void* only_source) {
    const auto tit = m_direct_patch_targets.find(block.guest_addr);
    if (tit == m_direct_patch_targets.end() || tit->second.empty() ||
        block.code == nullptr || block.requires_protection_lease) {
        return;
    }
    DirectPatchTarget& entry = tit->second;
    // A target whose guest extent is unknown could not be unlinked by the
    // write that rewrites it (unlink_direct_links_overlapping): leave its
    // edges on the generic tail.
    if (block.guest_size == 0) return;
    // Patching writes arena bytes: hold the lease so neither the source nor
    // the target chunk can be recycled under the write, and validate both
    // under it. A closed gate simply leaves the edge for a later publish.
    JitCodeArena::ArenaReadLease patch_lease;
    if (!patch_lease) return;
    if (!JitCodeArena::arena_code_live(block.code, block.arena_generation)) {
        return;
    }

    void* const target_link = block.link_entry();
    const u32 gen = s_link_generation.load(std::memory_order_relaxed);
    // The compact site embeds its expected generation in one MOVZ. Never
    // truncate: after the 16-bit horizon, leaving the existing full-width
    // guard in place fails closed to the generic L1 path for the remaining
    // non-wrapping lifetime of the generation counter.
    if (gen > std::numeric_limits<u16>::max()) return;
    const bool count_stats = link_stats_on();
    if (count_stats) g_patch_walks.fetch_add(1, std::memory_order_relaxed);
    entry.for_each([&](DirectPatchSite& site) {
        if (count_stats) {
            g_patch_site_visits.fetch_add(1, std::memory_order_relaxed);
        }
        void* const src_code = site.code;
        if (!src_code) return;
        if (only_source != nullptr && src_code != only_source) return;
        const auto reject = [&](usize reason) {
            if (count_stats) {
                g_patch_rejects[reason].fetch_add(1, std::memory_order_relaxed);
            }
        };
        if (site.expected_pin_layout != block.pin_layout) return reject(0);
        // The source chunk may have been retired since the site was
        // recorded; synchronize_arena_generation() drops such sites at the
        // next dispatcher entry, but this publish can run before it.
        if (!JitCodeArena::arena_code_live(src_code, site.arena_generation)) {
            return reject(1);
        }
        auto* gen_site = reinterpret_cast<u32*>(
            static_cast<u8*>(src_code) + site.gen_offset);
        auto* branch_site = reinterpret_cast<u32*>(
            static_cast<u8*>(src_code) + site.patch_offset);
        // A native guest-CALL site is a BL from the moment it is emitted
        // (emit_linked_call_continue) and enters the callee's landing pad,
        // which pushes the continuation frame its RET returns through; every
        // other site is a B to the linked entry. The opcode is the record:
        // it survives split relocation, memo sharing and re-patching.
        const bool call_site = (branch_site[0] & 0xFC000000u) == 0x94000000u;
        void* const target_entry = call_site ? block.call_entry() : target_link;
        if (target_entry == nullptr) return reject(2);
        const auto bytes_to = [&](const void* to) {
            return static_cast<i64>(reinterpret_cast<intptr_t>(to) -
                                    reinterpret_cast<intptr_t>(branch_site));
        };
        const auto reachable = [](i64 bytes) {  // B/BL imm26: +-128 MiB
            return (bytes & 3) == 0 && bytes >= -(i64{1} << 27) &&
                   bytes < (i64{1} << 27);
        };
        i64 branch_bytes = bytes_to(target_entry);
        if (!reachable(branch_bytes)) {
            // Out of range: branch to a far-branch veneer in the source's own
            // chunk instead, `LDR X16, #8; BR X16; .quad entry`. X16 is what
            // the L1 probe tail enters a linked entry through as well, and
            // neither instruction touches NZCV, X0 or X30 (a BL site's
            // return address), so the target sees exactly a probe hop.
            // Reuse the veneer this site already reaches when it still names
            // this entry, so a re-patch after a generation bump costs no
            // island space.
            const u32 current = branch_site[0];
            u32* veneer = nullptr;
            if ((current & 0x7C000000u) == 0x14000000u) {  // B or BL
                const i64 imm = static_cast<i64>(
                    static_cast<i32>(current << 6) >> 6) * 4;
                auto* const reached = reinterpret_cast<u32*>(
                    reinterpret_cast<u8*>(branch_site) + imm);
                if (JitCodeArena::in_far_branch_island(src_code, reached) &&
                    reached[0] == kFarBranchLdrX16 &&
                    reached[1] == arm64::br(16) &&
                    *reinterpret_cast<void* const*>(reached + 2) ==
                        target_entry) {
                    veneer = reached;
                }
            }
            if (veneer == nullptr) {
                veneer = static_cast<u32*>(JitCodeArena::alloc_far_branch(
                    src_code, site.arena_generation, 4 * sizeof(u32)));
                if (veneer == nullptr) return reject(3);
                // Complete and visible before any branch can reach it.
                jit_set_writable(true);
                veneer[0] = kFarBranchLdrX16;
                veneer[1] = arm64::br(16);
                *reinterpret_cast<void**>(veneer + 2) = target_entry;
                jit_set_writable(false);
                sys_icache_invalidate(veneer, 4 * sizeof(u32));
                if (count_stats) {
                    g_far_veneers.fetch_add(1, std::memory_order_relaxed);
                }
            }
            branch_bytes = bytes_to(veneer);
            if (!reachable(branch_bytes)) return reject(3);
        }
        // Already current: the branch reaches this very entry and the guard
        // admits the current generation. Rewriting it costs two W^X flips,
        // two icache flushes, and a re-registration of the incoming link,
        // per site, on every publish that touches a popular target. That
        // cost is quadratic in a callee's caller count. A browser compile
        // thread spent a quarter of its time here with CALL rel links on.
        const u32 want_branch = call_site
            ? arm64::bl_imm(static_cast<i32>(branch_bytes))
            : arm64::b_imm(static_cast<i32>(branch_bytes));
        if (branch_site[0] == want_branch &&
            gen_site[0] == arm64::movz_w(3, static_cast<u16>(gen))) {
            if (count_stats) {
                g_patch_skip_current.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        // The target's cache entry is not the lifetime of a direct edge. An
        // LRU eviction may drop the cache's final code-page reference while
        // this source and its embedded host branch remain executable. Retain
        // the target pages independently before publishing the patch so any
        // later rewrite still invalidates the link generation. Retained once
        // per target: a second site into the same target, or a re-patch
        // after a generation bump, finds its pages already held and only
        // widens the bound if the target grew.
        if (block.guest_size != 0) {
            const GuestAddr first_page = block.guest_addr & ~GuestAddr{0xFFF};
            const GuestAddr new_end =
                ((block.guest_addr + block.guest_size - 1) & ~GuestAddr{0xFFF}) +
                0x1000;
            if (new_end > entry.retained_end) {
                for (GuestAddr page = std::max(first_page, entry.retained_end);
                     page < new_end; page += 0x1000) {
                    m_memory.retain_jit_code_page(page);
                }
                entry.retained_end = new_end;
            }
        }
        if (block.delegate_insns && entry.retained_insns != block.delegate_insns) {
            entry.retained_insns = block.delegate_insns;
        }
        // Publish the single-instruction direct branch first and the matching
        // generation last. The old generation cannot reach a partly-published
        // edge, and a future generation bump fails closed to the generic tail.
        jit_set_writable(true);
        branch_site[0] = want_branch;
        jit_set_writable(false);
        sys_icache_invalidate(branch_site, sizeof(u32));
        jit_set_writable(true);
        write_movz_w(gen_site, 3, static_cast<u16>(gen));
        jit_set_writable(false);
        sys_icache_invalidate(gen_site, sizeof(u32));
        // A later write to the target's guest bytes must fail this site
        // closed: file it, unless an unlink this thread has not consumed yet
        // already covered those bytes, in which case the translation being
        // linked may be the stale one and the site goes back to failing
        // closed right now (see the registry comment above).
        bool stale = false;
        {
            const GuestAddr lo = block.guest_addr;
            const GuestAddr hi = block.guest_addr + block.guest_size;
            std::lock_guard<std::mutex> lock(g_linked_mu);
            stale = unlinked_since_locked(m_unlink_seq_at_sync, lo, hi);
            if (!stale) {
                register_linked_site_locked(LinkedSite{
                    gen_site, branch_site, want_branch, site.arena_generation,
                    lo, hi, block.code});
            }
        }
        if (stale) {
            jit_set_writable(true);
            write_movz_w(gen_site, 3, 0);
            jit_set_writable(false);
            sys_icache_invalidate(gen_site, sizeof(u32));
            g_patch_self_poisoned.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        m_patched_since_sync = true;
        // Retiring the target's chunk must fail this site closed again.
        JitCodeArena::arena_note_incoming_link(block.code, src_code,
                                 site.arena_generation, gen_site);
        stats::record_link_patch();
        // Only after the slot is current: the B.cond is that slot's edge.
        if (!call_site) retarget_jcc_into(gen_site, block.code, target_link, gen);
    });
}

void JitCompiler::remember_direct_link(const CompiledBlock& block) {
    const auto it = m_direct_links.find(block.guest_addr);
    if (it == m_direct_links.end() || block.code == nullptr) {
        return;
    }
    for (const JitCompileMemo::DirectLinkMeta& meta : it->second) {
        // An adopted block is re-linked every time this thread re-adopts it
        // (e.g. after LRU eviction). Recording the same site again would make
        // the target's list grow without bound: every duplicate is walked by
        // every try_patch_direct_link.
        DirectPatchTarget& entry = m_direct_patch_targets[meta.target];
        bool known = false;
        entry.for_each([&](const DirectPatchSite& site) {
            if (site.code == block.code && site.patch_offset == meta.patch_offset) {
                known = true;
            }
        });
        if (known) continue;
        entry.push(DirectPatchSite{block.code, block.pin_layout,
                                   block.arena_generation, meta.patch_offset,
                                   meta.gen_offset});
        if (link_stats_on()) {
            g_patch_sites_recorded.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void JitCompiler::release_patch_target_retains(GuestAddr target,
                                               DirectPatchTarget& entry) noexcept {
    if (entry.retained_end != 0) {
        for (GuestAddr page = target & ~GuestAddr{0xFFF};
             page < entry.retained_end; page += 0x1000) {
            m_memory.release_jit_code_page(page);
        }
        entry.retained_end = 0;
    }
    entry.retained_insns.reset();
}

void JitCompiler::release_direct_link_targets() noexcept {
    for (auto& [target, entry] : m_direct_patch_targets) {
        release_patch_target_retains(target, entry);
    }
}

} // namespace vitreo::cpu::jit
