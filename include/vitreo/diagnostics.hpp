#pragma once
/// @file diagnostics.hpp
/// @brief Runtime diagnostic hooks owned below every runtime layer.
///
/// Some diagnostics are produced by a low layer but can only be *rendered* by
/// a higher one. The hostcall ring is the standing example: the CPU and the
/// WinAPI thunks are what notice a spin, an access violation, or a guest exit
/// worth dumping, but the ring itself belongs to the process runtime that
/// records hostcalls.
///
/// Resolving that by calling the owner's symbol directly inverts the
/// dependency — and because everything historically linked into one binary,
/// the inversion was invisible to CMake. These hooks make the direction
/// explicit: low layers *request* a dump, the owning layer *installs* how one
/// is produced, and neither has to link the other.

#include <vitreo/common.hpp>

namespace vitreo::diagnostics {

/// Renders recent hostcall history to the diagnostic stream.
/// @param reason Short tag naming what prompted the dump, e.g. "jit-av".
using HostcallDumpFn = void (*)(const char* reason);

/// Install the implementation. Called by whichever component owns the hostcall
/// ring; passing nullptr restores the default no-op.
///
/// Expected to run during process startup, before any guest code executes.
/// Installing later is safe but loses dumps requested in the interim.
void set_hostcall_dump(HostcallDumpFn fn) noexcept;

/// Request a hostcall dump. Safe to call from any layer at any time: with no
/// implementation installed this does nothing, which is the correct behavior
/// for a unit test that links neither the ring nor the process runtime.
void dump_hostcalls(const char* reason) noexcept;

// -----------------------------------------------------------------------------
// Trace gates
// -----------------------------------------------------------------------------
//
// Expensive traces are usually only interesting after the run reaches some
// point, or only while the guest is somewhere specific. The layer that can
// observe that condition and the layer that owns the trace budget are not the
// same one, so the gate itself lives here, below both.
//
// These affect diagnostics only. Nothing guest-visible may depend on them.

/// Latch marking that the run reached the point after which tracing is worth
/// its cost. Set by whichever layer observes the milestone; read by the trace
/// budget owner. Never cleared during a run.
void set_trace_milestone_reached(bool reached) noexcept;
bool trace_milestone_reached() noexcept;

/// Depth of module-initialization code (DllMain, TLS callbacks) on the current
/// guest thread's stack, maintained by the module loader so a trace can be
/// restricted to — or excluded from — module init.
///
/// Per-thread, because the guest stack this describes is per-thread.
void enter_module_init() noexcept;
void leave_module_init() noexcept;
int module_init_depth() noexcept;

// -----------------------------------------------------------------------------
// Load-phase clock
// -----------------------------------------------------------------------------
//
// Cheap wall-clock marks for a guest load. Each phase prints once to stderr.
// Present is a counter: first, 10th, 60th, and 300th successful Present.
// Nothing here takes a lock or runs on the per-block execute path.

enum class LoadPhase : u8 {
    ProcessStart = 0,
    PeEntry,
    FirstWindow,
    FirstDevice,
    FirstDraw,
    FirstPresent,
    Present10,
    Present60,
    Present300,
    Present1000,
    Present5000,
};

/// Called once per phase, on the thread that reached it, right after the
/// `[load-phase]` line. One observer per process (the last one set).
using PhaseObserver = void (*)(LoadPhase phase) noexcept;
void phase_set_observer(PhaseObserver observer) noexcept;
[[nodiscard]] const char* phase_name(LoadPhase phase) noexcept;

/// Latch the origin and print `process_start`. Safe to call more than once:
/// later calls keep the first origin.
void phase_clock_start() noexcept;

/// Print `+N ms` for `phase` the first time it is reached. Later calls are a
/// single atomic bit test.
void phase_mark(LoadPhase phase) noexcept;

/// Count a successful Present. Marks FirstPresent / Present10 / Present60 /
/// Present300 / Present1000 / Present5000 on those counts.
void phase_note_present() noexcept;

/// Accumulate Present host time. Always on — the load-phase heartbeat
/// prints it so the official interval can be attributed without
/// `VITREO_PERF_WINDOW`.
void phase_add_present_wall(u64 ns) noexcept;

/// Host time inside swap-chain Present / Metal submit (after the D3D9
/// pass flush). Heartbeat prints `submit_ms` next to `present_ms`.
void phase_add_present_submit_wall(u64 ns) noexcept;

/// Host time inside Sleep / SleepEx. Heartbeat prints `sleep_ms`.
void phase_add_sleep_wall(u64 ns) noexcept;

/// Host time inside WaitForSingleObject*. Heartbeat prints `wfso_ms`.
void phase_add_wfso_wall(u64 ns) noexcept;

/// Count a submitted draw. Heartbeat prints this next to the Present count
/// so a vsync-off Present storm can be told from a draw+present loop.
void phase_note_draw() noexcept;

/// Count a `jit_sse_op` helper call. The loading-screen loop is execute-bound;
/// this tells a later heartbeat whether leftover SSE is still on the helper.
void phase_note_sse_helper() noexcept;

/// Count FindFirst/FindNext. Package scans show up here rather than in Present.
void phase_note_find() noexcept;

/// Record the guest RIP (and a couple of GPRs) at a JIT block entry so a
/// stalled-Present heartbeat can show where T1 is without a debugger.
void phase_note_guest_exec(u64 rip, u32 eax, u32 ecx) noexcept;

/// Print `heartbeat +N ms presents=M ... present_ms=X submit_ms=S
/// sleep_ms=A wfso_ms=B since_first=Y` at most once per 5 s. Cheap
/// enough for the GUI pump and the execute poll. No-op before
/// phase_clock_start. When presents have not moved since the previous
/// slot, also prints the last `phase_note_guest_exec` RIP.
void phase_maybe_heartbeat() noexcept;

/// Test-only: clear origin, marks, and Present count so contract tests can
/// observe a fresh clock. Not used by the guest runtime.
void phase_clock_reset_for_tests() noexcept;

// -----------------------------------------------------------------------------
// Performance window
// -----------------------------------------------------------------------------
//
// Opt-in counters that can be reset/started/stopped so a report covers one
// interval (for example, first Present through process exit) rather than the
// whole run. Disabled by default: every increment early-returns on one relaxed
// atomic load.
//
// VITREO_PERF_WINDOW selects the start milestone:
//   unset / 0          disabled
//   1 / process_start  start at process_start
//   pe_entry
//   first_window
//   first_device
//   first_draw
//   first_present
//
// The window is generic. It does not name titles or guest addresses. A report
// is printed at stop() and again at process exit (one-shot).

enum class PerfWaitKind : u8 {
    Sleep = 0,
    WaitForSingleObject,
    WaitForMultipleObjects,
    SignalObjectAndWait,
};

/// How a guest->host thunk crossing was served. Ranked by *time*, because the
/// slow-dispatch histogram ranks only crossings and the cost of a crossing is
/// the dispatch itself, not the exported function's body.
enum class PerfThunkKind : u8 {
    /// `__guard_dispatch_icall`: served in-chain as `RIP := RAX`, no host call.
    GuardIcall = 0,
    /// Served inline by `maybe_fast_thunk_call`, no Marshaler.
    Fast,
    /// Full `dispatch_slow_api_thunk`: handler resolve, Marshaler, marshaling.
    Slow,
    /// In the thunk region but not dispatched here (patched sled, unresolved);
    /// the outer dispatcher owns it. Separates the cost of the checks.
    NotServed,
    /// Served from the export's direct entry (runtime/execution/thunk_direct:
    /// the D3D11/DXGI COM methods): one plain call, no handler resolve.
    Direct,
};

enum class PerfFsKind : u8 {
    Create = 0,
    Read,
    Write,
};

struct PerfWindowSnapshot {
    bool enabled{false};
    bool active{false};
    u64 window_ns{0};

    u64 guest_insns{0};
    u64 jit_insns{0};
    u64 interp_insns{0};
    u64 fallback_insns{0};

    u64 compile_attempts{0};
    u64 compile_successes{0};
    u64 compile_failures{0};
    u64 compile_memo_hits{0};
    u64 compile_ns{0};
    u64 compile_ns_max{0};
    u64 native_bytes{0};

    u64 dispatcher_entries{0};
    u64 linked_hops{0};
    /// Linked-tail returns where hop count hit kMaxLinkedHops (4095).
    u64 hop_limit_fallbacks{0};

    /// Decode-on-miss / chain-lookup miss RIP histogram (window only).
    /// Unique is hash-bounded (65536 slots); collisions are counted separately.
    u64 block_miss_total{0};
    u64 block_miss_unique{0};
    u64 block_miss_repeat{0};
    u64 block_miss_collision{0};
    u64 block_miss_empty{0};
    u64 block_miss_empty_repeat{0};
    u64 block_miss_chain{0};
    /// Chain lookup miss where some cached block covers RIP mid-body
    /// (start < rip < start+len). Diagnostics only; not executed.
    u64 chain_miss_mid_block{0};
    /// Chain lookup miss whose RIP is in the Vitreo API thunk region
    /// (intentionally never JIT-compiled). Subset of `block_miss_chain`.
    u64 chain_miss_thunk{0};
    /// In-chain thunk handled without a lookup miss or outer break (fast+slow).
    u64 chain_thunk_inline{0};
    /// Subset: `maybe_fast_thunk_call` path.
    u64 chain_thunk_inline_fast{0};
    /// Subset: full `resolve_handler` path kept in-chain.
    u64 chain_thunk_inline_slow{0};
    /// Subset: direct-entry crossing (thunk_direct) kept in-chain.
    u64 chain_thunk_inline_direct{0};

    /// Compile-time Jcc terminal/loop-exit path mix (window only).
    /// Counts emit decisions, not runtime taken/not-taken.
    u64 jcc_emit_fused{0};
    u64 jcc_emit_carried{0};
    u64 jcc_emit_helper{0};

    /// Compile-time REP MOVS/STOS lowering mix (window only). "inline" means
    /// the unrolled short-count loop was emitted (with the bulk helper kept
    /// as its long-count tail); "helper" means the call was all that was
    /// emitted. Counts emit decisions, not executed iterations.
    u64 rep_emit_inline{0};
    u64 rep_emit_helper{0};
    u64 cf_emit_known{0};
    u64 cf_emit_nzcv{0};
    u64 cf_emit_dispatch{0};

    u64 helper_reads{0};
    u64 helper_writes{0};
    u64 mem_faults{0};
    u64 linear_mem_emits{0};
    u64 helper_mem_emits{0};

    u64 rep_movs_calls{0};
    u64 rep_movs_bytes{0};
    u64 rep_stos_calls{0};
    u64 rep_stos_bytes{0};
    u64 fused_memcpy_calls{0};
    u64 fused_memcpy_bytes{0};
    u64 fused_memset_calls{0};
    u64 fused_memset_bytes{0};

    u64 wait_ns{0};
    u64 sleep_ns{0};
    u64 fs_ns{0};
    u64 registry_calls{0};
    u64 registry_ns{0};
    u64 presents{0};
    u64 draws{0};
    u64 graphics_ns{0};

    u32 guest_threads{0};
    u32 guest_runnable{0};
    u32 guest_suspended{0};
    u32 host_workers{0};
    u32 guest_threads_max{0};
    u32 host_workers_max{0};

    bool guest_linear{false};
    /// 32-bit guest-base JIT lowering is active for the latest compiler.
    bool jit_linear{false};
    /// x64 guest-base scalar lowering is active for the latest compiler.
    bool jit_linear64{false};
    bool no_jitcache{false};
};

/// True when VITREO_PERF_WINDOW requested a window. Cached after first call.
[[nodiscard]] bool perf_window_enabled() noexcept;

/// True while the window is collecting. One relaxed load; safe on any thread.
[[nodiscard]] bool perf_window_active() noexcept;

void perf_window_reset() noexcept;
void perf_window_start() noexcept;
void perf_window_stop() noexcept;
void perf_window_dump(const char* reason) noexcept;

/// Called from load-phase marks so an env-selected milestone can start the
/// window. No-op when disabled or when the phase is not the configured start.
void perf_window_on_phase(LoadPhase phase) noexcept;

void perf_add_jit_insns(u64 n) noexcept;
void perf_add_interp_insns(u64 n) noexcept;
void perf_add_fallback_insns(u64 n) noexcept;
void perf_add_dispatcher_entry(u32 linked_hops) noexcept;
/// Count a dispatcher return caused by the linked-hop cap (kMaxLinkedHops).
void perf_note_hop_limit_fallback() noexcept;

/// Bucket count for the dispatcher exit-reason histogram. Must be at least
/// `cpu::jit::JitExitReason::Count`; asserted at the call site in
/// jit_compiler.cpp so that `core/base` keeps no dependency on `core/cpu`.
inline constexpr u32 kPerfJitExitReasonCount = 32;
/// Bucket count for the returned `cpu::jit::JitExecResult`. Asserted the
/// same way.
inline constexpr u32 kPerfJitExecResultCount = 8;

/// Attribute one dispatcher entry to the reason generated code recorded for
/// the exit path it took (`cpu::jit::JitExitReason`), the `JitExecResult` it
/// returned, and whether a guest exception was pending. Exactly one call per
/// `perf_add_dispatcher_entry()`, so the histogram sums to the entry total.
/// Out-of-range values are folded into bucket 0 (Unattributed) rather than
/// dropped, so the sum stays exact.
void perf_add_jit_exit(u32 reason, u32 result_code,
                       bool pending_raise) noexcept;
/// DIAGNOSTIC: an exit that stored no reason, keyed by the opcode that ended
/// the block, so "Unattributed" can be read as "blocks ending in X".
void perf_add_jit_exit_unattributed_terminal(u32 opcode) noexcept;
/// Names opcodes in the dump; registered by the JIT so this layer needs no
/// decoder dependency.
void perf_set_opcode_namer(const char* (*namer)(u32)) noexcept;
/// Called once when the window stops (after the stop line, before the
/// report). Registered by subsystems that want to dump their own state at
/// the same instant; one slot, set once.
void perf_set_stop_hook(void (*hook)()) noexcept;

/// Fold one dispatcher entry's linked-tail predictor hit counts into the
/// window. `sticky0`/`sticky1` are the two-deep RET sticky ring, `rsb` the
/// software return-stack buffer, `ret_l1` a RET served only by the L1 probe,
/// and `ind_l1` an indirect JMP/CALL served by L1. The corresponding MISSES
/// are the `RetTail*` / `IndTail*` exit-reason buckets.
void perf_add_link_predictor(u32 sticky0, u32 sticky1, u32 rsb, u32 ret_l1,
                             u32 ind_l1) noexcept;
void perf_add_helper_mem(bool is_write, bool fault) noexcept;
/// Record the guest PAGE a helper write targeted, for the perf window's
/// helper-write histogram. Separate from perf_add_helper_mem because it costs
/// a hash and a store rather than one increment, and only the write side has
/// a question worth asking: a store that reaches the helper has failed the
/// code-span / chunk / page guard, and whether that is a few hot pages or the
/// whole address space calls for opposite fixes.
void perf_note_helper_write_page(u64 addr) noexcept;

/// Record WHICH linear check-map reason bits were set on the page a helper
/// write landed on. The byte's reasons -- mixed protection, holds code,
/// write-watched -- are independent and call for different fixes, so the page
/// histogram alone cannot say what to do.
/// @param check_bits the store's own check-map byte
/// @param next_bits  the NEXT page's byte. The emitted halfword probe consults
///        both, so a store can reach the helper on its neighbour's account.
void perf_note_helper_write_reason(u8 check_bits, u8 next_bits) noexcept;

/// Which width helper a store reached (1/2/4/8), narrowing which emitter
/// produced it.
void perf_note_helper_write_width(int width) noexcept;

/// Read-side helper attribution. `perf_note_helper_read_cross` splits by
/// whether the access straddles a 16 KiB host page -- the x64 load guard
/// routes those to the helper as a "rare" case, and rare is exactly the
/// assumption that measured false on the store side.
void perf_note_helper_read(u64 addr, int width) noexcept;
void perf_note_helper_read_cross(bool cross) noexcept;
void perf_note_helper_read_reason(u8 check_bits) noexcept;
void perf_note_helper_read_site(u64 guest_rip) noexcept;

/// Resolve and remember the host symbol owning @p host_pc, for helper writes
/// whose caller turns out to be host C++ rather than generated code.
void perf_note_helper_write_symbol(u64 host_pc) noexcept;

/// True once every N helper writes: the sampling gate for site attribution.
/// Resolving a host PC to a guest RIP on every call would dominate the
/// workload being measured -- there are ~1.9 G of them on Sims 3 alone.
[[nodiscard]] bool perf_helper_write_site_sample_due() noexcept;

/// Record the guest instruction that issued a sampled helper write. Pass 0
/// when the host PC could not be mapped; that count is reported separately,
/// because "the store came from block-owned cold code" is itself an answer.
/// @param kind 0 = mapped to a guest instruction, 1 = inside a published JIT
///             block but block-owned cold/gap code, 2 = in no published block.
void perf_note_helper_write_site(u64 guest_rip, u64 host_pc, int kind) noexcept;
void perf_add_linear_mem_emit(bool linear) noexcept;
void perf_add_rep_movs(u64 bytes) noexcept;
void perf_add_rep_stos(u64 bytes) noexcept;
void perf_add_fused_memcpy(u64 bytes) noexcept;
void perf_add_fused_memset(u64 bytes) noexcept;
void perf_add_wait(PerfWaitKind kind, u64 ns) noexcept;
/// Inclusive wall time of one thunk crossing, measured from the thunk-region
/// test to the point the chain resumes, so the arena safepoint and the
/// dispatch checks are charged to the crossing that paid for them.
void perf_add_thunk(PerfThunkKind kind, u64 ns) noexcept;
void perf_add_fs(PerfFsKind kind, u64 ns, u64 bytes) noexcept;
void perf_add_registry(u64 ns) noexcept;
void perf_add_present() noexcept;
void perf_add_draw() noexcept;
void perf_add_graphics_ns(u64 ns) noexcept;
void perf_note_threads(u32 guest_active, u32 guest_runnable,
                       u32 guest_suspended, u32 host_workers) noexcept;

void perf_note_compile(bool success, bool memo_hit, u64 ns, u32 native_bytes,
                       u32 guest_insns, u32 guest_bytes, u64 guest_addr) noexcept;
void perf_note_memory_model(bool guest_linear, bool jit_linear,
                            bool jit_linear64, bool no_jitcache) noexcept;

/// Count a C++ block-cache miss that then decoded at `rip`.
/// `empty_decode` is true when the decode inserted nothing (hot RIP can miss
/// forever). No-op unless the performance window is collecting.
void perf_note_block_miss(u64 rip, bool empty_decode) noexcept;

/// Count an in-chain lookup miss that broke back to the dispatcher
/// without decoding. Same unique/repeat table as `perf_note_block_miss`.
/// Also updates the top-N chain-miss RIP histogram (exact counts for RIPs
/// that remain in the 64-slot table).
void perf_note_chain_lookup_miss(u64 rip) noexcept;

/// Count a chain lookup miss whose RIP sits inside a cached block body
/// (not at that block's start). Probe only — do not execute the covering block.
void perf_note_chain_miss_mid_block() noexcept;

/// Count a chain lookup miss whose RIP is in the API thunk region.
/// Call in addition to `perf_note_chain_lookup_miss` so top-N stays intact.
void perf_note_chain_miss_thunk() noexcept;

/// Count an in-chain thunk that resumed without a chain miss (total).
void perf_note_chain_thunk_inline() noexcept;
/// Fast-path (`maybe_fast_thunk_call`) in-chain inline; also bumps total.
void perf_note_chain_thunk_inline_fast() noexcept;
/// Slow-path (`resolve_handler`) in-chain inline; also bumps total.
void perf_note_chain_thunk_inline_slow() noexcept;
/// Direct-entry (thunk_direct) in-chain inline; also bumps total.
void perf_note_chain_thunk_inline_direct() noexcept;

/// Count a Jcc emit decision: fused B.cond, NZCV-kind carried B.cond, or
/// flag-helper eval. Compile-time only; no-op unless the window is collecting.
void perf_note_jcc_emit_fused() noexcept;
void perf_note_jcc_emit_carried() noexcept;
void perf_note_jcc_emit_helper() noexcept;
/// Which condition, and what the incoming NzcvState was, for a Jcc that fell
/// to the flag helper. "N sites take the helper" is not actionable without it.
void perf_note_jcc_emit_helper_detail(u8 x86_cc, u8 nzcv_kind) noexcept;

/// Count a REP MOVS/STOS emit decision: inline short-count loop, or the bulk
/// C++ helper alone. Compile-time only; no-op unless the window is collecting.
void perf_note_rep_emit_inline() noexcept;
void perf_note_rep_emit_helper() noexcept;

/// Compile time: how one x86 CF capture (emit_lazy_cf_to_call_tmp) was
/// lowered. `nzcv` is the cheapest — a live host NZCV read straight off the
/// predecessor. `known` is the named-CarrySource formula. `dispatch` is the
/// eight-way runtime FlagOp fallback, and a class of block that keeps landing
/// there is a missing CarrySource publisher, not a missing lowering.
void perf_note_cf_emit_known() noexcept;
void perf_note_cf_emit_nzcv() noexcept;
void perf_note_cf_emit_dispatch() noexcept;

[[nodiscard]] PerfWindowSnapshot perf_window_snapshot() noexcept;

/// Test-only: enable a manual window without reading the environment.
void perf_window_enable_for_tests() noexcept;
void perf_window_enable_for_tests(LoadPhase start_at) noexcept;
void perf_window_reset_for_tests() noexcept;

/// Installed by the block cache so a dump can walk live caches without the
/// base library linking the JIT. Uninstalled visitors are no-ops.
using PerfVisitCachesFn = void (*)(void (*fn)(u64 l1_hits, u64 hash_hits,
                                              u64 misses, u64 evictions,
                                              u64 invalidations, u64 size,
                                              void* ctx),
                                   void* ctx);
using PerfVisitCompiledFn = void (*)(void (*fn)(u64 guest_addr, u64 exec_count,
                                                u32 native_bytes,
                                                u32 guest_insns, void* ctx),
                                     void* ctx);
void set_perf_cache_visitors(PerfVisitCachesFn caches,
                             PerfVisitCompiledFn compiled) noexcept;

/// Installed by ThreadManager so a dump/heartbeat can snapshot guest threads
/// without the base library linking WinAPI.
using PerfThreadSnapshotFn = void (*)();
void set_perf_thread_snapshot(PerfThreadSnapshotFn fn) noexcept;
void perf_window_poll_threads() noexcept;

/// Optional name for a top chain-miss RIP (e.g. "kernel32.dll!Sleep").
/// Called only from `perf_window_dump`, never from the hot miss path.
/// Write a NUL-terminated string into `out` (may truncate); empty if unknown.
using PerfMissNameFn = void (*)(u64 rip, char* out, size_t out_sz);
void perf_set_miss_rip_name_fn(PerfMissNameFn fn) noexcept;

} // namespace vitreo::diagnostics
