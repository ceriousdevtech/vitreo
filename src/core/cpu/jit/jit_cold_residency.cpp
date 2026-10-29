// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_cold_residency.cpp
/// @brief Cold-entry counters, residency profiles and the one-shot
///        residency recompile (jit_compiler_cold_residency.hpp).

#include "jit_compiler.hpp"
#include "jit_emit_internal.hpp"
#include "jit_helpers.hpp"
#include "stencils.hpp"
#include "jit_compile_internal.hpp"
#include "block_cache.hpp"
#include "jit_split_layout.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>

namespace vitreo::cpu::jit {

using namespace jit_detail;

namespace {

std::atomic<int> g_residency_override{-1};
std::atomic<int> g_retarget_override{-1};

bool env_residency() noexcept {
    static const bool on = environment_flag_enabled("VITREO_JIT_COLD_RESIDENCY", true);
    return on;
}
bool env_retarget() noexcept {
    static const bool on = environment_flag_enabled("VITREO_JIT_JCC_RETARGET", true);
    return on;
}

// ---- Counter runs -----------------------------------------------------------
// One mapping, bump-allocated in 8-byte units. A site's data word names its
// run as (unit << 6) | ordinal: 20 unit bits keep the word below 2^26, which
// no branch decoder (jit_split_layout) reads as a branch.
constexpr u32 kRunUnitBits = 20;
constexpr usize kRegionBytes = usize{8} << kRunUnitBits;
struct RunHeader {
    u64 start;      ///< guest block start; 0 for the overflow run
    u64 code_hash;  ///< identity the run was bound under
    u32 n;          ///< counters that follow
    u32 reserved;
};
static_assert(sizeof(RunHeader) == 24, "the counter tail adds 24 to the header");
constexpr u32 kSiteOrdinalMask = 63;
static_assert(JitCompiler::kMaxColdPieces == kSiteOrdinalMask + 1);
// Run 0 is the overflow run: sites bound after the mapping is full count
// there and their requests are ignored.
constexpr usize kOverflowRunBytes =
    (sizeof(RunHeader) + JitCompiler::kMaxColdPieces * sizeof(u32) + 7) & ~usize{7};

std::mutex g_region_mu;
u8* g_region{nullptr};
bool g_region_failed{false};
usize g_region_used{0};

u8* region_base() {
    std::lock_guard<std::mutex> lock(g_region_mu);
    if (g_region == nullptr && !g_region_failed) {
        void* p = ::mmap(nullptr, kRegionBytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) {
            g_region_failed = true;
            return nullptr;
        }
        g_region = static_cast<u8*>(p);
        reinterpret_cast<RunHeader*>(g_region)->n = JitCompiler::kMaxColdPieces;
        g_region_used = kOverflowRunBytes;
    }
    return g_region;
}

u32 alloc_run(GuestAddr start, u64 code_hash, u32 n) {
    const usize bytes = (sizeof(RunHeader) + n * sizeof(u32) + 7) & ~usize{7};
    std::lock_guard<std::mutex> lock(g_region_mu);
    if (g_region == nullptr || g_region_used + bytes > kRegionBytes) return 0;
    const usize at = g_region_used;
    g_region_used += bytes;
    auto* h = reinterpret_cast<RunHeader*>(g_region + at);
    h->start = start;
    h->code_hash = code_hash;
    h->n = n;
    return static_cast<u32>(at / 8);
}

const RunHeader* header_from_request(u64 request) noexcept {
    const u8* region = g_region;
    if (region == nullptr) return nullptr;
    const auto p = static_cast<uintptr_t>(request);
    const auto lo = reinterpret_cast<uintptr_t>(region);
    if (p < lo || p >= lo + kRegionBytes - sizeof(RunHeader) || (p & 7) != 0) {
        return nullptr;
    }
    return reinterpret_cast<const RunHeader*>(p);
}

const u32* run_counters(const RunHeader* h) noexcept {
    return reinterpret_cast<const u32*>(h + 1);
}

// ---- Profiles ---------------------------------------------------------------
// One per block start. `code_hash` 0 (a stack-proof deopt, whose bytes the
// runtime did not hash) adopts the identity of the first compile after it.
struct Profile {
    u64 code_hash;
    u64 hot_mask;
    bool drop_proof;
};
constexpr usize kMaxProfiles = usize{1} << 16;
std::mutex g_profile_mu;
std::unordered_map<GuestAddr, Profile> g_profiles;
std::atomic<usize> g_profile_count{0};

struct Stats {
    std::atomic<u64> counter_runs{0};
    std::atomic<u64> counter_sites{0};
    std::atomic<u64> requests{0};
    std::atomic<u64> profiles{0};
    std::atomic<u64> withdrawals{0};
    std::atomic<u64> variant_compiles{0};
    std::atomic<u64> variant_split_moves{0};
    std::atomic<u64> proof_drops{0};
};
Stats g_stats;

void bump(std::atomic<u64>& a, u64 n = 1) noexcept {
    a.fetch_add(n, std::memory_order_relaxed);
}

/// Record a profile unless one already describes this identity. A profile
/// for different code bytes is a new epoch and is replaced.
void record_profile(GuestAddr start, u64 code_hash, u64 hot_mask, bool drop_proof) {
    std::lock_guard<std::mutex> lock(g_profile_mu);
    const auto it = g_profiles.find(start);
    if (it != g_profiles.end()) {
        const u64 have = it->second.code_hash;
        if (have == 0 || code_hash == 0 || have == code_hash) return;
        it->second = Profile{code_hash, hot_mask, drop_proof};
    } else {
        if (g_profiles.size() >= kMaxProfiles) return;
        g_profiles.emplace(start, Profile{code_hash, hot_mask, drop_proof});
        g_profile_count.store(g_profiles.size(), std::memory_order_release);
    }
    bump(g_stats.profiles);
    if (drop_proof) bump(g_stats.proof_drops);
}

u64 withdrawal_key(GuestAddr start, u64 code_hash) noexcept {
    return start ^ (code_hash * 0x9E3779B97F4A7C15ull);
}

constexpr u32 kBlrX30 = 0xD63F03C0u;  // BLR X30

}  // namespace

bool JitCompiler::cold_residency_enabled() noexcept {
    const int v = g_residency_override.load(std::memory_order_relaxed);
    return v < 0 ? env_residency() : v != 0;
}

bool JitCompiler::jcc_retarget_enabled() noexcept {
    const int v = g_retarget_override.load(std::memory_order_relaxed);
    return cold_residency_enabled() && (v < 0 ? env_retarget() : v != 0);
}

void JitCompiler::set_cold_residency_for_test(int on) noexcept {
    g_residency_override.store(on < 0 ? -1 : (on != 0), std::memory_order_relaxed);
}

void JitCompiler::set_jcc_retarget_for_test(int on) noexcept {
    g_retarget_override.store(on < 0 ? -1 : (on != 0), std::memory_order_relaxed);
}

void JitCompiler::reset_cold_residency_for_test() {
    {
        std::lock_guard<std::mutex> lock(g_profile_mu);
        g_profiles.clear();
        g_profile_count.store(0, std::memory_order_release);
    }
    m_residency_withdrawn.clear();
    m_proof_deopts.clear();
    m_proof_request_rip = 0;
    m_jit_ctx.cold_residency_request = 0;
    m_jit_ctx.tier_request_count = 0;
}

JitCompiler::ColdResidencyStats JitCompiler::cold_residency_stats() noexcept {
    ColdResidencyStats out;
    const auto ld = [](const std::atomic<u64>& a) {
        return a.load(std::memory_order_relaxed);
    };
    out.counter_runs = ld(g_stats.counter_runs);
    out.counter_sites = ld(g_stats.counter_sites);
    out.requests = ld(g_stats.requests);
    out.profiles = ld(g_stats.profiles);
    out.withdrawals = ld(g_stats.withdrawals);
    out.variant_compiles = ld(g_stats.variant_compiles);
    out.variant_split_moves = ld(g_stats.variant_split_moves);
    out.proof_drops = ld(g_stats.proof_drops);
    jcc_retarget_stats(out);
    return out;
}

namespace {

void print_cold_residency_stats() {
    const JitCompiler::ColdResidencyStats s = JitCompiler::cold_residency_stats();
    std::fprintf(stderr,
                 "[jit] cold-residency runs=%llu sites=%llu requests=%llu "
                 "profiles=%llu withdrawals=%llu variants=%llu moves=%llu "
                 "proof-drops=%llu jcc-candidates=%llu retargets=%llu "
                 "undos=%llu\n",
                 static_cast<unsigned long long>(s.counter_runs),
                 static_cast<unsigned long long>(s.counter_sites),
                 static_cast<unsigned long long>(s.requests),
                 static_cast<unsigned long long>(s.profiles),
                 static_cast<unsigned long long>(s.withdrawals),
                 static_cast<unsigned long long>(s.variant_compiles),
                 static_cast<unsigned long long>(s.variant_split_moves),
                 static_cast<unsigned long long>(s.proof_drops),
                 static_cast<unsigned long long>(s.retarget_candidates),
                 static_cast<unsigned long long>(s.retargets),
                 static_cast<unsigned long long>(s.retarget_undos));
}

[[maybe_unused]] const bool g_stats_at_exit = [] {
    if (environment_flag_enabled("VITREO_JIT_COLD_RESIDENCY_STATS", false)) {
        std::atexit(print_cold_residency_stats);
    }
    return true;
}();

}  // namespace

u32 JitCompiler::cold_counter_site_word() noexcept {
    return arm64::ldr_x_uoff(30, REG_JIT_CTX,
                             (JIT_CTX_OFF_DISPATCH_TAIL >> 3) + kColdCounter);
}

u32 JitCompiler::cold_counter_value_for_test(u32 data_word) noexcept {
    std::lock_guard<std::mutex> lock(g_region_mu);
    if (g_region == nullptr) return 0;
    const usize at = static_cast<usize>(data_word >> 6) * 8;
    if (at + sizeof(RunHeader) > g_region_used) return 0;
    const auto* h = reinterpret_cast<const RunHeader*>(g_region + at);
    const u32 ordinal = data_word & kSiteOrdinalMask;
    return ordinal < h->n ? run_counters(h)[ordinal] : 0;
}

void JitCompiler::select_cold_residency(GuestAddr start, u64 code_hash,
                                        bool have_hash) {
    m_residency_counters = false;
    m_residency_variant = false;
    m_residency_drop_proof = false;
    m_residency_hot_mask = 0;
    if (!cold_residency_enabled() || !have_hash) return;
    if (g_profile_count.load(std::memory_order_acquire) != 0) {
        std::lock_guard<std::mutex> lock(g_profile_mu);
        const auto it = g_profiles.find(start);
        if (it != g_profiles.end()) {
            Profile& p = it->second;
            if (p.code_hash == 0) p.code_hash = code_hash;
            if (p.code_hash == code_hash) {
                m_residency_variant = true;
                m_residency_drop_proof = p.drop_proof;
                m_residency_hot_mask = p.hot_mask;
                return;
            }
            // The bytes changed under the profile: a new epoch, counted afresh.
            g_profiles.erase(it);
            g_profile_count.store(g_profiles.size(), std::memory_order_release);
        }
    }
    // A tier-1 compile is final: the tier-0 translation's counters already
    // decided its profile.
    m_residency_counters = m_cfg.split_cold && !(m_tier_active && !m_tier_stencil);
}

void JitCompiler::emit_cold_entry_counter(u32 ordinal) {
    if (!m_residency_counters || m_cold_lazy_late || ordinal >= kMaxColdPieces) {
        return;
    }
    // Unconditionally: materialize_dispatch_tail also notes the tail in the
    // translation-store record, which a restoring process publishes.
    if (materialize_dispatch_tail(kColdCounter) == nullptr) {
        m_residency_counters = false;
        return;
    }
    emit(cold_counter_site_word());
    emit(kBlrX30);
    emit(ordinal);  // data word; bind_cold_counters names the run
}

void JitCompiler::emit_cold_counter_tail_body() {
    using namespace arm64;
    constexpr u32 save = JIT_CTX_OFF_COLD_COUNTER_SAVE >> 3;
    u8* const base = region_base();
    if (base == nullptr) {
        emit(add_x_imm(30, 30, 4));
        emit(ret());
        return;
    }
    const auto b = reinterpret_cast<u64>(base);
    emit(str_x_uoff(16, REG_JIT_CTX, save));
    emit(str_x_uoff(17, REG_JIT_CTX, save + 1));
    emit(str_x_uoff(14, REG_JIT_CTX, save + 2));
    emit(ldr_w_uoff(16, 30, 0));  // the site's data word
    emit(add_x_imm(30, 30, 4));
    emit(movz_x(17, b & 0xFFFF, 0));
    emit(movk_x(17, (b >> 16) & 0xFFFF, 16));
    emit(movk_x(17, (b >> 32) & 0xFFFF, 32));
    emit(movk_x(17, (b >> 48) & 0xFFFF, 48));
    emit(ubfx_x(14, 16, 6, kRunUnitBits));
    emit(add_x_lsl(17, 17, 14, 3));  // X17 = run header
    emit(and_w_imm(14, 16, kSiteOrdinalMask));
    emit(add_x_lsl(16, 17, 14, 2));  // X16 + 24 = this piece's counter
    emit(ldr_w_uoff(14, 16, sizeof(RunHeader) / 4));
    emit(add_w_imm(14, 14, 1));
    emit(str_w_uoff(14, 16, sizeof(RunHeader) / 4));
    emit(and_w_imm(14, 14, kColdEntryThreshold - 1));
    emit(cbnz_w(14, 8));
    emit(str_x_uoff(17, REG_JIT_CTX, JIT_CTX_OFF_COLD_RESIDENCY_REQUEST >> 3));
    emit(ldr_x_uoff(14, REG_JIT_CTX, save + 2));
    emit(ldr_x_uoff(17, REG_JIT_CTX, save + 1));
    emit(ldr_x_uoff(16, REG_JIT_CTX, save));
    emit(ret());
}

void JitCompiler::bind_cold_counters(GuestAddr start) {
    m_bound_cold_run = 0;
    const usize total = m_code_buf.size();
    if (total < 3) return;
    const u32 site = cold_counter_site_word();
    const usize from = m_hot_end_pos;
    u32 sites = 0;
    u32 n = 0;
    for (usize i = from; i + 2 < total; ++i) {
        if (m_code_buf[i] == site && m_code_buf[i + 1] == kBlrX30) {
            ++sites;
            n = std::max(n, (m_code_buf[i + 2] & kSiteOrdinalMask) + 1);
            i += 2;
        }
    }
    if (sites == 0) return;
    // A restored or shared emission may carry sites this compile must not
    // count (residency off, a variant, no tail): branch over them.
    bool bind = m_residency_counters && cold_residency_enabled();
    if (bind && m_jit_ctx.dispatch_tail[kColdCounter] == nullptr &&
        materialize_dispatch_tail(kColdCounter) == nullptr) {
        bind = false;
    }
    const u32 run = bind ? alloc_run(start, m_emit_code_hash, n) : 0;
    for (usize i = from; i + 2 < total; ++i) {
        if (m_code_buf[i] != site || m_code_buf[i + 1] != kBlrX30) continue;
        if (bind) {
            m_code_buf[i + 2] = (run << 6) | (m_code_buf[i + 2] & kSiteOrdinalMask);
        } else {
            m_code_buf[i] = arm64::b_imm(3 * sizeof(u32));
        }
        i += 2;
    }
    if (bind) {
        m_bound_cold_run = run;
        bump(g_stats.counter_runs);
        bump(g_stats.counter_sites, sites);
    }
}

u64 JitCompiler::cold_run_hot_mask(u32 run, u32 entries) noexcept {
    if (run == 0 || g_region == nullptr) return 0;
    const auto* h = header_from_request(reinterpret_cast<u64>(g_region) + u64{run} * 8);
    if (h == nullptr || h->n == 0 || h->n > kMaxColdPieces) return 0;
    const u32 need = std::max<u32>(entries / (kColdEntryThreshold / kColdHotPieceEntries), 1);
    u64 hot = 0;
    const u32* counts = run_counters(h);
    for (u32 i = 0; i < h->n; ++i) {
        if (counts[i] >= need) hot |= u64{1} << i;
    }
    return hot;
}

void JitCompiler::note_residency_profile(GuestAddr start, u64 code_hash, u64 hot_mask) {
    record_profile(start, code_hash, hot_mask, false);
}

void JitCompiler::note_stack_proof_deopt(GuestAddr rip) {
    if (!cold_residency_enabled() || m_proof_request_rip != 0) return;
    if (m_proof_deopts.size() >= kMaxProfiles) m_proof_deopts.clear();
    if (++m_proof_deopts[rip] == kStackProofDeoptThreshold) {
        m_proof_request_rip = rip;
    }
}

void JitCompiler::service_cold_residency_slow() {
    const u64 request = m_jit_ctx.cold_residency_request;
    m_jit_ctx.cold_residency_request = 0;
    const GuestAddr proof_rip = m_proof_request_rip;
    m_proof_request_rip = 0;
    const u32 queued = std::min(m_jit_ctx.tier_request_count, JIT_CTX_TIER_REQUEST_RING);
    m_jit_ctx.tier_request_count = 0;
    for (u32 i = 0; i < queued; ++i) service_tier_request(m_jit_ctx.tier_requests[i]);
    if (request == 0 || !cold_residency_enabled()) {
        // No residency request.
    } else if (const RunHeader* h = header_from_request(request)) {
        bump(g_stats.requests);
        if (h->start != 0 && h->n != 0 && h->n <= kMaxColdPieces) {
            u64 hot = 0;
            const u32* counts = run_counters(h);
            for (u32 i = 0; i < h->n; ++i) {
                if (counts[i] >= kColdHotPieceEntries) hot |= u64{1} << i;
            }
            if (hot != 0) {
                record_profile(h->start, h->code_hash, hot, false);
                // A piece entered this often is hot code: if the block is
                // tier 0, the residency recompile is its tier-1 compile.
                mark_tier_hot(h->start, h->code_hash);
                withdraw_for_residency(h->start);
            }
        }
    }
    if (!cold_residency_enabled()) return;
    if (proof_rip != 0) {
        record_profile(proof_rip, 0, 0, true);
        withdraw_for_residency(proof_rip);
    }
}

void JitCompiler::withdraw_for_residency(GuestAddr start) {
    if (m_block_cache == nullptr) return;
    u64 identity = 0;
    {
        std::lock_guard<std::mutex> lock(g_profile_mu);
        const auto it = g_profiles.find(start);
        if (it == g_profiles.end()) return;
        identity = it->second.code_hash;
    }
    if (withdraw_block(start, identity)) bump(g_stats.withdrawals);
}

bool JitCompiler::withdraw_block(GuestAddr start, u64 identity) {
    if (m_block_cache == nullptr) return false;
    if (m_residency_withdrawn.size() >= kMaxProfiles) m_residency_withdrawn.clear();
    if (!m_residency_withdrawn.insert(withdrawal_key(start, identity)).second) return false;
    return withdraw_block_now(start);
}

bool JitCompiler::withdraw_block_now(GuestAddr start) {
    if (m_block_cache == nullptr) return false;
    BasicBlock* block = m_block_cache->lookup(start);
    if (block == nullptr || block->compiled_code == nullptr) return false;
    const void* const old_code = block->compiled_code;
    block->compiled_code = nullptr;
    block->compiled_link_code = nullptr;
    block->compiled_call_code = nullptr;
    block->compiled_pin_layout = 0;
    block->compiled_requires_protection_lease = false;
    l1_note_published(*block);
    invalidate_return_predictors();

    // This thread's edges into the old code fail closed to the L1 probe,
    // which now misses, so the next entry dispatches and recompiles. The old
    // code stays valid: no generation moves, and another thread may still
    // run it until its own counters ask.
    undo_jcc_retargets(old_code);
    const auto tit = m_direct_patch_targets.find(start);
    if (tit == m_direct_patch_targets.end()) return true;
    tit->second.for_each([&](DirectPatchSite& site) {
        if (site.code == nullptr) return;
        (void)JitCodeArena::arena_write_word_if_live(
            reinterpret_cast<u32*>(static_cast<u8*>(site.code) + site.gen_offset),
            site.arena_generation, arm64::movz_w(3, 0));
    });
    return true;
}

namespace {

bool in_thunk_region(const JitContext& ctx, u64 target) noexcept {
    return ctx.thunk_region_hi > ctx.thunk_region_lo &&
           target >= ctx.thunk_region_lo && target < ctx.thunk_region_hi;
}

}  // namespace

void JitCompiler::emit_mid_exit_kind_clear(u64 target) {
    if (m_exit_kind_cleared || !jcc_retarget_enabled() ||
        m_stack_entry_load_proof || target <= m_block_start_addr ||
        in_thunk_region(m_jit_ctx, target) ||
        !link_edge_enabled(LinkEdge::MidJcc) ||
        !s_jit_link_enabled.load(std::memory_order_acquire)) {
        return;
    }
    emit(arm64::strb_uoff(31, REG_CPU_STATE, nzcv_kind_offset()));
    m_exit_kind_cleared = true;
}

NzcvState JitCompiler::exit_carry_after_kind_clear(
    u64 target, NzcvState carry) const noexcept {
    // The thunk-target tail treats a Carried exit's PSTATE as live.
    if (!m_exit_kind_cleared || carry != NzcvState::None ||
        in_thunk_region(m_jit_ctx, target)) {
        return carry;
    }
    return NzcvState::Carried;
}

void JitCompiler::emit_flush_residency_hot_pieces() {
    bump(g_stats.variant_compiles);
    // Hot deferred arms first, in their original order; the rest stay for
    // the ordinary cold flush.
    std::vector<DeferredColdArm> cold_arms;
    std::vector<DeferredColdArm> hot_arms;
    for (usize j = 0; j < m_deferred_cold_arms.size(); ++j) {
        (residency_piece_hot(static_cast<u32>(j)) ? hot_arms : cold_arms)
            .push_back(std::move(m_deferred_cold_arms[j]));
    }
    bool hot_mem = false;
    for (usize i = 0; i < m_cold_mem_stubs.size(); ++i) {
        hot_mem |= residency_piece_hot(m_cold_mem_ordinal_base + static_cast<u32>(i));
    }
    m_deferred_cold_arms = std::move(hot_arms);
    emit_flush_deferred_cold_arms();
    m_deferred_cold_arms = std::move(cold_arms);
    // The memory stubs share bodies and a group layout: all of them or none.
    // A lazy slot carries a record word the B.cond retarget scan must not
    // read as hot code, so a block with lazy sites keeps them cold.
    if (hot_mem && m_cold_lazy_sites.empty() && m_deferred_cold_arms.empty()) {
        emit_flush_cold_mem_stubs();
        if (!m_cold_lazy_sites.empty()) return;
    }
    const usize end = m_code_buf.size();
    if (end > m_hot_end_pos && ends_control_flow(m_code_buf[end - 1])) {
        m_hot_end_pos = static_cast<u32>(end);
        bump(g_stats.variant_split_moves);
    }
}

}  // namespace vitreo::cpu::jit
