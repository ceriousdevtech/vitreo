// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler.cpp
/// @brief Copy-and-patch JIT compiler implementation (CPU-040)
///
/// Compiles decoded x86 BasicBlocks into ARM64 machine code using
/// direct instruction encoding. Uses lazy flag evaluation to avoid
/// computing x86 flags after every ALU instruction.
///
/// Register convention for JIT code:
///   X19 = CpuState*        (callee-saved, pinned)
///   X20 = JitContext*       (callee-saved, pinned — helper fn ptrs + memory)
///   W0-W15 = scratch registers
///   X16-X17 = intra-procedure scratch
///   X30 = LR (saved/restored in prologue/epilogue for BLR calls)

#include "jit_crash_breadcrumbs.hpp"
constinit thread_local volatile vitreo::cpu::jit::JitCrashBreadcrumbs
    vitreo::cpu::jit::jit_crash_breadcrumbs{};

#include "jit_compiler.hpp"
#include "jit_inval_stats.hpp"
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

namespace {
struct CompileWindowTimer {
    const bool on;
    bool memo_hit{false};
    bool success{false};
    u32 native_bytes{0};
    u32 guest_insns{0};
    u32 guest_bytes{0};
    u64 addr{0};
    std::chrono::steady_clock::time_point t0{};

    explicit CompileWindowTimer(const BasicBlock& block) noexcept
        : on(diagnostics::perf_window_active())
        , guest_insns(block.instruction_count)
        , guest_bytes(block.byte_length)
        , addr(block.start_addr)
        , t0(on ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{}) {}

    ~CompileWindowTimer() {
        if (!on) return;
        const u64 ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
        diagnostics::perf_note_compile(success, memo_hit, ns, native_bytes,
                                       guest_insns, guest_bytes, addr);
    }
};
}  // namespace

// ============================================================================
// Block compilation
// ============================================================================

JitCompiler::SelectedPinLayout JitCompiler::select_pin_layout(
    const BasicBlock& block) const noexcept {
    // Fixed pins for both bitnesses: the low eight guest GPRs, ESP/EBP in
    // X12/X13. Per-block x64 selection (VITREO_JIT_DYNAMIC_PINS) paid a layout
    // compare on every linked hop and a spill/reload wherever neighbouring
    // blocks chose differently;
    // on a 64-bit title menu the fixed layout measured 18.2 FPS against
    // 16.8 dynamic on the same build (2026-10-05, and 18.1-18.8 vs 15.1-15.8
    // on 2026-10-03). Removed 2026-10-11.
    SelectedPinLayout selected;
    selected.arm.fill(0xFF);

    if (!g_env_no_pin) {
        selected.pins_live = true;
        selected.arm[0] = 23;
        selected.arm[1] = 24;
        selected.arm[2] = 25;
        selected.arm[3] = 26;
        selected.arm[4] = REG_PIN_ESP;
        selected.arm[5] = REG_PIN_EBP;
        selected.arm[6] = 27;
        selected.arm[7] = 28;
    }

    selected.linear_base_pinned = m_memory.is_linear() &&
        ((!block.is_64bit && m_cfg.linear_jit) ||
         (block.is_64bit && m_cfg.linear64));
    JitPinMap pins = kNoJitPins;
    for (u8 guest = 0; guest < 16; ++guest) {
        pins.host_reg[guest] = selected.arm[guest];
    }
    selected.id = jit_pin_layout_id(pins, block.is_64bit);
    if (selected.linear_base_pinned) selected.id |= u64{1} << 41;
    if (block.is_64bit && m_cfg.linear64 && selected.linear_base_pinned) {
        selected.id |= u64{1} << 42;
    }
    return selected;
}

CompiledBlock* JitCompiler::compile(const BasicBlock& source_block) {
    const bool ts_timing = tstats::measuring();
    const u64 ts_start = ts_timing ? tstats::now_ticks() : 0;
    m_ts_outcome = TranslationOutcome::None;
    m_force_no_omit = false;
    m_retry_no_omit = false;
    m_force_no_elide = false;
    m_retry_no_elide = false;
    m_retry_tier0 = false;
    CompiledBlock* compiled = compile_attempt(source_block);
    if (compiled == nullptr && m_retry_tier0) {
        // Still set during the retry: it may not be abandoned again.
        compiled = compile_attempt(source_block);
        m_retry_tier0 = false;
    }
    if (compiled == nullptr && m_retry_no_omit) {
        m_retry_no_omit = false;
        m_force_no_omit = true;
        compiled = compile_attempt(source_block);
        m_force_no_omit = false;
    }
    if (compiled == nullptr && m_retry_no_elide) {
        m_retry_no_elide = false;
        m_force_no_elide = true;
        compiled = compile_attempt(source_block);
        m_force_no_elide = false;
    }
    if (ts_timing) {
        tstats::note_compile(compiled != nullptr ? m_ts_outcome : TranslationOutcome::None,
                             tstats::ticks_to_ns(tstats::now_ticks() - ts_start));
    }
    return compiled;
}

namespace {
std::atomic<bool> g_reach_knobs_loaded{false};
std::atomic<bool> g_reach_knob_fatal{false};
std::atomic<i64> g_reach_knob_words{0};
}  // namespace

void JitCompiler::reload_branch_reach_test_knobs_for_test() noexcept {
    const char* fatal = std::getenv("VITREO_JIT_TEST_BRANCH_REACH_FATAL");
    g_reach_knob_fatal.store(fatal != nullptr && fatal[0] == '1',
                             std::memory_order_relaxed);
    const char* cap = std::getenv("VITREO_JIT_TEST_BRANCH_REACH_WORDS");
    g_reach_knob_words.store(cap != nullptr ? std::atoll(cap) : 0,
                             std::memory_order_relaxed);
    g_reach_knobs_loaded.store(true, std::memory_order_release);
}

CompiledBlock* JitCompiler::compile_attempt(const BasicBlock& source_block) {
    m_compile_deferred = false;
    m_emit_has_hash = false;
    m_branch_reach_exceeded = false;
    // TEST-ONLY knobs; no guest run sets them. FATAL turns every branch-reach
    // decline into an abort (the CPU test binary sets it, so CI cannot miss
    // one); WORDS caps every deferred branch's reach so a test can provoke the
    // decline in an ordinary block. Cached: libc getenv takes a process-wide
    // lock, and every compiling thread passes here.
    {
        if (!g_reach_knobs_loaded.load(std::memory_order_acquire)) {
            reload_branch_reach_test_knobs_for_test();
        }
        m_test_reach_fatal = g_reach_knob_fatal.load(std::memory_order_relaxed);
        m_test_reach_words = g_reach_knob_words.load(std::memory_order_relaxed);
    }
    // A full arena slot fails at JitCodeArena::alloc() *after* the whole block has been
    // emitted, and a deferred compile is deliberately not recorded as a failure
    // so that a rollover does not permanently demote everything hot at that
    // instant. Together that means a block whose allocation cannot succeed is
    // re-emitted and thrown away on every single execution — strictly worse
    // than interpreting it. Bail out before doing any of that work, and keep
    // asking for the rollover that would make room (the request is throttled by
    // its own backoff, so this is cheap even when reclamation never succeeds).
    if (!JitCodeArena::has_room(m_arena)) {
        // has_room() already tried to take a chunk and, failing that,
        // asked for the gate fallback when appropriate.
        m_compile_deferred = true;
        return nullptr;
    }
    // A block with no instructions cannot represent any guest code, so there
    // is nothing to emit and — more importantly — nothing to emit a terminal
    // branch for: compiling one produces a bare prologue whose execution runs
    // off the end into unwritten arena and kills the host with SIGILL.
    // A 64-bit title does this live: a guest thread calls through a null
    // function pointer, and the rip=0 block that reaches this point is a
    // zeroed husk (0 instructions, 0 bytes). Refusing it permanently, rather
    // than deferring it, routes the dispatcher to the interpreter, which
    // raises the access violation the guest's own exception handling expects.
    if (source_block.instruction_count == 0) {
        return nullptr;
    }
    // `source_block` is a REFERENCE into this thread's BlockCache. Anything
    // that synchronously re-enters guest execution during this compile shares
    // that cache and can replace or evict the entry, dangling the reference.
    // The GUI pump below is one such re-entry — but NOT the only one: a guest
    // callback (cli::run_guest_callback) does exactly the same thing, on any
    // thread, and was unprotected.
    //
    // Measured: SIGSEGV in compute_omit_lazy_flags reading block.instructions,
    // under JitCompiler::compile <- run_guest_callback, when a launcher UI
    // callback ran mid-compile. flag_liveness, idiom_detector and the emit
    // loops all index by instruction_count and trust the array behind it.
    //
    // Copying unconditionally is cheap and is what makes this safe: BasicBlock
    // is POD fields plus a shared_ptr to the decoded array, so this is a
    // refcount bump — NOT a clone of the instructions — and that reference
    // keeps the array alive even if the cache entry disappears underneath us.
    // (The old "a cold compile storm must not clone every decoded block"
    // reasoning predates DecodedInsns holding the array by shared_ptr.)
    const BasicBlock stable_block = source_block;
    const BasicBlock& block = stable_block;
    if (m_gui_pump && pthread_main_np() != 0) {
        constexpr u32 POLL_INTERVAL = 16;
        if (++m_compile_gui_poll_count >= POLL_INTERVAL) {
            m_compile_gui_poll_count = 0;
            const auto now = std::chrono::steady_clock::now();
            if (m_last_gui_pump.time_since_epoch().count() == 0 ||
                now - m_last_gui_pump >= std::chrono::milliseconds(16)) {
                m_last_gui_pump = now;
                m_gui_pump();
            }
        }
    }
    CompileWindowTimer compile_timer(block);

    // Reclamation cannot overwrite arena bytes while a compile is reusing a
    // memo record or publishing a newly emitted block. Enter the reader epoch
    // after pumping the UI so a re-entrant callback does not hold a lease.
    JitCodeArena::ArenaReadLease arena_lease;
    if (!arena_lease) {
        // The arena gate is closed for an imminent slot rollover. Nothing is
        // wrong with this block; it simply cannot be emitted this instant.
        m_compile_deferred = true;
        return nullptr;
    }

    // Rate-limit: a slideshow cold-compile can emit thousands of these into
    // a multi-hundred-MiB VITREO_TRACE log and dominate wall-clock time on
    // the UI thread.
    {
        static std::atomic<u32> s_compile_trace{0};
        const u32 n = s_compile_trace.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8u || (n & 0xFFu) == 0u) {
            VITREO_TRACE("[jit] compile block 0x%08x (insns=%u bytes=%u) (#%u)\n",
                         static_cast<unsigned>(block.start_addr),
                         static_cast<unsigned>(block.instruction_count),
                         static_cast<unsigned>(block.byte_length),
                         n);
        }
    }

    // Hash the block's current code bytes once, up front for the process-wide
    // compile memo further down. Walk the guest mapping in place: a cold
    // compile storm must not heap-allocate a second copy of every block.
    const SelectedPinLayout selected_pins = select_pin_layout(block);
    const bool memo_on =
        JitCompileMemo::s_compiled_memo_enabled.load(std::memory_order_acquire);
    u64 code_hash = 0;
    bool have_hash = false;
    u32 hot_dump_variant = hot_dump::kInvalidCodeVariant;
    if (block.byte_length != 0) {
        constexpr u64 kFnvOffset = 14695981039346656037ull;
        constexpr u64 kFnvPrime = 1099511628211ull;
        auto fnv1a = [](u64 hash, const u8* bytes, usize len) noexcept {
            for (usize i = 0; i < len; ++i) {
                hash ^= bytes[i];
                hash *= kFnvPrime;
            }
            return hash;
        };
        if (const Byte* mapped = m_memory.host_ptr(
                block.start_addr, block.byte_length)) {
            code_hash = fnv1a(kFnvOffset, mapped, block.byte_length);
            have_hash = true;
            hot_dump_variant = hot_dump::record_compile(
                block.start_addr, mapped,
                static_cast<u32>(block.byte_length),
                block.instruction_count, false);
        } else {
            u8 chunk[64];
            u64 hash = kFnvOffset;
            usize remaining = block.byte_length;
            GuestAddr cursor = block.start_addr;
            have_hash = true;
            while (remaining != 0) {
                const usize n = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
                if (m_memory.read_bytes(cursor, chunk, n).is_err()) {
                    have_hash = false;
                    break;
                }
                hash = fnv1a(hash, chunk, n);
                cursor += static_cast<GuestAddr>(n);
                remaining -= n;
            }
            if (have_hash) {
                code_hash = hash;
            }
        }
    }
    m_emit_code_hash = code_hash;
    m_emit_has_hash = have_hash;
    struct InvalStatsCompileTimer {
        int cls{-1};
        u64 start{0};
        ~InvalStatsCompileTimer() {
            if (cls >= 0) inval_stats::note_compile_done(cls, inval_stats::now_ns() - start);
        }
    } inval_stats_timer;
    if (inval_stats::g_enabled && have_hash) {
        inval_stats_timer.cls = inval_stats::note_compile(block.start_addr, code_hash);
        inval_stats_timer.start = inval_stats::now_ns();
    }
    select_ir_tier(block, code_hash, have_hash);
    select_cold_residency(block.start_addr, code_hash, have_hash);
    m_cold_eager_variant = cold_lazy_enabled() &&
                           cold_eager_requested(block.start_addr, block.byte_length);

    // The process memo counts actual rewrites, not initial compilations or
    // cache evictions. Locally track rewritten addresses as a backstop for
    // translations without a shared record.
    bool shared_identity_tracked = false;
    const bool shared_accepts = !memo_on || (have_hash
        ? JitCompileMemo::accept_smc_compilation(
              block.start_addr, code_hash, shared_identity_tracked)
        : !JitCompileMemo::shared_smc_demoted(block.start_addr));
    if (!shared_accepts) {
        smc_note_interpret_hit();
        return nullptr;
    }
    if (!shared_identity_tracked) {
        std::lock_guard<std::mutex> churn_lock(m_compile_mutex);
        if (m_churn_interpret.count(block.start_addr) != 0) {
            smc_note_interpret_hit();
            return nullptr;
        }
        if (!memo_on || m_recompile_counts.count(block.start_addr) != 0) {
            const u32 n = memo_on ? m_recompile_counts.at(block.start_addr)
                                 : ++m_recompile_counts[block.start_addr];
            if (memo_on ? n >= JitCompileMemo::kChurnRecompileLimit : n > JitCompileMemo::kChurnRecompileLimit) {
                m_churn_interpret.insert(block.start_addr);
                m_recompile_counts.erase(block.start_addr);
                smc_note_demote();
                std::fprintf(stderr,
                    "[jit] block 0x%08x reached rewrite/recompile limit (%u) — routing to the"
                    " interpreter (self-modifying or code/data aliased)\n",
                    static_cast<unsigned>(block.start_addr), n);
                return nullptr;
            }
        }
    }

    // Check skip list — force interpreter for specific blocks
    for (auto addr : m_cfg.jit_skip) {
        if (block.start_addr == addr) {
            VITREO_TRACE("[jit] SKIP block 0x%08x (in VITREO_JIT_SKIP)\n",
                         static_cast<unsigned>(block.start_addr));
            return nullptr;
        }
    }
    // Range-based skip
    if (m_cfg.jit_skip_above && block.start_addr >= m_cfg.jit_skip_above) {
        return nullptr;
    }
    if (m_cfg.jit_skip_below && block.start_addr < m_cfg.jit_skip_below) {
        return nullptr;
    }
    for (u8 i = 0; i < m_cfg.jit_skip_range_count; ++i) {
        if (block.start_addr >= m_cfg.jit_skip_range_from[i] &&
            block.start_addr < m_cfg.jit_skip_range_to[i]) {
            return nullptr;
        }
    }

    // Stage A2: process-wide compile memo.  If another thread (or this
    // thread on a previous block-cache eviction) already compiled this
    // guest VA, reuse the existing arena code without re-decoding or
    // re-emitting ARM64.  The arena is immutable RX, so sharing the
    // pointer across threads is safe.  Reuses the byte hash computed above.
    JitCompileMemo::CompiledKey compiled_key{};
    bool have_compiled_key = false;
    if (memo_on && have_hash) {
        compiled_key = JitCompileMemo::CompiledKey{block.start_addr, code_hash,
                                   selected_pins.id, block.byte_length,
                                   block.is_64bit,
                                   m_cfg.native_ud2_enabled,
                                   m_cfg.forward_join_state_enabled,
                                   m_cfg.native_callret_enabled,
                                   ir_key_policy(),
                                   m_cfg.linear64_jit_view,
                                   inchain_thunks_codegen(),
                                   m_residency_variant,
                                   m_cold_eager_variant};
        have_compiled_key = true;
    }
    if (have_compiled_key && memo_stats_enabled()) {
        memo_stats_arm_atexit();
    }
    if (have_compiled_key) {
        g_memo_compile_probes.fetch_add(1, std::memory_order_relaxed);
        JitCompileMemo::Shard& shard = JitCompileMemo::shard_for(block.start_addr);
        std::shared_lock<JitMemoLock> rlock(shard.lock);
        const JitCompileMemo::CompiledEntry* entry = nullptr;
        if (auto lit = shard.compiled.find(block.start_addr);
            lit != shard.compiled.end()) {
            entry = lit->second.find(compiled_key);
        }
        if (entry != nullptr &&
            JitCodeArena::arena_code_live(entry->record.code, entry->record.arena_generation) &&
            // A delegate-bearing record is only reusable for a block whose
            // decode has the same shape as the one its stencils point into.
            // The key already pins (address, bytes, size, mode), so this can
            // only differ if decoding is not the pure function of those the
            // memo assumes — in which case reuse would silently execute the
            // wrong instruction, so refuse and compile locally.
            (!entry->record.insns ||
             entry->record.instruction_count == block.instruction_count)) {
            JitCompileMemo::CompiledRecord rec = entry->record;
            rlock.unlock();
            g_memo_compile_hits.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(m_compile_mutex);
            m_compiled_blocks.push_back(CompiledBlock{
                rec.code,
                rec.code_size,
                block.start_addr,
                block.byte_length,
                rec.arena_generation,
                {}
            });
            m_compiled_blocks.back().native_insns = rec.native_insns;
            m_compiled_blocks.back().terminal_opcode = rec.terminal_opcode;
            m_compiled_blocks.back().hot_dump_variant = hot_dump_variant;
            m_compiled_blocks.back().link_code_offset = rec.link_code_offset;
            m_compiled_blocks.back().call_code_offset = rec.call_code_offset;
            m_compiled_blocks.back().requires_protection_lease =
                rec.requires_protection_lease;
            m_compiled_blocks.back().cold_offset = rec.cold_offset;
            m_compiled_blocks.back().cold_size = rec.cold_size;
            m_compiled_blocks.back().pin_layout = compiled_key.pin_layout;
            // Carry the array the shared code dereferences. publish_compiled
            // adopts it into this thread's cache entry, so the baked pointers
            // stay valid even if the memo is later cleared.
            m_compiled_blocks.back().delegate_insns = std::move(rec.insns);
            symbol_map::record(m_compiled_blocks.back());
            if (!rec.links.empty()) {
                m_direct_links[block.start_addr] = rec.links;
            }
            if (m_compiled_blocks.back().delegate_insns) {
                g_memo_delegate_reuses.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_memo_native_reuses.fetch_add(1, std::memory_order_relaxed);
            }
            compile_timer.memo_hit = true;
            compile_timer.success = true;
            compile_timer.native_bytes = rec.code_size;
            m_ts_outcome = TranslationOutcome::MemoHit;
            return &m_compiled_blocks.back();
        }
    }

    std::lock_guard<std::mutex> lock(m_compile_mutex);

    // ---- Persistent translation store (VITREO_TRANSLATION_STORE=warm) ----
    // Only the first translation of a clean image block in this process is
    // looked up or stored; any doubt compiles fresh below.
    m_ts_relocs.clear();
    m_ts_unrelocatable = false;
    m_ts_stubs = 0;
    m_ts_mem_helpers.clear();
    bool ts_candidate = false;
    bool ts_verify = false;
    bool ts_found = false;
    StoredKey ts_key{};
    const u8* ts_bytes = nullptr;
    // A residency variant is laid out from this process's profile.
    if (have_compiled_key && !m_residency_variant && !m_cold_eager_variant &&
        translation_store_mode() == TranslationStoreMode::Warm &&
        TranslationStore::active()) {
        GuestImageIdentity image{};
        if (stats::enabled() || g_emit_attribution_on.load(std::memory_order_relaxed) ||
            m_test_reach_words != 0) {
            tstats::note(tstats::Event::SkipDiagnostics);
        } else if (!m_memory.images().lookup_clean(block.start_addr, block.byte_length,
                                                   image) ||
                   (ts_bytes = reinterpret_cast<const u8*>(
                        m_memory.fetch_ptr(block.start_addr, block.byte_length))) == nullptr) {
            tstats::note(tstats::Event::SkipNotImage);
        } else if (TranslationStore::seen(block.start_addr) &&
                   !(m_tier_active && !m_tier_stencil)) {
            // A tier-1 compile follows its own tier-0 translation of the same
            // clean bytes, so it may still look up and store its own record.
            tstats::note(tstats::Event::SkipSeen);
        } else if (!selected_pins.pins_live) {
            tstats::note(tstats::Event::SkipPinless);
        } else {
            if (m_ts_cfg_hash == 0) m_ts_cfg_hash = TranslationStore::config_hash(m_cfg) | 1;
            ts_key.start_addr = block.start_addr;
            ts_key.code_hash = code_hash;
            ts_key.pin_layout = selected_pins.id;
            ts_key.image_base = image.base;
            ts_key.image_id = image.id;
            ts_key.thunk_lo = m_jit_ctx.thunk_region_lo;
            ts_key.thunk_hi = m_jit_ctx.thunk_region_hi;
            ts_key.cfg_hash = m_ts_cfg_hash;
            ts_key.guest_size = block.byte_length;
            ts_key.ir_policy = ir_key_policy();
            u32 flags = 0;
            if (block.is_64bit) flags |= StoredKey::Is64;
            if (m_cfg.native_ud2_enabled) flags |= StoredKey::NativeUd2;
            if (m_cfg.forward_join_state_enabled) flags |= StoredKey::ForwardJoin;
            if (m_cfg.native_callret_enabled) flags |= StoredKey::NativeCallRet;
            if (m_cfg.linear64_jit_view) flags |= StoredKey::Linear64JitView;
            if (inchain_thunks_codegen()) flags |= StoredKey::InchainThunks;
            if (s_jit_link_enabled.load(std::memory_order_acquire)) flags |= StoredKey::LinkEnabled;
            if (m_memory.is_linear()) flags |= StoredKey::LinearMemory;
            ts_key.flags = flags;
            ts_candidate = true;
            bool ts_hit = false;
            if (m_tier_stencil) {
                // A block that reached tier 1 in an earlier process starts
                // there: its stored tier-1 record is the tier-up result.
                StoredKey tier1_key = ts_key;
                tier1_key.ir_policy =
                    ts_key.ir_policy & ~(kIrKeyTier0 | kIrKeyUncounted);
                if (TranslationStore::lookup(tier1_key, ts_bytes, m_ts_record,
                                             ts_verify)) {
                    promote_compile_to_tier1(block.start_addr, code_hash);
                    ts_key = tier1_key;
                    compiled_key.ir_policy = tier1_key.ir_policy;
                    ts_hit = true;
                }
            }
            if (!ts_hit) {
                ts_hit = TranslationStore::lookup(ts_key, ts_bytes, m_ts_record,
                                                  ts_verify);
            }
            if (ts_hit && !ts_verify) {
                ts_found = true;
                bool pins_match = m_ts_record.pin_layout == selected_pins.id;
                for (u8 g = 0; g < 16 && pins_match; ++g) {
                    pins_match = m_ts_record.pin_arm[g] == selected_pins.arm[g];
                }
                // The stub publishers read the state the emitter would have.
                m_linear_base_pinned = selected_pins.linear_base_pinned;
                m_attr_on = false;
                const bool restored = pins_match && restore_emission(block, m_ts_record);
                const bool stubs_published = restored && ts_publish_stubs(m_ts_record);
                if (restored && !stubs_published) tstats::note(tstats::Event::RejectStub);
                if (stubs_published) {
                    m_ts_outcome = TranslationOutcome::StoreHit;
                    CompiledBlock* adopted = finalize_code(block, &compiled_key);
                    if (adopted == nullptr) {
                        m_ts_outcome = TranslationOutcome::None;
                        return nullptr;
                    }
                    TranslationStore::mark_seen(block.start_addr);
                    adopted->native_insns = m_block_native_insns;
                    adopted->terminal_opcode = m_block_terminal_opcode;
                    adopted->hot_dump_variant = hot_dump_variant;
                    compile_timer.success = true;
                    compile_timer.native_bytes = adopted->code_size;
                    compile_timer.guest_insns = block.instruction_count;
                    return adopted;
                }
                if (!restored) tstats::note(tstats::Event::RejectShape);
                // Compile fresh from a clean slate: the restore left the
                // record's relocations and stubs behind.
                m_ts_relocs.clear();
                m_ts_stubs = 0;
                m_ts_mem_helpers.clear();
            }
        }
    }

    m_code_buf.clear();
    m_attr_on = g_emit_attribution_on.load(std::memory_order_relaxed);
    m_attr_suspend = 0;
    m_attr_cat = EmitAttribution::Prologue;
    m_attr_mark = 0;
    m_attr_tags.clear();
    m_cold_jcc_exits.clear();
    m_forward_jcc_fixups.clear();
    m_native_ud2_linear_unreachable = false;
    m_block_insn_rip_count = 0;
    m_cold_mem_stubs.clear();
    m_stack_proof_branch_pos.clear();
    m_stack_proof_mixed_branch_pos.clear();
    m_stack_entry_load_proof = false;
    m_raise_branch_pos.clear();
    for (auto& branches : m_raise_restore_nzcv_branch_pos) branches.clear();
    m_direct_raise_branch_pos.clear();
    m_cold_lazy_sites.clear();
    m_raise_lazy_plain = false;
    m_raise_lazy_restore.fill(false);
    m_raise_entry_plain_pos = static_cast<usize>(-1);
    m_raise_entry_restore_pos.fill(static_cast<usize>(-1));
    m_cold_ud2_exits.clear();
    for (auto& v : m_continue_exit_pos) v.clear();
    m_continue_exit_body.fill(static_cast<usize>(-1));
    m_api_exit_sites.clear();
    m_api_exit_hot = static_cast<usize>(-1);
    m_api_exit_to_cold = 0;
    m_api_exit_plain = 0;
    m_api_exit_cold_done = false;
    m_deferred_cold_arms.clear();
    m_cold_arm_ranges.clear();
    m_lean_variant_ranges.clear();
    m_flushing_cold_arms = false;
    m_cont_tail_stubs.clear();
    m_rip_reg_pos = static_cast<usize>(-1);
    m_link_entry_pos = 1;
    m_hot_end_pos = 0;
    m_call_entry_pos = std::numeric_limits<u32>::max();
    m_emit_links.clear();
    m_block_start_addr = block.start_addr;
    m_block_is_64bit = block.is_64bit;
    m_xmm_v0_forward_in = 0xFF;
    m_xmm_cache = 0;
    m_xmm_v0_forward_out = 0xFF;
    m_xmm_v0_alias_in = 0xFF;
    m_xmm_v0_alias_out = 0xFF;
    m_x87_d0_forward_in = false;
    m_x87_d0_forward_out = false;
    m_has_predecoded_delegate = false;
    m_block_native_insns = 0;
    m_block_terminal_opcode = block.instructions.size() != 0
        ? static_cast<u16>(block.instructions[block.instructions.size() - 1].opcode)
        : 0;
    m_stats_insns.clear();
    // Blocks are reached from many predecessors: the record's kind byte on
    // entry is whatever the last one left, so no prediction at block start.
    m_flags.static_lazy_kind = kLazyKindUnknown;
    m_flags.nzcv = NzcvState::None;
    // A block is reached from many predecessors, so the host FPU's rounding
    // mode on entry is unknown: the first FP instruction must re-install it.
    m_fp_class_installed = FpRoundingClass::None;
    m_flags.incoming_nzcv = NzcvState::None;
    m_flags.hold_nzcv = false;
    m_flags.nzcv_hold_depth = 0;
    m_flags.pred_nzcv_live = true;
    m_flags.preserved_pred_nzcv = false;
    m_flags.hold_nzcv = false;
    m_flags.pending_carry_source = CarrySource::Unknown;
    m_flags.incoming_carry_source = CarrySource::Unknown;
    m_pending_stub_store = PendingStubStore{};
    m_incoming_stub_store = PendingStubStore{};
    m_pending_alu32_nz_result = 0xFF;
    m_incoming_alu32_nz_result = 0xFF;
    m_omit_lazy_current = false;
    m_omit_lazy_keep_overlay = false;
    m_stencil_flag_recovery.clear();
    compute_omit_lazy_flags(block, m_omit_lazy, &m_stencil_flag_recovery,
                            &m_elided_span,
                            /*cross_nzcv_readers=*/!m_force_no_omit);
    if (m_force_no_elide) {
        // Publishing every record is always sound: nothing is elided, so no
        // emitted read can find a stale one.
        m_omit_lazy.assign(m_omit_lazy.size(), 0);
        m_stencil_flag_recovery.clear();
        m_elided_span.assign(m_elided_span.size(), 0);
    }
    m_elided_live_current = false;
    m_elided_record_read = false;
    // Fault recovery can use a host register only while the guest value is
    // pinned there. Otherwise keep the architectural lazy record at its
    // producer; this also covers partially pinned layouts.
    //
    // A 64-bit block is the exception: an unpinned guest register (R8-R15)
    // is written through to CpuState by the instruction that defines it, so
    // its value is in CpuState at every later fault, and both fault paths
    // read it there (JitFlagRecovery::kCpuStateGpr). The 32-bit cold-stub
    // replay (m_ir_stub_replay) needs a host register, so PE32 keeps the
    // pinned-only rule.
    std::erase_if(m_stencil_flag_recovery, [&](const StencilFlagRecovery& plan) {
        if (!plan.active()) return false;
        // VITREO_JIT_NO_CPUSTATE_RECOVERY=1 restores the pinned-only rule.
        static const bool no_cpu_state_recovery =
            std::getenv("VITREO_JIT_NO_CPUSTATE_RECOVERY") != nullptr;
        const bool cpu_state_ok = block.is_64bit && !no_cpu_state_recovery;
        const bool unavailable =
            (!plan.src_is_imm && !cpu_state_ok &&
             selected_pins.arm[plan.src_reg] == 0xFF) ||
            (!plan.dst_is_imm && !cpu_state_ok &&
             selected_pins.arm[plan.dst_reg] == 0xFF);
        if (!unavailable) return false;
        GuestAddr rip = block.start_addr;
        for (u32 i = 0; i < block.instruction_count; ++i) {
            if (rip == plan.def_rip) {
                m_omit_lazy[i] = 0;
                // The record is published after all: its span is not elided.
                for (u32& owner : m_elided_span) {
                    if (owner == i + 1) owner = 0;
                }
            }
            rip += block.instructions[i].length;
        }
        return true;
    });

    // A mid-block backward edge gives its target two predecessors: the
    // straight-line preheader and the loop latch.  Their lazy-flag producer
    // kinds need not match.  Record those join points up front so instruction
    // lowering there cannot specialize an ADC/SBB/INC/DEC (or a later Jcc)
    // to metadata that is true only on the first entry.  The generated code
    // then consults CpuState's architectural lazy flags on every entry while
    // the branch itself can remain an in-JIT backedge.
    std::array<GuestAddr, kMaxInsnArmPos> flag_join_rips{};
    u32 flag_join_count = 0;
    {
        GuestAddr scan_rip = block.start_addr;
        for (u32 i = 0;
             i < block.instruction_count &&
             m_block_insn_rip_count < m_block_insn_rips.size();
             ++i) {
            m_block_insn_rips[m_block_insn_rip_count++] = scan_rip;
            scan_rip += block.instructions[i].length;
        }
        scan_rip = block.start_addr;
        for (u32 i = 0; i < block.instruction_count; ++i) {
            const DecodedInstruction& instruction = block.instructions[i];
            const bool is_jcc = jcc_condition_code(instruction.opcode) != 0xFF;
            const bool is_loop = instruction.opcode == Opcode::LOOP_REL8 ||
                                 instruction.opcode == Opcode::LOOPE_REL8 ||
                                 instruction.opcode == Opcode::LOOPNE_REL8;
            if ((is_jcc || is_loop) && instruction.operand_count != 0) {
                const OperandType type = instruction.operands[0].type;
                const bool relative = type == OperandType::Rel8 ||
                                      type == OperandType::Rel16 ||
                                      type == OperandType::Rel32;
                if (relative) {
                    const GuestAddr next = scan_rip + instruction.length;
                    const GuestAddr target = next +
                        static_cast<u64>(instruction.operands[0].imm);
                    if (target > block.start_addr && target < scan_rip &&
                        flag_join_count < flag_join_rips.size()) {
                        bool duplicate = false;
                        for (u32 j = 0; j < flag_join_count; ++j) {
                            duplicate |= flag_join_rips[j] == target;
                        }
                        if (!duplicate) flag_join_rips[flag_join_count++] = target;
                    }
                }
            }
            scan_rip += instruction.length;
        }
    }
    m_pins_live = selected_pins.pins_live;
    m_linear_base_pinned = selected_pins.linear_base_pinned;
    for (u8 guest = 0; guest < 16; ++guest) {
        m_pin_arm[guest] = selected_pins.arm[guest];
    }
    m_pin_layout = selected_pins.id;
    m_esp_frame_live = false;

    // ---- Seam A: the Tier 1 boundary (ADR-0001) ----------------------------
    //
    // This is the last point at which the decoded instructions, the
    // instruction count and the selected pin ABI are all valid while
    // `m_code_buf` is still EMPTY -- the first ARM word goes in a few lines
    // below. A whole-block attempt has to sit here: Tier 1 is all-or-nothing,
    // so it must decide before Tier 0 has emitted anything it would have to
    // unwind.
    //
    // The verdict is recorded for coverage and then DISCARDED. This milestone
    // ships the IR frontend and its decline path, not an ARM64 backend, so
    // every block continues to Tier 0 immediately below regardless of what the
    // frontend said. When the backend lands (M6) this becomes the branch that
    // returns Tier 1 code, and the accept rate this telemetry is collecting now
    // is what says which opcode families are worth teaching it first.
    //
    // Cost when the gate is off (the default): one relaxed load of a cached
    // static inside `ir_enabled_for`, before any instruction is touched.
    // X15 caches `linear_base + ESP` for ESP-relative stack accesses, and
    // `m_esp_frame_live` says whether the ADD that establishes it has been
    // emitted. It is COMPILE-TIME state describing a RUNTIME register, so it
    // has to be false at the start of every block — a block is entered fresh
    // from the dispatcher with X15 undefined.
    //
    // It was not reset here. Every other reset lives inside a memory emitter,
    // so the invariant held only by accident: a block reaching an ESP-disp
    // access had almost always executed some other memory op first, and that
    // cleared the flag. When it did not, `try_esp_disp_load32` saw a stale
    // true left by the PREVIOUS block, skipped the establisher, and emitted a
    // bare `LDR Wd,[X15,#disp]` against an undefined register.
    //
    // Host SIGSEGV at guest address 0x30. The crash dump decodes it as
    // `LDR W0,[X15,#44]` with X15 = 4. The fault is pre-existing and belongs
    // to Tier 0; the 8/16-bit tranche only changes which blocks take which
    // path, and therefore how often an unset frame register is consumed.
    m_esp_frame_live = false;

    m_ir_emit_active = false;
    ir_attempt_block(block);
    if (decline_tier1_compile(block.start_addr, code_hash,
                              have_compiled_key && !m_retry_tier0)) {
        m_retry_tier0 = true;
        return nullptr;
    }

    // Prologue: save LR so BLR calls inside JIT code don't lose it.
    // STR X30, [SP, #-16]!  (pre-index, 16-byte aligned)
    //
    // W22, the chain's count of loop iterations and counted hops, is set to 1
    // by execute() before it calls the block, not here.
    emit(0xF81F0FFEu);

    // Dispatcher entry loads pins from CpuState. Linked hops skip this
    // (compiled_link_code) and keep X23–X28 / X12 / X13 live. A standard-pin
    // x64 block calls the process-wide copy of that load sequence.
    const u8 entry_kind = m_cfg.linear64_jit_view ? kEntryJitView : kEntryCheckMap;
    if (shared_prologue_enabled() && std_pin_block() && m_linear_base_pinned &&
        materialize_dispatch_tail(entry_kind) != nullptr) {
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                               (JIT_CTX_OFF_DISPATCH_TAIL >> 3) + entry_kind));
        emit(0xD63F0200u);  // BLR X16
    } else {
        emit_load_pins();
        if (m_linear_base_pinned) emit_load_linear_base();
        if (m_block_is_64bit && m_cfg.linear64 && m_linear_base_pinned) {
            emit_load_linear64_check_map();
        }
    }
    if (m_cfg.native_callret_enabled && m_block_is_64bit) {
        emit_native_call_landing_pad();
    }
    m_link_entry_pos = static_cast<u32>(m_code_buf.size());
    // VITREO_JIT_PROBE_POISON_LINK_X0=1: **MEASUREMENT PROBE, not an
    // optimisation.** Overwrites X0 at every block's linked entry, so any
    // code that assumed a predecessor left X0 holding this block's RIP reads
    // garbage instead. It audits the contract emit_linked_tail_continue's
    // slot relies on (a direct-linked hop does not materialize X0).
    static const bool probe_poison_link_x0 =
        std::getenv("VITREO_JIT_PROBE_POISON_LINK_X0") != nullptr;
    if (probe_poison_link_x0) {
        emit(arm64::movz_x(0, 0xDEAD, 48));
    }

    // Deliberately narrow block-entry proof: two or more native
    // MOV r{32,64},[rsp+disp] loads, with non-negative small displacements,
    // plus only flag-neutral register/immediate MOVs and true no-ops. The
    // explicit filler whitelist proves RSP is invariant, introduces neither
    // memory nor helpers, and keeps every guest access inside the interval.
    i32 stack_min = 0;
    i32 stack_max = 0;
    if (m_block_is_64bit && m_cfg.linear64 && m_cfg.linear64_hw_protect &&
        (block.instruction_count >= 2 || stack_proof_stats_enabled())) {
        StackProofOutcome outcome = StackProofOutcome::Eligible;
        Opcode rejected_opcode = Opcode::INVALID;
        u32 stack_load_count = 0;
        stack_min = 4095;
        for (u32 i = 0; i < block.instruction_count; ++i) {
            const DecodedInstruction& insn = block.instructions[i];
            const bool has_two_operands = insn.operand_count == 2;
            const bool load64 = has_two_operands &&
                                insn.opcode == Opcode::MOV_R64_RM64 &&
                                insn.operands[0].type == OperandType::Reg64 &&
                                insn.operands[1].type == OperandType::Mem64;
            const bool load32 = has_two_operands &&
                                insn.opcode == Opcode::MOV_R32_RM32 &&
                                insn.operands[0].type == OperandType::Reg32 &&
                                insn.operands[1].type == OperandType::Mem32;
            if (!load64 && !load32) {
                const u8 rsp = static_cast<u8>(RegIndex::RSP);
                const bool mov64_imm =
                    insn.opcode == Opcode::MOV_R64_IMM64 &&
                    has_two_operands &&
                    insn.operands[0].type == OperandType::Reg64 &&
                    insn.operands[1].type == OperandType::Imm64;
                const bool mov32_imm =
                    (insn.opcode == Opcode::MOV_R32_IMM32 ||
                     insn.opcode == Opcode::MOV_RM32_IMM32) &&
                    has_two_operands &&
                    insn.operands[0].type == OperandType::Reg32 &&
                    insn.operands[1].type == OperandType::Imm32;
                const bool mov64_imm32 =
                    insn.opcode == Opcode::MOV_RM64_IMM32 &&
                    has_two_operands &&
                    insn.operands[0].type == OperandType::Reg64 &&
                    insn.operands[1].type == OperandType::Imm32;
                const bool mov64_reg =
                    (insn.opcode == Opcode::MOV_R64_RM64 ||
                     insn.opcode == Opcode::MOV_RM64_R64) &&
                    has_two_operands &&
                    insn.operands[0].type == OperandType::Reg64 &&
                    insn.operands[1].type == OperandType::Reg64;
                const bool mov32_reg =
                    (insn.opcode == Opcode::MOV_R32_RM32 ||
                     insn.opcode == Opcode::MOV_RM32_R32) &&
                    has_two_operands &&
                    insn.operands[0].type == OperandType::Reg32 &&
                    insn.operands[1].type == OperandType::Reg32;
                // Intel SDM Vol. 2A, LEA: compute EffectiveAddress(SRC),
                // flags affected: none. Its syntactic memory operand never
                // accesses guest memory, so it is a safe proof filler as long
                // as the destination does not mutate RSP.
                const bool lea64 =
                    insn.opcode == Opcode::LEA_R64_M && has_two_operands &&
                    insn.operands[0].type == OperandType::Reg64 &&
                    is_memory_operand(insn.operands[1].type);
                const bool lea32 =
                    insn.opcode == Opcode::LEA_R32_M && has_two_operands &&
                    insn.operands[0].type == OperandType::Reg32 &&
                    is_memory_operand(insn.operands[1].type);
                const bool noop = insn.opcode == Opcode::NOP ||
                                  insn.opcode == Opcode::NOP_RM ||
                                  insn.opcode == Opcode::PAUSE;
                const bool filler = mov64_imm || mov32_imm || mov64_imm32 ||
                                    mov64_reg || mov32_reg || lea64 || lea32;
                if (filler && insn.operands[0].reg == rsp) {
                    outcome = StackProofOutcome::RspWrite;
                    break;
                }
                if (filler || noop) {
                    continue;
                }

                bool has_memory = false;
                for (u8 operand = 0; operand < insn.operand_count; ++operand) {
                    has_memory |= is_memory_operand(insn.operands[operand].type);
                }
                outcome = has_memory ? StackProofOutcome::OtherMemory
                                     : StackProofOutcome::OtherOpcode;
                rejected_opcode = insn.opcode;
                break;
            }
            if (insn.operands[0].reg == static_cast<u8>(RegIndex::RSP)) {
                outcome = StackProofOutcome::RspWrite;
                break;
            }
            const i32 width = load64 ? 8 : 4;
            const MemoryOperand& mem = insn.operands[1].mem;
            if (mem.base != static_cast<u8>(RegIndex::RSP) ||
                mem.has_index() || mem.vsib || mem.has_absolute ||
                mem.segment != SegReg::Default ||
                insn.prefixes.address_size || mem.disp < 0 ||
                mem.disp > 4096 - width) {
                outcome = StackProofOutcome::BadStackLoad;
                break;
            }
            ++stack_load_count;
            stack_min = std::min(stack_min, mem.disp);
            stack_max = std::max(stack_max, mem.disp + width - 1);
        }
        if (outcome == StackProofOutcome::Eligible) {
            if (stack_load_count == 0) {
                outcome = StackProofOutcome::NoStackLoad;
            } else if (stack_load_count == 1) {
                outcome = StackProofOutcome::OneStackLoad;
            }
        }
        // A residency profile drops a proof this block keeps failing.
        m_stack_entry_load_proof = outcome == StackProofOutcome::Eligible &&
                                   !m_residency_drop_proof;
        record_stack_proof_outcome(outcome, rejected_opcode,
                                   stack_load_count);
        assert(!m_stack_entry_load_proof ||
               stack_max - stack_min < (1 << kLinearHostPageShift));
    }

    if (m_stack_entry_load_proof) {
        if (stack_min == 0) {
            emit(arm64::mov_x(9, REG_PIN_ESP));
        } else {
            emit(arm64::add_x_imm(9, REG_PIN_ESP,
                                  static_cast<u32>(stack_min)));
        }
        emit(arm64::add_x_imm(17, REG_PIN_ESP,
                              static_cast<u32>(stack_max)));
        // Include raw RSP as well as both endpoints: near 2^64, both positive
        // displacement endpoints can wrap low together.
        emit(arm64::orr_x(8, REG_PIN_ESP, 9));
        emit(arm64::orr_x(8, 8, 17));
        emit(arm64::lsr_x_imm(8, 8, m_cfg.linear64_address_bits));
        m_stack_proof_branch_pos.push_back(m_code_buf.size());
        emit(0);

        // The protection lease makes this observation durable until native
        // return. Check both interval endpoints because the stack span may
        // cross a 16 KiB host-page boundary even though every individual load
        // is small. A mixed endpoint deopts the whole block before instruction
        // zero; the interpreter then applies exact 4 KiB guest permissions.
        usize ignored = static_cast<usize>(-1);
        emit_linear64_check_page_test(9, 17, 2, ignored);
        m_stack_proof_mixed_branch_pos.push_back(ignored);
    }

    // DIAGNOSTIC (VITREO_BLOCK_ORIGIN=1): bucket every compiled block by guest
    // address. One load compiles ~92k blocks that each execute
    // once, and the fix depends on where they come from: static image code
    // can be translated once and cached across runs, while code a managed
    // runtime generates at runtime cannot and needs a cheaper cold path.
    static const bool origin_hist = std::getenv("VITREO_BLOCK_ORIGIN") != nullptr;
    if (origin_hist) {
        static std::mutex omu;
        static std::unordered_map<u32, u64> by_mb;
        static u64 total = 0;
        std::lock_guard<std::mutex> lk(omu);
        ++by_mb[static_cast<u32>(block.start_addr >> 20)];  // 1 MiB buckets
        // Block caches are per-thread (guest_run_loop.cpp t_owned_cache), so
        // the same guest code could in principle be compiled once PER THREAD.
        // Measured on that load it is not: dup_factor 1.00 over 60k
        // compiles, max 7 copies of any single address.
        static std::unordered_map<u32, u32> by_addr;
        const u32 dup = ++by_addr[static_cast<u32>(block.start_addr)];
        static u32 max_dup = 0;
        if (dup > max_dup) max_dup = dup;
        if ((++total % 20000) == 0) {
            std::fprintf(stderr,
                "[block-origin] total=%llu distinct=%zu dup_factor=%.2f "
                "max_copies_of_one_addr=%u\n",
                (unsigned long long)total, by_addr.size(),
                static_cast<double>(total) /
                    static_cast<double>(by_addr.size()),
                max_dup);

            std::vector<std::pair<u32, u64>> rows(by_mb.begin(), by_mb.end());
            std::sort(rows.begin(), rows.end(),
                      [](const auto& a, const auto& b) {
                          return a.second > b.second;
                      });
            std::fprintf(stderr,
                         "[block-origin] %llu compiled blocks, top ranges:\n",
                         (unsigned long long)total);
            for (usize i = 0; i < rows.size() && i < 12; ++i) {
                std::fprintf(stderr,
                    "[block-origin]   0x%08x-0x%08x  %llu blocks (%.1f%%)\n",
                    rows[i].first << 20, ((rows[i].first + 1) << 20) - 1,
                    (unsigned long long)rows[i].second,
                    100.0 * static_cast<double>(rows[i].second) /
                        static_cast<double>(total));
            }
        }
    }

    if (code_watch_hit(block.start_addr)) {
        emit_nzcv_spill();
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit_mov_imm64(2, block.start_addr);
        note_unrelocatable_host_bake();
        emit_mov_imm64(16, reinterpret_cast<u64>(&report_watched_block));
        emit_helper_blr();
        emit_nzcv_fill();
    }

    if (u64* slot = rip_counter_slot(block.start_addr)) {
        // X0/X1 are caller-saved scratch and carry nothing at a block entry.
        const auto p = reinterpret_cast<u64>(slot);
        note_unrelocatable_host_bake();
        emit(arm64::movz_x(0, static_cast<u16>(p), 0));
        emit(arm64::movk_x(0, static_cast<u16>(p >> 16), 16));
        emit(arm64::movk_x(0, static_cast<u16>(p >> 32), 32));
        emit(arm64::movk_x(0, static_cast<u16>(p >> 48), 48));
        emit(arm64::ldr_x_uoff(1, 0, 0));
        emit(arm64::add_x_imm(1, 1, 1));
        emit(arm64::str_x_uoff(1, 0, 0));
    }

    // Scalar byte-zeroing loops are common in allocator and decompressor
    // output paths. Execute up to the normal JIT loop budget with one checked
    // bulk write. If the runtime range is unsafe (zero count, wraparound,
    // protection boundary, or executable memory), branch into the ordinary
    // per-instruction body below with no state changed.
    if (auto zero_loop = detect_zero_fill_loop(block)) {
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit_mov_imm32(2, zero_loop->dst_reg);
        emit_mov_imm32(3, zero_loop->count_reg);
        emit_mov_imm32(
            4, static_cast<u32>(block.start_addr + zero_loop->branch_offset));
        emit_mov_imm32(
            5, static_cast<u32>(block.start_addr + zero_loop->fallthrough_offset));
        emit(arm64::ldr_x_uoff(
            16, REG_JIT_CTX, JIT_CTX_OFF_ZERO_FILL_LOOP >> 3));
        emit_helper_blr(); // BLR X16

        const usize fallback_pos = m_code_buf.size();
        emit(0); // CBZ W0, ordinary body
        emit_exit_reason(JitExitReason::EpilogueZeroFillLoop);  // DIAGNOSTIC
        emit_epilogue(JitExecResult::Continue);
        const usize ordinary_body_pos = m_code_buf.size();
        m_code_buf[fallback_pos] = arm64::cbz_w(
            0, static_cast<i32>(ordinary_body_pos - fallback_pos) * 4);
    }

    // Record the ordinary body start for loop-back branches. The zero-fill
    // prefix above is intentionally outside the loop: a rejected fast path
    // must not be retried on every scalar iteration.
    // Host rounding mode: install the block's class here, once, rather than
    // in front of the first FP instruction where a mid-block loop would
    // re-run it every iteration. See emit_block_fp_rounding_prologue().

    emit_block_fp_rounding_prologue(block);

    m_body_start_pos = static_cast<u32>(m_code_buf.size());
    // After the body start, which in-block loop-backs branch to, so a loop
    // iteration counts as an entry.
    emit_tier_entry_counter();
    attr_set(EmitAttribution::Body);
    m_insn_pos_count = 0;
    // Recovery descriptors are per block; a stale one would tell the fault
    // handler to rebuild a record this block never elided.
    m_insn_flag_recovery.fill(JitFlagRecovery{});
    m_insn_rsp_undo.fill(0);
    m_current_site = ~0u;

    u64 rip = block.start_addr;
    bool all_compiled = true;
    // Compile-record bookkeeping only (perf window's codegen size model).
    // CompileWindowTimer seeds guest_insns from block.instruction_count, which
    // is the DECODED count. A block that bails on an unsupported opcode emits
    // code for fewer instructions than that, and reporting the decoded count
    // would credit the emitted bytes to guest instructions that were never
    // lowered -- which biases the FIXED/PER_INSN regression downward on
    // exactly the shortest, most numerous blocks. Track what was actually
    // lowered and hand that to the timer instead.
    u32 lowered_insn_count = block.instruction_count;
    const bool trace_ops = g_env_trace_opcodes;

    // Pre-pass: detect fusable SSE idioms (classification / future bulk
    // paths). SseMemcpy currently compiles as native MOVDQU so XMM state
    // matches the interpreter. Disable detection with VITREO_DISABLE_IDIOMS=1.
    auto idioms = g_env_enable_idioms ? detect_idioms(block)
                                  : std::vector<IdiomMatch>{};
    u32 next_idiom = 0;

    // Tier 1 (ADR-0002) lowers the whole body instead of the stencil loop.
    // The decision was taken at Seam A, before the prologue; by here it is
    // final, and the loop below is skipped in its entirety rather than
    // consulted per instruction.
    if (m_ir_emit_active) ir_emit_block(block);

    for (u32 i = 0; !m_ir_emit_active && i < block.instruction_count; ++i) {
        // Check if this instruction starts a fused idiom
        if (next_idiom < idioms.size() &&
            i == idioms[next_idiom].start_idx) {
            // SseMemcpy compiles as native MOVDQU (XMM + memory). A C++
            // helper BLR lost to that path on the warm-loop scoreboard.
            ++next_idiom;
        }

        const auto& instr = block.instructions[i];
        u64 next_rip = rip + instr.length;
        bool is_last = (i == block.instruction_count - 1);
        bool forward_join = false;
        bool homogeneous_forward_join = false;
        bool have_forward_state = false;
        bool forward_state_conflict = false;
        NzcvState forward_join_nzcv = NzcvState::None;
        CarrySource forward_join_carry = CarrySource::Unknown;
        const usize join_pos = m_code_buf.size();
        for (const ForwardJccFixup& fixup : m_forward_jcc_fixups) {
            if (fixup.target != rip) continue;
            m_code_buf[fixup.branch_pos] = arm64::b_cond(
                fixup.arm_cond,
                cold_branch_disp(fixup.branch_pos, join_pos, 19,
                                 "in-block forward Jcc"));
            forward_join = true;
            if (!have_forward_state && !forward_state_conflict) {
                forward_join_nzcv = fixup.nzcv;
                forward_join_carry = fixup.carry;
                have_forward_state = true;
            } else if (fixup.nzcv != forward_join_nzcv ||
                       fixup.carry != forward_join_carry) {
                forward_state_conflict = true;
            }
        }
        homogeneous_forward_join = forward_join && have_forward_state &&
            !forward_state_conflict && m_native_ud2_linear_unreachable &&
            m_cfg.forward_join_state_enabled;
        if (forward_join) {
            std::erase_if(m_forward_jcc_fixups,
                          [rip](const ForwardJccFixup& fixup) {
                              return fixup.target == rip;
                          });
        }
        bool flag_join = false;
        for (u32 j = 0; j < flag_join_count; ++j) {
            flag_join |= flag_join_rips[j] == rip;
        }
        if (flag_join) {
            // A backward predecessor can carry a different lazy-flag record
            // from the guarded forward edge.  Keep loop headers conservative
            // even when the forward sources themselves are homogeneous.
            homogeneous_forward_join = false;
        }
        if (flag_join || forward_join) {
            m_flags.nzcv = NzcvState::None;
            m_flags.incoming_nzcv = NzcvState::None;
            // Predecessors may disagree on the record kind; the taken edge
            // skipped whatever the fall-through path published.
            m_flags.static_lazy_kind = kLazyKindUnknown;
            m_flags.pending_carry_source = CarrySource::Unknown;
            m_flags.incoming_carry_source = CarrySource::Unknown;
            m_pending_stub_store = PendingStubStore{};
            m_incoming_stub_store = PendingStubStore{};
            m_pending_alu32_nz_result = 0xFF;
            m_incoming_alu32_nz_result = 0xFF;
            m_flags.pred_nzcv_live = false;
            m_flags.preserved_pred_nzcv = false;
            m_xmm_v0_forward_in = 0xFF;
            m_xmm_v0_forward_out = 0xFF;
            m_xmm_v0_alias_in = 0xFF;
            m_xmm_v0_alias_out = 0xFF;
            m_xmm_cache = 0;  // predecessors disagree on what V16-V30 hold
            m_x87_d0_forward_in = false;
            m_x87_d0_forward_out = false;
            m_esp_frame_live = false;
            m_rip_reg_pos = static_cast<usize>(-1);
            if (homogeneous_forward_join) {
                // Intel Jcc and UD2 are flag-neutral, and UD2 cannot fall
                // through. The branch source is therefore this target's only
                // architectural predecessor; restore its compile-time view.
                m_flags.nzcv = forward_join_nzcv;
                m_flags.pending_carry_source = forward_join_carry;
            }
        }
        const bool recorded_insn = m_insn_pos_count < kMaxInsnArmPos;
        const u32 recorded_index = m_insn_pos_count;
        m_current_site = recorded_insn ? recorded_index : ~0u;
        if (recorded_insn) {
            m_insn_rips[m_insn_pos_count] = rip;
            m_insn_arm_pos[m_insn_pos_count] =
                static_cast<u32>(m_code_buf.size());
            ++m_insn_pos_count;
        }
        if (forward_join && !homogeneous_forward_join) {
            // nzcv_kind describes live host PSTATE carried across a linked
            // edge. This join has heterogeneous predecessors, so force later
            // Jccs to consult the architectural lazy-flag record instead.
            emit_store_nzcv_kind(NzcvState::None);
        }
        if (forward_join) m_native_ud2_linear_unreachable = false;

        // Bits 0-1 are the publisher's omit state; kInternalJoinJcc (bit 2)
        // rides on a Jcc index and must not be read as an omission here.
        m_ir_stub_replay = StubFlagReplay{};
        for (const StencilFlagRecovery& plan : m_stencil_flag_recovery) {
            // The cold-stub replay below is the 32-bit linear store's; a
            // 64-bit block's software raises are rebuilt by execute() from
            // the published descriptors (raise_block_pc).
            if (!plan.active() || m_block_is_64bit) continue;
            if (rip >= plan.first_rip && rip <= plan.last_rip) {
                m_ir_stub_replay.active = true;
                m_ir_stub_replay.flag_op = plan.flag_op;
                m_ir_stub_replay.recompute = plan.recompute_dst;
                if (plan.src_is_imm) {
                    m_ir_stub_replay.src_imm = static_cast<u32>(plan.src_imm);
                } else {
                    m_ir_stub_replay.src_reg = pinned_gpr32(plan.src_reg);
                }
                if (plan.dst_is_imm) {
                    m_ir_stub_replay.dst_imm = static_cast<u32>(plan.dst_imm);
                } else {
                    m_ir_stub_replay.dst_reg = pinned_gpr32(plan.dst_reg);
                }
                break;
            }
        }
        m_omit_lazy_current =
            (i < m_omit_lazy.size() && (m_omit_lazy[i] & 3u) != 0);
        m_elided_live_current =
            i < m_elided_span.size() && m_elided_span[i] != 0;
        m_current_insn_index = i;
        m_omit_lazy_keep_overlay =
            (i < m_omit_lazy.size() && (m_omit_lazy[i] & 3u) == 2);
        m_internal_join_jcc_current =
            (i < m_omit_lazy.size() &&
             (m_omit_lazy[i] & kInternalJoinJcc) != 0);
        m_next_opcode = is_last ? Opcode::INVALID
                                : block.instructions[i + 1].opcode;
        m_next_self_test_gpr = 0xFF;
        if (!is_last) {
            const DecodedInstruction& next = block.instructions[i + 1];
            if (next.opcode == Opcode::TEST_RM32_R32 &&
                next.operand_count >= 2 &&
                is_register_operand(next.operands[0].type) &&
                is_register_operand(next.operands[1].type) &&
                next.operands[0].reg == next.operands[1].reg) {
                m_next_self_test_gpr = next.operands[0].reg;
            }
        }

        if (trace_ops) {
            u32 o0b = 0, o0i = 0, o0s = 0, o0d = 0;
            u32 o1b = 0, o1i = 0, o1s = 0, o1d = 0;
            if (instr.operand_count > 0 && is_memory_operand(instr.operands[0].type)) {
                o0b = instr.operands[0].mem.base;
                o0i = instr.operands[0].mem.index;
                o0s = instr.operands[0].mem.scale;
                o0d = static_cast<u32>(instr.operands[0].mem.disp);
            }
            if (instr.operand_count > 1 && is_memory_operand(instr.operands[1].type)) {
                o1b = instr.operands[1].mem.base;
                o1i = instr.operands[1].mem.index;
                o1s = instr.operands[1].mem.scale;
                o1d = static_cast<u32>(instr.operands[1].mem.disp);
            }
            VITREO_TRACE("[jit][op] block=0x%08x i=%u rip=0x%08x opc=%u len=%u op0t=%u op1t=%u\n",
                         static_cast<unsigned>(block.start_addr),
                         static_cast<unsigned>(i),
                         static_cast<unsigned>(rip),
                         static_cast<unsigned>(instr.opcode),
                         static_cast<unsigned>(instr.length),
                         static_cast<unsigned>(instr.operand_count > 0 ? instr.operands[0].type : OperandType::None),
                         static_cast<unsigned>(instr.operand_count > 1 ? instr.operands[1].type : OperandType::None));
            VITREO_TRACE("[jit][opm] o0(b=%u i=%u s=%u d=%d) o1(b=%u i=%u s=%u d=%d)\n",
                         o0b, o0i, o0s, static_cast<int>(o0d),
                         o1b, o1i, o1s, static_cast<int>(o1d));
        }

        m_vregs_clobbered = false;
        m_xmm_cache_aware = false;
        const bool instruction_compiled =
            compile_instruction(instr, rip, next_rip, is_last);
        xmm_cache_note_instruction(instr);
        if (!instruction_compiled) {
            // The interpreter runs this instruction from CpuState next; an
            // elided record there would be stale for it.
            note_lazy_record_read(1);
            if (recorded_insn) {
                --m_insn_pos_count;
            }
            VITREO_TRACE("[jit] Block 0x%08x: can't compile opcode %u at insn %u\n",
                         static_cast<unsigned>(block.start_addr),
                         static_cast<unsigned>(instr.opcode), i);
            if (i == 0) {
                return nullptr;
            }
            if (!m_forward_jcc_fixups.empty()) {
                return nullptr;
            }
            u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                                    ji_off >> 2));
            emit_update_rip(rip);
            emit_exit_reason(JitExitReason::EpilogueUncompilable);  // DIAGNOSTIC
            emit_epilogue(JitExecResult::Continue);
            all_compiled = false;
            lowered_insn_count = i;  // compile-record bookkeeping only
            break;
        }
        if (recorded_insn) {
            m_insn_arm_end[recorded_index] =
                static_cast<u32>(m_code_buf.size());
        }
        // We skipped recording this producer's lazy flags, so no fused carry
        // claim from it is valid for a following ADC/SBB in this block.
        if (m_omit_lazy_current) {
            m_flags.pending_carry_source = CarrySource::Unknown;
        }

        rip = next_rip;
    }

    if (!m_forward_jcc_fixups.empty()) {
        return nullptr;
    }
    attr_set(EmitAttribution::Other);

    // Linked-tail stubs first, adjacent to the body: every block exit that is
    // not direct-linked runs one, so it belongs in the body's last cache line
    // and not past the cold memory stubs. The cold Jcc stubs below may add
    // tails; the second flush places any stub only they use. Shared continue
    // exits last: every stub above ends in one. A block laid out for split
    // placement also flushes the continue exits its hot exits use before
    // the cold region -- every non-hop exit runs one, so they are hot code --
    // and the final flush only adds a variant the cold region alone needs.
    emit_flush_cont_tail_stubs();
    if (m_cfg.split_cold) {
        // Mid-block Jcc taken exits are warm, not cold: x64 traces absorb
        // forward Jccs, so every taken one runs its exit stub (direct-link
        // slot, then B to the target). They go with the hot part, and the
        // tails and continue exits they add with them.
        emit_flush_mid_jcc_exits();
        emit_flush_cont_tail_stubs();
        emit_flush_api_call_exit_stubs();
        emit_flush_continue_exit_stubs();
    }
    attr_credit();
    m_hot_end_pos = static_cast<u32>(m_code_buf.size());
    emit_flush_cold_jcc_exits();
    emit_flush_cont_tail_stubs();
    // Before the continue exits: the in-chain API-call exit ends in one.
    // Its call part is cold: only an in-chain export runs it.
    emit_flush_api_call_exit_stubs();
    emit_flush_api_call_exit_cold();
    emit_flush_continue_exit_stubs();
    m_elided_live_current = false;

    if (m_elided_record_read) {
        static std::atomic<u32> reported{0};
        if (!m_force_no_omit) {
            // Some emitted code reads a lazy record that liveness elided: the
            // analysis crossed a reader it assumed would fuse from host NZCV.
            // Discard this attempt before anything is published.
            if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
                const u32 at = m_elided_read_insn;
                const u32 pub = m_elided_read_owner - 1;
                std::fprintf(stderr,
                    "[jit] block 0x%llx reads an elided flag record (%s at "
                    "insn %u %s, publisher insn %u %s); recompiling without "
                    "crossing SETcc/CMOVcc\n",
                    static_cast<unsigned long long>(block.start_addr),
                    m_elided_read_kind ? "truncation" : "record read", at,
                    at < block.instruction_count
                        ? opcode_name(block.instructions[at].opcode) : "?",
                    pub,
                    pub < block.instruction_count
                        ? opcode_name(block.instructions[pub].opcode) : "?");
            }
            m_retry_no_omit = true;
            return nullptr;
        }
        // The analysis that never crosses a reader still elided a record
        // this code reads: a liveness/lowering disagreement to fix. This
        // code would read a stale record, so it is discarded too, and the
        // block is compiled once more publishing every record.
        if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
            const u32 at = m_elided_read_insn;
            const u32 pub = m_elided_read_owner - 1;
            std::fprintf(stderr,
                "[jit] WARNING: block 0x%llx reads an elided flag record "
                "with reader crossing off (%s at insn %u %s, publisher insn "
                "%u %s); recompiling with no record elided\n",
                static_cast<unsigned long long>(block.start_addr),
                m_elided_read_kind ? "truncation" : "record read", at,
                at < block.instruction_count
                    ? opcode_name(block.instructions[at].opcode) : "?",
                pub,
                pub < block.instruction_count
                    ? opcode_name(block.instructions[pub].opcode) : "?");
        }
        m_retry_no_elide = true;
        return nullptr;
    }

    if (m_code_buf.empty()) {
        return nullptr;
    }

    // ---- VITREO_JIT_STATS / VITREO_JIT_DISASM ----
    // Placed after the cold-stub flush so total_arm covers the whole block.
    // The words are captured before finalize_code() (which may relocate them
    // for split placement) but RECORDED only once it succeeds: a block it
    // declines -- a branch that cannot reach, an arena refusal -- never ran
    // and must not count. Purely observational: nothing here emits, patches
    // or reorders code.
    std::vector<u32> stats_words;
    if (stats::enabled()) stats_words = m_code_buf;
    attr_credit();
    const usize attr_words = m_code_buf.size();
    const u32 attr_hot_end = m_hot_end_pos;
    // ----------------------------------------------

    bool ts_capture = false;
    if (ts_candidate) {
        if (m_has_predecoded_delegate) {
            tstats::note(tstats::Event::SkipDelegate);
        } else if (m_ts_unrelocatable) {
            tstats::note(tstats::Event::SkipUnrelocatable);
        } else if (!m_pins_live) {
            tstats::note(tstats::Event::SkipPinless);
        } else {
            ts_capture = true;
        }
        if (ts_capture || ts_verify) capture_emission(block, m_ts_fresh);
        if (ts_verify) {
            TranslationStore::note_verification(
                ts_key, ts_capture && stored_emission_equal(m_ts_fresh, m_ts_record));
        }
    }

    CompiledBlock* compiled = finalize_code(
        block, have_compiled_key ? &compiled_key : nullptr);
    if (compiled != nullptr && ts_candidate) {
        // A record that was found but not adoptable here stays; storing the
        // same block again would only grow the store.
        if (ts_capture && !ts_verify && !ts_found) {
            TranslationStore::offer(ts_key, ts_bytes, m_ts_fresh);
        }
        TranslationStore::mark_seen(block.start_addr);
        m_ts_outcome = ts_verify ? TranslationOutcome::StoreVerified : TranslationOutcome::Fresh;
    } else if (compiled != nullptr) {
        m_ts_outcome = TranslationOutcome::Fresh;
    }
    if (compiled != nullptr) note_tier1_compile();
    if (compiled != nullptr && stats::enabled()) {
        stats::record_block(static_cast<u64>(block.start_addr),
                            block.byte_length, block.is_64bit,
                            static_cast<u32>(stats_words.size()),
                            m_stats_insns.data(),
                            static_cast<u32>(m_stats_insns.size()));
        if (stats::disasm_enabled()) {
            // host_ptr() may fail for a block spanning a mapping edge; the
            // dump then simply omits the x86 bytes.
            const Byte* guest_bytes =
                m_memory.fetch_ptr(block.start_addr, block.byte_length);
            stats::maybe_dump_block(
                static_cast<u64>(block.start_addr), block.byte_length,
                block.is_64bit, stats_words.data(),
                static_cast<u32>(stats_words.size()), m_stats_insns.data(),
                static_cast<u32>(m_stats_insns.size()),
                reinterpret_cast<const u8*>(guest_bytes));
        }
    }
    if (compiled != nullptr && m_attr_on) {
        m_attr_tags.resize(attr_words, EmitAttribution::Other);
        m_hot_end_pos = attr_hot_end;
        attr_commit();
    }
    if (compiled != nullptr) {
        compiled->native_insns = m_block_native_insns;
        compiled->terminal_opcode = m_block_terminal_opcode;
        compiled->hot_dump_variant = hot_dump_variant;
        if (m_has_predecoded_delegate) {
            compiled->decoded_owner =
                std::make_shared<BasicBlock>(std::move(stable_block));
        }
        compile_timer.success = true;
        compile_timer.native_bytes = compiled->code_size;
        // Compile-record bookkeeping only: report the instructions actually
        // lowered into this buffer, not the decoded count. See
        // `lowered_insn_count` above.
        compile_timer.guest_insns = lowered_insn_count;
    }
    return compiled;
}

} // namespace vitreo::cpu::jit
