// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_finalize.cpp
/// @brief Code placement and publication into the arena
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

// ---- Cached diagnostic env knobs -------------------------------------------
// Namespace scope, initialised once at image load. The alternatives both cost
// real time on paths that run per compiled instruction and per dispatched
// block: a bare `std::getenv()` takes an environ lock and a linear scan
// (`__findenv_locked` was visible in a live browser-renderer profile), and a
// function-local `static const bool` pays a guard check on every call, which
// on Apple is a `_tlv_get_addr` call. Every knob below is process-wide and
// fixed for the run, so hoisting the read changes nothing observable — each
// env var still selects exactly the behaviour it always did.
const bool g_env_no_pin = std::getenv("VITREO_JIT_NO_PIN") != nullptr;
const bool g_env_trace_opcodes =
    std::getenv("VITREO_JIT_TRACE_OPCODES") != nullptr;
const bool g_env_enable_idioms =
    std::getenv("VITREO_DISABLE_IDIOMS") == nullptr;

namespace {
const bool g_env_jit_map = std::getenv("VITREO_JIT_MAP") != nullptr;
const bool g_env_jit_site_map = std::getenv("VITREO_JIT_MAP_SITES") != nullptr;
const u64 g_env_jit_site_map_limit = [] {
    const char* value = std::getenv("VITREO_JIT_MAP_SITES_LIMIT");
    if (!g_env_jit_site_map || value == nullptr) return u64{4000000};
    u64 limit = 0;
    for (const char* digit = value; *digit != '\0'; ++digit) {
        if (*digit < '0' || *digit > '9' ||
            limit > (64000000u - static_cast<u64>(*digit - '0')) / 10u) {
            std::fprintf(stderr, "[vitreo] invalid VITREO_JIT_MAP_SITES_LIMIT: expected 1..64000000\n");
            std::abort();
        }
        limit = limit * 10u + static_cast<u64>(*digit - '0');
    }
    if (limit == 0) {
        std::fprintf(stderr, "[vitreo] invalid VITREO_JIT_MAP_SITES_LIMIT: expected 1..64000000\n");
        std::abort();
    }
    return limit;
}();

// VITREO_JIT_MAP records go to a fully buffered per-process file,
// $VITREO_LOG_DIR/jitmap_<pid>.txt (stderr when the directory is unset). One
// unbuffered stderr line per compiled block was a write() per block: it slowed
// a multi-process browser startup enough to change its thread interleaving. The file is
// flushed at most once a second so a live `sample` can still be attributed
// (tools/perf/attribute_jit_sample.py reads the same line format).
// `hot_bytes`: how much of [native, native+size) is the block's hot part;
// kAllHot means all of it. The cold part of a split-placed block is its own
// record with hot=0.
constexpr u32 kAllHot = std::numeric_limits<u32>::max();
void jit_map_record(const void* native, usize size, u64 guest, u32 bytes,
                    bool site = false, const char* opcode = "unknown",
                    u32 hot_bytes = kAllHot) {
    static std::mutex mutex;
    static std::FILE* const file = [] {
        const char* dir = std::getenv("VITREO_LOG_DIR");
        if (dir == nullptr || dir[0] == '\0') return stderr;
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/jitmap_%d.txt", dir,
                      static_cast<int>(::getpid()));
        std::FILE* opened = std::fopen(path, "a");
        if (opened == nullptr) return stderr;
        std::setvbuf(opened, nullptr, _IOFBF, usize{1} << 20);
        return opened;
    }();
    static auto last_flush = std::chrono::steady_clock::now();
    static u64 site_records = 0;
    std::lock_guard<std::mutex> lock(mutex);
    if (site) {
        // Bound diagnostic output independently of the block map. Missing
        // intervals remain unattributed, never guessed from a nearby site.
        const u64 limit = g_env_jit_site_map_limit;
        if (site_records >= limit) {
            if (site_records == limit) {
                std::fprintf(file, "[jit-site-map-limit] records=%llu\n",
                             static_cast<unsigned long long>(limit));
                std::fflush(file);
                ++site_records;
            }
            return;
        }
        ++site_records;
        std::fprintf(file, "[jit-site-map] native=%p size=%zu guest=0x%llx opcode=%s\n",
                     native, size, static_cast<unsigned long long>(guest), opcode);
    } else {
        std::fprintf(file, "[jit-map] native=%p size=%zu guest=0x%llx bytes=%u hot=%u\n",
                     native, size, static_cast<unsigned long long>(guest), bytes,
                     hot_bytes != kAllHot ? hot_bytes : static_cast<u32>(size));
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_flush >= std::chrono::seconds(1)) {
        std::fflush(file);
        last_flush = now;
    }
}
}  // namespace

namespace {
// VITREO_JIT_SPLIT_COLD: how each block was placed. Printed every 65536
// blocks so a killed run still reports it.
enum class SplitOutcome : u8 {
    Placed,       // hot and cold parts placed apart
    NoCold,       // nothing after the hot part
    Entry,        // a linked or CALL entry lies in the cold part
    Fallthrough,  // the hot part's last word can fall through
    TooBig,       // the two parts do not fit one split window
    Site,         // an instruction interval straddles the split
    PcRel,        // a PC-relative word the relocator cannot place
    Relocate,     // a crossing branch left its range after allocation
    Count,
};
std::array<std::atomic<u64>, static_cast<usize>(SplitOutcome::Count)>
    g_split_outcomes{};
std::atomic<u64> g_split_total{0};
std::atomic<u64> g_split_hot_bytes{0};
std::atomic<u64> g_split_cold_bytes{0};

void note_split_outcome(SplitOutcome outcome, usize hot_bytes,
                        usize cold_bytes) noexcept {
    g_split_outcomes[static_cast<usize>(outcome)].fetch_add(
        1, std::memory_order_relaxed);
    if (outcome == SplitOutcome::Placed) {
        g_split_hot_bytes.fetch_add(hot_bytes, std::memory_order_relaxed);
        g_split_cold_bytes.fetch_add(cold_bytes, std::memory_order_relaxed);
    }
    const u64 n = g_split_total.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & 0xFFFFu) != 0 && n != 1) return;
    const auto get = [](SplitOutcome o) {
        return static_cast<unsigned long long>(
            g_split_outcomes[static_cast<usize>(o)].load(
                std::memory_order_relaxed));
    };
    std::fprintf(stderr,
                 "[jit] split-cold blocks=%llu placed=%llu hot=%lluKB "
                 "cold=%lluKB contiguous: no-cold=%llu entry=%llu "
                 "fallthrough=%llu too-big=%llu site=%llu pcrel=%llu "
                 "relocate=%llu\n",
                 static_cast<unsigned long long>(n), get(SplitOutcome::Placed),
                 static_cast<unsigned long long>(
                     g_split_hot_bytes.load(std::memory_order_relaxed) >> 10),
                 static_cast<unsigned long long>(
                     g_split_cold_bytes.load(std::memory_order_relaxed) >> 10),
                 get(SplitOutcome::NoCold), get(SplitOutcome::Entry),
                 get(SplitOutcome::Fallthrough), get(SplitOutcome::TooBig),
                 get(SplitOutcome::Site), get(SplitOutcome::PcRel),
                 get(SplitOutcome::Relocate));
    if (JitCompiler::cold_lazy_enabled()) {
        const ColdLazyStats lazy = cold_lazy_stats();
        std::fprintf(stderr,
                     "[jit] lazy-cold sites=%llu materialized=%llu (%lluKB) "
                     "races=%llu side-exits=%llu eager-withdrawn=%llu "
                     "mixed-eager=%llu\n",
                     static_cast<unsigned long long>(lazy.sites),
                     static_cast<unsigned long long>(lazy.materialized),
                     static_cast<unsigned long long>(lazy.materialized_bytes >> 10),
                     static_cast<unsigned long long>(lazy.races),
                     static_cast<unsigned long long>(lazy.side_exits),
                     static_cast<unsigned long long>(lazy.eager_withdrawn),
                     static_cast<unsigned long long>(lazy.mixed_eager));
    }
}
std::array<std::atomic<u64>, ColdTailFootprint::Count> g_cold_tail_words{};
}  // namespace

ColdTailFootprint cold_tail_footprint() noexcept {
    ColdTailFootprint out;
    for (u32 i = 0; i < ColdTailFootprint::Count; ++i) {
        out.words[i] = g_cold_tail_words[i].load(std::memory_order_relaxed);
    }
    out.split_placed =
        g_split_outcomes[static_cast<usize>(SplitOutcome::Placed)].load(
            std::memory_order_relaxed);
    out.split_hot_bytes = g_split_hot_bytes.load(std::memory_order_relaxed);
    out.split_cold_bytes = g_split_cold_bytes.load(std::memory_order_relaxed);
    return out;
}

void reset_cold_tail_footprint() noexcept {
    for (auto& w : g_cold_tail_words) w.store(0, std::memory_order_relaxed);
    for (auto& o : g_split_outcomes) o.store(0, std::memory_order_relaxed);
    g_split_total.store(0, std::memory_order_relaxed);
    g_split_hot_bytes.store(0, std::memory_order_relaxed);
    g_split_cold_bytes.store(0, std::memory_order_relaxed);
}

void note_cold_tail_words(ColdTailFootprint::Piece piece, u64 words) noexcept {
    if (words != 0) {
        g_cold_tail_words[piece].fetch_add(words, std::memory_order_relaxed);
    }
}

CompiledBlock* JitCompiler::finalize_code(const BasicBlock& block,
                                          const JitCompileMemo::CompiledKey* compiled_key) {
    // A deferred branch that could not reach its target was patched with a
    // placeholder displacement (cold_branch_disp): the words are not this
    // block, so it is declined -- permanently, not deferred -- and the
    // interpreter runs it.
    if (m_branch_reach_exceeded) {
        const u64 n = stats::record_branch_reach_decline();
        // Rate-limited: the first eight, then every 256th.
        if (n <= 8 || n % 256 == 0) {
            std::fprintf(stderr,
                         "[jit] block 0x%llx declined to the interpreter "
                         "(branch-reach decline #%llu): %s displacement %lld "
                         "words exceeds the imm%u branch field%s\n",
                         static_cast<unsigned long long>(block.start_addr),
                         static_cast<unsigned long long>(n), m_reach_what,
                         static_cast<long long>(m_reach_words), m_reach_bits,
                         m_test_reach_words > 0
                             ? " (or the VITREO_JIT_TEST_BRANCH_REACH_WORDS cap)"
                             : "");
        }
        if (m_ir_emit_active) {
            ir::BuildResult declined{};
            declined.ok = false;
            declined.reason = ir::DeclineReason::BranchReach;
            ir::stats::record(declined);
        }
        return nullptr;
    }
    bind_cold_counters(block.start_addr);
    bind_tier_counter(block.start_addr);
    const usize total_words = m_code_buf.size();

    // Split placement (JitConfig::split_cold, jit_split_layout.hpp): the hot
    // part [0, m_hot_end_pos) goes to `mem`, the rest to `cold_mem` higher in
    // the same arena window. Everything that keeps a block placeable is
    // checked before allocating, so a block that cannot be split costs no
    // window space.
    const auto split_outcome = [&]() -> SplitOutcome {
        const usize hot = m_hot_end_pos;
        if (hot == 0 || hot >= total_words) return SplitOutcome::NoCold;
        if (m_link_entry_pos >= hot ||
            (m_call_entry_pos != std::numeric_limits<u32>::max() &&
             m_call_entry_pos >= hot)) {
            return SplitOutcome::Entry;
        }
        if (!ends_control_flow(m_code_buf[hot - 1])) {
            return SplitOutcome::Fallthrough;
        }
        if (!JitCodeArena::split_fits(hot * sizeof(u32),
                                      (total_words - hot) * sizeof(u32))) {
            return SplitOutcome::TooBig;
        }
        for (u32 i = 0; i < m_insn_pos_count; ++i) {
            if (m_insn_arm_pos[i] < hot && m_insn_arm_end[i] > hot) {
                return SplitOutcome::Site;
            }
        }
        // A zero displacement re-encodes every crossing branch unchanged, so
        // this only checks what no placement can fix: a PC-relative form the
        // relocator does not handle, or a branch leaving the buffer.
        if (!relocate_split_branches(m_code_buf.data(), total_words, hot, 0)) {
            return SplitOutcome::PcRel;
        }
        return SplitOutcome::Placed;
    };
    SplitOutcome outcome = SplitOutcome::Count;
    if (m_cfg.split_cold) outcome = split_outcome();

    usize code_size = total_words * sizeof(u32);
    usize cold_size = 0;
    u8* cold_mem = nullptr;
    u32 cold_delta_bytes = 0;
    u64 chunk_generation = 0;
    void* mem = nullptr;
    if (outcome == SplitOutcome::Placed) {
        const usize hot_words = m_hot_end_pos;
        const usize hot_size = hot_words * sizeof(u32);
        const usize split_cold_size = code_size - hot_size;
        void* cold = nullptr;
        if (!JitCodeArena::alloc_split(m_arena, hot_size, split_cold_size, mem,
                                       cold, chunk_generation)) {
            // Same transient refusal as alloc() below.
            m_compile_deferred = true;
            return nullptr;
        }
        const i64 delta_words =
            (static_cast<u8*>(cold) - static_cast<u8*>(mem)) /
                static_cast<i64>(sizeof(u32)) -
            static_cast<i64>(hot_words);
        if (relocate_split_branches(m_code_buf.data(), total_words, hot_words,
                                    delta_words)) {
            code_size = hot_size;
            cold_size = split_cold_size;
            cold_mem = static_cast<u8*>(cold);
            cold_delta_bytes = static_cast<u32>(delta_words) * sizeof(u32);
        } else {
            // Cannot happen inside one window (every distance in it fits
            // TBZ); the space taken is lost, the block placed contiguously.
            outcome = SplitOutcome::Relocate;
            mem = nullptr;
        }
    }
    if (m_cfg.split_cold) note_split_outcome(outcome, code_size, cold_size);
    if (mem == nullptr) {
        mem = JitCodeArena::alloc(m_arena, code_size, chunk_generation);
    }
    if (!mem) {
        // Out of arena space is a transient, retryable condition: every
        // chunk is live or still pending its epoch, and a safe-point sweep
        // frees one. Say so, because the caller's other reason for a null
        // return — this block cannot be compiled at all — is permanent, and
        // conflating them blacklists a perfectly compilable block from the JIT
        // for the life of the process on every retirement.
        m_compile_deferred = true;
        return nullptr;
    }
    const bool split = cold_mem != nullptr;
    const u32 split_bytes = split ? static_cast<u32>(code_size) : 0;

    g_fresh_emits.fetch_add(1, std::memory_order_relaxed);
    if (m_emit_has_hash) {
        // Retirement thrash: is this a translation a recent round retired?
        JitCodeArena::note_fresh_translation(
            block.start_addr, m_emit_code_hash, code_size + cold_size);
    }
    jit_set_writable(true);
    std::memcpy(mem, m_code_buf.data(), code_size);
    if (split) {
        std::memcpy(cold_mem, m_code_buf.data() + m_hot_end_pos, cold_size);
    }
    if (!m_cold_lazy_sites.empty()) {
        cold_lazy_write_records(static_cast<u8*>(mem), split_bytes,
                                cold_delta_bytes);
    }
    jit_set_writable(false);

    sys_icache_invalidate(mem, code_size);
    if (split) sys_icache_invalidate(cold_mem, cold_size);

    // Direct-link slots are addressed from the block base; one in the cold
    // part (a mid-block Jcc exit's) moves with it.
    if (split) {
        for (JitCompileMemo::DirectLinkMeta& link : m_emit_links) {
            link.patch_offset = split_placed_offset(link.patch_offset,
                                                    split_bytes,
                                                    cold_delta_bytes);
            link.gen_offset = split_placed_offset(link.gen_offset, split_bytes,
                                                  cold_delta_bytes);
        }
    }
    // The scan reads the hot part as instructions, as the split relocator
    // does; lazy records are data and live past m_hot_end_pos.
    if (!m_emit_links.empty() && jcc_retarget_enabled() &&
        (m_hot_end_pos != 0 || m_cold_lazy_sites.empty())) {
        const usize hot_words =
            split || m_hot_end_pos == 0 ? code_size / sizeof(u32) : m_hot_end_pos;
        note_jcc_retarget_candidates(static_cast<u8*>(mem),
                                     static_cast<u32>(hot_words * sizeof(u32)),
                                     chunk_generation);
    }

    // Tier-0 kept-by-fault elision: translate guest-level recovery plans into
    // per-instruction descriptors the fault handler can rebuild from.
    for (const StencilFlagRecovery& plan : m_stencil_flag_recovery) {
        if (!plan.active()) continue;
        JitFlagRecovery fr{};
        fr.flag_op = plan.flag_op;
        fr.width = plan.width;
        fr.mode = plan.merge_cf ? JitFlagRecovery::MergeCf
                  : plan.recompute_dst ? JitFlagRecovery::RecomputeDst
                                       : JitFlagRecovery::DirectDst;
        bool ok = true;
        const auto place = [&](bool is_imm, u8 guest, u64 imm, u8& out_reg,
                               u64& out_imm) {
            if (is_imm) {
                out_reg = JitFlagRecovery::kImmediate;
                out_imm = imm;
                return;
            }
            const u8 pin = pinned_gpr32(guest);
            if (pin == 0xFF) {
                // 64-bit: an unpinned guest register lives in CpuState
                // (see the plan filter in compile_attempt).
                if (!m_block_is_64bit || guest >= 16) { ok = false; return; }
                out_reg = static_cast<u8>(JitFlagRecovery::kCpuStateGpr + guest);
                return;
            }
            out_reg = pin;
        };
        place(plan.src_is_imm, plan.src_reg, plan.src_imm, fr.src_reg,
              fr.src_imm);
        place(plan.dst_is_imm, plan.dst_reg, plan.dst_imm, fr.dst_reg,
              fr.dst_imm);
        if (!ok) {
            std::fprintf(stderr,
                         "[jit] FATAL: stencil flag-recovery plan names an "
                         "unpinned guest register; block 0x%llx\n",
                         static_cast<unsigned long long>(block.start_addr));
            std::abort();
        }
        for (u32 si = 0; si < m_insn_pos_count; ++si) {
            if (m_insn_rips[si] >= plan.first_rip &&
                m_insn_rips[si] <= plan.last_rip) {
                m_insn_flag_recovery[si] = fr;
            }
        }
    }

    std::vector<JitRipSite> rip_sites(m_insn_pos_count +
                                      m_lean_variant_ranges.size());
    u32 rip_site_count = 0;
    for (u32 i = 0; i < m_insn_pos_count; ++i) {
        if (m_insn_arm_end[i] <= m_insn_arm_pos[i]) continue;
        rip_sites[rip_site_count++] = JitRipSite{
            m_insn_arm_pos[i] * static_cast<u32>(sizeof(u32)),
            m_insn_arm_end[i] * static_cast<u32>(sizeof(u32)),
            m_insn_rips[i],
            m_insn_flag_recovery[i],
        };
        rip_sites[rip_site_count - 1].flags.rsp_undo = m_insn_rsp_undo[i];
    }
    // A lean site's checked variant is its instruction's second interval: a
    // fault there (the host-view access of a page the guest denies) is that
    // instruction's fault, with its flag-recovery descriptor.
    for (const LeanVariantRange& r : m_lean_variant_ranges) {
        if (r.end <= r.begin || r.owner >= m_insn_pos_count) continue;
        rip_sites[rip_site_count++] = JitRipSite{
            static_cast<u32>(r.begin * sizeof(u32)),
            static_cast<u32>(r.end * sizeof(u32)),
            m_insn_rips[r.owner],
            m_insn_flag_recovery[r.owner],
        };
        rip_sites[rip_site_count - 1].flags.rsp_undo = m_insn_rsp_undo[r.owner];
    }
    if (!m_lean_variant_ranges.empty()) {
        std::sort(rip_sites.begin(), rip_sites.begin() + rip_site_count,
                  [](const JitRipSite& a, const JitRipSite& b) {
                      return a.host_begin < b.host_begin;
                  });
    }
    JitPinMap pins = kNoJitPins;
    for (u8 guest = 0; guest < 16; ++guest) {
        pins.host_reg[guest] = m_pin_arm[guest];
    }
    // A block with no instruction site emitted no guest-attributable code:
    // every instruction lowered to nothing (a 64-bit `mov rax, rax`). Nothing
    // in it can fault with a guest RIP, so it has no map to publish -- and no
    // per-access fallback is missing.
    const bool needs_metadata = rip_site_count != 0;
    // A split block's sites are addressed from `mem` in both parts, the cold
    // ones by their placed offset. None straddles the split, so order holds.
    for (u32 i = 0; split && i < rip_site_count; ++i) {
        if (rip_sites[i].host_begin < split_bytes) continue;
        rip_sites[i].host_begin += cold_delta_bytes;
        rip_sites[i].host_end += cold_delta_bytes;
    }
    const bool metadata_published = needs_metadata &&
        publish_jit_rip_block(reinterpret_cast<uintptr_t>(mem),
                              static_cast<u32>(code_size), rip_sites.data(),
                              rip_site_count, pins,
                              split ? split_bytes + cold_delta_bytes : 0,
                              static_cast<u32>(cold_size));
    if (needs_metadata && !metadata_published) {
        // Existing per-access RIP stores remain the correctness fallback for
        // the fixed pin layout.
        std::fprintf(stderr,
                     "[linear-fault] WARNING: precise RIP map publication failed "
                     "guest=%llx native=%p bytes=%zu sites=%u\n",
                     static_cast<unsigned long long>(block.start_addr), mem,
                     code_size, rip_site_count);
        // Diagnostic for the SC2 guest=0 SIGILL: a block published without a
        // RIP map has been observed executing off its own end into zeroed
        // arena. Dump what was compiled so the next crash names the content.
        {
            std::string words;
            for (usize i = 0; i < m_code_buf.size() && i < 16; ++i) {
                char one[12];
                std::snprintf(one, sizeof one, " %08x", m_code_buf[i]);
                words += one;
            }
            std::fprintf(stderr,
                         "[linear-fault]   block insns=%u bytes=%u 64bit=%d"
                         " arm_words=%zu:%s\n",
                         block.instruction_count, block.byte_length,
                         block.is_64bit ? 1 : 0, m_code_buf.size(),
                         words.c_str());
        }
    }

    // Optional native-PC map for correlating macOS `sample` output (which
    // reports generated code as "unknown binary") with guest blocks.
    if (g_env_jit_map) {
        jit_map_record(mem, code_size, block.start_addr, block.byte_length,
                       false, "unknown",
                       split || m_hot_end_pos == 0
                           ? kAllHot
                           : m_hot_end_pos * static_cast<u32>(sizeof(u32)));
        if (split) {
            jit_map_record(cold_mem, cold_size, block.start_addr, 0, false,
                           "unknown", 0);
        }
    }
    // Diagnostic only: do not infer an opcode from the neighboring site when
    // its guest bytes cannot be decoded (for example SMC).
    const auto site_opcode = [&](const JitRipSite& site) {
        GuestMappedByteReader reader(m_memory, site.guest_rip);
        DecodedInstruction instruction;
        const bool decoded = decode_instruction(reader,
            block.is_64bit ? DecodeMode::Mode64 : DecodeMode::Mode32,
            instruction) == DecodeError::Success;
        return decoded ? opcode_name(instruction.opcode) : "unknown";
    };
    if (g_env_jit_site_map && metadata_published) {
        // Reuse the already published precise-RIP intervals. No lowering,
        // native instruction, guest state, or recovery metadata is changed.
        // Prologues, epilogues, and shared helpers have no guessed owner.
        // A lean site's checked variant is published for faults only: it is
        // cold code that runs after a patch, so the symbol map (and the hot
        // word census read from it) leaves it out.
        std::vector<u32> variant_begins;
        for (const LeanVariantRange& r : m_lean_variant_ranges) {
            u32 begin = static_cast<u32>(r.begin * sizeof(u32));
            if (split && begin >= split_bytes) begin += cold_delta_bytes;
            variant_begins.push_back(begin);
        }
        for (u32 i = 0; i < rip_site_count; ++i) {
            const JitRipSite& site = rip_sites[i];
            if (std::find(variant_begins.begin(), variant_begins.end(),
                          site.host_begin) != variant_begins.end()) {
                continue;
            }
            jit_map_record(static_cast<const u8*>(mem) + site.host_begin,
                           site.host_end - site.host_begin, site.guest_rip, 0, true,
                           site_opcode(site));
        }
    }

    m_compiled_blocks.push_back(CompiledBlock{
        mem,
        static_cast<u32>(code_size),
        block.start_addr,
        block.byte_length,
        chunk_generation,
        {}
    });
    m_compiled_blocks.back().link_code_offset =
        static_cast<u32>(m_link_entry_pos * sizeof(u32));
    const u32 call_code_offset =
        m_call_entry_pos == std::numeric_limits<u32>::max()
            ? static_cast<u32>(code_size)
            : static_cast<u32>(m_call_entry_pos * sizeof(u32));
    m_compiled_blocks.back().call_code_offset = call_code_offset;
    m_compiled_blocks.back().requires_protection_lease =
        m_stack_entry_load_proof;
    const u16 cold_offset =
        split ? static_cast<u16>(split_bytes + cold_delta_bytes) : 0;
    m_compiled_blocks.back().cold_offset = cold_offset;
    m_compiled_blocks.back().cold_size = static_cast<u16>(cold_size);
    m_compiled_blocks.back().pin_layout = m_pin_layout;

    ++m_blocks_compiled;
    m_bytes_generated += code_size + cold_size;

    // Diagnostic ownership must precede shared visibility: a cold adopter
    // may execute immediately after memo publication without another hook.
    symbol_map::record_provenance(m_compiled_blocks.back());

    // Stage A2: publish into the process-wide memo so other threads can
    // skip re-compiling this guest VA.  If another thread raced and
    // already inserted, keep theirs (their arena bytes are equivalent).
    //
    // A delegate-bearing block is publishable too, but only together with the
    // decoded array its stencils point into, and only while the memo's
    // retained-IR budget allows: those arrays would otherwise be freed when
    // the compiling thread's BlockCache evicts the entry, so retaining them is
    // the one new cost of sharing this class of block.
    std::shared_ptr<const DecodedInsns::Storage> delegate_insns;
    if (m_has_predecoded_delegate) {
        delegate_insns = block.instructions.share();
        // The stencils baked pointers into this array, so the compiled record
        // this call returns must keep it alive regardless of what the memo
        // decides below.
        if (!delegate_insns ||
            delegate_insns->size() != block.instruction_count) {
            // No shareable array backs the baked pointers (the block was
            // decoded into storage this handle no longer owns). Never publish
            // such a block; its arena bytes are not self-contained.
            delegate_insns.reset();
        }
    }
    m_compiled_blocks.back().delegate_insns = delegate_insns;

    const usize ir_bytes =
        delegate_insns
            ? delegate_insns->size() * sizeof(cpu::DecodedInstruction)
            : 0;
    const bool memo_candidate =
        compiled_key != nullptr &&
        JitCompileMemo::s_compiled_memo_enabled.load(std::memory_order_acquire) &&
        (!m_has_predecoded_delegate ||
         delegate_insns != nullptr);
    if (memo_candidate) {
        // A retirement purges the memo by chunk, so a record for a chunk
        // retired while this block was being emitted must not be inserted
        // after that purge ran; publish_record re-checks under the lock.
        const bool inserted = JitCompileMemo::publish_record(
            *compiled_key, mem, chunk_generation, ir_bytes,
            [&](JitCompileMemo::CompiledRecord& record) {
                record.code = mem;
                record.code_size = static_cast<u32>(code_size);
                record.arena_generation = chunk_generation;
                record.native_insns = m_block_native_insns;
                record.terminal_opcode = m_block_terminal_opcode;
                record.link_code_offset =
                    static_cast<u32>(m_link_entry_pos * sizeof(u32));
                record.call_code_offset = call_code_offset;
                record.requires_protection_lease =
                    m_stack_entry_load_proof;
                record.cold_offset = cold_offset;
                record.cold_size = static_cast<u16>(cold_size);
                record.links = m_emit_links;
                record.insns = delegate_insns;
                // Decoded shape for cold-path adopters that never decode.
                record.instruction_count = block.instruction_count;
                record.ends_with_branch = block.ends_with_branch;
                record.ir_bytes = static_cast<u32>(ir_bytes);
                record.memory = &m_memory;
            });
        if (inserted && ir_bytes == 0) {
            g_memo_native_publishes.fetch_add(1, std::memory_order_relaxed);
        }
        if (memo_stats_enabled()) {
            memo_stats_arm_atexit();
        }
    }

    if (!m_emit_links.empty()) {
        m_direct_links[block.start_addr] = m_emit_links;
    }

    symbol_map::record(m_compiled_blocks.back());
    if (metadata_published && symbol_map::wants_sites(block.start_addr)) {
        for (u32 i = 0; i < rip_site_count; ++i) {
            symbol_map::record_site(rip_sites[i].host_begin,
                                    rip_sites[i].host_end,
                                    site_opcode(rip_sites[i]));
        }
    }
    return &m_compiled_blocks.back();
}

} // namespace vitreo::cpu::jit
