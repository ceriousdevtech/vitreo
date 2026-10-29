// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compile_stats.cpp
/// @brief Compile-memo, stack-proof and emit-attribution counters
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

#include <array>
#include <atomic>
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

// ---- Process-wide compile-memo retained-IR budget ----
namespace {
std::atomic<usize> g_memo_ir_bytes{0};
std::atomic<u64> g_memo_delegate_publishes{0};
std::atomic<u64> g_memo_delegate_rejects{0};
}  // namespace

std::atomic<u64> g_memo_delegate_reuses{0};
std::atomic<u64> g_memo_native_publishes{0};
std::atomic<u64> g_memo_native_reuses{0};

// Shared-table duplication counters (JitCompileMemo::compile_memo_stats).
std::atomic<u64> g_memo_compile_probes{0};
std::atomic<u64> g_memo_compile_hits{0};
std::atomic<u64> g_fresh_emits{0};

std::atomic<u64> g_cold_probes{0};
std::atomic<u64> g_cold_adoptions{0};
std::atomic<u64> g_cold_hash_mismatches{0};
std::atomic<u64> g_cold_demoted{0};

/// VITREO_JIT_MEMO_STATS=1: report what the process-wide compile memo actually
/// shared, split by block class. Without the split, a run cannot tell whether
/// delegate-bearing blocks are a large enough share of compiles to be worth
/// sharing at all.
bool memo_stats_enabled() noexcept {
    static const bool on = std::getenv("VITREO_JIT_MEMO_STATS") != nullptr;
    return on;
}

namespace {
void memo_stats_write(std::FILE* out) noexcept {
    if (out == nullptr) return;
    std::fprintf(out,
        "[jit-memo] native: published=%llu reused=%llu | delegate: "
        "published=%llu reused=%llu rejected_over_budget=%llu | "
        "retained IR=%.1f MB\n",
        static_cast<unsigned long long>(
            g_memo_native_publishes.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_memo_native_reuses.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_memo_delegate_publishes.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_memo_delegate_reuses.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_memo_delegate_rejects.load(std::memory_order_relaxed)),
        static_cast<double>(g_memo_ir_bytes.load(std::memory_order_relaxed)) /
            (1024.0 * 1024.0));
    std::fprintf(out,
        "[jit-memo] fresh_emits=%llu compile_probes=%llu compile_hits=%llu | "
        "cold_probes=%llu cold_adoptions=%llu cold_hash_mismatch=%llu "
        "cold_demoted=%llu | smc_purges=%llu\n",
        static_cast<unsigned long long>(
            g_fresh_emits.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_memo_compile_probes.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_memo_compile_hits.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_cold_probes.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_cold_adoptions.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_cold_hash_mismatches.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_cold_demoted.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(shared_table_smc_purges()));
}

void memo_stats_dump() noexcept { memo_stats_write(stderr); }
}  // namespace

void memo_stats_arm_atexit() noexcept {
    static std::once_flag once;
    std::call_once(once, [] { std::atexit(memo_stats_dump); });
}

/// Exported wrapper: the counters live in this file's anonymous namespace, but
/// the SIGUSR1 hang dump needs an external symbol to register.
void memo_stats_dump_to(std::FILE* out) noexcept { memo_stats_write(out); }

namespace {

std::array<std::atomic<u64>,
           static_cast<usize>(StackProofOutcome::Count)>
    g_stack_proof_outcomes{};
std::array<std::atomic<u64>, opcode_count()> g_stack_proof_rejected_opcodes{};
std::array<std::atomic<u64>, opcode_count()> g_stack_proof_near_miss_opcodes{};
std::array<std::atomic<u64>, 3> g_stack_proof_reject_load_buckets{};
std::atomic<bool> g_stack_proof_dumped{false};
}  // namespace

bool stack_proof_stats_enabled() noexcept {
    static const bool on =
        std::getenv("VITREO_JIT_STACK_PROOF_STATS") != nullptr;
    return on;
}

namespace {
void stack_proof_stats_dump() noexcept {
    if (!stack_proof_stats_enabled() ||
        g_stack_proof_dumped.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    const auto count = [](StackProofOutcome outcome) {
        return g_stack_proof_outcomes[static_cast<usize>(outcome)].load(
            std::memory_order_relaxed);
    };
    std::fprintf(stderr,
        "[jit-stack-proof] pid=%d eligible=%llu no_stack_load=%llu "
        "one_stack_load=%llu rsp_write=%llu other_memory=%llu "
        "bad_stack_load=%llu other_opcode=%llu\n",
        static_cast<int>(getpid()),
        static_cast<unsigned long long>(count(StackProofOutcome::Eligible)),
        static_cast<unsigned long long>(count(StackProofOutcome::NoStackLoad)),
        static_cast<unsigned long long>(count(StackProofOutcome::OneStackLoad)),
        static_cast<unsigned long long>(count(StackProofOutcome::RspWrite)),
        static_cast<unsigned long long>(count(StackProofOutcome::OtherMemory)),
        static_cast<unsigned long long>(count(StackProofOutcome::BadStackLoad)),
        static_cast<unsigned long long>(count(StackProofOutcome::OtherOpcode)));
    std::fprintf(stderr,
        "[jit-stack-proof] reject-after-loads zero=%llu one=%llu "
        "two_plus=%llu\n",
        static_cast<unsigned long long>(
            g_stack_proof_reject_load_buckets[0].load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_stack_proof_reject_load_buckets[1].load(
                std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_stack_proof_reject_load_buckets[2].load(
                std::memory_order_relaxed)));

    std::vector<std::pair<u64, Opcode>> top;
    for (usize i = 1; i < opcode_count(); ++i) {
        const u64 n = g_stack_proof_rejected_opcodes[i].load(
            std::memory_order_relaxed);
        if (n != 0) top.emplace_back(n, static_cast<Opcode>(i));
    }
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return static_cast<u16>(a.second) < static_cast<u16>(b.second);
    });
    const usize shown = std::min<usize>(top.size(), 12);
    for (usize i = 0; i < shown; ++i) {
        std::fprintf(stderr,
                     "[jit-stack-proof] first-reject-opcode %-24s %llu\n",
                     opcode_name(top[i].second),
                     static_cast<unsigned long long>(top[i].first));
    }

    top.clear();
    for (usize i = 1; i < opcode_count(); ++i) {
        const u64 n = g_stack_proof_near_miss_opcodes[i].load(
            std::memory_order_relaxed);
        if (n != 0) top.emplace_back(n, static_cast<Opcode>(i));
    }
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return static_cast<u16>(a.second) < static_cast<u16>(b.second);
    });
    for (usize i = 0; i < std::min<usize>(top.size(), 12); ++i) {
        std::fprintf(stderr,
                     "[jit-stack-proof] near-miss-opcode %-24s %llu\n",
                     opcode_name(top[i].second),
                     static_cast<unsigned long long>(top[i].first));
    }
}
}  // namespace

void record_stack_proof_outcome(StackProofOutcome outcome,
                                Opcode rejected_opcode,
                                u32 stack_load_count) noexcept {
    if (!stack_proof_stats_enabled()) return;
    static std::once_flag once;
    std::call_once(once, [] { std::atexit(stack_proof_stats_dump); });
    g_stack_proof_outcomes[static_cast<usize>(outcome)].fetch_add(
        1, std::memory_order_relaxed);
    if (outcome != StackProofOutcome::Eligible) {
        g_stack_proof_reject_load_buckets[
            std::min<usize>(stack_load_count, 2)].fetch_add(
                1, std::memory_order_relaxed);
    }
    if ((outcome == StackProofOutcome::OtherMemory ||
         outcome == StackProofOutcome::OtherOpcode) &&
        is_valid_opcode(rejected_opcode)) {
        g_stack_proof_rejected_opcodes[static_cast<usize>(rejected_opcode)]
            .fetch_add(1, std::memory_order_relaxed);
        if (stack_load_count >= 2) {
            g_stack_proof_near_miss_opcodes[
                static_cast<usize>(rejected_opcode)].fetch_add(
                    1, std::memory_order_relaxed);
        }
    }
}

namespace {
usize memo_ir_cap() noexcept {
    static const usize cap = [] () -> usize {
        const char* e = std::getenv("VITREO_JIT_MEMO_IR_MB");
        usize mb = 192;
        if (e && *e) {
            char* end = nullptr;
            const unsigned long long v = std::strtoull(e, &end, 10);
            if (end != e) mb = static_cast<usize>(v);
        }
        return mb << 20;
    }();
    return cap;
}
}  // namespace

void dump_stack_proof_stats() noexcept {
    stack_proof_stats_dump();
}

bool memo_ir_admit(usize bytes) noexcept {
    const usize cap = memo_ir_cap();
    usize cur = g_memo_ir_bytes.load(std::memory_order_relaxed);
    for (;;) {
        if (cur + bytes > cap) {
            g_memo_delegate_rejects.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (g_memo_ir_bytes.compare_exchange_weak(cur, cur + bytes,
                                                  std::memory_order_relaxed)) {
            const u64 n = g_memo_delegate_publishes.fetch_add(
                              1, std::memory_order_relaxed) + 1;
            // Periodic, not only atexit: a guest process that is SIGKILLed
            // (or never exits) still leaves its last line in the log.
            if (memo_stats_enabled() && (n % 10000u) == 0u) {
                memo_stats_dump();
            }
            return true;
        }
    }
}

void memo_ir_release(usize bytes) noexcept {
    g_memo_ir_bytes.fetch_sub(bytes, std::memory_order_relaxed);
}

void memo_ir_reset() noexcept {
    g_memo_ir_bytes.store(0, std::memory_order_relaxed);
}

usize memo_ir_bytes() noexcept {
    return g_memo_ir_bytes.load(std::memory_order_relaxed);
}

JitCompileMemo::CompileMemoStats JitCompileMemo::compile_memo_stats() noexcept {
    JitCompileMemo::CompileMemoStats s;
    s.compile_probes = g_memo_compile_probes.load(std::memory_order_relaxed);
    s.compile_hits = g_memo_compile_hits.load(std::memory_order_relaxed);
    s.fresh_emits = g_fresh_emits.load(std::memory_order_relaxed);
    s.cold_probes = g_cold_probes.load(std::memory_order_relaxed);
    s.cold_adoptions = g_cold_adoptions.load(std::memory_order_relaxed);
    s.cold_hash_mismatches =
        g_cold_hash_mismatches.load(std::memory_order_relaxed);
    s.cold_demoted = g_cold_demoted.load(std::memory_order_relaxed);
    s.smc_purges = shared_table_smc_purges();
    s.records = JitCompileMemo::compile_memo_size();
    return s;
}


namespace {
void memo_dump_at_stop() {
    if (::access("/tmp/vitreo-jit-memo-dump", F_OK) != 0) return;
    char path[96];
    std::snprintf(path, sizeof(path), "/tmp/vitreo-memo-%d.txt",
                  static_cast<int>(::getpid()));
    JitCompileMemo::dump_records(path);
}
const bool s_memo_dump_hook_registered = [] {
    diagnostics::perf_set_stop_hook(&memo_dump_at_stop);
    return true;
}();
}  // namespace

void JitCompileMemo::reset_compile_memo_stats() noexcept {
    g_memo_compile_probes.store(0, std::memory_order_relaxed);
    g_memo_compile_hits.store(0, std::memory_order_relaxed);
    g_fresh_emits.store(0, std::memory_order_relaxed);
    g_cold_probes.store(0, std::memory_order_relaxed);
    g_cold_adoptions.store(0, std::memory_order_relaxed);
    g_cold_hash_mismatches.store(0, std::memory_order_relaxed);
    g_cold_demoted.store(0, std::memory_order_relaxed);
    reset_shared_table_smc_purges();
}

std::atomic<bool> g_emit_attribution_on{
    std::getenv("VITREO_JIT_EMIT_ATTRIBUTION") != nullptr};

namespace {
std::array<std::atomic<u64>, EmitAttribution::Count> g_attr_hot{};
std::array<std::atomic<u64>, EmitAttribution::Count> g_attr_cold{};
std::atomic<u64> g_attr_blocks{0};
std::atomic<u64> g_attr_split_blocks{0};
}  // namespace

void set_emit_attribution(bool on) noexcept {
    g_emit_attribution_on.store(on, std::memory_order_relaxed);
}

EmitAttribution emit_attribution() noexcept {
    EmitAttribution out;
    for (u32 i = 0; i < EmitAttribution::Count; ++i) {
        out.hot[i] = g_attr_hot[i].load(std::memory_order_relaxed);
        out.cold[i] = g_attr_cold[i].load(std::memory_order_relaxed);
    }
    out.blocks = g_attr_blocks.load(std::memory_order_relaxed);
    out.split_blocks = g_attr_split_blocks.load(std::memory_order_relaxed);
    return out;
}

void reset_emit_attribution() noexcept {
    for (auto& w : g_attr_hot) w.store(0, std::memory_order_relaxed);
    for (auto& w : g_attr_cold) w.store(0, std::memory_order_relaxed);
    g_attr_blocks.store(0, std::memory_order_relaxed);
    g_attr_split_blocks.store(0, std::memory_order_relaxed);
}

void JitCompiler::attr_commit() noexcept {
    std::array<u64, EmitAttribution::Count> hot{}, cold{};
    const usize split = m_hot_end_pos != 0 ? m_hot_end_pos : m_attr_tags.size();
    for (usize i = 0; i < m_attr_tags.size(); ++i) {
        (i < split ? hot : cold)[m_attr_tags[i]] += 4;
    }
    for (u32 i = 0; i < EmitAttribution::Count; ++i) {
        if (hot[i] != 0) g_attr_hot[i].fetch_add(hot[i], std::memory_order_relaxed);
        if (cold[i] != 0) g_attr_cold[i].fetch_add(cold[i], std::memory_order_relaxed);
    }
    const u64 blocks = g_attr_blocks.fetch_add(1, std::memory_order_relaxed) + 1;
    if (split < m_attr_tags.size()) {
        g_attr_split_blocks.fetch_add(1, std::memory_order_relaxed);
    }
    // Periodic per-process line, so a child killed with its tree still
    // reports. ARM words per compiled block, hot/cold, by category.
    if ((blocks % 50000u) == 0) {
        static const char* const kNames[EmitAttribution::Count] = {
            "prologue", "body", "address", "memfast", "flags", "gpr", "rip",
            "helper", "guard", "exit_link", "exit_probe", "exit_dispatch",
            "exit_other", "raise", "cold_mem", "cold_arms", "other"};
        char line[1024];
        int used = std::snprintf(line, sizeof(line),
                                 "[jit-attr] pid=%d blocks=%llu words/block hot|cold:",
                                 static_cast<int>(getpid()),
                                 static_cast<unsigned long long>(blocks));
        for (u32 i = 0; i < EmitAttribution::Count &&
                        used > 0 && used < static_cast<int>(sizeof(line));
             ++i) {
            used += std::snprintf(
                line + used, sizeof(line) - static_cast<usize>(used),
                " %s=%.1f|%.1f", kNames[i],
                static_cast<double>(g_attr_hot[i].load(std::memory_order_relaxed)) /
                    4.0 / static_cast<double>(blocks),
                static_cast<double>(g_attr_cold[i].load(std::memory_order_relaxed)) /
                    4.0 / static_cast<double>(blocks));
        }
        std::fprintf(stderr, "%s\n", line);
    }
}

} // namespace vitreo::cpu::jit
