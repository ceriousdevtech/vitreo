// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_ir_tier.cpp
/// @brief IR tier-up: tier-0 entry counters, the process-wide hot identity
///        set and the tier-1 recompile they request
///        (jit_compiler_cold_residency.hpp).

#include "jit_compiler.hpp"
#include "jit_emit_internal.hpp"
#include "jit_helpers.hpp"
#include "stencils.hpp"
#include "jit_compile_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_set>
#include <sys/mman.h>

namespace vitreo::cpu::jit {

using namespace jit_detail;

namespace {

std::atomic<int> g_threshold_override{-1};

u32 env_threshold() noexcept {
    static const u32 value = [] {
        const char* s = std::getenv("VITREO_JIT_IR_TIER_THRESHOLD");
        if (s == nullptr || *s == '\0') return JitCompiler::kDefaultIrTierThreshold;
        char* end = nullptr;
        const unsigned long v = std::strtoul(s, &end, 0);
        if (end == s || *end != '\0') {
            std::fprintf(stderr,
                         "[vitreo] VITREO_JIT_IR_TIER_THRESHOLD=\"%s\" is not a "
                         "number; using %u\n",
                         s, JitCompiler::kDefaultIrTierThreshold);
            return JitCompiler::kDefaultIrTierThreshold;
        }
        return v > JitCompiler::kMaxIrTierThreshold
                   ? JitCompiler::kMaxIrTierThreshold
                   : static_cast<u32>(v);
    }();
    return value;
}

// ---- Tier slots -------------------------------------------------------------
// One per tier-0 translation, in one lazily committed mapping: the slot's
// metadata, and after all of them one u32 down-counter per slot that the
// site decrements inline (JitContext::tier_counters is its base). A site's
// data word is its slot index: below 2^26, so no branch decoder
// (jit_split_layout) reads it as a branch. Slot 0 is never bound.
struct TierSlot {
    u64 start;      ///< guest block start
    u64 code_hash;  ///< identity the slot was bound under
    u32 cold_run;   ///< the translation's cold-counter run unit, 0 for none
    u32 threshold;  ///< entries that request tier 1, as bound
    u64 reserved;
};
constexpr usize kMaxSlots = usize{1} << 21;
constexpr usize kSlotBytes = kMaxSlots * sizeof(TierSlot);
constexpr usize kRegionBytes = kSlotBytes + kMaxSlots * sizeof(u32);
// A site reaches its counter with ADD #hi, LSL #12 and a scaled LDR/STR #lo.
static_assert((kMaxSlots - 1) * sizeof(u32) >> 12 <= 4095,
              "ADD (immediate, LSL #12) cannot reach every counter");
// Bit 31 clear: not yet requested. A parked counter (queued in a request
// ring, or an IR-declined identity) needs 2^31 more entries before it asks
// again.
constexpr u32 kCounterParked = 0x7FFFFFFFu;
constexpr u32 kTierRetryEntries = JitCompiler::kTierRetryEntries;
static_assert(kTierRetryEntries >= 1 && kTierRetryEntries <= 0x10000,
              "the tail re-arms with one MOVZ");

std::mutex g_slot_mu;
TierSlot* g_slots{nullptr};
std::atomic<u32*> g_counters{nullptr};
bool g_slots_failed{false};
usize g_slots_used{1};
usize g_slot_limit{kMaxSlots};
bool g_exhaustion_logged{false};

TierSlot* slot_region() {
    std::lock_guard<std::mutex> lock(g_slot_mu);
    if (g_slots == nullptr && !g_slots_failed) {
        void* p = ::mmap(nullptr, kRegionBytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) {
            g_slots_failed = true;
            std::fprintf(stderr,
                         "[vitreo] JIT IR tier: counter region mmap failed; "
                         "blocks stay at tier 0 (stencils, no counter)\n");
            return nullptr;
        }
        g_slots = static_cast<TierSlot*>(p);
        g_counters.store(reinterpret_cast<u32*>(static_cast<u8*>(p) + kSlotBytes),
                         std::memory_order_release);
    }
    return g_slots;
}

bool slots_available() {
    if (slot_region() == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_slot_mu);
    return g_slots_used < g_slot_limit;
}

u32 alloc_slot(GuestAddr start, u64 code_hash, u32 cold_run) {
    std::lock_guard<std::mutex> lock(g_slot_mu);
    if (g_slots == nullptr) return 0;
    if (g_slots_used >= g_slot_limit) {
        if (!g_exhaustion_logged) {
            g_exhaustion_logged = true;
            std::fprintf(stderr,
                         "[vitreo] JIT IR tier: %zu counter slots exhausted; "
                         "later blocks stay at tier 0 (stencils, no counter)\n",
                         g_slot_limit);
        }
        return 0;
    }
    const usize at = g_slots_used++;
    TierSlot& s = g_slots[at];
    s.start = start;
    s.code_hash = code_hash;
    s.cold_run = cold_run;
    s.threshold = JitCompiler::ir_tier_threshold();
    // The threshold-th decrement takes the counter to -1, which requests.
    __atomic_store_n(&g_counters.load(std::memory_order_relaxed)[at],
                     s.threshold - 1, __ATOMIC_RELAXED);
    return static_cast<u32>(at);
}

/// Entries counted against a bound slot; the site's plain read-modify-write
/// may lose a concurrent decrement, so this is approximate under threads.
u32 slot_entries(usize index) noexcept {
    const u32* counters = g_counters.load(std::memory_order_acquire);
    if (counters == nullptr || g_slots == nullptr) return 0;
    const auto left =
        static_cast<i32>(__atomic_load_n(&counters[index], __ATOMIC_RELAXED));
    // Above its initial value only once parked or re-armed, both of which
    // happen only after the threshold was reached.
    if (i64{left} > i64{g_slots[index].threshold} - 1) return g_slots[index].threshold;
    const i64 entries = i64{g_slots[index].threshold} - 1 - left;
    return entries <= 0 ? 0u
                        : static_cast<u32>(std::min<i64>(entries, 0xFFFFFFFFll));
}

/// A ring entry is a bound site's data word; slot 0 is the unbound form.
const TierSlot* slot_from_request(u64 index) noexcept {
    if (g_slots == nullptr || index == 0 || index >= kMaxSlots) return nullptr;
    return g_slots + index;
}

// ---- Hot identities ---------------------------------------------------------
struct Identity {
    GuestAddr start;
    u64 code_hash;
    bool operator==(const Identity&) const = default;
};
struct IdentityHash {
    usize operator()(const Identity& id) const noexcept {
        return static_cast<usize>(id.start ^ (id.code_hash * 0x9E3779B97F4A7C15ull));
    }
};
constexpr usize kHotShards = 16;
struct HotShard {
    std::mutex mu;
    std::unordered_set<Identity, IdentityHash> set;
};
/// One process-wide identity set: hot (tier 1 requested) or IR-declined.
struct IdentitySet {
    std::array<HotShard, kHotShards> shards;
    std::atomic<usize> count{0};

    HotShard& shard(GuestAddr start) noexcept {
        return shards[(start >> 4) & (kHotShards - 1)];
    }
    bool insert(GuestAddr start, u64 code_hash) {
        HotShard& s = shard(start);
        std::lock_guard<std::mutex> lock(s.mu);
        if (!s.set.insert(Identity{start, code_hash}).second) return false;
        count.fetch_add(1, std::memory_order_release);
        return true;
    }
    bool contains(GuestAddr start, u64 code_hash) {
        if (count.load(std::memory_order_acquire) == 0) return false;
        HotShard& s = shard(start);
        std::lock_guard<std::mutex> lock(s.mu);
        return s.set.count(Identity{start, code_hash}) != 0;
    }
    void clear() {
        for (HotShard& s : shards) {
            std::lock_guard<std::mutex> lock(s.mu);
            s.set.clear();
        }
        count.store(0, std::memory_order_release);
    }
};
IdentitySet g_hot;
IdentitySet g_declined;

struct Stats {
    std::atomic<u64> tier0_compiles{0};
    std::atomic<u64> uncounted{0};
    std::atomic<u64> tier1_compiles{0};
    std::atomic<u64> requests{0};
    std::atomic<u64> promotions{0};
    std::atomic<u64> withdrawals{0};
    std::atomic<u64> store_tier1{0};
    std::atomic<u64> declines{0};
    std::atomic<u64> parked{0};
};
Stats g_stats;

void bump(std::atomic<u64>& a) noexcept { a.fetch_add(1, std::memory_order_relaxed); }

constexpr u32 kBlrX30 = 0xD63F03C0u;  // BLR X30

void print_ir_tier_stats() {
    const JitCompiler::IrTierStats s = JitCompiler::ir_tier_stats();
    std::fprintf(stderr,
                 "[jit] ir-tier threshold=%u tier0=%llu uncounted=%llu tier1=%llu "
                 "requests=%llu promotions=%llu withdrawals=%llu store-tier1=%llu "
                 "declines=%llu parked=%llu\n",
                 JitCompiler::ir_tier_threshold(),
                 static_cast<unsigned long long>(s.tier0_compiles),
                 static_cast<unsigned long long>(s.uncounted),
                 static_cast<unsigned long long>(s.tier1_compiles),
                 static_cast<unsigned long long>(s.requests),
                 static_cast<unsigned long long>(s.promotions),
                 static_cast<unsigned long long>(s.withdrawals),
                 static_cast<unsigned long long>(s.store_tier1),
                 static_cast<unsigned long long>(s.declines),
                 static_cast<unsigned long long>(s.parked));
}

/// The inline tier site. Flag-neutral, X30 clobbered, every other register
/// preserved (X16 through JitContext::cold_counter_save[0]); `slot` 0 is the
/// unbound form a translation-store record carries.
void write_tier_site(u32* w, u32 slot, u32 tail_word) noexcept {
    using namespace arm64;
    constexpr u32 save = JIT_CTX_OFF_COLD_COUNTER_SAVE >> 3;
    const u64 off = u64{slot} * sizeof(u32);
    w[0] = str_x_uoff(16, REG_JIT_CTX, save);
    w[1] = ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_TIER_COUNTERS >> 3);
    w[2] = add_x_imm_lsl12(16, 16, off >> 12);
    w[3] = ldr_w_uoff(30, 16, (off & 0xFFF) >> 2);
    w[4] = sub_w_imm(30, 30, 1);
    w[5] = str_w_uoff(30, 16, (off & 0xFFF) >> 2);
    w[6] = ldr_x_uoff(16, REG_JIT_CTX, save);
    // Past the threshold the counter is negative: call the request tail.
    w[7] = tbz(30, 31, 4 * sizeof(u32));
    w[8] = tail_word;
    w[9] = kBlrX30;
    w[10] = slot;
}

bool is_tier_site(const u32* w, u32 tail_word) noexcept {
    return w[0] == arm64::str_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_COLD_COUNTER_SAVE >> 3) &&
           w[1] == arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_TIER_COUNTERS >> 3) &&
           w[8] == tail_word && w[9] == kBlrX30;
}

[[maybe_unused]] const bool g_stats_at_exit = [] {
    if (environment_flag_enabled("VITREO_JIT_IR_TIER_STATS", false)) {
        std::atexit(print_ir_tier_stats);
    }
    return true;
}();

}  // namespace

u32 JitCompiler::ir_tier_threshold() noexcept {
    const int v = g_threshold_override.load(std::memory_order_relaxed);
    return v < 0 ? env_threshold() : static_cast<u32>(v);
}

void JitCompiler::set_ir_tier_threshold_for_test(int threshold) noexcept {
    g_threshold_override.store(
        threshold < 0 ? -1
                      : static_cast<int>(std::min<u32>(static_cast<u32>(threshold),
                                                       kMaxIrTierThreshold)),
        std::memory_order_relaxed);
}

void JitCompiler::reset_ir_tier_for_test() {
    g_hot.clear();
    g_declined.clear();
}

void JitCompiler::set_tier_slot_limit_for_test(usize limit) noexcept {
    std::lock_guard<std::mutex> lock(g_slot_mu);
    g_slot_limit = limit == 0 ? kMaxSlots : std::min(limit, kMaxSlots);
    g_exhaustion_logged = false;
}

usize JitCompiler::tier_slots_used_for_test() noexcept {
    std::lock_guard<std::mutex> lock(g_slot_mu);
    return g_slots_used;
}

JitCompiler::IrTierStats JitCompiler::ir_tier_stats() noexcept {
    const auto ld = [](const std::atomic<u64>& a) {
        return a.load(std::memory_order_relaxed);
    };
    IrTierStats out;
    out.tier0_compiles = ld(g_stats.tier0_compiles);
    out.uncounted = ld(g_stats.uncounted);
    out.tier1_compiles = ld(g_stats.tier1_compiles);
    out.requests = ld(g_stats.requests);
    out.promotions = ld(g_stats.promotions);
    out.withdrawals = ld(g_stats.withdrawals);
    out.store_tier1 = ld(g_stats.store_tier1);
    out.declines = ld(g_stats.declines);
    out.parked = ld(g_stats.parked);
    return out;
}

u32 JitCompiler::tier_counter_site_word() noexcept {
    return arm64::ldr_x_uoff(30, REG_JIT_CTX,
                             (JIT_CTX_OFF_DISPATCH_TAIL >> 3) + kTierCounter);
}

u32* JitCompiler::tier_counter_base() noexcept {
    return g_counters.load(std::memory_order_acquire);
}

u32 JitCompiler::tier_counter_value_for_test(u32 data_word) noexcept {
    std::lock_guard<std::mutex> lock(g_slot_mu);
    if (g_slots == nullptr || data_word == 0 || data_word >= g_slots_used) return 0;
    return slot_entries(data_word);
}

void JitCompiler::mark_tier_hot(GuestAddr start, u64 code_hash) {
    if (g_hot.insert(start, code_hash)) bump(g_stats.promotions);
}

bool JitCompiler::tier_hot(GuestAddr start, u64 code_hash) {
    return g_hot.contains(start, code_hash);
}

void JitCompiler::mark_tier_declined(GuestAddr start, u64 code_hash) {
    if (g_declined.insert(start, code_hash)) bump(g_stats.declines);
}

bool JitCompiler::tier_declined(GuestAddr start, u64 code_hash) {
    return g_declined.contains(start, code_hash);
}

void JitCompiler::select_ir_tier(const BasicBlock& block, u64 code_hash,
                                 bool have_hash) {
    m_tier_active = false;
    m_tier_stencil = false;
    m_tier_counted = false;
    if (!have_hash || ir_tier_threshold() <= 1 || !ir_gate_open(block)) return;
    m_tier_active = true;
    // The IR declined this identity at its tier-up: tier 0 is final.
    if (tier_declined(block.start_addr, code_hash)) {
        m_tier_stencil = true;
        return;
    }
    if (tier_hot(block.start_addr, code_hash)) return;
    m_tier_stencil = true;
    // Past the slot cap a block stays at tier 0 without a counter: paying
    // the IR on every first compile is the startup cost tiering removes.
    m_tier_counted = slots_available();
}

u32 JitCompiler::ir_key_policy() const noexcept {
    u32 policy = ir_policy();
    if (m_tier_active) policy |= kIrKeyTiered;
    if (m_tier_stencil) policy |= kIrKeyTier0;
    if (m_tier_stencil && !m_tier_counted) policy |= kIrKeyUncounted;
    return policy;
}

void JitCompiler::promote_compile_to_tier1(GuestAddr start, u64 code_hash) {
    m_tier_stencil = false;
    m_tier_counted = false;
    m_residency_counters = false;
    mark_tier_hot(start, code_hash);
    bump(g_stats.store_tier1);
}

bool JitCompiler::decline_tier1_compile(GuestAddr start, u64 code_hash,
                                        bool retry_possible) {
    if (!m_tier_active || m_tier_stencil || m_ir_emit_active ||
        m_residency_variant) {
        return false;
    }
    mark_tier_declined(start, code_hash);
    // The retry is tier 0 again; with the memo on it adopts the tier-0
    // translation this identity already has instead of emitting the same
    // stencils a second time.
    return retry_possible;
}

void JitCompiler::emit_tier_entry_counter() {
    if (!m_tier_stencil) return;
    // Unconditionally: materialize_dispatch_tail also notes the tail in the
    // translation-store record, which a restoring process publishes.
    if (materialize_dispatch_tail(kTierCounter) == nullptr) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::fprintf(stderr,
                         "[vitreo] JIT IR tier: counter tail unavailable; "
                         "tier-0 blocks will not tier up\n");
        }
        return;
    }
    // Emitted even for an uncounted compile (bind_tier_counter branches over
    // it), so a stored record always carries the site.
    const usize at = m_code_buf.size();
    m_code_buf.resize(at + kTierSiteWords);
    write_tier_site(&m_code_buf[at], 0, tier_counter_site_word());
}

void JitCompiler::emit_tier_counter_tail_body() {
    using namespace arm64;
    constexpr u32 save = JIT_CTX_OFF_COLD_COUNTER_SAVE >> 3;
    TierSlot* const base = slot_region();
    if (base == nullptr) {
        emit(add_x_imm(30, 30, 4));
        emit(ret());
        return;
    }
    constexpr u32 count = JIT_CTX_OFF_TIER_REQUEST_COUNT >> 2;
    constexpr u32 full_bit = std::countr_zero(JIT_CTX_TIER_REQUEST_RING);
    // Reached when a site's counter goes negative: the threshold-th entry, or
    // the end of a full-ring retry window. The slot index joins this thread's
    // request ring, so one dispatcher entry services every block that crossed
    // in the same chain, and the counter parks until that service decides
    // (a queued block must not call here on every entry, nor queue twice).
    // With the ring full the counter re-arms for kTierRetryEntries instead.
    // Flag-neutral throughout: a site sits where the predecessor's NZCV may
    // still be live.
    emit(str_x_uoff(16, REG_JIT_CTX, save));
    emit(str_x_uoff(17, REG_JIT_CTX, save + 1));
    emit(str_x_uoff(14, REG_JIT_CTX, save + 2));
    emit(ldr_w_uoff(16, 30, 0));  // the site's data word: the slot index
    emit(add_x_imm(30, 30, 4));
    emit(ldr_w_uoff(14, REG_JIT_CTX, count));
    emit(tbnz(14, full_bit, 8 * sizeof(u32)));  // full: to the re-arm
    emit(add_x_lsl(17, REG_JIT_CTX, 14, 3));
    emit(str_x_uoff(16, 17, JIT_CTX_OFF_TIER_REQUESTS >> 3));
    emit(add_w_imm(14, 14, 1));
    emit(str_w_uoff(14, REG_JIT_CTX, count));
    emit(movz_w(14, kCounterParked & 0xFFFF));
    emit(movk_w(14, kCounterParked >> 16, 16));
    emit(b_imm(2 * sizeof(u32)));  // over the re-arm
    emit(movz_w(14, kTierRetryEntries - 1));
    emit(ldr_x_uoff(17, REG_JIT_CTX, JIT_CTX_OFF_TIER_COUNTERS >> 3));
    emit(add_x_lsl(17, 17, 16, 2));
    emit(str_w_uoff(14, 17, 0));
    emit(ldr_x_uoff(14, REG_JIT_CTX, save + 2));
    emit(ldr_x_uoff(17, REG_JIT_CTX, save + 1));
    emit(ldr_x_uoff(16, REG_JIT_CTX, save));
    emit(ret());
}

void JitCompiler::bind_tier_counter(GuestAddr start) {
    const usize total = m_code_buf.size();
    if (total < kTierSiteWords) return;
    const u32 tail_word = tier_counter_site_word();
    bool any = false;
    for (usize i = 0; i + kTierSiteWords <= total; ++i) {
        if (is_tier_site(&m_code_buf[i], tail_word)) {
            any = true;
            break;
        }
    }
    if (!any) return;
    // A restored or shared emission may carry sites this compile must not
    // count, and an uncounted tier-0 compile has no slot: branch over them.
    u32 slot = 0;
    if (m_tier_stencil && m_tier_counted &&
        (m_jit_ctx.dispatch_tail[kTierCounter] != nullptr ||
         materialize_dispatch_tail(kTierCounter) != nullptr)) {
        slot = alloc_slot(start, m_emit_code_hash, m_bound_cold_run);
    }
    if (slot != 0 && m_jit_ctx.tier_counters == nullptr) {
        m_jit_ctx.tier_counters = tier_counter_base();
    }
    for (usize i = 0; i + kTierSiteWords <= total; ++i) {
        u32* w = &m_code_buf[i];
        if (!is_tier_site(w, tail_word)) continue;
        if (slot != 0) {
            write_tier_site(w, slot, tail_word);
        } else {
            w[0] = arm64::b_imm(kTierSiteWords * sizeof(u32));
            w[8] = arm64::nop();
        }
        i += kTierSiteWords - 1;
    }
    if (slot != 0) {
        bump(g_stats.tier0_compiles);
    } else if (m_tier_stencil) {
        bump(g_stats.uncounted);
    }
}

bool JitCompiler::service_tier_request(u64 request) {
    const TierSlot* s = slot_from_request(request);
    if (s == nullptr) return false;
    bump(g_stats.requests);
    if (s->start == 0) return true;
    const GuestAddr start = s->start;
    const u64 code_hash = s->code_hash;
    u32* const counter = &g_counters.load(std::memory_order_relaxed)[request];
    if (tier_declined(start, code_hash)) {
        // Its tier-1 compile found the IR declines, so tier 0 is final -- but
        // not THIS tier-0 translation, whose every entry still loads and
        // stores the slot's counter. Parking it left that read-modify-write
        // on every entry forever, and the slot is shared by every thread
        // running the block: four threads decoding with the same hot loop
        // spent ~3x the CPU of one (an LZMA range decoder, tools/stress). A
        // declined identity compiles to an uncounted tier 0 (kIrKeyUncounted),
        // so withdraw this thread's counted translation and leave the counter
        // at zero, which makes every other thread still running it ask on its
        // next entry and swap too. A translation this thread could not
        // withdraw is parked as before. The withdrawal is once per thread
        // like a tier-up's, but counted apart from it: the same thread
        // usually withdrew this identity once already, to tier it up.
        constexpr u64 kDeclineSwap = 0xD1EC11AE5A7E0001ull;
        const bool withdrew = withdraw_block(start, code_hash ^ kDeclineSwap);
        if (withdrew) bump(g_stats.withdrawals);
        __atomic_store_n(counter, withdrew ? 0u : kCounterParked,
                         __ATOMIC_RELAXED);
        if (!withdrew) bump(g_stats.parked);
        return true;
    }
    mark_tier_hot(start, code_hash);
    if (cold_residency_enabled() && s->cold_run != 0) {
        // The same hot ratio the residency request uses (1/8 of the block's
        // entries), so the one tier-1 compile is also the residency variant.
        const u64 hot = cold_run_hot_mask(s->cold_run, s->threshold);
        if (hot != 0) note_residency_profile(start, code_hash, hot);
    }
    // Withdrawal is per thread. Another thread still running this tier-0
    // translation asks on its next entry; a translation this thread could
    // not withdraw waits a full threshold before asking again.
    const bool withdrew = withdraw_block(start, code_hash);
    if (withdrew) bump(g_stats.withdrawals);
    __atomic_store_n(counter, withdrew ? 0u : s->threshold - 1, __ATOMIC_RELAXED);
    return true;
}

void JitCompiler::note_tier1_compile() noexcept {
    if (m_tier_active && !m_tier_stencil) bump(g_stats.tier1_compiles);
}

}  // namespace vitreo::cpu::jit
