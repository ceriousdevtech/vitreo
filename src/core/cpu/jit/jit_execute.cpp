// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_execute.cpp
/// @brief Dispatcher entry: JitCompiler::execute
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

// Per-thread tracking of the current JIT W^X state.  On Apple Silicon
// pthread_jit_write_protect_np() is a real syscall (manipulates per-thread
// APRR/MMU state) and is hot in profiles when called millions of times for
// short JIT blocks.  Skip redundant toggles when the requested state matches.
//
// New threads start with MAP_JIT pages executable (write-protected), so the
// initial cached state must be `false` (executable).  If we cached `true`
// here, the first finalize_code() call on a worker thread would skip the
// transition to writable and SIGBUS on memcpy into the arena.
constinit thread_local __attribute__((tls_model("initial-exec")))
    bool t_jit_writable = false;  // Default: executable (the macOS
                                  // per-thread default for MAP_JIT).

namespace {
u64 current_native_sp_floor() noexcept {
    pthread_t thread = pthread_self();
    const auto stack_high = reinterpret_cast<uintptr_t>(
        pthread_get_stackaddr_np(thread));
    const usize stack_size = pthread_get_stacksize_np(thread);
    if (stack_high == 0 || stack_size > stack_high) return kNativeCallNoFrames;

    // An upper bound on the chain's dispatcher frame: execute() pushes its
    // saved registers and the LR frame below this marker before entering.
    const u8 stack_marker = 0;
    const auto current_sp = reinterpret_cast<uintptr_t>(&stack_marker);
    return native_sp_floor_for_stack(
        current_sp, stack_high - stack_size, stack_high);
}

thread_local __attribute__((tls_model("initial-exec")))
    JitCompiler* t_gui_jit = nullptr;
void trampoline_gui_pump() {
    if (t_gui_jit != nullptr) t_gui_jit->poll_gui_pump();
}
}  // namespace

namespace {
/// Rebuild the lazy-flag record that a block elided under a recovery plan,
/// after a SOFTWARE raise out of it (see JitContext::raise_block_pc). The
/// descriptor names host registers; the raise exit's store_pins has written
/// each pinned guest register back, so the operand is read from the guest
/// register that host register pins. A descriptor naming an unpinned host
/// register cannot be published, so failing to find one leaves the state as
/// it is rather than guessing.
void rebuild_flags_after_software_raise(uintptr_t block_pc, CpuState& cpu) {
    JitFaultMetadata md{};
    if (!jit_flag_recovery_for_guest_rip(block_pc, cpu.rip, md) ||
        !md.has_guest_rip) {
        return;
    }
    // An instruction that moved RSP before the access that raised (see
    // JitFlagRecovery::rsp_undo): store_pins published the moved RSP.
    if (md.flags.rsp_undo != 0) {
        if (cpu.is_64bit()) {
            cpu.gpr.set64(4, cpu.gpr.get64(4) +
                                 static_cast<u64>(static_cast<i64>(md.flags.rsp_undo)));
        } else {
            cpu.gpr.set32(4, cpu.gpr.get32(4) +
                                 static_cast<u32>(static_cast<i32>(md.flags.rsp_undo)));
        }
    }
    if (!md.flags.active()) return;
    const auto guest_value = [&](u8 host, u64& value) {
        if (host == JitFlagRecovery::kImmediate) { value = 0; return true; }
        if (JitFlagRecovery::is_cpu_state_gpr(host)) {
            value = cpu.gpr.get64(host - JitFlagRecovery::kCpuStateGpr);
            return true;
        }
        for (u8 guest = 0; guest < 16; ++guest) {
            if (md.pins.host_reg[guest] == host) {
                value = cpu.gpr.get64(guest);
                return true;
            }
        }
        return false;
    };
    u64 src = 0, dst = 0;
    if (!guest_value(md.flags.src_reg, src) ||
        !guest_value(md.flags.dst_reg, dst)) {
        return;
    }
    (void)jit_rebuild_flag_record(md.flags, src, dst, cpu);
}
}  // namespace

void JitCompiler::maybe_pump_gui() {
    if (!m_gui_pump) {
        return;
    }
    // Only the main guest UI thread owns AppKit / the Win32 message pump.
    // Worker-thread JIT must not call into Cocoa.
    if (pthread_main_np() == 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (m_last_gui_pump.time_since_epoch().count() != 0 &&
        now - m_last_gui_pump < std::chrono::milliseconds(16)) {
        return;
    }
    m_last_gui_pump = now;
    m_gui_pump();
}

void JitCompiler::maybe_pump_gui_after_execute() {
    // execute() is a dispatcher hot path. Calling steady_clock::now() before
    // and after every native block used a material fraction of one core in
    // workloads with short linked chains. A small counter preserves the
    // time-based 16 ms contract while amortizing the host clock query.
    constexpr u32 POLL_INTERVAL = 16;
    if (++m_execute_gui_poll_count < POLL_INTERVAL) {
        return;
    }
    m_execute_gui_poll_count = 0;
    if (CpuState* cpu = m_jit_ctx.cpu; cpu && pthread_main_np() != 0) {
        diagnostics::phase_note_guest_exec(
            cpu->rip,
            cpu->gpr.get32(static_cast<u8>(RegIndex::RAX)),
            cpu->gpr.get32(static_cast<u8>(RegIndex::RCX)));
    }
    diagnostics::phase_maybe_heartbeat();
    maybe_pump_gui();
}

// Called at entry to the scalar memory helpers, before they resolve the
// access. Emitted code trusts a slot only while protect_sequence_seen equals
// the live sequence, and only execute() used to advance it: after any
// map/commit/protect anywhere in the process, every scattered access of a
// linked chain missed its slot and came here until the chain next returned
// to the dispatcher -- for an x64 guest that is every access to a DLL loaded
// above the linear window. Draining here first, then resolving, keeps the
// seen sequence at or behind what the published slots reflect.
void jit_revalidate_memory_caches(JitContext* ctx) noexcept {
    // The running block belongs to this thread's compiler, which execute()
    // publishes in t_gui_jit for exactly as long as it runs.
    JitCompiler* const jit = t_gui_jit;
    if (jit == nullptr || ctx == nullptr || &jit->m_jit_ctx != ctx) return;
    if (jit->m_cfg.no_jitcache || ctx->cpu == nullptr) return;
    // 32-bit guest-base execution uses no slots and never drains (execute()).
    if (jit->m_memory.is_linear() && !ctx->cpu->is_64bit() &&
        jit->m_cfg.linear_jit) {
        return;
    }
    jit->revalidate_memory_caches();
}

// ============================================================================
// Execution
// ============================================================================

JitExecResult JitCompiler::execute(CompiledBlock& block, CpuState& cpu) {
    // A dispatch chain owns one epoch for all of its blocks. Standalone users
    // (including tests and nested entry paths) acquire one here so reclamation
    // can never unmap the native pointer between validation and entry.
    JitCodeArena::ArenaReadLease execute_lease{JitCodeArena::ArenaReadLease::DeferAcquire{}};
    // JitCompiler is thread-local, so the first execute() is on the thread
    // that owns it (the LOCK reader slot below rests on the same fact).
    // Resolving the lease state once removes two thread_local lookups --
    // two `_tlv_get_addr` calls on Darwin -- from every dispatched block.
    if (m_arena_tls == nullptr) [[unlikely]] {
        m_arena_tls = JitCodeArena::ArenaReadLease::thread_state();
        m_tls_gui_jit = &t_gui_jit;
        m_tls_last_block_pc = &last_jit_block_pc();
        m_tls_jit_writable = &t_jit_writable;
        m_tls_crash = &jit_crash_breadcrumbs;
    }
    void* const arena_tls = m_arena_tls;
    if (!JitCodeArena::ArenaReadLease::thread_holds_at(arena_tls) &&
        !execute_lease.acquire()) {
        cpu.jit_iters = 0;
        return JitExecResult::StaleCode;
    }
    // A chain lease can live for a long time; move its published epoch
    // forward here, where every pointer this thread will enter through is
    // re-validated below, so a retirement that happened during the chain
    // stops waiting on this thread. Not nested under an outer entry's chain
    // (native_sp_base is that entry's while it is suspended in a helper): its
    // native CALL frames hold return addresses into arena code this entry
    // never sees.
    if (!m_cfg.native_callret_enabled || m_jit_ctx.native_sp_base == 0) {
        JitCodeArena::ArenaReadLease::refresh_epoch_at(arena_tls);
    }
    // The retirement sequence and the pointer's chunk generation are read
    // here, under the lease (and after the epoch publication above), because
    // only that makes them decisive: a retirement can otherwise land between
    // a dispatcher's synchronize_arena_generation() and its entry into native
    // code, leaving this thread's L1 holding pointers into a retired chunk.
    // The JIT-emitted link tail tests only compiled_link_code for null, never
    // a generation, so it would branch straight into that chunk — and once
    // recycled the address holds other code, which makes it a wrong-execution
    // bug rather than a fault. Purging before the first entry closes that
    // window for every dispatch site at the cost of two loads and compares.
    const u64 retire_seq = JitCodeArena::s_arena_retire_seq.load(std::memory_order_acquire);
    if (retire_seq != m_arena_retire_seq) {
        (void)synchronize_arena_generation();
        cpu.jit_iters = 0;
        return JitExecResult::StaleCode;
    }
    if (!JitCodeArena::arena_code_live(block.code, block.arena_generation)) {
        cpu.jit_iters = 0;
        return JitExecResult::StaleCode;
    }
    // Recent-hit sample for chunk victim selection: one shared RMW per 64
    // dispatcher entries, never per hop.
    if ((++m_exec_sample & 63u) == 0) {
        JitCodeArena::note_chunk_hit(block.code, 64);
    }
    // Warm codecs (ijl15) can run for a long time with no compile() and no
    // PeekMessage. The linked-hop cap returns here regularly; the post-execute
    // poll below drains AppKit without querying the host clock for every block.

    // Wire current CpuState into JitContext so slow-path helpers can
    // raise pending exceptions (e.g. STATUS_ACCESS_VIOLATION) on the
    // active CPU when guest code dereferences an unmapped address.
    // cpu / L1 are stable across a dispatch chain; rewriting them every
    // block dirties the line the linked tail loads. Restore L1 when SMC
    // nulls it. The hop budget needs no reset here: it counts in W22, which
    // the prologue sets to 1 on every entry.
    if (m_jit_ctx.cpu != &cpu) {
        m_jit_ctx.cpu = &cpu;
    }
    // The compiler may be executed on a different host thread from the one
    // that constructed it. Never bake a TLS address into shared native code.
    static const bool sse_environment_guard = environment_flag_enabled(
        "VITREO_JIT_SSE_ENV_GUARD", true);
    m_jit_ctx.installed_sse_environment = sse_environment_guard
        ? math::installed_sse_environment_key() : nullptr;
    void** const l1 = m_block_cache
        ? reinterpret_cast<void**>(m_block_cache->l1_base())
        : nullptr;
    if (m_jit_ctx.block_cache_l1 != l1) {
        m_jit_ctx.block_cache_l1 = l1;
    }
    // Predictor host pointers and linked entries are valid only for the pin
    // ABI of the chain that created them. A dispatcher transition is the
    // canonical place to discard predictions before a different layout runs.
    if (m_jit_ctx.active_pin_layout != block.pin_layout) {
        invalidate_return_predictors();
        m_jit_ctx.active_pin_layout = block.pin_layout;
    }
    // Publish link generation and drop stale RSB host pointers after SMC /
    // arena reclaim (bump_link_generation). Prefer C++ clear over emitted
    // per-RET generation checks.
    // A guest code write no longer moves the link generation (it unlinks
    // only the edges into the rewritten bytes), but every thread's return
    // predictors still go: they follow s_predictor_epoch, which both advance.
    {
        const u32 gen = s_link_generation.load(std::memory_order_relaxed);
        m_jit_ctx.link_generation = gen;
        const u32 epoch = s_predictor_epoch.load(std::memory_order_relaxed);
        if (m_jit_ctx.rsb_link_generation != epoch) {
            invalidate_return_predictors();
            m_jit_ctx.rsb_link_generation = epoch;
        }
    }

    // Leave the pump installed across sequential execute() calls on this
    // thread. Re-writing the thread_local on every block was _tlv_get_addr
    // in the sample. Only swap when a nested compiler takes over.
    JitCompiler* const previous_gui_jit = *m_tls_gui_jit;
    if (previous_gui_jit != this) {
        *m_tls_gui_jit = this;
        jit_install_gui_pump(&trampoline_gui_pump);
    }
    // The native LOCK fast path's reader slot is this thread's. JitCompiler is
    // thread-local, so the first execute() is on the thread that owns it.
    if (m_jit_ctx.lock_reader_active == nullptr) [[unlikely]] {
        m_jit_ctx.lock_reader_active =
            &lock_prefix_slot(g_lock_prefix_tls).active;
        m_jit_ctx.lock_prefix_writers = &lock_prefix_writers();
    }

    // Denominator for the delegate telemetry: instructions this block runs
    // natively. Counted per block entry, not per instruction, so the cost is
    // one relaxed atomic add per dispatch and only when telemetry is enabled.
    // Natively-linked hops to other blocks are not counted here — see
    // jit_coverage.hpp for what the resulting figure does and does not cover.
    if (coverage::enabled()) {
        coverage::record_block_exec(block.native_insns, cpu.is_64bit());
    }

    // Execution weight for the emitted-ARM report. Same dispatcher-only blind
    // spot as the line above: natively-linked hops never come back here, so
    // these counts are a lower bound (jit_stats.hpp documents it).
    if (stats::enabled()) {
        stats::record_block_exec(static_cast<u64>(block.guest_addr));
    }

    // Attribute watched writes to the block that performed them. Only the
    // interpreter publishes a per-instruction RIP, so without this a write
    // done by JIT code is reported against whatever RIP was interpreted last
    // — usually a thunk, thousands of instructions away. Block granularity is
    // enough to name the guest function.
    if (mem_watch_enabled()) mem_watch_cur_rip() = block.guest_addr;

    // Same attribution problem, but for faults rather than writes: when compiled
    // code branches to an unmapped address the interpreter reports the fault, and
    // it needs to name the block that branched. Unconditional because a fault can
    // happen on any dispatch and one store per block is noise next to the call.
    *m_tls_last_block_pc = block.guest_addr;

    // 32-bit linear accesses never use region slots. Long mode also uses
    // validated slots for scattered/mixed-page accesses outside its direct
    // fast path, so it must drain the same invalidations as the region JIT.
    const bool executing_linear =
        m_memory.is_linear() &&
        !cpu.is_64bit() && m_cfg.linear_jit;
    if (executing_linear) {
        // nothing to seed; linear_base + code_span are published below.
    } else
    // Debug: force all memory accesses through slow path
    if (m_cfg.no_jitcache) {
        m_jit_ctx.region_base = 0;
        m_jit_ctx.region_end = 0;
        m_jit_ctx.region_host_base = nullptr;
        m_jit_ctx.region2_base = 0;
        m_jit_ctx.region2_end = 0;
        m_jit_ctx.region2_host_base = nullptr;
        m_jit_ctx.wregion_base = 0;
        m_jit_ctx.wregion_end = 0;
        m_jit_ctx.wregion_host_base = nullptr;
        m_jit_ctx.wregion2_base = 0;
        m_jit_ctx.wregion2_end = 0;
        m_jit_ctx.wregion2_host_base = nullptr;
        m_jit_ctx.x64_read16_region_base = 0;
        m_jit_ctx.x64_read16_region_end = 0;
        m_jit_ctx.x64_read16_region_host_base = nullptr;
        m_jit_ctx.x64_read16_region2_base = 0;
        m_jit_ctx.x64_read16_region2_end = 0;
        m_jit_ctx.x64_read16_region2_host_base = nullptr;
        m_x64_rslots.fill(X64MemSlot{});
        m_x64_wslots.fill(X64MemSlot{});
    } else {
    revalidate_memory_caches();

    // Long-mode blocks use the full-width x64 read/write slot tables. The
    // legacy region1/region2/wregion caches have 32-bit bounds and cannot
    // represent normal PE32+ code, data, or stack addresses. Seeding them on
    // every x64 dispatcher entry therefore misses repeatedly and convoys on
    // GuestMemory's region-map lock without serving any emitted x64 access.
    if (!cpu.is_64bit()) {
    // Update region1 to track the region containing the current block (code/data).
    auto block_addr = static_cast<GuestAddr>(block.guest_addr);
    if (block_addr < m_jit_ctx.region_base || block_addr >= m_jit_ctx.region_end) {
        // Use the cached (TLB-backed) lookup — under multi-threaded
        // workloads, the uncached path's shared_mutex becomes the dominant
        // bottleneck (4+ JIT threads each ping-pong on lock_shared).
        auto code_region = m_memory.get_region_info_cached(block_addr);
        m_jit_ctx.region_base = code_region.base;
        m_jit_ctx.region_end = code_region.end;
        m_jit_ctx.region_host_base = code_region.host_ptr;
    }

    // Proactively seed region2 from the stack pointer so that the very
    // first stack access in a block hits the inline fast path instead of
    // falling through to the slow helper + region refresh.
    auto sp = static_cast<GuestAddr>(cpu.sp());
    if (sp < m_jit_ctx.region2_base || sp >= m_jit_ctx.region2_end) {
        // Validate the cached stack region against the memory generation;
        // a map/unmap/protect bump invalidates it.
        u64 mem_gen = m_memory.generation();
        if (m_jit_ctx.stack_region_gen != mem_gen) {
            m_jit_ctx.stack_region_host_base = nullptr;
            m_jit_ctx.stack_region_gen = mem_gen;
        }
        if (m_jit_ctx.stack_region_host_base &&
            sp >= m_jit_ctx.stack_region_base &&
            sp < m_jit_ctx.stack_region_end) {
            // Cache hit — promote the cached stack region into region2
            // without paying for another TLB / region lookup.
            m_jit_ctx.region2_base = m_jit_ctx.stack_region_base;
            m_jit_ctx.region2_end = m_jit_ctx.stack_region_end;
            m_jit_ctx.region2_host_base = m_jit_ctx.stack_region_host_base;
        } else {
            auto stack_region = m_memory.get_region_info_cached(sp);
            if (stack_region.host_ptr) {
                m_jit_ctx.region2_base = stack_region.base;
                m_jit_ctx.region2_end = stack_region.end;
                m_jit_ctx.region2_host_base = stack_region.host_ptr;
                m_jit_ctx.stack_region_base = stack_region.base;
                m_jit_ctx.stack_region_end = stack_region.end;
                m_jit_ctx.stack_region_host_base = stack_region.host_ptr;
            }
        }
    }

    // Seed the first inline-store region from the stack. The stack is the one
    // region a block can be sure it will store into, and it is writable and
    // code-free in every normal case — so this makes the very first push /
    // spill in a block hit the inline path instead of paying a helper call to
    // discover the same thing. Anything that fails the checks (a stack the
    // guest made read-only, a stack page holding compiled code) is left
    // unpublished and falls back to the helper.
    if (sp < m_jit_ctx.wregion_base || sp >= m_jit_ctx.wregion_end) {
        auto info = m_memory.probe_access(sp, 1);
        if (info.region_host_base && info.writable &&
            (!m_block_cache ||
             m_block_cache->range_is_code_free(
                 info.region_base, info.region_end - info.region_base))) {
            m_jit_ctx.wregion_base = info.region_base;
            m_jit_ctx.wregion_end = info.region_end;
            m_jit_ctx.wregion_host_base = info.region_host_base;
        }
    }
    }
    } // !m_cfg.no_jitcache

    sync_process_wide_stubs();

    // Every native chain gets independent temporary spill backing. A helper
    // can enter execute() recursively; the outer pointer and contents survive
    // that invocation. Guest faults return through the existing native frame
    // ABI and then destroy this scope normally.
    ir::SpillFrameScope spill_frame(m_jit_ctx.ir_spill_frame);

    using JitFunc = u32(*)();
    auto func = reinterpret_cast<JitFunc>(block.code);

    // Ensure JIT pages are in execute mode (no-op if already executable)
    jit_set_writable_at(m_tls_jit_writable, false);

    // Diagnostics belong to the executing thread. Shared volatile stores here
    // raced across workers and bounced one cache line on every block entry.
    auto& crash = *m_tls_crash;
    crash.last_block_addr = block.guest_addr;
    crash.guest_esp = static_cast<uint32_t>(cpu.sp());
    // Linear JIT never seeds region1/region2; the crash-dump crumbs would
    // just rewrite stale zeros on every dispatcher entry.
    if (!executing_linear) {
        crash.region1_base = m_jit_ctx.region_base;
        crash.region1_end = m_jit_ctx.region_end;
        crash.region2_base = m_jit_ctx.region2_base;
        crash.region2_end = m_jit_ctx.region2_end;
        crash.region1_host = reinterpret_cast<uint64_t>(m_jit_ctx.region_host_base);
        crash.region2_host = reinterpret_cast<uint64_t>(m_jit_ctx.region2_host_base);
    }

    // Persist stable JitContext publishes across sequential execute()
    // calls. write_watch_pages is the process-wide slot address (not the
    // map); the constructor publishes it once and the 32-bit
    // emit_mark_write_watch double-loads it. The x64 guest-base path no longer
    // reads it at all — a watched host page states itself through
    // kLinearCheckWatched in the check byte. code_page_map / chunk tables are
    // GuestMemory-owned and never relocate. The compiled-code span can grow —
    // republish only when the acquire-load differs so SMC coverage stays
    // current.
    if (u8* linear = m_memory.linear_base(); m_jit_ctx.linear_base != linear) {
        m_jit_ctx.linear_base = linear;
    }
    const u32 span_lo = m_memory.jit_code_span_low();
    const u32 span_hi = m_memory.jit_code_span_high();
    if (m_jit_ctx.code_span_lo != span_lo) {
        m_jit_ctx.code_span_lo = span_lo;
    }
    if (m_jit_ctx.code_span_hi != span_hi) {
        m_jit_ctx.code_span_hi = span_hi;
    }
    const auto* span64_lo = m_memory.jit_code_span64_low_atomic();
    const auto* span64_hi = m_memory.jit_code_span64_high_atomic();
    if (m_jit_ctx.code_span64_lo != span64_lo) {
        m_jit_ctx.code_span64_lo = span64_lo;
    }
    if (m_jit_ctx.code_span64_hi != span64_hi) {
        m_jit_ctx.code_span64_hi = span64_hi;
    }
    if (m_jit_ctx.code_page_map == nullptr) {
        m_jit_ctx.code_page_map = m_memory.jit_code_page_map();
        m_jit_ctx.code_chunk_pages = m_memory.jit_code_chunk_pages();
    }

    u32 result_code;

    // DIAGNOSTIC (perf window only): clear the exit-reason slot so a return
    // through a path that stores no reason (any emit_epilogue(), a raised
    // guest exception, a thunk return) is reported as Unattributed instead of
    // inheriting the previous entry's label. One relaxed load + one store
    // when the window is on; nothing at all when it is off. The same local is
    // reused after the BLR so the clear and the read can never straddle a
    // window start and mis-attribute.
    // The linked-tail predictor counters are cleared here too, not after the
    // drain below: generated code keeps bumping them whenever
    // jit_exit_stats_enabled() is on, including before a phase-triggered
    // window ever opens, and folding that pre-window backlog into the first
    // in-window sample would silently inflate the totals.
    const bool perf_exit_stats = diagnostics::perf_window_active_fast();
    if (perf_exit_stats) {
        m_jit_ctx.exit_reason = static_cast<u32>(JitExitReason::Unattributed);
        m_jit_ctx.ret_sticky0_hits = 0;
        m_jit_ctx.ret_sticky1_hits = 0;
        m_jit_ctx.ret_rsb_hits = 0;
        m_jit_ctx.ret_l1_hits = 0;
        m_jit_ctx.ind_l1_hits = 0;
    }

    const bool protection_lease = block.requires_protection_lease;
    if (protection_lease &&
        !m_memory.try_enter_linear_proof_lease()) {
        cpu.jit_iters = 0;
        return JitExecResult::Unsupported;
    }

    if (hot_dump::enabled()) {
        hot_dump::record_exec(block.guest_addr, block.hot_dump_variant,
                              block.native_insns);
    }

    // Set pinned registers: X19 = CpuState*, X20 = JitContext*
    // NOTE: The ldp must come BEFORE the mov to the output operand,
    // because the compiler may assign the output to w19/w20.  If the
    // mov came first, the ldp would clobber the result.
    // Restore NZCV saved on a linked-tail miss. Must happen after X19 is
    // CpuState* and immediately before BLR so the compiler's "cc" clobber
    // cannot kill the flags the Jcc-first path is about to read.
    // Native guest-CALL frames are pushed below this entry's dispatcher LR
    // frame and discarded by restoring SP to native_sp_base, so each entry
    // publishes its own base and floor and a nested entry (a guest callback
    // under a helper of the outer chain) gives the outer ones back.
    const u64 outer_native_sp_base = m_jit_ctx.native_sp_base;
    const u64 outer_native_sp_floor = m_jit_ctx.native_sp_floor;
    m_jit_ctx.native_sp_floor = m_cfg.native_callret_enabled
                                    ? current_native_sp_floor()
                                    : kNativeCallNoFrames;
    // The {0, 0} pair pushed right before the BLR sits directly above the
    // block prologue's 16-byte LR frame: it is the frame a native RET at the
    // bottom of the chain inspects, and its zero tag never matches.
    __asm__ volatile(
        "stp x19, x20, [sp, #-16]!\n\t"
        "stp x23, x24, [sp, #-16]!\n\t"
        "stp x25, x26, [sp, #-16]!\n\t"
        "stp x27, x28, [sp, #-16]!\n\t"
        "stp xzr, xzr, [sp, #-16]!\n\t"
        "mov x19, %1\n\t"
        "mov x20, %2\n\t"
        "sub x3, sp, #16\n\t"
        "str x3, [x20, %[sboff]]\n\t"
        "ldrb w3, [x19, %[koff]]\n\t"
        "cbz w3, 1f\n\t"
        "ldr w3, [x19, %[boff]]\n\t"
        "msr nzcv, x3\n\t"
        "1:\n\t"
        // W22 = 1: the chain's loop-iteration and counted-hop count starts
        // here for every dispatcher entry (kMaxLinkedHops).
        "mov w22, #1\n\t"
        "blr %3\n\t"
        "add sp, sp, #16\n\t"
        "ldp x27, x28, [sp], #16\n\t"
        "ldp x25, x26, [sp], #16\n\t"
        "ldp x23, x24, [sp], #16\n\t"
        "ldp x19, x20, [sp], #16\n\t"
        "mov %w0, w0\n\t"
        : "=r"(result_code)
        : "r"(&cpu), "r"(&m_jit_ctx), "r"(func),
          [koff] "i"(static_cast<u32>(offsetof(CpuState, nzcv_kind))),
          [boff] "i"(static_cast<u32>(offsetof(CpuState, nzcv_bits))),
          [sboff] "i"(JIT_CTX_OFF_NATIVE_SP_BASE)
        : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
          "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
          "x16", "x17", "x21", "x22", "x30", "memory", "cc",
          // Translated code uses the vector file freely -- V0-V9 as lowering
          // scratch, V31 for lazy-flag publication, V16-V30 for the XMM
          // register cache -- and saves none of it, including the AAPCS64
          // callee-saved low halves of V8-V15. Without these the compiler
          // may keep a value in a V register across this call.
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
          "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15",
          "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
          "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31"
    );

    m_jit_ctx.native_sp_base = outer_native_sp_base;
    m_jit_ctx.native_sp_floor = outer_native_sp_floor;

    if (protection_lease) {
        m_memory.exit_linear_proof_lease();
    }

    // A software raise out of a block that elided flag records: rebuild the
    // record the faulting instruction's state must carry (Intel SDM Vol. 3A
    // 6.6) from that block's descriptor, now that store_pins has made the
    // guest registers authoritative.
    if (const u64 raise_pc = m_jit_ctx.raise_block_pc; raise_pc != 0) [[unlikely]] {
        m_jit_ctx.raise_block_pc = 0;
        if (cpu.pending_raise.active || m_jit_ctx.cold_side_exit) {
            rebuild_flags_after_software_raise(
                static_cast<uintptr_t>(raise_pc), cpu);
        }
    }
    // A lazy slow path that could not be materialized left the block at its
    // instruction, in the state a raise there would leave: run it here.
    if (m_jit_ctx.cold_side_exit) [[unlikely]] finish_cold_side_exit(cpu);

    // The [jit-av] register dump is deferred to here on purpose: a memory
    // helper runs with RAX/RCX/RDX/RBX/RSI/RDI still in their callee-saved
    // ARM pins (emit_memory_helper_blr spills only RSP/RBP), so CpuState is
    // authoritative for all sixteen GPRs only after the raise-exit stub's
    // store_pins has run -- which is now. Printing from inside the helper
    // would silently report six stale registers.
    if (cpu.pending_raise.active) jit_flush_pending_av_dump();

    // AppKit pumping may re-enter guest callbacks. Release this frame's lease
    // first. An outer dispatch-chain lease must also be dropped by its owner
    // before any re-entrant pump; skip the pump while that outer epoch is live.
    if (previous_gui_jit != nullptr && previous_gui_jit != this) {
        *m_tls_gui_jit = previous_gui_jit;
        jit_install_gui_pump(&trampoline_gui_pump);
    }
    if (!JitCodeArena::ArenaReadLease::thread_holds_at(arena_tls)) {
        maybe_pump_gui_after_execute();
    } else if (++m_execute_gui_poll_count >= 16) {
        // The dispatch chain holds the arena lease, so the GUI pump is
        // deferred. Heartbeats must still fire or a long linked walk looks
        // hung in the log (presents freeze while T1 is in native JIT).
        m_execute_gui_poll_count = 0;
        if (pthread_main_np() != 0) {
            diagnostics::phase_note_guest_exec(
                cpu.rip,
                cpu.gpr.get32(static_cast<u8>(RegIndex::RAX)),
                cpu.gpr.get32(static_cast<u8>(RegIndex::RCX)));
        }
        diagnostics::phase_maybe_heartbeat();
    }
    if (diagnostics::perf_window_active()) {
        // jit_iters is W22 as the chain left it: its counted hops and its
        // in-block loop iterations together. A hop-cap exit leaves bit 12 of
        // it set (kHopCapTestWord); other exits may too, so this is approximate.
        const u32 chain = cpu.jit_iters;
        diagnostics::perf_add_dispatcher_entry(chain);
        if ((chain & kMaxLinkedHops) != 0) {
            diagnostics::perf_note_hop_limit_fallback();
        }
        const u32 iters = cpu.jit_iters != 0 ? cpu.jit_iters : 1u;
        diagnostics::perf_add_jit_insns(
            static_cast<u64>(block.native_insns) * iters);

        // DIAGNOSTIC: attribute this dispatcher entry. Exactly one bucket is
        // bumped per perf_add_dispatcher_entry() above, so the histogram
        // always sums to the dispatcher-entry total. When the window opened
        // *during* this block's execution the slot was never cleared, so the
        // entry is charged to Unattributed rather than to a stale label.
        if (perf_exit_stats && m_jit_ctx.thunk_region_hi > m_jit_ctx.thunk_region_lo &&
            cpu.rip >= m_jit_ctx.thunk_region_lo &&
            cpu.rip < m_jit_ctx.thunk_region_hi) {
            // The probe cannot find a thunk in the L1 (thunks are not
            // blocks); say so instead of reporting a cache miss.
            if (m_jit_ctx.exit_reason ==
                static_cast<u32>(JitExitReason::IndTailL1AddrMismatch) ||
                m_jit_ctx.exit_reason ==
                static_cast<u32>(JitExitReason::IndTailL1Empty)) {
                m_jit_ctx.exit_reason =
                    static_cast<u32>(JitExitReason::IndTailThunkTarget);
            } else if (m_jit_ctx.exit_reason ==
                           static_cast<u32>(JitExitReason::ConstTailL1AddrMismatch) ||
                       m_jit_ctx.exit_reason ==
                           static_cast<u32>(JitExitReason::ConstTailL1Empty)) {
                m_jit_ctx.exit_reason =
                    static_cast<u32>(JitExitReason::ConstTailThunkTargetRuntime);
            }
        }
        diagnostics::perf_add_jit_exit(
            perf_exit_stats ? m_jit_ctx.exit_reason
                            : static_cast<u32>(JitExitReason::Unattributed),
            result_code, cpu.pending_raise.active);
        if (perf_exit_stats &&
            m_jit_ctx.exit_reason == static_cast<u32>(JitExitReason::Unattributed)) {
            static const bool s_namer_set = [] {
                diagnostics::perf_set_opcode_namer([](u32 op) {
                    return cpu::opcode_name(static_cast<cpu::Opcode>(op));
                });
                return true;
            }();
            (void)s_namer_set;
            diagnostics::perf_add_jit_exit_unattributed_terminal(
                block.terminal_opcode);
        }

        // Drain the per-thread linked-tail predictor counters. JitContext is
        // owned by this JitCompiler (one per guest thread), so these are
        // plain non-atomic u32s bumped by generated code and folded into the
        // process-wide relaxed atomics here, off the linked-hop fast path.
        // (Cleared before the BLR above, so no reset is needed here.)
        if (perf_exit_stats &&
            (m_jit_ctx.ret_sticky0_hits | m_jit_ctx.ret_sticky1_hits |
             m_jit_ctx.ret_rsb_hits | m_jit_ctx.ret_l1_hits |
             m_jit_ctx.ind_l1_hits) != 0) {
            diagnostics::perf_add_link_predictor(
                m_jit_ctx.ret_sticky0_hits, m_jit_ctx.ret_sticky1_hits,
                m_jit_ctx.ret_rsb_hits, m_jit_ctx.ret_l1_hits,
                m_jit_ctx.ind_l1_hits);
        }
    }
    // A stack-entry proof rejected the block at cpu.rip (possibly a linked
    // successor of `block`) before it ran an instruction.
    if (result_code == static_cast<u32>(JitExecResult::Unsupported)) {
        note_stack_proof_deopt(cpu.rip);
    }
    return static_cast<JitExecResult>(result_code);
}

void JitCompiler::free_block(CompiledBlock& block) {
    // Arena-allocated — individual blocks are not freed.
    // Memory is reclaimed when the slab allocator is destroyed.
    block.code = nullptr;
    block.code_size = 0;
}

} // namespace vitreo::cpu::jit
