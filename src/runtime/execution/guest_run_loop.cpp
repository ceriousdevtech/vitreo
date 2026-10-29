// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Execution loop: interpreter dispatch with thunk handling

#include "runtime/execution/guest_run_loop.hpp"
#include "runtime/execution/guest_run_loop_internal.hpp"
#if defined(VITREO_ARCH_X86_64)
#include "runtime/execution/native32_loop.hpp"
#include "runtime/execution/native64_loop.hpp"
#endif
#include "core/cpu/state/cpu_state.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/jit_helpers.hpp"
#include "core/cpu/jit/hot_block_dump.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/system/time.hpp"
#include "winapi/kernel32/system/kuser_shared_data.hpp"
#include "winapi/media/winmm/winmm.hpp"
#include "graphics/common/autorelease_scope.hpp"
#include "graphics/common/perf_diag.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/memory/mem_watch.hpp"
#include "core/cpu/seh/seh32.hpp"
#include "core/cpu/lock_prefix.hpp"
#include "winapi/msvcrt/runtime/cxx_eh.hpp"
#include <vitreo/diagnostics.hpp>
#include "core/base/perf_window.hpp"
#include "runtime/diagnostics/pc_trace.hpp"
#include "runtime/diagnostics/hostcall_ring.hpp"
#include "runtime/diagnostics/callstream.hpp"
#include "runtime/execution/exception_dispatch.hpp"
#include "runtime/execution/block_decoder.hpp"
#include "runtime/execution/exception_dispatch32.hpp"
#include "runtime/diagnostics/fault_report.hpp"
#include "runtime/execution/thunk_fast_path.hpp"
#include "runtime/execution/thunk_direct.hpp"
#include "runtime/execution/thunk_inchain.hpp"
#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <execinfo.h>
#include <unistd.h>
#include <vector>
#include <mach/mach_time.h>
#include <csignal>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

// The "trace milestone" gate read below for VITREO_PC_WATCH_AFTER_PING lives
// in vitreo::diagnostics, because the layer that observes the milestone —
// kernel32's overlapped-IO path — cannot link this one.

// SIGTERM/SIGINT handler: sets flag for clean exit (so profiling reports print)
static volatile sig_atomic_t g_stop_requested = 0;
static void stop_signal_handler(int /*sig*/) {
    g_stop_requested = 1;
}

namespace vitreo::cli {

namespace {

// DIAGNOSTIC (VITREO_THUNK_MISS_BYTES=1): 19.7M chain-thunk misses per
// One load, topped by memset, EnterCriticalSection,
// LeaveCriticalSection, and _setjmp3, all of which guest_accel.cpp patches
// with `jmp rel32`. Report what is actually at the RIP, once per distinct
// address.
const bool t_thunk_miss_bytes = std::getenv("VITREO_THUNK_MISS_BYTES") != nullptr;

void log_thunk_miss_bytes(const cpu::CpuState& cpu, GuestMemory& memory,
                          const winapi::ThunkRegistry& thunk_registry) {
    static std::mutex bmu;
    static std::unordered_map<u32, u64> seen;
    // Per-(rip, thread) miss counts. Block caches are per-thread, so the
    // question "why does no block exist at this sled" is really "which threads
    // are missing here" -- one thread missing millions of times means the block
    // is absent in ITS cache; many threads missing a few times each means the
    // work is spread and no single cache ever warms up.
    static std::unordered_map<u64, u64> by_rip_tid;
    static u64 n_calls = 0;
    std::lock_guard<std::mutex> lk(bmu);
    const u32 tid = winapi::ThreadManager::get_current_thread_id_static();
    const u64 key = (static_cast<u64>(tid) << 32) |
                    static_cast<u64>(static_cast<u32>(cpu.rip));
    ++by_rip_tid[key];
    if ((++n_calls % 2000000) == 0) {
        std::vector<std::pair<u64, u64>> rows(by_rip_tid.begin(),
                                              by_rip_tid.end());
        std::sort(rows.begin(), rows.end(),
                  [](const auto& a, const auto& b) {
                      return a.second > b.second;
                  });
        std::fprintf(stderr,
                     "[thunk-miss] after %llu misses: %zu distinct (rip,tid) "
                     "pairs\n",
                     (unsigned long long)n_calls, by_rip_tid.size());
        for (usize i = 0; i < rows.size() && i < 10; ++i) {
            std::fprintf(stderr,
                         "[thunk-miss]   rip=0x%08x tid=%-4u misses=%llu\n",
                         static_cast<u32>(rows[i].first & 0xFFFFFFFFu),
                         static_cast<u32>(rows[i].first >> 32),
                         (unsigned long long)rows[i].second);
        }
    }
    if (seen[static_cast<u32>(cpu.rip)]++ != 0) return;
    u8 b[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
        const auto r = memory.read_u8(cpu.rip + static_cast<GuestAddr>(i));
        b[i] = r.is_ok() ? r.value() : 0xFFu;
    }
    std::fprintf(stderr,
        "[thunk-miss] rip=0x%08llx bytes=%02x %02x %02x %02x dispatch=0x%08llx\n",
        (unsigned long long)cpu.rip, b[0], b[1], b[2], b[3],
        (unsigned long long)thunk_registry.stub_dispatch_target(cpu.rip));
}
} // namespace

static winapi::ThunkRegistry*              g_thunk_registry = nullptr;

/// Name the export owning `addr` when it lands in the WinAPI thunk region.
///
/// Installed into the CPU layer so an InvalidOpcode inside a thunk slot names
/// the API instead of printing bare hex. Returns false for ordinary addresses
/// so the diagnostic stays quiet on non-thunk faults.
static bool resolve_thunk_name_for_fault(u64 addr, char* out,
                                         unsigned out_size) {
    auto* reg = g_thunk_registry;
    if (reg == nullptr || out == nullptr || out_size == 0) return false;
    std::string name = reg->debug_name_for(static_cast<GuestAddr>(addr));
    if (name.empty()) return false;
    std::snprintf(out, out_size, "%s", name.c_str());
    return true;
}

/// PERF_WINDOW dump helper: resolve a chain-miss RIP to "dll!function" when it
/// lands in the process thunk region. Invoked only from dump, never note_miss.
static void resolve_perf_miss_rip_name(u64 rip, char* out, size_t out_sz) {
    if (out == nullptr || out_sz == 0) {
        return;
    }
    out[0] = '\0';
    auto* reg = g_thunk_registry;
    if (reg == nullptr) {
        return;
    }
    const GuestAddr addr = static_cast<GuestAddr>(rip);
    if (!reg->is_in_thunk_region(addr)) {
        return;
    }
    const std::string name = reg->resolve_name(addr);
    if (name.empty()) {
        return;
    }
    std::snprintf(out, out_sz, "%s", name.c_str());
}

// ============================================================================
// Profiling structures (enabled by VITREO_PROFILE env var)
// ============================================================================
struct BlockProfile {
    u64 exec_count = 0;       // How many times block was entered
    u64 insn_count = 0;       // Total instructions attributed (exec_count * block_size)
    u32 block_insns = 0;      // Instructions in this block
    bool jit_compiled = false; // Was it JIT-compiled?
    bool jit_failed = false;   // Did JIT attempt and fail?
};

RunResult run_execution_loop(cpu::CpuState& cpu,
                             GuestMemory& memory,
                             winapi::ThunkRegistry& thunk_registry,
                             winapi::ProcessManager& process_mgr,
                             const RunConfig& config,
                             EventPumpCallback event_pump) {
    ActiveGuestCpuScope active_cpu_scope(cpu);

    // Process-wide thunk registry for PERF_WINDOW top chain-miss name lookup.
    // Lifetime matches the registry owned by the process runtime (≥ this run).
    g_thunk_registry = &thunk_registry;
    diagnostics::perf_set_miss_rip_name_fn(&resolve_perf_miss_rip_name);

    // Let the CPU layer dump recent control flow when it hits a fault, without
    // the CPU libraries having to link against this loop.
    cpu::set_pctrace_dump_hook(&vitreo_pctrace_dump_ring);
    cpu::set_thunk_name_hook(&resolve_thunk_name_for_fault);

    // Install SIGTERM/SIGINT handler for clean exit (profile report)
    g_stop_requested = 0;
    struct sigaction sa{}, old_term{}, old_int{};
    sa.sa_handler = stop_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, &old_term);
    sigaction(SIGINT,  &sa, &old_int);

#if defined(VITREO_ARCH_X86_64)
    if (!cpu.is_64bit() && memory.is_identity() && native32_wanted()) {
        if (native32_prepare(memory, thunk_registry).is_ok()) {
            return run_native32_loop(cpu, memory, thunk_registry, process_mgr,
                                     config, event_pump);
        }
        std::fprintf(stderr,
                     "[native32] prepare failed; falling back to interpreter\n");
    }
    // Win64 native execution. VITREO_NATIVE64=0 remains a diagnostic fallback
    // to the interpreter; Intel hosts otherwise execute Win64 guest code on
    // the native backend.
    if (cpu.is_64bit() && memory.is_identity() && native64_wanted()) {
        if (native64_prepare(memory, thunk_registry,
                             process_mgr.module_manager()).is_ok()) {
            return run_native64_loop(cpu, memory, thunk_registry, process_mgr,
                                     config, event_pump);
        }
        std::fprintf(stderr,
                     "[native64] prepare failed; falling back to interpreter\n");
    }
#endif

    auto to_hex_string = [](u64 val) -> std::string {
        char buf[20];
        std::snprintf(buf, sizeof(buf), "%llx",
                      static_cast<unsigned long long>(val));
        return std::string(buf);
    };

    cpu::Interpreter interp(cpu, memory);
    cpu::jit::BlockCache block_cache(per_thread_block_cache_capacity());
    cpu::jit::JitCompiler jit_compiler(memory);
    jit_compiler.set_block_cache(&block_cache);
    jit_compiler.set_thunk_region(thunk_registry.thunk_base(),
                                  thunk_registry.thunk_end());
    jit_compiler.set_inchain_slot_map(thunk_registry.in_chain_slot_map());
    // L3 codegen only where something could be served in-chain at all
    // (thunk_inchain.hpp): an arm without the direct crossing, or with a
    // diagnostic that needs the full path, emits no in-chain stub.
    cpu::jit::JitCompiler::set_inchain_thunks_runtime_gate(
        in_chain_thunks_possible());
    if (event_pump) {
        // Cold JIT (e.g. ijl15 during menu slideshow JPEG decode) can run for
        // seconds without returning to the instruction-count pump below.
        jit_compiler.set_gui_pump(event_pump);
    }
    bool jit_enabled = (std::getenv("VITREO_NO_JIT") == nullptr)
                       && !(cpu.is_64bit()
                            && std::getenv("VITREO_JIT_64BIT_DISABLE") != nullptr);

    // Stage A2: enable the process-wide compile memo.  Worker threads
    // spawned via CreateThread share this map and skip re-codegening
    // already-compiled guest VAs.  Enabled for every guest run unless
    // VITREO_JIT_MEMO=0; unit tests that drive the JIT directly enable it
    // themselves where they need it.
    if (std::getenv("VITREO_JIT_MEMO") == nullptr ||
        std::string(std::getenv("VITREO_JIT_MEMO")) != "0") {
        cpu::jit::JitCompileMemo::enable_compile_memo();
    }

    // Stage B1: enable JIT-emitted inline block-link tails for direct
    // unconditional JMP REL terminators.  When the next block is already
    // L1-cached and compiled, the JIT'd tail re-enters it via BR Xn
    // without returning to the C++ dispatcher (no BlockCache lookup,
    // BLR, prologue, etc. on the round-trip).  Falls back cleanly on
    // miss/mismatch/uncompiled.  Override at runtime with
    // VITREO_JIT_LINK=0 to disable for bisection.
    if ((std::getenv("VITREO_JIT_LINK") == nullptr ||
         std::string(std::getenv("VITREO_JIT_LINK")) != "0") &&
        !pctrace_enabled()) {
        cpu::jit::JitCompiler::enable_jit_link();
    }
    // When VITREO_PCTRACE is active we also force-skip the block cache for
    // PCs inside the range (see pctrace_active() check in the dispatch loop).
    // Disabling jit-link above guarantees every block boundary returns to the
    // dispatcher so the pctrace hook is reachable.

    // VITREO_JIT_STOP=N  — disable JIT after N million instructions
    // Supports decimals: VITREO_JIT_STOP=154.5 means 154,500,000
    // (for bisecting JIT bugs: binary search the cutoff)
    u64 jit_stop_at = 0;
    if (const char* stop_env = std::getenv("VITREO_JIT_STOP")) {
        double val = std::atof(stop_env);
        jit_stop_at = static_cast<u64>(val * 1000000.0);
    }
    u64 jit_disable_begin = 0;
    u64 jit_disable_end = 0;
    if (const char* range = std::getenv("VITREO_JIT_DISABLE_RANGE")) {
        char* separator = nullptr;
        jit_disable_begin = std::strtoull(range, &separator, 0);
        if (separator != range && *separator == '-') {
            jit_disable_end = std::strtoull(separator + 1, nullptr, 0);
        }
    }

    // Publish JIT state so run_guest_callback (and worker threads) can use it
    t_block_cache  = &block_cache;
    t_jit_compiler = &jit_compiler;
    t_jit_enabled  = jit_enabled;

    // RAII guard: clear the thread-local JIT pointers on scope exit so
    // chain-loaded guests don't accidentally use the dangling stack-
    // allocated BlockCache / JitCompiler from a previous run when their
    // PE loader fires TLS callbacks before reaching the next
    // run_execution_loop call.
    struct ClearJitTls {
        ~ClearJitTls() {
            t_block_cache  = nullptr;
            t_jit_compiler = nullptr;
            t_jit_enabled  = false;
        }
    } clear_jit_tls_guard;

    winapi::Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    // Hot-thunk fast-path address cache (Stage B2).
    FastThunkAddrs fast_thunks;
    resolve_fast_thunks(fast_thunks, thunk_registry);

    RunResult result;
    result.instructions_executed = 0;
    u64 count = 0;
    bool running = true;
    const u64 process_return_sentinel = cpu.is_64bit()
        ? PROCESS_RETURN_SENTINEL
        : static_cast<u32>(PROCESS_RETURN_SENTINEL);

    // Profiling (enabled by VITREO_PROFILE=1)
    const bool profiling = (std::getenv("VITREO_PROFILE") != nullptr);
    std::unordered_map<GuestAddr, BlockProfile> block_profiles;
    u64 jit_insn_count = 0;
    u64 interp_block_insn_count = 0;
    u64 interp_single_insn_count = 0;
    u64 thunk_count = 0;
    u64 jit_chain_entries = 0;  // how many times we entered JIT chain dispatch
    u64 jit_chain_blocks = 0;  // total blocks executed in chain dispatch
    u64 jit_compile_attempts = 0;
    u64 jit_compile_successes = 0;
    const bool slow_hostcall_diag =
        std::getenv("VITREO_SLOW_HOSTCALL_DIAG") != nullptr;
    const u64 slow_hostcall_threshold_ms = [] {
        const char* value = std::getenv("VITREO_SLOW_HOSTCALL_THRESHOLD_MS");
        return value != nullptr ? std::strtoull(value, nullptr, 0) : u64{10};
    }();
    const bool live_profile_snapshots =
        profiling && std::getenv("VITREO_PROFILE_LIVE") != nullptr;
    const u64 live_profile_interval = [] {
        const char* value = std::getenv("VITREO_PROFILE_LIVE_INTERVAL");
        if (value == nullptr || value[0] == '\0') return u64{10000000};
        const u64 parsed = std::strtoull(value, nullptr, 0);
        return parsed != 0 ? parsed : u64{10000000};
    }();
    u64 next_live_profile_at = live_profile_interval;
    std::unordered_map<GuestAddr, u64> live_profile_previous_insns;
    const auto live_profile_started = std::chrono::steady_clock::now();

    auto print_live_profile_snapshot = [&]() {
        struct LiveBlockDelta {
            GuestAddr addr;
            const BlockProfile* profile;
            u64 interval_insns;
        };
        std::vector<LiveBlockDelta> sorted_blocks;
        sorted_blocks.reserve(block_profiles.size());
        for (const auto& [addr, bp] : block_profiles) {
            const auto previous = live_profile_previous_insns.find(addr);
            const u64 previous_insns = previous == live_profile_previous_insns.end()
                ? 0
                : previous->second;
            sorted_blocks.push_back(
                LiveBlockDelta{addr, &bp, bp.insn_count - previous_insns});
            live_profile_previous_insns[addr] = bp.insn_count;
        }
        std::sort(sorted_blocks.begin(), sorted_blocks.end(),
                  [](const auto& a, const auto& b) {
                      return a.interval_insns > b.interval_insns;
                  });

        const auto elapsed_ms = std::chrono::duration_cast<
            std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                      live_profile_started).count();
        std::fprintf(stderr,
                     "[profile-live pid=%d] elapsed_ms=%lld "
                     "instructions=%llu blocks=%zu\n",
                     static_cast<int>(::getpid()),
                     static_cast<long long>(elapsed_ms),
                     static_cast<unsigned long long>(count),
                     sorted_blocks.size());
        vitreo::cpu::jit::hot_dump::dump();
        usize shown = 0;
        for (const auto& live : sorted_blocks) {
            if (shown >= 10) break;
            if (live.interval_insns == 0) break;
            const auto& bp = *live.profile;
            std::fprintf(
                stderr,
                "[profile-live pid=%d] #%zu rip=0x%016llx execs=%llu "
                "interval_insns=%llu total_insns=%llu block_insns=%u "
                "jit=%u failed=%u\n",
                static_cast<int>(::getpid()), shown + 1,
                static_cast<unsigned long long>(live.addr),
                static_cast<unsigned long long>(bp.exec_count),
                static_cast<unsigned long long>(live.interval_insns),
                static_cast<unsigned long long>(bp.insn_count),
                bp.block_insns, bp.jit_compiled ? 1u : 0u,
                bp.jit_failed ? 1u : 0u);
            ++shown;
        }
        std::fflush(stderr);
    };

    // Threshold-based event pump and milestone — avoids modulo per iteration
    constexpr u64 MILESTONE_INTERVAL = 10000000;  // every 10M instructions
    u64 next_milestone_at = MILESTONE_INTERVAL;

    // Opt-in, low-overhead guest stack sampling for diagnosing CPU-bound
    // stalls without forcing watched blocks out of the JIT. The first sample
    // is selected with VITREO_CPU_STACK_SAMPLE_AT; a non-zero
    // VITREO_CPU_STACK_SAMPLE_INTERVAL requests subsequent samples.
    const u64 cpu_stack_sample_interval = [] {
        const char* value = std::getenv("VITREO_CPU_STACK_SAMPLE_INTERVAL");
        return value != nullptr ? std::strtoull(value, nullptr, 0) : u64{0};
    }();
    u64 next_cpu_stack_sample_at = [] {
        const char* value = std::getenv("VITREO_CPU_STACK_SAMPLE_AT");
        return value != nullptr ? std::strtoull(value, nullptr, 0) : u64{0};
    }();
    usize cpu_stack_sample_count = 0;

    auto print_cpu_stack_sample = [&]() {
        cpu.materialize_lazy_flags();
        const GuestAddr sp = static_cast<GuestAddr>(cpu.sp());
        const GuestAddr bp = static_cast<GuestAddr>(
            cpu.is_64bit() ? cpu.gpr.get64(5) : cpu.gpr.get32(5));
        std::fprintf(
            stderr,
            "[cpu-stack-sample] index=%zu instructions=%llu rip=0x%llx "
            "sp=0x%llx bp=0x%llx mode=%s\n",
            cpu_stack_sample_count,
            static_cast<unsigned long long>(count),
            static_cast<unsigned long long>(cpu.rip),
            static_cast<unsigned long long>(sp),
            static_cast<unsigned long long>(bp),
            cpu.is_64bit() ? "x64" : "x86");

        constexpr usize kStackValues = 64;
        for (usize index = 0; index < kStackValues; ++index) {
            const GuestAddr address = sp + static_cast<GuestAddr>(
                index * (cpu.is_64bit() ? sizeof(u64) : sizeof(u32)));
            const u64 value = cpu.is_64bit()
                ? memory.read_u64(address).value_or(0)
                : memory.read_u32(address).value_or(0);
            std::fprintf(stderr, "[cpu-stack-sample] stack[%02zu]=0x%llx\n",
                         index, static_cast<unsigned long long>(value));
        }

        GuestAddr frame = bp;
        for (usize index = 0; index < 32 && frame != 0; ++index) {
            const u64 next = cpu.is_64bit()
                ? memory.read_u64(frame).value_or(0)
                : memory.read_u32(frame).value_or(0);
            const u64 ret = cpu.is_64bit()
                ? memory.read_u64(frame + sizeof(u64)).value_or(0)
                : memory.read_u32(frame + sizeof(u32)).value_or(0);
            std::fprintf(
                stderr,
                "[cpu-stack-sample] frame[%02zu]=0x%llx next=0x%llx ret=0x%llx\n",
                index, static_cast<unsigned long long>(frame),
                static_cast<unsigned long long>(next),
                static_cast<unsigned long long>(ret));
            if (next <= frame || next - frame > 16 * 1024 * 1024) break;
            frame = static_cast<GuestAddr>(next);
        }
        std::fflush(stderr);
        ++cpu_stack_sample_count;
    };

    // Time-based event pump: ~60Hz (16ms) using mach_absolute_time.
    // This avoids hammering dispatch_sync during heavy computation while
    // still keeping GUI apps responsive.
    static mach_timebase_info_data_t s_timebase = {0, 0};
    if (s_timebase.denom == 0) mach_timebase_info(&s_timebase);
    // 16ms in mach absolute time ticks
    const u64 pump_interval_ticks =
        (16000000ULL * s_timebase.denom) / s_timebase.numer;
    u64 last_pump_time = mach_absolute_time();
    // Also keep an instruction-count gate so we don't call mach_absolute_time
    // on every single iteration — check the clock at most every 5000 insns
    constexpr u64 PUMP_CHECK_INTERVAL = 5000;
    u64 next_pump_check_at = PUMP_CHECK_INTERVAL;

    // Ring buffer for last N instruction RIPs (crash debugging)
    constexpr size_t TRACE_SIZE = 65536;
    constexpr size_t TRACE_MASK = TRACE_SIZE - 1;
    static u64 trace_rips[TRACE_SIZE] = {};
    size_t trace_idx = 0;
    // Only record trace when VITREO_TRACE_RING is set (or crash path)
    static const bool trace_ring_enabled = (std::getenv("VITREO_TRACE_RING") != nullptr);

    // ---------------------------------------------------------------------
    // Hostcall ring buffer for crash debugging
    //
    // Records each dispatched API call (name + caller RIP + key arg/return
    // registers) so that when a guest STATUS_ACCESS_VIOLATION fires we can
    // dump the last N hostcalls leading up to it. This is how we
    // root-cause "API returned wrong success state" contract bugs: the
    // crash deref is just the symptom — the cause is usually a Win32 API
    // we returned the wrong value from a few calls earlier.
    //
    // File-static so the run_guest_callback path (which has its own
    // dispatch loop and is where DllMain crashes are surfaced) writes
    // into the same ring.
    // ---------------------------------------------------------------------
    auto dump_trace = [&]() {
        VITREO_TRACE( "[vitreo] Last %zu instruction RIPs:\n",
                     TRACE_SIZE);
        for (size_t i = 0; i < TRACE_SIZE; ++i) {
            size_t idx = (trace_idx + i) & TRACE_MASK;
            if (trace_rips[idx] != 0) {
                u8 ibytes[8] = {};
                auto rd = memory.read_bytes(trace_rips[idx], ibytes, sizeof(ibytes));
                if (rd.is_ok()) {
                    VITREO_TRACE( "  [%zu] RIP=0x%llx  bytes:",
                                 i, static_cast<unsigned long long>(trace_rips[idx]));
                    for (u8 b : ibytes) {
                        VITREO_TRACE( " %02x", static_cast<unsigned>(b));
                    }
                    // If in thunk region, show the thunk name
                    if (thunk_registry.is_in_thunk_region(trace_rips[idx])) {
                        std::string name = thunk_registry.resolve_name(trace_rips[idx]);
                        VITREO_TRACE( "  [%s]", name.c_str());
                    }
                    VITREO_TRACE( "\n");
                } else {
                    VITREO_TRACE( "  [%zu] RIP=0x%llx  <unmapped>",
                                 i, static_cast<unsigned long long>(trace_rips[idx]));
                    if (thunk_registry.is_in_thunk_region(trace_rips[idx])) {
                        std::string name = thunk_registry.resolve_name(trace_rips[idx]);
                        VITREO_TRACE( "  [%s]", name.c_str());
                    }
                    VITREO_TRACE( "\n");
                }
            }
        }
    };

    // Publish this thread's live register file so a peer thread's SuspendThread
    // and GetThreadContext can capture a consistent snapshot. Sampling and
    // watchdog threads rely on that snapshot.
    auto suspend_guard = winapi::ThreadSuspendRegistry::instance().publish(
        winapi::ThreadManager::get_current_thread_id_static(), &cpu);

    // L3: what an in-chain API crossing (thunk_inchain.hpp) borrows from this
    // loop while its chains run translated code. Each chain sets its lease.
    InChainThunkEnv inchain_env{&thunk_registry, &memory, &marshaler,
                                &suspend_guard, nullptr, slow_hostcall_diag};
    // L3-K: the fast-thunk bodies its kernel32 exports run, as here.
    inchain_env.fast_thunks = &fast_thunks;
    // The crossing observes ExitProcess as this loop does after a thunk.
    inchain_env.process = &process_mgr;
    const cpu::jit::JitCompiler::InChainScope inchain_scope{
        &jit_compiler, &serve_in_chain_thunk, &inchain_env};

    while (running && !g_stop_requested) {
        suspend_guard.checkpoint();
        if (suspend_guard.terminate_requested()) {
            cpu.rip = process_return_sentinel;
        }
        if (cpu.rip == process_return_sentinel) {
            result.exit_code = static_cast<int>(cpu.is_64bit()
                ? cpu.gpr.get64(static_cast<u8>(cpu::RegIndex::RAX))
                : cpu.gpr.get32(static_cast<u8>(cpu::RegIndex::RAX)));
            result.success = true;
            break;
        }

        static const u64 s_arg_watch = [] {
            const char* value = std::getenv("VITREO_ARG_WATCH");
            return value != nullptr ? std::strtoull(value, nullptr, 0) : 0;
        }();
        static const u64 s_arg_watch_mem = [] {
            const char* value = std::getenv("VITREO_ARG_WATCH_MEM");
            return value != nullptr ? std::strtoull(value, nullptr, 0) : 0;
        }();
        static int s_arg_watch_count = 0;
        if (s_arg_watch != 0 && cpu.rip == s_arg_watch && s_arg_watch_count++ < 8) {
            u64 args[8]{};
            usize arg_count = 0;
            if (cpu.is_64bit()) {
                const auto stack_arg = memory.read_u64(cpu.sp() + 0x28);
                args[0] = cpu.gpr.rcx;
                args[1] = cpu.gpr.rdx;
                args[2] = cpu.gpr.r8;
                args[3] = cpu.gpr.r9;
                args[4] = stack_arg.is_ok() ? stack_arg.value() : 0;
                arg_count = 5;
            } else {
                for (usize index = 0; index < std::size(args); ++index) {
                    const auto stack_arg = memory.read_u32(
                        cpu.sp() + 4 + static_cast<GuestAddr>(index * sizeof(u32)));
                    args[index] = stack_arg.is_ok() ? stack_arg.value() : 0;
                }
                arg_count = std::size(args);
            }
            std::fprintf(stderr,
                         "[arg-watch] rip=0x%llx rsp=0x%llx rax=0x%llx rbx=0x%llx\n",
                         (unsigned long long)cpu.rip, (unsigned long long)cpu.sp(),
                         (unsigned long long)cpu.gpr.rax,
                         (unsigned long long)cpu.gpr.rbx);
            GuestAddr null_region_base = 0;
            usize null_region_size = 0;
            MemoryProtection null_region_prot = MemoryProtection::None;
            const bool null_mapped = memory.query_region(
                0, null_region_base, null_region_size, null_region_prot);
            std::fprintf(stderr, "[arg-watch]   null-mapped=%u base=0x%llx size=0x%llx\n",
                         null_mapped ? 1u : 0u,
                         (unsigned long long)null_region_base,
                         (unsigned long long)null_region_size);
            if (s_arg_watch_mem != 0) {
                auto watched = memory.read_u64(s_arg_watch_mem);
                std::fprintf(stderr, "[arg-watch]   mem[0x%llx]=0x%llx\n",
                             (unsigned long long)s_arg_watch_mem,
                             (unsigned long long)(watched.is_ok() ? watched.value() : 0));
            }
            for (usize index = 0; index < arg_count; ++index) {
                std::fprintf(stderr, "[arg-watch]   arg%zu=0x%llx", index,
                             (unsigned long long)args[index]);
                for (u64 offset = 0; offset < 0x20; offset += 8) {
                    auto value = memory.read_u64(args[index] + offset);
                    std::fprintf(stderr, " %016llx",
                                 (unsigned long long)(value.is_ok() ? value.value() : 0));
                }
                std::fprintf(stderr, "\n");
            }
        }

        // Check instruction limit
        if (config.max_instructions > 0 && count >= config.max_instructions) {
            result.exit_code = 0;
            result.success = true;
            break;
        }

        // Pump native events periodically to keep UI responsive (~60Hz)
        if (event_pump && count >= next_pump_check_at) {
            next_pump_check_at = count + PUMP_CHECK_INTERVAL;
            u64 now = mach_absolute_time();
            if (now - last_pump_time >= pump_interval_ticks) {
                event_pump();
                last_pump_time = now;
            }
        }

        // Periodic instruction milestone logging
        if (live_profile_snapshots && count >= next_live_profile_at) {
            print_live_profile_snapshot();
            next_live_profile_at = count + live_profile_interval;
        }

        if (next_cpu_stack_sample_at != 0 &&
            count >= next_cpu_stack_sample_at) {
            print_cpu_stack_sample();
            next_cpu_stack_sample_at = cpu_stack_sample_interval != 0
                ? count + cpu_stack_sample_interval
                : 0;
        }

        if (count >= next_milestone_at) {
            cpu.materialize_lazy_flags();
            const char* thunk_name_cstr = "";
            std::string thunk_name_str;
            if (thunk_registry.is_in_thunk_region(cpu.rip)) {
                thunk_name_str = thunk_registry.resolve_name(static_cast<GuestAddr>(cpu.rip));
                thunk_name_cstr = thunk_name_str.c_str();
            }
            VITREO_TRACE("[cpu] milestone: %lluM instructions, RIP=0x%llx %s\n",
                         static_cast<unsigned long long>(count / 1000000),
                         static_cast<unsigned long long>(cpu.rip),
                         thunk_name_cstr);
            if (cpu.is_64bit()) {
                VITREO_TRACE("[cpu]   RAX=%016llx RCX=%016llx RDX=%016llx RBX=%016llx\n",
                             (unsigned long long)cpu.gpr.get64(0), (unsigned long long)cpu.gpr.get64(1),
                             (unsigned long long)cpu.gpr.get64(2), (unsigned long long)cpu.gpr.get64(3));
                VITREO_TRACE("[cpu]   RSP=%016llx RBP=%016llx RSI=%016llx RDI=%016llx FL=%08x\n",
                             (unsigned long long)cpu.gpr.get64(4), (unsigned long long)cpu.gpr.get64(5),
                             (unsigned long long)cpu.gpr.get64(6), (unsigned long long)cpu.gpr.get64(7),
                             static_cast<u32>(cpu.flags.rflags));
            } else {
                VITREO_TRACE("[cpu]   EAX=%08x ECX=%08x EDX=%08x EBX=%08x\n",
                             cpu.gpr.get32(0), cpu.gpr.get32(1),
                             cpu.gpr.get32(2), cpu.gpr.get32(3));
                VITREO_TRACE("[cpu]   ESP=%08x EBP=%08x ESI=%08x EDI=%08x FL=%08x\n",
                             cpu.gpr.get32(4), cpu.gpr.get32(5),
                             cpu.gpr.get32(6), cpu.gpr.get32(7),
                             static_cast<u32>(cpu.flags.rflags));
            }
            next_milestone_at = count + MILESTONE_INTERVAL;
        }

        // VITREO_PC_WATCH_ADDR=addr[,addr...]: dispatch-level PC watch. Catches
        // indirect-call targets that bypass JitCompiler::execute() entry
        // logging, and forces overlapping JIT blocks to the interpreter (see
        // below). VITREO_PC_WATCH_AFTER_PING=1 defers logging until the trace
        // milestone.
        {
            static int s_custom_watch_n = 0;
            static const bool s_after_ping =
                (std::getenv("VITREO_PC_WATCH_AFTER_PING") != nullptr);
            static const bool s_pc_watch_enabled = !pc_watch_addrs().empty();
            static const int s_watch_limit = s_after_ping ? 5000 : 2000;
            if (s_pc_watch_enabled && pc_watch_hit(cpu.rip) &&
                s_custom_watch_n < s_watch_limit &&
                (!s_after_ping || vitreo::diagnostics::trace_milestone_reached())) {
                const GuestAddr sp = static_cast<GuestAddr>(cpu.sp());
                u32 stack[4]{};
                for (int i = 0; i < 4; ++i) {
                    stack[i] = memory.read_u32(sp + static_cast<GuestAddr>(i * 4))
                                   .value_or(0);
                }
                const bool long_mode = cpu.mode == cpu::CpuMode::Long;
                const u64 ret = long_mode ? memory.read_u64(sp).value_or(0)
                                          : stack[0];
                const u64 arg0 = long_mode ? cpu.gpr.rcx : stack[1];
                const u64 arg1 = long_mode ? cpu.gpr.rdx : stack[2];
                const u64 arg2 = long_mode ? cpu.gpr.r8 : stack[3];
                std::fprintf(
                    stderr,
                    "[pcwatch-custom] pid=%d rip=0x%llx tid=%u ret=0x%llx "
                    "eax=0x%x ecx=0x%x edx=0x%x ebx=0x%x esi=0x%x edi=0x%x "
                    "esp=0x%llx arg0=0x%llx arg1=0x%llx arg2=0x%llx",
                    static_cast<int>(::getpid()),
                    static_cast<unsigned long long>(cpu.rip),
                    winapi::ThreadManager::get_current_thread_id_static(),
                    static_cast<unsigned long long>(ret),
                    cpu.gpr.get32(0), cpu.gpr.get32(1), cpu.gpr.get32(2),
                    cpu.gpr.get32(3), cpu.gpr.get32(6), cpu.gpr.get32(7),
                    static_cast<unsigned long long>(sp),
                    static_cast<unsigned long long>(arg0),
                    static_cast<unsigned long long>(arg1),
                    static_cast<unsigned long long>(arg2));
                append_pc_watch_memory(cpu, memory);
                std::fprintf(stderr, " (#%d)\n", s_custom_watch_n++);
                std::fflush(stderr);
            }
        }

        // (dump_int3_context is defined as a lambda here so both the
        // pending-raise path and the interpreter Interrupt path share it.)
        auto dump_int3_context = [&](void) {
            auto dump_ascii = [&](const char* label, u64 address) {
                if (address < 0x10000 || address >= 0x0000800000000000ull) {
                    return;
                }
                char text[241] = {};
                usize length = 0;
                for (; length < sizeof(text) - 1; ++length) {
                    auto byte = memory.read_u8(address + length);
                    if (byte.is_err() || byte.value() == 0) break;
                    if (byte.value() < 0x20 || byte.value() > 0x7e) break;
                    text[length] = static_cast<char>(byte.value());
                }
                if (length >= 4) {
                    std::fprintf(stderr, "[int3-ascii] %s@0x%llx: %s\n",
                                 label, (unsigned long long)address, text);
                }
            };
            std::fprintf(stderr,
                "[int3-ctx] rip=0x%llx jit_block=0x%llx rax=0x%llx rcx=0x%llx"
                " rdx=0x%llx rsi=0x%llx rdi=0x%llx rbx=0x%llx r14=0x%llx"
                " r15=0x%llx rsp=0x%llx\n",
                (unsigned long long)cpu.rip,
                (unsigned long long)last_jit_block_pc(),
                (unsigned long long)cpu.gpr.rax,
                (unsigned long long)cpu.gpr.rcx, (unsigned long long)cpu.gpr.rdx,
                (unsigned long long)cpu.gpr.rsi, (unsigned long long)cpu.gpr.rdi,
                (unsigned long long)cpu.gpr.rbx, (unsigned long long)cpu.gpr.r14,
                (unsigned long long)cpu.gpr.r15, (unsigned long long)cpu.gpr.rsp);
            // Deref pointer-looking registers one level (vtable / members —
            // a module .rdata vtable address identifies the failing class).
            for (auto [name, rv] : {std::pair<const char*, u64>{"rcx", cpu.gpr.rcx},
                                    {"rdx", cpu.gpr.rdx},
                                    {"rsi", cpu.gpr.rsi},
                                    {"rdi", cpu.gpr.rdi},
                                    {"rbx", cpu.gpr.rbx},
                                    {"r14", cpu.gpr.r14},
                                    {"r15", cpu.gpr.r15}}) {
                if (rv < 0x10000 || rv >= 0x0000800000000000ull) continue;
                auto q0 = memory.read_u64(rv);
                auto q1 = memory.read_u64(rv + 8);
                auto q2 = memory.read_u64(rv + 16);
                auto q3 = memory.read_u64(rv + 24);
                if (q0.is_ok()) {
                    std::fprintf(stderr,
                                 "[int3-ctx]   [%s]=0x%llx [%s+8]=0x%llx"
                                 " [%s+16]=0x%llx [%s+24]=0x%llx\n",
                                 name, (unsigned long long)q0.value(), name,
                                 (unsigned long long)(q1.is_ok() ? q1.value() : 0),
                                 name,
                                 (unsigned long long)(q2.is_ok() ? q2.value() : 0),
                                 name,
                                 (unsigned long long)(q3.is_ok() ? q3.value() : 0));
                    dump_ascii(name, rv);
                    dump_ascii("indirect", q0.value());
                }
                // Allocator bucket: slot_size +0x18, pages +0x1c,
                // reciprocal +0x20.
                auto slot = memory.read_u32(rv + 0x18);
                auto pages = memory.read_u32(rv + 0x1c);
                auto recip = memory.read_u64(rv + 0x20);
                if (slot.is_ok() && pages.is_ok()) {
                    std::fprintf(stderr,
                                 "[int3-obj] %s+0x18 slot=%u pages=%u "
                                 "recip=0x%llx\n",
                                 name, slot.value(), pages.value(),
                                 static_cast<unsigned long long>(
                                     recip.is_ok() ? recip.value() : 0));
                }
            }
            u64 rsp = cpu.sp();
            for (u64 off = 0; off <= 0x100; off += 0x20) {
                std::fprintf(stderr, "[int3-stack] rsp+0x%03llx", (unsigned long long)off);
                for (u64 slot = 0; slot < 0x20; slot += 8) {
                    auto value = memory.read_u64(rsp + off + slot);
                    std::fprintf(stderr, " %016llx",
                                 (unsigned long long)(value.is_ok() ? value.value() : 0));
                }
                std::fprintf(stderr, "\n");
            }
            int found = 0;
            for (u64 off = 0; off < 0x800 && found < 16; off += 8) {
                auto v = memory.read_u64(rsp + off);
                if (!v.is_ok()) break;
                u64 a = v.value();
                if (a < 0x10000 || a >= 0x0000800000000000ull) continue;
                auto b5 = memory.read_u8(a - 5);
                auto b6 = memory.read_u8(a - 6);
                auto b2 = memory.read_u8(a - 2);
                bool is_ret = (b5.is_ok() && b5.value() == 0xE8) ||
                              (b6.is_ok() && b6.value() == 0xFF) ||
                              (b2.is_ok() && b2.value() == 0xFF);
                if (is_ret) {
                    std::fprintf(stderr, "[int3-bt] rsp+0x%03llx ret=0x%llx\n",
                                 (unsigned long long)off, (unsigned long long)a);
                    ++found;
                }
            }
        };

        // x64 pending raise from a JIT helper (AV / INT3 surfaced by the
        // interpreter-delegate): consume it HERE, at the top of the loop.
        // The pre-existing check further down only runs on the thunk-call
        // path, so helper raises set mid-JIT-chain were never dispatched —
        // the chain re-entered the faulting block forever (observed: 3.4B
        // int3 iterations in a browser helper's abort).
        // Also consume RaiseException after in-chain slow thunk inline, which
        // RETs to the caller before breaking the chain (rip is no longer a
        // dispatch address).
        if (cpu.is_64bit() && cpu.pending_raise.active &&
            (cpu.pending_raise.from_helper ||
             !thunk_registry.is_dispatch_address(cpu.rip))) {
            // Breakpoint context BEFORE SEH dispatch mutates anything.
            if (cpu.pending_raise.code == cpu::STATUS_BREAKPOINT) {
                dump_int3_context();
            }
            u32 raise_code = cpu.pending_raise.code;
            u32 raise_flags = cpu.pending_raise.flags;
            u32 raise_nargs = cpu.pending_raise.num_args;
            u64 raise_exception_addr = cpu.rip;
            if (raise_code == cpu::STATUS_BREAKPOINT &&
                raise_exception_addr > 0) {
                --raise_exception_addr;
            }
            u64 raise_args[15] = {};
            for (u32 ri = 0; ri < raise_nargs; ++ri)
                raise_args[ri] = cpu.pending_raise.args[ri];
            if (raise_code == 0xc0000005u) {
                std::fprintf(
                    stderr,
                    "[av-dispatch] rip=0x%llx nargs=%u access=0x%llx "
                    "address=0x%llx rsp=0x%llx\n",
                    static_cast<unsigned long long>(raise_exception_addr),
                    raise_nargs,
                    static_cast<unsigned long long>(
                        raise_nargs > 0 ? raise_args[0] : 0),
                    static_cast<unsigned long long>(
                        raise_nargs > 1 ? raise_args[1] : 0),
                    static_cast<unsigned long long>(cpu.sp()));
            }
            cpu.pending_raise.clear();

            if (raise_code == cpu::STATUS_STACK_BUFFER_OVERRUN) {
                dump_fastfail_context(cpu, memory, raise_args[0]);
                result.exit_code = static_cast<int>(cpu::STATUS_STACK_BUFFER_OVERRUN);
                result.success = false;
                result.error_message = "guest fast fail (INT 0x29)";
                running = false;
                break;
            }

            const bool stack_overflow =
                raise_code == cpu::STATUS_STACK_OVERFLOW;
            if (stack_overflow) ++cpu.stack_overflow_dispatch_depth;
            bool x64_handled = dispatch_x64_exception(cpu, memory, thunk_registry,
                process_mgr, raise_code, raise_flags, raise_nargs,
                raise_args, raise_exception_addr, module_manager());
            if (stack_overflow) --cpu.stack_overflow_dispatch_depth;
            if (process_mgr.has_exited()) {
                result.exit_code = static_cast<int>(process_mgr.exit_code());
                result.success = true;
                running = false;
                break;
            }
            if (!x64_handled && raise_code == cpu::STATUS_BREAKPOINT) {
                if (vitreo::detail::g_int3_nop) {
                    ++count;
                    continue;
                }
                std::fprintf(stderr,
                    "[vitreo] unhandled INT3 (STATUS_BREAKPOINT) at rip=0x%llx"
                    " — terminating process (0x80000003)\n",
                    static_cast<unsigned long long>(cpu.rip));
                hostcall_dump("int3-unhandled");
                result.exit_code = static_cast<int>(0x80000003u);
                result.success = false;
                result.error_message = "unhandled STATUS_BREAKPOINT (guest CHECK failure)";
                running = false;
                break;
            }
            if (!x64_handled) {
                std::fprintf(stderr,
                    "[vitreo pid=%d] unhandled exception 0x%08x at rip=0x%llx"
                    " — terminating process\n",
                    static_cast<int>(::getpid()), raise_code,
                    static_cast<unsigned long long>(cpu.rip));
                result.exit_code = static_cast<int>(raise_code);
                result.success = false;
                result.error_message = "unhandled guest exception";
                running = false;
                break;
            }
            ++count;
            continue;
        }

        // The guest's IAT points at a thunk slot base, so a call lands on the
        // 8-byte NOP entry stub that precedes the internal dispatch gateway.
        // The thunk region is never JIT-compiled, so executing the stub means
        // single-stepping every NOP through the interpreter. A hot inner loop
        // calling one import ~18M times (RCT3 terrain load) turned that into
        // hundreds of millions of interpreter steps — the dominant loader
        // stall. When the sled is still all NOPs (no hook engine has patched
        // it), advance rip straight to the dispatch gateway: skipping NOPs is
        // architecturally identical to running them (no flags/stack effect).
        maybe_skip_thunk_nop_sled(cpu, memory, thunk_registry);

        // Dispatch only the exact internal gateway. The preceding entry stub
        // remains guest-executable so hook engines can patch or trampoline it.
        if (thunk_registry.is_dispatch_address(cpu.rip)) {
            if (profiling) ++thunk_count;
            if (trace_ring_enabled) {
                trace_rips[trace_idx & TRACE_MASK] = cpu.rip;
                trace_idx++;
            }

            // Stage B2: fast-path for hot kernel32 thunks. Bypasses
            // resolve_handler, std::function dispatch, callee-saved
            // save/restore, TLS dump check, and pre/post TEB sync.
            if (maybe_fast_thunk_call(cpu, memory, fast_thunks)) {
                // Thunk RET emulation: pop return address and (x86-only)
                // apply stdcall callee cleanup. On x64, caller cleans up
                // and the return address is 8 bytes wide.
                const auto thunk_addr = cpu.rip;
                if (!finish_thunk_ret(cpu, memory, thunk_registry, thunk_addr)) {
                    result.exit_code = -1;
                    result.error_message = "Stack underflow during thunk RET";
                    result.success = false;
                    break;
                }
                ++count;
                continue;
            }

            // In-chain slow thunk already ran the host handler and broke out
            // with pending_raise. Skip re-dispatch; fall through to SEH.
            const bool skip_slow_body = t_chain_slow_thunk_pending_raise &&
                                        cpu.pending_raise.active;
            t_chain_slow_thunk_pending_raise = false;
            // The chain already ran this gateway's handler and it requested
            // an unwind: treat it as that dispatch's ControlTransferred.
            const bool chain_unwinding = t_chain_slow_thunk_unwinding &&
                                         cpu.pending_unwind.active;
            t_chain_slow_thunk_unwinding = false;
            // x64 RtlUnwindEx crossing out of a guest callback this thunk's
            // host code ran goes on from the thunk's caller; it may end in a
            // raise taken by the SEH path below (rip still at the gateway).
            bool unwind_raised = false;
            if (chain_unwinding) {
                if (cpu.is_64bit() &&
                    apply_pending_unwind_64(cpu, memory, module_manager(),
                                            process_mgr, /*in_callback=*/false) ==
                        PendingUnwind64Action::Raise) {
                    unwind_raised = true;
                }
                if (process_mgr.has_exited()) {
                    result.exit_code = static_cast<int>(process_mgr.exit_code());
                    result.success = true;
                    break;
                }
                if (!unwind_raised) {
                    ++count;
                    continue;
                }
            }

            if (!skip_slow_body && !unwind_raised) {
                const auto slow = dispatch_slow_api_thunk(
                    cpu, memory, thunk_registry, marshaler, suspend_guard,
                    fast_thunks, module_manager(), /*chain_lease=*/nullptr,
                    slow_hostcall_diag, slow_hostcall_threshold_ms);
                if (slow == SlowThunkDispatch::Unresolved) {
                    result.exit_code = -1;
                    result.error_message =
                        "Unresolved API call at thunk address 0x" +
                        to_hex_string(cpu.rip);
                    result.success = false;
                    break;
                }
                if (slow == SlowThunkDispatch::Fatal) {
                    result.exit_code = -1;
                    result.error_message = "Stack underflow during thunk RET";
                    result.success = false;
                    break;
                }
                if (slow == SlowThunkDispatch::ControlTransferred) {
                    if (cpu.is_64bit() && cpu.pending_unwind.active &&
                        apply_pending_unwind_64(cpu, memory, module_manager(),
                                                process_mgr,
                                                /*in_callback=*/false) ==
                            PendingUnwind64Action::Raise) {
                        unwind_raised = true;
                    }
                    if (process_mgr.has_exited()) {
                        result.exit_code =
                            static_cast<int>(process_mgr.exit_code());
                        result.success = true;
                        break;
                    }
                    if (!unwind_raised) {
                        ++count;
                        continue;
                    }
                }

                // Ok: helper RETed (or set guard-dispatch rip), unless
                // pending_raise is set — then rip is still at the thunk and
                // the SEH path below simulates RET before dispatching.
            }

            // Check if ExitProcess was called
            if (process_mgr.has_exited()) {
                result.exit_code = static_cast<int>(process_mgr.exit_code());
                result.success = true;
                break;
            }

            // Check if TLS alert diagnostic requested a trace ring dump
            if (g_trace_ring_dump_requested.exchange(false, std::memory_order_acq_rel)) {
                VITREO_TRACE("[vitreo] TLS ALERT trace ring dump (last %zu blocks):\n", TRACE_SIZE);
                for (size_t i = 0; i < TRACE_SIZE; ++i) {
                    size_t idx = (trace_idx + i) & TRACE_MASK;
                    if (trace_rips[idx] != 0) {
                        if (thunk_registry.is_in_thunk_region(trace_rips[idx])) {
                            std::string name = thunk_registry.resolve_name(trace_rips[idx]);
                            VITREO_TRACE("  [%04zu] 0x%08x  %s\n", i,
                                         static_cast<unsigned>(trace_rips[idx]), name.c_str());
                        } else {
                            VITREO_TRACE("  [%04zu] 0x%08x\n", i,
                                         static_cast<unsigned>(trace_rips[idx]));
                        }
                    }
                }
                VITREO_TRACE("[vitreo] End trace ring dump\n");
            }

            // Check for pending RaiseException — dispatch SEH before continuing
            if (!cpu.is_64bit() && cpu.pending_raise.active) {
                const bool raise_from_helper = cpu.pending_raise.from_helper;
                // If the raise originated from a Win32 RaiseException thunk,
                // simulate RET from the thunk so cpu.rip/sp reflect the caller.
                // For raises from JIT slow-path helpers (e.g. STATUS_ACCESS_VIOLATION
                // from an unmapped guest read), cpu.rip/sp already point at the
                // faulting block so we MUST NOT pop the return address.
                if (!raise_from_helper) {
                    auto thunk_addr_re = cpu.rip;
                    auto ret_re = memory.read_u32(cpu.sp());
                    if (ret_re.is_ok()) {
                        cpu.rip = static_cast<u64>(ret_re.value());
                        u16 cleanup_re = thunk_registry.resolve_stdcall_bytes(thunk_addr_re);
                        cpu.set_sp(cpu.sp() + 4 + cleanup_re);
                    }
                }

                u32 raise_code = cpu.pending_raise.code;
                u32 raise_flags = cpu.pending_raise.flags;
                u32 raise_nargs = cpu.pending_raise.num_args;
                u32 raise_exception_addr = static_cast<u32>(cpu.rip);
                if (raise_from_helper &&
                    raise_code == cpu::STATUS_BREAKPOINT &&
                    raise_exception_addr > 0) {
                    --raise_exception_addr;
                }
                u32 raise_args[15] = {};
                for (u32 ri = 0; ri < raise_nargs; ++ri)
                    raise_args[ri] = cpu.pending_raise.args[ri];
                cpu.pending_raise.clear();

                // Learn: /cpp/intrinsics/fastfail — user-mode INT 0x29 is
                // STATUS_STACK_BUFFER_OVERRUN and "no exception handlers
                // are invoked". Do not walk VEH/SEH.
                if (raise_code == cpu::STATUS_STACK_BUFFER_OVERRUN) {
                    dump_fastfail_context(cpu, memory, raise_args[0]);
                    result.exit_code = static_cast<int>(cpu::STATUS_STACK_BUFFER_OVERRUN);
                    result.success = false;
                    result.error_message = "guest fast fail (INT 0x29)";
                    running = false;
                    break;
                }

                // Learn: VEH is first-chance -- it runs before ANY frame
                // handler, and it runs even when the thread has no
                // registration record at all. Both facts matter: gating this
                // on a non-empty fs:[0] chain would reproduce the bug it
                // fixes.
                if (cli::invoke_vectored_handlers_32(
                        cpu, memory, thunk_registry, process_mgr,
                        raise_code, raise_flags, raise_nargs, raise_args,
                        raise_exception_addr)) {
                    ++count;
                    continue;
                }

                auto chain_head = cpu::read_seh_chain_head(memory, cpu);
                std::fprintf(stderr,
                    "[seh32/cb] RaiseException chain result=%s head=0x%08x "
                    "esp=0x%08x ebp=0x%08x\n",
                    chain_head.is_ok() ? "ok" : "error",
                    chain_head.is_ok() ? chain_head.value() : 0u,
                    static_cast<u32>(cpu.gpr.rsp),
                    static_cast<u32>(cpu.gpr.rbp));
                std::fflush(stderr);
                u32 orig_esp = static_cast<u32>(cpu.gpr.rsp);
                u32 scratch_esp = orig_esp - (cpu::CTX32_SIZE + cpu::EREC32_SIZE + 16);
                scratch_esp &= ~0xF;

                u32 ctx_addr = scratch_esp;
                u32 erec_addr = scratch_esp + cpu::CTX32_SIZE;
                u32 disp_ctx_addr = erec_addr + cpu::EREC32_SIZE;

                // A bare `throw;` arrives as a C++ exception with a NULL
                // ThrowInfo; give it back the exception the enclosing
                // catch is handling so it can match an outer handler.
                (void)winapi::resolve_cxx_rethrow_32(
                    memory, raise_code, raise_nargs, raise_args);
                cpu::write_exception_record_32(memory, erec_addr,
                    raise_code, raise_flags, raise_exception_addr,
                    raise_nargs, raise_args);
                cpu::write_context_32(memory, ctx_addr, cpu);
                (void)memory.write_u32(disp_ctx_addr, 0);

                if (chain_head.is_ok() && chain_head.value() != 0xFFFFFFFF && chain_head.value() != 0) {
                    u32 frame_addr = chain_head.value();
                    bool handled = false;
                    u32 chain_depth = 0;

                    static const bool s_cxx_eh_diag =
                        std::getenv("VITREO_CXX_EH_DIAG") != nullptr;
                    static const char* s_cxx_eh_dump_type =
                        std::getenv("VITREO_CXX_EH_DUMP_TYPE");
                    if (s_cxx_eh_diag && raise_code == 0xE06D7363u) {
                        const std::string desc =
                            winapi::describe_cxx_throw_32(memory, erec_addr);
                        std::fprintf(stderr, "[cxx-diag] throw %s\n",
                                     desc.c_str());
                        std::fflush(stderr);
                        if (s_cxx_eh_dump_type != nullptr &&
                            desc.find(s_cxx_eh_dump_type) != std::string::npos) {
                            hostcall_dump("cxx throw of watched type");
                        }
                    }

                    while (frame_addr != 0xFFFFFFFF && frame_addr != 0 && chain_depth < 256) {
                        if (!cpu::seh32_registration_dispatchable(
                                memory, cpu, frame_addr)) {
                            std::fprintf(stderr,
                                "[seh32] registration 0x%08x is outside the "
                                "thread stack (depth=%u) — chain corrupt, "
                                "stopping dispatch\n",
                                frame_addr, chain_depth);
                            break;
                        }
                        auto handler_info = cpu::read_seh_handler(memory, frame_addr);
                        if (handler_info.is_err()) break;
                        auto info = handler_info.value();

                        if (!seh_handler_looks_valid(memory, info.handler_addr)) {
                            std::fprintf(stderr,
                                "[seh32] RaiseException: skipping bogus handler 0x%llx (frame=0x%08x depth=%u) — chain may be corrupt\n",
                                static_cast<unsigned long long>(info.handler_addr), info.frame_addr, chain_depth);
                            frame_addr = info.next_frame;
                            ++chain_depth;
                            continue;
                        }

                        VITREO_TRACE("[seh32] RaiseException: calling handler 0x%08x (frame=0x%08x) for code 0x%08x\n",
                                     info.handler_addr, info.frame_addr, raise_code);

                        // A C++ __ehhandler thunk that jumps into msvcrt's
                        // Vitreo-hosted __CxxFrameHandler3 is dispatched by
                        // that hosted personality. One that jumps into guest
                        // code (the real vcruntime140, a static CRT) runs as
                        // the program's own handler, as on Windows.
                        if (raise_code == 0xE06D7363u) {
                            u32 fi = cxx_ehhandler_uses_host_personality(
                                memory, thunk_registry, info.handler_addr)
                                ? detect_cxx_ehhandler_funcinfo(memory, info.handler_addr)
                                : 0;
                            if (s_cxx_eh_diag && fi == 0) {
                                std::fprintf(stderr,
                                    "[cxx-diag]   depth=%u frame=0x%08x "
                                    "handler=0x%08x not-a-cxx-ehhandler\n",
                                    chain_depth, info.frame_addr,
                                    static_cast<u32>(info.handler_addr));
                                std::fflush(stderr);
                            }
                            if (fi != 0) {
                                auto r = winapi::dispatch_cxx_exception_32(
                                    memory, fi, info.frame_addr,
                                    erec_addr, ctx_addr);
                                if (s_cxx_eh_diag) {
                                    std::fprintf(stderr,
                                        "[cxx-diag]   depth=%u frame=0x%08x "
                                        "fi=0x%08x match=%d %s\n",
                                        chain_depth, info.frame_addr, fi,
                                        r.match ? 1 : 0,
                                        winapi::describe_cxx_frame_catches_32(
                                            memory, fi,
                                            info.frame_addr).c_str());
                                    std::fflush(stderr);
                                }
                                if (r.match) {
                                    VITREO_TRACE("[cxx_eh] caught in frame=0x%08x handler=0x%08x dispCatchObj=%d\n",
                                                 info.frame_addr, r.handler_addr, r.disp_catch_obj);
                                    if (enter_cxx_catch_32(
                                            cpu, memory, thunk_registry,
                                            process_mgr, r, info.frame_addr,
                                            ctx_addr, erec_addr, "cxx_eh")) {
                                        handled = true;
                                        break;
                                    }
                                    // The search stops at the first matching
                                    // catch, so this frame owns the exception.
                                    // Resuming the walk would run an outer
                                    // handler the program never selected and
                                    // silently skip this one; fail visibly.
                                    std::fprintf(stderr,
                                        "[cxx_eh] catch entry failed in "
                                        "frame=0x%08x handler=0x%08x — cannot "
                                        "deliver exception\n",
                                        info.frame_addr, r.handler_addr);
                                    std::fflush(stderr);
                                    break;
                                }
                                // No match in this function — try the next frame.
                                VITREO_TRACE("[cxx_eh] no match in funcinfo=0x%08x — continuing chain\n", fi);
                                frame_addr = info.next_frame;
                                ++chain_depth;
                                continue;
                            }
                        }

                        auto disposition = invoke_seh32_frame_handler(
                            cpu, memory, thunk_registry, process_mgr,
                            info.handler_addr, info.frame_addr,
                            erec_addr, ctx_addr, disp_ctx_addr, scratch_esp);

                        // A frame handler that terminated the process ends the
                        // search: remaining frames belong to a process that no
                        // longer exists.
                        if (process_mgr.has_exited()) {
                            handled = true;
                            break;
                        }

                        if (apply_pending_unwind_32(cpu, memory, ctx_addr, "seh32")) {
                            handled = true;
                            break;
                        }

                        VITREO_TRACE("[seh32] RaiseException: handler returned disposition=%llu\n",
                                     static_cast<unsigned long long>(disposition.disposition));

                        if (disposition.disposition == cpu::ExceptionContinueExecution) {
                            if (!disposition.transferred) {
                                (void)cli::resume_from_context_32(cpu, memory, thunk_registry, process_mgr,
                                                             erec_addr, ctx_addr, scratch_esp);
                            }
                            handled = true;
                            break;
                        }
                        frame_addr = info.next_frame;
                        ++chain_depth;
                    }

                    if (handled) {
                        // The handler may have ended the process instead of
                        // resuming — a top-level catch that calls ExitProcess
                        // is the usual shape. Resuming the guest afterwards
                        // walks a process that is already gone.
                        if (process_mgr.has_exited()) {
                            result.exit_code =
                                static_cast<int>(process_mgr.exit_code());
                            result.success = true;
                            running = false;
                            break;
                        }
                        ++count;
                        continue;
                    }

                    VITREO_TRACE("[seh32] RaiseException: no handler caught 0x%08x\n", raise_code);

                    if (s_cxx_eh_diag && raise_code == 0xE06D7363u) {
                        std::fprintf(stderr,
                            "[cxx-diag] UNCAUGHT 0x%08x depth=%u eip=0x%08x\n",
                            raise_code, chain_depth,
                            static_cast<u32>(cpu.rip));
                        std::fflush(stderr);
                        hostcall_dump("uncaught C++ exception (run_loop)");
                    }

                }

                // An empty registration chain is still an unhandled exception.
                // Learn SetUnhandledExceptionFilter: the process filter runs on
                // the faulting thread and may edit the continuation CONTEXT.
                const bool filter_continued = invoke_unhandled_exception_filter_32(
                        cpu, memory, thunk_registry, process_mgr,
                        erec_addr, ctx_addr, scratch_esp);
                if (process_mgr.has_exited()) {
                    result.exit_code = static_cast<int>(process_mgr.exit_code());
                    result.success = true;
                } else if (filter_continued) {
                    ++count;
                    continue;
                } else {
                    result.exit_code = static_cast<int>(raise_code);
                    result.success = false;
                    result.error_message = "unhandled x86 guest exception";
                    std::fprintf(stderr,
                        "[vitreo] unhandled exception 0x%08x at EIP=0x%08x — terminating process\n",
                        raise_code, static_cast<unsigned>(cpu.rip));
                    hostcall_dump("unhandled x86 raised exception");
                }
                running = false;
                break;
            }

            // x64 table-based SEH dispatch
            if (cpu.is_64bit() && cpu.pending_raise.active) {
                const bool raise_from_helper = cpu.pending_raise.from_helper;
                if (cpu.pending_raise.from_helper &&
                    cpu.pending_raise.code == cpu::STATUS_BREAKPOINT) {
                    dump_int3_context();
                }

                // Simulate RET from RaiseException thunk (skip for raises from
                // JIT slow-path helpers, where cpu.rip/sp already reflect the
                // faulting instruction).
                if (!raise_from_helper) {
                    auto ret_re = memory.read_u64(cpu.sp());
                    if (ret_re.is_ok()) {
                        cpu.rip = ret_re.value();
                        cpu.set_sp(cpu.sp() + 8);
                    }
                }

                u32 raise_code = cpu.pending_raise.code;
                u32 raise_flags = cpu.pending_raise.flags;
                u32 raise_nargs = cpu.pending_raise.num_args;
                u64 raise_exception_addr = cpu.rip;
                if (raise_from_helper &&
                    raise_code == cpu::STATUS_BREAKPOINT &&
                    raise_exception_addr > 0) {
                    --raise_exception_addr;
                }
                u64 raise_args[15] = {};
                for (u32 ri = 0; ri < raise_nargs; ++ri)
                    raise_args[ri] = cpu.pending_raise.args[ri];
                cpu.pending_raise.clear();

                bool x64_handled = dispatch_x64_exception(cpu, memory, thunk_registry,
                    process_mgr, raise_code, raise_flags, raise_nargs,
                    raise_args, raise_exception_addr, module_manager());

                // If the handler called TerminateProcess / ExitProcess:
                if (process_mgr.has_exited()) {
                    result.exit_code = static_cast<int>(process_mgr.exit_code());
                    result.success = true;
                    running = false;
                    break;
                }

                // Unhandled STATUS_BREAKPOINT (guest CHECK / __debugbreak):
                // Windows kills the process. Re-executing (the generic
                // fallthrough below) spins forever on the int3.
                if (!x64_handled && raise_code == cpu::STATUS_BREAKPOINT) {
                    if (vitreo::detail::g_int3_nop) {
                        ++count;
                        continue;
                    }
                    std::fprintf(stderr,
                        "[vitreo] unhandled INT3 (STATUS_BREAKPOINT) at rip=0x%llx"
                        " — terminating process (0x80000003)\n",
                        static_cast<unsigned long long>(cpu.rip));
                    hostcall_dump("int3-unhandled");
                    result.exit_code = static_cast<int>(0x80000003u);
                    result.success = false;
                    result.error_message = "unhandled STATUS_BREAKPOINT (guest CHECK failure)";
                    running = false;
                    break;
                }
                if (!x64_handled) {
                    std::fprintf(stderr,
                        "[vitreo] unhandled exception 0x%08x at rip=0x%llx"
                        " — terminating process\n",
                        raise_code, static_cast<unsigned long long>(cpu.rip));
                    // A fatal x64 exception kills the process, so the register
                    // set and a raw stack window are the only chance to
                    // attribute it. Same shape as the [int3-ctx] dump, which
                    // only covers STATUS_BREAKPOINT.
                    if (cpu.is_64bit()) {
                        std::fprintf(stderr,
                            "[exc-ctx] rax=0x%llx rcx=0x%llx rdx=0x%llx "
                            "rbx=0x%llx rsi=0x%llx rdi=0x%llx rbp=0x%llx "
                            "rsp=0x%llx\n",
                            (unsigned long long)cpu.gpr.rax,
                            (unsigned long long)cpu.gpr.rcx,
                            (unsigned long long)cpu.gpr.rdx,
                            (unsigned long long)cpu.gpr.rbx,
                            (unsigned long long)cpu.gpr.rsi,
                            (unsigned long long)cpu.gpr.rdi,
                            (unsigned long long)cpu.gpr.rbp,
                            (unsigned long long)cpu.gpr.rsp);
                        std::fprintf(stderr,
                            "[exc-ctx] r8=0x%llx r9=0x%llx r10=0x%llx "
                            "r11=0x%llx r12=0x%llx r13=0x%llx r14=0x%llx "
                            "r15=0x%llx\n",
                            (unsigned long long)cpu.gpr.r8,
                            (unsigned long long)cpu.gpr.r9,
                            (unsigned long long)cpu.gpr.r10,
                            (unsigned long long)cpu.gpr.r11,
                            (unsigned long long)cpu.gpr.r12,
                            (unsigned long long)cpu.gpr.r13,
                            (unsigned long long)cpu.gpr.r14,
                            (unsigned long long)cpu.gpr.r15);
                        for (u64 off = 0; off < 0x400; off += 0x20) {
                            u64 word[4];
                            bool ok = true;
                            for (int i = 0; i < 4; ++i) {
                                auto v = memory.read_u64(
                                    static_cast<GuestAddr>(cpu.gpr.rsp + off +
                                                           8u * i));
                                if (!v.is_ok()) { ok = false; break; }
                                word[i] = v.value();
                            }
                            if (!ok) break;
                            std::fprintf(stderr,
                                "[exc-stack] rsp+0x%03llx %016llx %016llx "
                                "%016llx %016llx\n",
                                (unsigned long long)off,
                                (unsigned long long)word[0],
                                (unsigned long long)word[1],
                                (unsigned long long)word[2],
                                (unsigned long long)word[3]);
                        }
                        // A runaway recursion buries its origin: the dump
                        // above covers 1 KiB, while the frames that started it
                        // sit megabytes away at the top of the stack. Walk the
                        // whole stack and name every module-backed return
                        // address, newest first, collapsing the repeating
                        // cycle. Whoever began the recursion is the last
                        // distinct entry.
                    }
                    result.exit_code = static_cast<int>(raise_code);
                    result.success = false;
                    result.error_message = "unhandled guest exception";
                    running = false;
                    break;
                }

                ++count;
                continue;
            }

            // Normal / guard-dispatch: helper already applied RET or rewrote rip.
            ++count;
            continue;
        }
        // Normal instruction execution
        cpu::ExecResult exec_result{};
        bool have_pctrace_result = false;

        // VITREO_PCTRACE single-steps only the selected range. Continue and
        // branch results can immediately resume the dispatch loop, but faults
        // must flow through the normal exception switch below rather than
        // retrying the same instruction forever.
        if (pctrace_active(cpu.rip)) {
            pctrace_capture(cpu, memory);
            exec_result = interp.step();
            ++count;
            if (exec_result == cpu::ExecResult::Continue ||
                exec_result == cpu::ExecResult::Branch) {
                continue;
            }
            have_pctrace_result = true;
            if (exec_result == cpu::ExecResult::Exception) {
                std::FILE* out = pctrace_output();
                std::fprintf(out,
                    "[pctrace] interp.step() exception at rip=%016llX\n",
                    static_cast<unsigned long long>(cpu.rip));
                pctrace_dump_caller(out, cpu, memory);
                std::fflush(out);
            }
        }

        if (trace_ring_enabled) {
            trace_rips[trace_idx & TRACE_MASK] = cpu.rip;
            trace_idx++;
        }

        // ----------------------------------------------------------------
        // JIT dispatch: try to look up and execute a compiled block
        // ----------------------------------------------------------------
        bool jit_handled = false;
        // L3-K: an in-chain crossing saw the process exit (below).
        bool inchain_exited = false;
        cpu::jit::BasicBlock* block = nullptr;
        // Thunk-region addresses are dispatch points and are never compiled;
        // a sled that guest_accel.cpp patched with `jmp rel32` is followed by
        // follow_thunk_sled_jump instead.
        bool jit_active_now = jit_enabled &&
                      !thunk_registry.is_in_thunk_region(cpu.rip) &&
                      !(jit_stop_at > 0 && count >= jit_stop_at) &&
                      !(cpu.rip >= jit_disable_begin &&
                        cpu.rip < jit_disable_end);
        // Log per-block detail in the last 1M instructions before JIT cutoff
        bool jit_detail_zone = jit_stop_at > 0 && jit_active_now &&
                               (jit_stop_at - count) <= 1000000;
        if (jit_detail_zone) {
            VITREO_TRACE("[jit-bisect] count=%llu EIP=0x%08x\n",
                         static_cast<unsigned long long>(count),
                         static_cast<unsigned>(cpu.rip));
        }
        static bool jit_stop_logged = false;
        if (jit_stop_at > 0 && !jit_active_now && !jit_stop_logged) {
            VITREO_TRACE("[jit-bisect] JIT disabled at count=%llu EIP=0x%08x\n",
                         static_cast<unsigned long long>(count),
                         static_cast<unsigned>(cpu.rip));
            jit_stop_logged = true;
        }
        if (!have_pctrace_result && jit_active_now) {
            block = prepare_dispatch_block(
                cpu, memory, block_cache, &jit_compiler,
                thunk_registry);

            if (block) {
                const auto compilation = compile_dispatch_block(*block, &jit_compiler);
                if (compilation.status != DispatchCompileStatus::NotAttempted) {
                    if (profiling) ++jit_compile_attempts;
                    if (compilation.status == DispatchCompileStatus::Published) {
                        if (profiling) ++jit_compile_successes;
                        static u32 s_compiled_trace = 0;
                        const u32 n = ++s_compiled_trace;
                        if (n <= 8u || (n & 0xFFu) == 0u) {
                            VITREO_TRACE("[jit] Compiled block at 0x%08x "
                                         "(%u insns, %u bytes ARM64) (#%u)\n",
                                         static_cast<unsigned>(block->start_addr),
                                         block->instruction_count,
                                         compilation.code_size, n);
                        }
                    } else if (compilation.status == DispatchCompileStatus::Failed &&
                               profiling) {
                        auto& bp = block_profiles[block->start_addr];
                        bp.jit_failed = true;
                        bp.block_insns = block->instruction_count;
                    }
                }

                if (block->compiled_code) {
                    // === JIT Block Chain Dispatch ===
                    // Execute compiled blocks back-to-back without
                    // returning to the main loop. This eliminates
                    // per-block overhead: thunk check, pump check,
                    // block cache lookup, trace write.
                    if (profiling) ++jit_chain_entries;
                    u32 chain_len = 0;
                    // Published segments are immutable. Keep only a local
                    // execution-depth marker so host calls and GUI pumping
                    // retain their chain boundary without contending on the
                    // process-wide emitter epoch.
                    cpu::jit::JitCodeArena::ArenaReadLease chain_lease{
                        cpu::jit::JitCodeArena::ArenaReadLease::StableExecution{}};
                    if (!chain_lease) {
                        (void)jit_compiler.synchronize_arena_generation();
                    } else {
                    bool jit_deopt = false;
                    const bool pctrace_on = pctrace_enabled();
                    // L3: this chain's lease is the one an in-chain crossing
                    // parks, exactly as try_chain_thunk_inline parks it.
                    inchain_env.chain_lease = &chain_lease;
                    struct InChainLeaseLend {
                        InChainThunkEnv& env;
                        ~InChainLeaseLend() { env.chain_lease = nullptr; }
                    } inchain_lease_lend{inchain_env};
                    do {
                        if (trace_ring_enabled) {
                            trace_rips[trace_idx & TRACE_MASK] = cpu.rip;
                            trace_idx++;
                        }
                        // Everything read from `block` after execute() is
                        // snapshotted here: an in-chain crossing inside it
                        // can free this very BasicBlock -- guest code run
                        // from the export (an AGuestComRef callback) through
                        // a nested dispatch, or the export's own write to
                        // translated code through the write observer.
                        const GuestAddr executed_block_start =
                            block->start_addr;
                        const u64 executed_block_insns =
                            block->instruction_count;
                        cpu::jit::CompiledBlock cb =
                            cpu::jit::compiled_from_cache(*block);

                        cpu.jit_iters = 1;
                        auto jresult = jit_compiler.execute(cb, cpu);
                        // Taken on every result so a break never outlives
                        // the execute() that set it.
                        const auto inchain_break =
                            jit_compiler.take_inchain_break();
                        if (jresult == cpu::jit::JitExecResult::StaleCode) {
                            (void)jit_compiler.synchronize_arena_generation();
                            block = nullptr;
                            break;
                        }
                        if (jresult == cpu::jit::JitExecResult::Unsupported) {
                            // A dynamic entry proof rejected this block before
                            // executing a guest instruction. Direct native
                            // links can leave `block` naming the source block,
                            // so force the precise one-step fallback at the
                            // RIP published by the rejecting target.
                            block = nullptr;
                            jit_deopt = true;
                            break;
                        }
                        u64 block_insns = executed_block_insns
                               * static_cast<u64>(cpu.jit_iters);
                        count += block_insns;
                        if (profiling) {
                            jit_insn_count += block_insns;
                            ++jit_chain_blocks;
                            auto& bp = block_profiles[executed_block_start];
                            bp.exec_count += cpu.jit_iters;
                            bp.insn_count += block_insns;
                            bp.block_insns = executed_block_insns;
                            bp.jit_compiled = true;
                        }

                        // L3: an in-chain API crossing ran and must leave the
                        // chain -- what try_chain_thunk_inline answers with
                        // BreakOuter (raise, unwind, transfer, lost lease,
                        // guest code run inside the export) or Fatal, and
                        // every crossing after which state the dispatcher
                        // re-establishes moved (an executable write, a map
                        // change, a retirement), and a process that exited
                        // while the call ran. Ahead of every other use of
                        // `block`: it may name a block a nested dispatch or
                        // the export's own code write freed.
                        if (inchain_break != cpu::jit::InChainBreak::None) {
                            block = nullptr;
                            if (inchain_break == cpu::jit::InChainBreak::Fatal) {
                                result.exit_code = -1;
                                result.error_message =
                                    "Stack underflow during thunk RET";
                                result.success = false;
                            } else if (process_mgr.has_exited()) {
                                // ExitProcess (this thread or another) seen
                                // by the crossing: stop as the dispatcher
                                // stops after a thunk, before the
                                // instruction after the call -- and before
                                // the SEH dispatch of any raise the crossing
                                // left pending (below).
                                result.exit_code =
                                    static_cast<int>(process_mgr.exit_code());
                                result.success = true;
                                running = false;
                                inchain_exited = true;
                            }
                            break;
                        }

                        if (jresult == cpu::jit::JitExecResult::Halt) {
                            result.exit_code = 0;
                            result.success = true;
                            running = false;
                            break;
                        }

                        // A JIT helper raised an exception (AV / INT3 from the
                        // interpreter-delegate): leave the chain so the main
                        // loop dispatches it instead of re-entering the
                        // faulting block.
                        if (cpu.pending_raise.active) {
                            break;
                        }

                        ++chain_len;
                        // A peer thread is in SuspendThread waiting for this
                        // one. Leave the chain -- releasing its arena lease --
                        // so the loop's checkpoint() parks us. Without this a
                        // hot guest loop chains forever, never parks, and the
                        // suspender (a garbage collector stopping the world,
                        // a sampling profiler) hangs with no timeout.
                        if (suspend_guard.suspend_pending()) break;

                        // Heartbeat: when VITREO_HEARTBEAT is set, log per-thread
                        // (tid, current rip, total blocks) every N JIT block
                        // executions. Diagnostic for CPU-bound spin loops where
                        // no API calls are emitted.
                        {
                            static const u64 hb_interval = []() -> u64 {
                                const char* e = std::getenv("VITREO_HEARTBEAT");
                                if (!e || !*e) return 0;
                                u64 v = std::strtoull(e, nullptr, 0);
                                return v ? v : 100000ULL;
                            }();
                            if (hb_interval) {
                                thread_local u64 hb_count = 0;
                                if ((++hb_count % hb_interval) == 0) {
                                    u32 tid = winapi::ThreadManager::get_current_thread_id_static();
                                    fprintf(stderr,
                                        "[heartbeat] T%u rip=0x%llx blocks=%llu (last=0x%x len=%u)\n",
                                        tid,
                                        (unsigned long long)cpu.rip,
                                        (unsigned long long)hb_count,
                                        (unsigned)block->start_addr,
                                        (unsigned)block->byte_length);
                                    fflush(stderr);
                                }
                            }
                        }

                        // Pump events periodically within chain (~60Hz).
                        // Check every 16 blocks so warm ijl15 decode — long
                        // JIT runs with no PeekMessage — still drains AppKit
                        // before the next UnlockRect. Paired with the linked-
                        // hop cap inside emit_linked_tail_continue.
                        // Worker threads have no event pump. They must still
                        // leave a warm chain when another thread needs the
                        // arena: otherwise their lease can close native entry
                        // process-wide for the duration of a guest spin loop.
                        if ((chain_len & 0xFu) == 0 &&
                            cpu::jit::JitCodeArena::arena_reclaim_pending()) {
                            break;
                        }
                        if (event_pump && (chain_len & 0xFu) == 0) {
                            u64 now = mach_absolute_time();
                            if (now - last_pump_time
                                    >= pump_interval_ticks) {
                                // DF/status flags are not needed by AppKit.
                                // Settling here used to force a full FlagOp
                                // materialize on every ~60Hz pump. The pump
                                // is a safepoint like any host call: it can
                                // re-enter this dispatcher and reclaim the
                                // arena, so park the chain lease around it.
                                cpu::jit::JitCodeArena::HostCallSafepoint
                                    pump_safepoint{&chain_lease};
                                pump_safepoint.park();
                                event_pump();
                                last_pump_time = now;
                                // Gate closed, or a slot retired while the
                                // pump ran: `block` may name recycled code.
                                // Leave through the outer dispatcher, which
                                // synchronizes and re-looks-up at RIP.
                                if (pump_safepoint.unpark() !=
                                    cpu::jit::JitCodeArena::HostCallSafepoint::
                                        Resume::Continue) {
                                    break;
                                }
                            }
                        }

                        // A foreign guest thread may have flushed executable
                        // bytes while this chain was running. Self-RIP re-entry
                        // (tight loop that exits to C++) keeps the current
                        // block; otherwise lookup_fast uses last-hit + sticky
                        // before L1/hash (sparse LRU splice on hash hits).
                        (void)jit_compiler.synchronize_executable_writes();
                        jit_compiler.service_cold_residency();
                        if (executed_block_start != cpu.rip) {
                            // API thunks are never JIT-compiled. Handle them
                            // in-chain (fast path + RET) instead of
                            // lookup_fast-miss → outer break.
                            const auto thunk_out = try_chain_thunk_inline(
                                cpu, memory, thunk_registry, marshaler,
                                suspend_guard, fast_thunks, block_cache, block,
                                module_manager(), &chain_lease,
                                slow_hostcall_diag, slow_hostcall_threshold_ms);
                            if (thunk_out == ChainThunkOutcome::Fatal) {
                                result.exit_code = -1;
                                result.error_message =
                                    "Stack underflow during thunk RET";
                                result.success = false;
                                break;
                            }
                            if (thunk_out == ChainThunkOutcome::BreakOuter) {
                                break;
                            }
                            if (thunk_out == ChainThunkOutcome::Handled) {
                                ++count;
                                if (cpu.pending_raise.active) {
                                    break;
                                }
                                // Slow inline / control-transfer may ExitProcess
                                // without pending_raise — mirror the outer thunk
                                // path and leave the chain before more native hops.
                                if (process_mgr.has_exited()) {
                                    result.exit_code =
                                        static_cast<int>(process_mgr.exit_code());
                                    result.success = true;
                                    running = false;
                                    break;
                                }
                                if (!block) {
                                    // Miss on the return RIP — real chain miss.
                                    diagnostics::perf_note_chain_lookup_miss(
                                        cpu.rip);
                                    if (diagnostics::perf_window_active() &&
                                        block_cache.find_covering(cpu.rip)) {
                                        diagnostics::perf_note_chain_miss_mid_block();
                                    }
                                    break;
                                }
                            } else {
                                block = block_cache.lookup_fast(cpu.rip);
                                if (!block) {
                                    diagnostics::perf_note_chain_lookup_miss(
                                        cpu.rip);
                                    if (thunk_registry.is_in_thunk_region(
                                            cpu.rip)) {
                                        // Race: region grew after the early
                                        // check, or RIP landed mid-stub.
                                        if (t_thunk_miss_bytes) {
                                            log_thunk_miss_bytes(cpu, memory,
                                                                 thunk_registry);
                                        }
                                        diagnostics::perf_note_chain_miss_thunk();
                                        if (follow_thunk_sled_jump(
                                                cpu, memory, thunk_registry)) {
                                            ++count;
                                            block = block_cache.lookup_fast(cpu.rip);
                                        }
                                        if (!block) {
                                            maybe_skip_thunk_nop_sled(
                                                cpu, memory, thunk_registry);
                                            break;
                                        }
                                    } else {
                                        if (diagnostics::perf_window_active() &&
                                            block_cache.find_covering(cpu.rip)) {
                                            diagnostics::perf_note_chain_miss_mid_block();
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                        if (pctrace_on && pctrace_overlaps(
                                block->start_addr, block->byte_length)) {
                            break;
                        }

                        // Compile if not yet compiled and hot. A cold
                        // successor leaves the chain uncounted: the
                        // dispatcher interprets it and counts that execution.
                        if (!block->compiled_code) {
                            if (thunk_registry.is_in_thunk_region(
                                    block->start_addr)) {
                                break;
                            }
                            if (block->compile_failed) break;
                            if (!cpu::jit::JitCompiler::
                                    execution_reaches_hot_threshold(*block)) {
                                break;
                            }
                            block->execution_count++;
                            if (block->instructions.size() !=
                                    block->instruction_count &&
                                !ensure_block_decoded(memory, block_cache,
                                                      *block)) {
                                block = nullptr;
                                break;
                            }
                            (void)compile_and_publish_dispatch_block(
                                *block, jit_compiler);
                            if (!block->compiled_code) break;
                        }
                    } while (true);

                    // Leave pending FlagOp intact across JIT→JIT. Interpreter
                    // flag-readers and try_lazy settle on demand. Host thunks
                    // settle at dispatch entry (below). This post-chain settle
                    // was the dominant exclusive materialize call on
                    // GoogleUpdater after M5v3 made the body itself cheap.
                    jit_handled = !jit_deopt;
                    }
                }
            }
        }
        if (inchain_exited) {
            break;
        }

        // ----------------------------------------------------------------
        // Fast path: execute pre-decoded block via interpreter, skipping
        // per-instruction decode / block-cache lookup / thunk check
        // ----------------------------------------------------------------
        const bool have_jit_exception =
            !have_pctrace_result && jit_handled &&
            cpu::consume_pending_helper_raise(
                cpu, interp.context(), exec_result);
        if (jit_handled && !have_jit_exception) {
            continue;
        }

        // A compiled block reaches the interpreter fast path whenever the JIT
        // did not run it: the arena read lease can be refused between an
        // entry's publication and its execution, in which case `jit_handled`
        // stays false with `block` still naming a compiled entry. If that
        // block's decoded array was released, the loop below would index a
        // handle that holds nothing. Rebuild it first; a stale entry (guest
        // bytes changed) is dropped and single-stepped instead.
        if (!have_pctrace_result && !have_jit_exception && block &&
            block->instruction_count > 0 &&
            block->instructions.size() != block->instruction_count) {
            if (!ensure_block_decoded(memory, block_cache, *block)) {
                block = nullptr;
            }
        }

        if (!have_pctrace_result && !have_jit_exception && block &&
            block->instruction_count > 0) {
            // JIT→interpreter (threshold > 1, compile-fail): settle lazy
            // flags so writers that update rflags but forget flag_op cannot
            // leave a stale compare for the next Jcc.
            cpu.materialize_lazy_flags();
            auto& bctx = interp.context();
            exec_result = cpu::ExecResult::Continue;
            u32 block_insns_run = 0;
            for (u32 bi = 0; bi < block->instruction_count; ++bi) {
                const auto& instr = block->instructions[bi];
                bctx.instr_length = instr.length;
                if (instr.prefixes.lock) {
                    cpu::LockPrefixGuard guard;
                    exec_result = cpu::execute_instruction(bctx, instr);
                } else {
                    exec_result = cpu::execute_instruction(bctx, instr);
                }
                ++count;
                ++block_insns_run;

                if (exec_result == cpu::ExecResult::Continue) {
                    cpu.advance_ip(instr.length);
                } else {
                    break;
                }
            }
            diagnostics::perf_add_interp_insns(block_insns_run);
            if (profiling) {
                interp_block_insn_count += block_insns_run;
                const GuestAddr addr = block->start_addr;
                auto& bp = block_profiles[addr];
                bp.exec_count++;
                bp.insn_count += block_insns_run;
                bp.block_insns = block->instruction_count;
            }
            if (exec_result == cpu::ExecResult::Continue ||
                exec_result == cpu::ExecResult::Branch) {
                continue;
            }
            // Exception/Halt/Interrupt — fall through to handler
        } else if (!have_pctrace_result && !have_jit_exception) {
            cpu.materialize_lazy_flags();
            const GuestAddr single_step_rip = cpu.rip;
            exec_result = interp.step();
            ++count;
            diagnostics::perf_add_interp_insns(1);
            if (profiling) {
                ++interp_single_insn_count;
                // A zero-length block decode (or an explicitly forced
                // interpreter path) used to disappear from the block profile,
                // making the hottest single-step RIP impossible to identify.
                // Account it as a one-instruction interpreter block; this is
                // diagnostic state only and does not affect dispatch.
                auto& bp = block_profiles[single_step_rip];
                ++bp.exec_count;
                ++bp.insn_count;
                if (bp.block_insns == 0) bp.block_insns = 1;
            }
        }

        switch (exec_result) {
            case cpu::ExecResult::Continue:
            case cpu::ExecResult::Branch:
                break;

            case cpu::ExecResult::Halt:
                result.exit_code = 0;
                result.success = true;
                running = false;
                break;

            case cpu::ExecResult::Interrupt: {
                const auto& ctx = interp.context();
                VITREO_TRACE("[cpu-int] vector=0x%x rip=0x%llx rax=0x%llx rcx=0x%llx\n",
                    ctx.interrupt_num, static_cast<unsigned long long>(cpu.rip),
                    static_cast<unsigned long long>(cpu.gpr.rax),
                    static_cast<unsigned long long>(cpu.gpr.rcx));
                if (ctx.interrupt_num == 0x80) {
                    result.exit_code = static_cast<int>(cpu.gpr.get32(
                        static_cast<u8>(cpu::RegIndex::RAX)));
                    result.success = true;
                    running = false;
                } else if (ctx.interrupt_num == 0x29) {
                    dump_fastfail_context(cpu, memory, cpu.gpr.rcx);
                    result.exit_code = static_cast<int>(cpu::STATUS_STACK_BUFFER_OVERRUN);
                    result.success = false;
                    result.error_message = "guest fast fail (INT 0x29)";
                    running = false;
                } else if (ctx.interrupt_num == 0x2C && cpu.is_64bit()) {
                    // Learn DbgRaiseAssertionFailure: __int2c raises an
                    // assertion exception; it is not an exit service.
                    const u64 address = cpu.rip;
                    cpu.rip += 2; // software-interrupt resume context
                    if (dispatch_x64_exception(
                            cpu, memory, thunk_registry, process_mgr,
                            cpu::STATUS_ASSERTION_FAILURE, 0, 0, nullptr,
                            address, module_manager())) {
                        ++count;
                        break;
                    }
                    process_mgr.exit_process(cpu::STATUS_ASSERTION_FAILURE);
                    result.exit_code = static_cast<int>(cpu::STATUS_ASSERTION_FAILURE);
                    result.success = false;
                    result.error_message = "unhandled STATUS_ASSERTION_FAILURE (INT 2C)";
                    running = false;
                } else if (ctx.interrupt_num == 3) {
                    // INT 3 → STATUS_BREAKPOINT: dispatch through SEH
                    const u64 breakpoint_addr = ctx.exception_addr;
                    if (cpu.is_64bit()) {
                        // x64: give guest SEH a chance (a debugger-style
                        // handler may continue), otherwise match Windows —
                        // an unhandled breakpoint kills the process with
                        // STATUS_BREAKPOINT. Treating it as a NOP (the old
                        // behavior, VITREO_INT3_NOP=1 to restore) turned
                        // guest abort breakpoints into invisible 100%-CPU
                        // spins instead of diagnosable crashes.
                        dump_int3_context();
                        bool handled = dispatch_x64_exception(
                            cpu, memory, thunk_registry, process_mgr,
                            cpu::STATUS_BREAKPOINT, 0, 0, nullptr,
                            breakpoint_addr, module_manager());
                        if (handled) {
                            ++count;
                            break;
                        }
                        if (vitreo::detail::g_int3_nop) {
                            ++count;
                            break;
                        }
                        std::fprintf(stderr,
                            "[vitreo] unhandled INT3 (STATUS_BREAKPOINT) at rip=0x%llx"
                            " — terminating process (0x80000003)\n",
                            static_cast<unsigned long long>(cpu.rip));
                        vitreo_hostcall_dump("int3-unhandled");
                        result.exit_code = static_cast<int>(0x80000003u);
                        result.success = false;
                        result.error_message = "unhandled STATUS_BREAKPOINT (guest CHECK failure)";
                        running = false;
                        break;
                    }
                    // x86 follows Windows' order: VEH first-chance, the
                    // fs:[0] chain, then the process's unhandled-exception
                    // filter; an unhandled breakpoint ends the process with
                    // STATUS_BREAKPOINT, as on x64 (VITREO_INT3_NOP=1 keeps
                    // the old skip). Handlers see the breakpoint address
                    // (instruction end - 1) as EIP and one parameter, 0
                    // (int3_record_probe.x86.expected).
                    const u32 bp_addr = static_cast<u32>(breakpoint_addr);
                    const u64 trap_eip = cpu.rip;
                    const u32 bp_args[1] = {0};
                    cpu.rip = bp_addr;
                    if (cli::invoke_vectored_handlers_32(
                            cpu, memory, thunk_registry, process_mgr,
                            cpu::STATUS_BREAKPOINT, 0, 1, bp_args, bp_addr)) {
                        ++count;
                        break;
                    }
                    const u32 orig_esp = static_cast<u32>(cpu.gpr.rsp);
                    u32 scratch_esp = orig_esp - (cpu::CTX32_SIZE + cpu::EREC32_SIZE + 16);
                    scratch_esp &= ~0xFu;
                    const u32 ctx_addr = scratch_esp;
                    const u32 erec_addr = scratch_esp + cpu::CTX32_SIZE;
                    const u32 disp_ctx_addr = erec_addr + cpu::EREC32_SIZE;
                    cpu::write_exception_record_32(memory, erec_addr,
                        cpu::STATUS_BREAKPOINT, 0, bp_addr, 1, bp_args);
                    cpu::write_context_32(memory, ctx_addr, cpu);
                    (void)memory.write_u32(disp_ctx_addr, 0);
                    bool handled = false;
                    auto chain_head = cpu::read_seh_chain_head(memory, cpu);
                    if (chain_head.is_ok() && chain_head.value() != 0xFFFFFFFF &&
                        chain_head.value() != 0) {
                        u32 frame_addr = chain_head.value();
                        u32 chain_depth = 0;
                        while (frame_addr != 0xFFFFFFFF && frame_addr != 0 && chain_depth < 256) {
                            if (!cpu::seh32_registration_dispatchable(
                                    memory, cpu, frame_addr)) {
                                std::fprintf(stderr,
                                    "[seh32] registration 0x%08x is outside "
                                    "the thread stack (depth=%u) — chain "
                                    "corrupt, stopping dispatch\n",
                                    frame_addr, chain_depth);
                                break;
                            }
                            auto handler_info = cpu::read_seh_handler(memory, frame_addr);
                            if (handler_info.is_err()) break;
                            auto info = handler_info.value();
                            if (!seh_handler_looks_valid(memory, info.handler_addr)) {
                                std::fprintf(stderr,
                                    "[seh32] INT3: skipping bogus handler 0x%llx (frame=0x%08x depth=%u) — chain may be corrupt\n",
                                    static_cast<unsigned long long>(info.handler_addr), info.frame_addr, chain_depth);
                                frame_addr = info.next_frame;
                                ++chain_depth;
                                continue;
                            }
                            auto disposition = invoke_seh32_frame_handler(
                                cpu, memory, thunk_registry, process_mgr,
                                info.handler_addr, info.frame_addr,
                                erec_addr, ctx_addr, disp_ctx_addr, scratch_esp);
                            if (disposition.disposition == cpu::ExceptionContinueExecution) {
                                if (!disposition.transferred) {
                                    (void)cli::resume_from_context_32(cpu, memory, thunk_registry, process_mgr,
                                                                 erec_addr, ctx_addr, scratch_esp);
                                }
                                handled = true;
                                break;
                            }
                            frame_addr = info.next_frame;
                            ++chain_depth;
                        }
                    }
                    if (!handled && !process_mgr.has_exited()) {
                        handled = cli::invoke_unhandled_exception_filter_32(
                            cpu, memory, thunk_registry, process_mgr,
                            erec_addr, ctx_addr, scratch_esp);
                    }
                    if (process_mgr.has_exited()) {
                        result.exit_code = static_cast<int>(process_mgr.exit_code());
                        result.success = true;
                        running = false;
                        break;
                    }
                    if (handled) {
                        ++count;
                        break;
                    }
                    // Handler probes run through callbacks and can clobber
                    // registers; restore the trap state before giving up.
                    (void)cpu::read_context_32(memory, ctx_addr, cpu);
                    cpu.rip = trap_eip;
                    if (vitreo::detail::g_int3_nop) {
                        ++count;
                        break;
                    }
                    std::fprintf(stderr,
                        "[vitreo] unhandled INT3 (STATUS_BREAKPOINT) at eip=0x%08x"
                        " — terminating process (0x80000003)\n", bp_addr);
                    vitreo_hostcall_dump("int3-unhandled");
                    result.exit_code = static_cast<int>(0x80000003u);
                    result.success = false;
                    result.error_message = "unhandled STATUS_BREAKPOINT (guest CHECK failure)";
                    running = false;
                } else if (ctx.interrupt_num == 0x2E && !cpu.is_64bit()) {
                    // INT 2Eh (KiSystemService): 32-bit ntdll native-service
                    // entry before SYSENTER. EAX = native service number;
                    // stdcall args sit on the user stack ([ESP] = return to
                    // the NtXxx caller, [ESP+4] = arg0). Intel SDM Vol. 2
                    // INT n does not advance RIP; kernel IRET returns to the
                    // next instruction. Vitreo exec_int leaves RIP at CD 2E.
                    cpu.rip += 2;
                    if (dispatch_native_service(
                            cpu, thunk_registry, marshaler, suspend_guard) ==
                        NativeServiceDispatch::UnknownService) {
                        result.exit_code = -1;
                        result.error_message =
                            "Unknown x86 INT 2E NT service number 0x" +
                            to_hex_string(cpu.gpr.get32(
                                static_cast<u8>(cpu::RegIndex::RAX)));
                        result.success = false;
                        running = false;
                    }
                } else {
                    result.exit_code = -1;
                    result.error_message = "Unhandled interrupt: " +
                        std::to_string(ctx.interrupt_num);
                    result.success = false;
                    running = false;
                }
                break;
            }

            case cpu::ExecResult::Exception: {
                const auto& ectx = interp.context();

                // Intel SDM Vol. 3A: a non-canonical linear address raises
                // #GP in 64-bit mode. Windows exposes invalid user-memory
                // reads/writes to the application as
                // EXCEPTION_ACCESS_VIOLATION, including the access type and
                // fault VA in ExceptionInformation. Direct interpreter/JIT
                // exceptions must enter the same x64 VEH/.pdata dispatcher as
                // RaiseException and helper-originated faults; falling through
                // to the fatal path bypasses the guest's guarded probes.
                if (cpu.is_64bit()) {
                    const u32 status_code =
                        cpu::cpu_exception_to_status(ectx.exception);
                    const u64 params[2] = {
                        static_cast<u64>(ectx.exception_error),
                        ectx.exception_addr};
                    const u32 num_params =
                        status_code == cpu::STATUS_ACCESS_VIOLATION ? 2u : ectx.exception_num_params;
                    if (dispatch_x64_exception(
                            cpu, memory, thunk_registry, process_mgr,
                            status_code, 0, num_params,
                            num_params != 0 ? params : nullptr,
                            cpu.rip, module_manager())) {
                        if (process_mgr.has_exited()) {
                            result.exit_code = static_cast<int>(
                                process_mgr.exit_code());
                            result.success = true;
                            running = false;
                        } else {
                            ++count;
                        }
                        break;
                    }
                    // Windows terminates an x64 process whose exception walk
                    // returns unhandled with the translated status. Do not
                    // fall through to the generic CpuException fatal path:
                    // that loses STATUS_ACCESS_VIOLATION and mislabels an
                    // ordinary invalid-memory read as GeneralProtection.
                    // ExceptionInformation[0]/[1] are the whole diagnosis for
                    // an access violation: without the access type and fault
                    // VA the report cannot distinguish a bad pointer from a
                    // bad RIP.
                    std::fprintf(stderr,
                        "[vitreo] unhandled exception 0x%08x at rip=0x%llx"
                        " vector=%u access=%llu fault_va=0x%llx"
                        " — terminating process\n",
                        status_code,
                        static_cast<unsigned long long>(cpu.rip),
                        static_cast<unsigned>(ectx.exception),
                        static_cast<unsigned long long>(params[0]),
                        static_cast<unsigned long long>(params[1]));
                    dump_fatal_guest_context(cpu, memory);
                    cpu::jit::LinearFaultDetail lf{};
                    if (cpu::jit::jit_last_linear_fault(lf)) {
                        std::fprintf(stderr,
                            "[exc-linear] host_pc=0x%llx host_addr=0x%llx "
                            "win_base=0x%llx guest_addr=0x%llx "
                            "attributed_rip=0x%llx metadata=%d write=%d\n",
                            (unsigned long long)lf.host_pc,
                            (unsigned long long)lf.host_addr,
                            (unsigned long long)lf.window_base,
                            (unsigned long long)lf.guest_addr,
                            (unsigned long long)lf.guest_rip,
                            lf.have_metadata ? 1 : 0, lf.is_write ? 1 : 0);
                    }
                    // The guest RIP alone cannot name the Vitreo path that
                    // synthesised the fault, and a fatal exception leaves no
                    // second chance to ask.
                    {
                        void* frames[48];
                        const int frame_count = ::backtrace(frames, 48);
                        std::fflush(stderr);
                        ::backtrace_symbols_fd(frames, frame_count, 2);
                    }
                    hostcall_dump("unhandled x64 CPU exception");
                    process_mgr.exit_process(status_code);
                    result.exit_code = static_cast<int>(status_code);
                    result.success = false;
                    result.error_message = "unhandled guest exception";
                    running = false;
                    break;
                }

                // Interpreter AVs used to be silent unless VITREO_SEH_DIAG was
                // set, so a guest filter that then ExitProcess(0xC0000005)
                // left no faulting RIP. Always log access / GP faults.
                if (ectx.exception == cpu::CpuException::PageFault ||
                    ectx.exception == cpu::CpuException::GeneralProtection ||
                    std::getenv("VITREO_SEH_DIAG")) {
                    GuestAddr region_base = 0;
                    usize region_size = 0;
                    MemoryProtection region_prot{};
                    const bool fault_mapped = memory.query_region(
                        static_cast<GuestAddr>(ectx.exception_addr),
                        region_base, region_size, region_prot);
                    GuestAddr near_base = 0;
                    usize near_size = 0;
                    MemoryProtection near_prot{};
                    const bool near_mapped = !fault_mapped && memory.query_region(
                        static_cast<GuestAddr>(ectx.exception_addr + 0x400),
                        near_base, near_size, near_prot);
                    u32 seh_head = 0;
                    if (auto head = cpu::read_seh_chain_head(memory, cpu);
                        head.is_ok()) {
                        seh_head = head.value();
                    }
                    std::fprintf(stderr,
                        "[seh32] FAULT %s rip=0x%08x fault_addr=0x%llx "
                        "eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x "
                        "ebp=%08x esp=%08x seh=0x%08x",
                        exception_name(ectx.exception),
                        static_cast<unsigned>(cpu.rip),
                        static_cast<unsigned long long>(ectx.exception_addr),
                        static_cast<unsigned>(cpu.gpr.get32(0)),
                        static_cast<unsigned>(cpu.gpr.get32(3)),
                        static_cast<unsigned>(cpu.gpr.get32(1)),
                        static_cast<unsigned>(cpu.gpr.get32(2)),
                        static_cast<unsigned>(cpu.gpr.get32(6)),
                        static_cast<unsigned>(cpu.gpr.get32(7)),
                        static_cast<unsigned>(cpu.gpr.get32(5)),
                        static_cast<unsigned>(cpu.gpr.get32(4)),
                        seh_head);
                    if (fault_mapped) {
                        std::fprintf(stderr,
                            " region=0x%llx+0x%zx prot=0x%x",
                            static_cast<unsigned long long>(region_base),
                            region_size,
                            static_cast<unsigned>(region_prot));
                    } else if (near_mapped) {
                        std::fprintf(stderr,
                            " unmapped next=0x%llx+0x%zx",
                            static_cast<unsigned long long>(near_base),
                            near_size);
                    } else {
                        std::fprintf(stderr, " unmapped");
                    }
                    GuestAddr edi_base = 0;
                    usize edi_size = 0;
                    MemoryProtection edi_prot{};
                    const u32 edi32 = static_cast<u32>(cpu.gpr.get32(7));
                    if (memory.query_region(static_cast<GuestAddr>(edi32),
                                            edi_base, edi_size, edi_prot)) {
                        const u64 edi_off = static_cast<u64>(edi32) - edi_base;
                        std::fprintf(stderr,
                            " edi_region=0x%llx+0x%zx prot=0x%x edi_off=0x%llx\n",
                            static_cast<unsigned long long>(edi_base),
                            edi_size,
                            static_cast<unsigned>(edi_prot),
                            static_cast<unsigned long long>(edi_off));
                    } else {
                        std::fprintf(stderr, " edi_region=unmapped\n");
                    }
                    // The faulting instruction's own bytes: a guest whose code
                    // was decrypted, patched, or overwritten in place cannot be
                    // disassembled from the on-disk image, so print what the
                    // CPU actually saw.
                    {
                        std::fprintf(stderr, "[seh32] code@rip:");
                        for (u32 i = 0; i < 16; ++i) {
                            auto b = memory.read_u8(
                                static_cast<GuestAddr>(cpu.rip) + i);
                            if (b.is_ok()) {
                                std::fprintf(stderr, " %02x",
                                             static_cast<unsigned>(b.value()));
                            } else {
                                std::fprintf(stderr, " ??");
                            }
                        }
                        std::fprintf(stderr, "\n");
                    }
                    std::fflush(stderr);
                }

                // Try 32-bit SEH dispatch for PE32 executables
                if (!cpu.is_64bit()) {
                    // Microsoft Learn: vectored handlers observe first-chance
                    // hardware exceptions before frame-based dispatch, even
                    // when FS:[0] contains no registration. Keep interpreter
                    // faults consistent with callback faults and RaiseException.
                    const u32 status_code =
                        cpu::cpu_exception_to_status(ectx.exception);
                    const u32 params[2] = {
                        ectx.exception_error,
                        static_cast<u32>(ectx.exception_addr),
                    };
                    const u32 num_params =
                        status_code == cpu::STATUS_ACCESS_VIOLATION ? 2u : ectx.exception_num_params;
                    if (cli::invoke_vectored_handlers_32(
                            cpu, memory, thunk_registry, process_mgr,
                            status_code, 0, num_params, params,
                            static_cast<u32>(cpu.rip))) {
                        ++count;
                        continue;
                    }

                    // Read SEH chain head from FS:[0]. An empty chain walks no
                    // frames but still reaches the top-level filter, as the
                    // RaiseException path does: on Windows the thread-start
                    // frame's filter is UnhandledExceptionFilter
                    // (vectored_continue_probe top_filter_continues).
                    auto chain_head = cpu::read_seh_chain_head(memory, cpu);
                    if (chain_head.is_ok()) {
                        // Map CPU exception to Win32 status code
                        u32 status_code = cpu::cpu_exception_to_status(ectx.exception);
                        
                        // Allocate scratch space on guest stack for EXCEPTION_RECORD + CONTEXT + DISPATCHER_CONTEXT
                        // Layout: [CONTEXT (0x2CC)] [EXCEPTION_RECORD (0x50)] [DISPATCHER_CONTEXT (4)] [padding]
                        u32 orig_esp = static_cast<u32>(cpu.gpr.rsp);
                        u32 scratch_esp = orig_esp - (cpu::CTX32_SIZE + cpu::EREC32_SIZE + 16);
                        scratch_esp &= ~0xF;  // 16-byte align
                        
                        u32 ctx_addr = scratch_esp;
                        u32 erec_addr = scratch_esp + cpu::CTX32_SIZE;
                        u32 disp_ctx_addr = erec_addr + cpu::EREC32_SIZE;
                        
                        // Write EXCEPTION_RECORD. For ACCESS_VIOLATION, Windows
                        // sets ExceptionInformation[0]=access type and [1]=VA.
                        u32 params[2] = {
                            ectx.exception_error,
                            static_cast<u32>(ectx.exception_addr)};
                        u32 num_params = (status_code == cpu::STATUS_ACCESS_VIOLATION) ? 2u : ectx.exception_num_params;
                        auto wr_erec = cpu::write_exception_record_32(memory, erec_addr,
                            status_code, 0, static_cast<u32>(cpu.rip), num_params, params);
                        
                        // Write CONTEXT with current CPU state  
                        auto wr_ctx = cpu::write_context_32(memory, ctx_addr, cpu);
                        
                        // Write dispatcher context (just a zero DWORD for now)
                        (void)memory.write_u32(disp_ctx_addr, 0);
                        
                        if (wr_erec.is_ok() && wr_ctx.is_ok()) {
                            // Walk the SEH chain
                            u32 frame_addr = chain_head.value();
                            bool handled = false;
                            u32 chain_depth = 0;
                            constexpr u32 MAX_SEH_CHAIN = 256;
                            
                            while (frame_addr != 0xFFFFFFFF && frame_addr != 0 && chain_depth < MAX_SEH_CHAIN) {
                                if (!cpu::seh32_registration_dispatchable(
                                        memory, cpu, frame_addr)) {
                                    std::fprintf(stderr,
                                        "[seh32] registration 0x%08x is outside "
                                        "the thread stack (depth=%u) — chain "
                                        "corrupt, stopping dispatch\n",
                                        frame_addr, chain_depth);
                                    break;
                                }
                                auto handler_info = cpu::read_seh_handler(memory, frame_addr);
                                if (handler_info.is_err()) break;
                                
                                auto info = handler_info.value();
                                
                                if (!seh_handler_looks_valid(memory, info.handler_addr)) {
                                    std::fprintf(stderr,
                                        "[seh32] Exception %s: skipping bogus handler 0x%llx (frame=0x%08x depth=%u) — chain may be corrupt\n",
                                        exception_name(ectx.exception),
                                        static_cast<unsigned long long>(info.handler_addr), info.frame_addr, chain_depth);
                                    frame_addr = info.next_frame;
                                    ++chain_depth;
                                    continue;
                                }

                                VITREO_TRACE( "[seh32] Exception %s at EIP=0x%08x, calling handler at 0x%08x (frame=0x%08x)\n",
                                             exception_name(ectx.exception), static_cast<unsigned>(cpu.rip),
                                             info.handler_addr, info.frame_addr);

                                // Snapshot fault GPRs before the handler runs so we can
                                // detect a true resume-loop (ContinueExecution with an
                                // unmodified CONTEXT). Packers often fix a register and
                                // resume the same EIP — that is not a Vitreo bug.
                                const u32 fault_eip = static_cast<u32>(cpu.rip);
                                const u32 fault_eax = static_cast<u32>(cpu.gpr.rax);
                                const u32 fault_ecx = static_cast<u32>(cpu.gpr.rcx);
                                const u32 fault_edx = static_cast<u32>(cpu.gpr.rdx);

                                // handler(pExceptionRecord, pEstablisherFrame,
                                //         pContextRecord, pDispatcherContext)
                                // under nested FS:[0] (RtlpExecuteHandlerForException).
                                auto disposition = invoke_seh32_frame_handler(
                                    cpu, memory, thunk_registry, process_mgr,
                                    info.handler_addr, info.frame_addr,
                                    erec_addr, ctx_addr, disp_ctx_addr, scratch_esp);

                                // If the handler called RtlUnwind, it already set up the
                                // CPU state for the __except target — just mark handled.
                                if (apply_pending_unwind_32(cpu, memory, ctx_addr,
                                                            "seh32/fault")) {
                                    handled = true;
                                    break;
                                }

                                VITREO_TRACE( "[seh32] Handler returned disposition=%llu\n",
                                             static_cast<unsigned long long>(disposition.disposition));

                                if (disposition.disposition == cpu::ExceptionContinueExecution) {
                                    // Restore CPU state from (possibly modified) CONTEXT
                                    if (!disposition.transferred) {
                                        (void)cli::resume_from_context_32(cpu, memory, thunk_registry, process_mgr,
                                                                     erec_addr, ctx_addr, scratch_esp);
                                    }
                                    handled = true;

                                    // Warn only when resume state matches the fault —
                                    // identical EIP+GPRs will re-raise forever.
                                    const bool unchanged =
                                        static_cast<u32>(cpu.rip) == fault_eip &&
                                        static_cast<u32>(cpu.gpr.rax) == fault_eax &&
                                        static_cast<u32>(cpu.gpr.rcx) == fault_ecx &&
                                        static_cast<u32>(cpu.gpr.rdx) == fault_edx;
                                    if (unchanged) {
                                        static int s_seh_loop_warns = 0;
                                        if (s_seh_loop_warns < 16) {
                                            std::fprintf(stderr,
                                                "[seh32] WARNING: SEH ContinueExecution left "
                                                "%s state unchanged at EIP=0x%08x "
                                                "(eax=%08x ecx=%08x edx=%08x) — likely resume loop\n",
                                                exception_name(ectx.exception),
                                                fault_eip, fault_eax, fault_ecx, fault_edx);
                                            ++s_seh_loop_warns;
                                        }
                                    } else {
                                        VITREO_TRACE(
                                            "[seh32] SEH caught %s at EIP=0x%08x, "
                                            "resuming at EIP=0x%08x (CONTEXT updated)\n",
                                            exception_name(ectx.exception), fault_eip,
                                            static_cast<unsigned>(cpu.rip));
                                    }
                                    break;
                                } else if (disposition.disposition == cpu::ExceptionNestedException) {
                                    // Windows sets EXCEPTION_NESTED_CALL and may
                                    // advance NestedFrame from DispatcherContext.
                                    // Continue searching; nested faults during the
                                    // handler already ran the protector.
                                    frame_addr = info.next_frame;
                                    ++chain_depth;
                                } else if (disposition.disposition == cpu::ExceptionContinueSearch) {
                                    frame_addr = info.next_frame;
                                    ++chain_depth;
                                } else {
                                    // Unknown disposition - treat as continue search
                                    frame_addr = info.next_frame;
                                    ++chain_depth;
                                }
                            }
                            
                            if (handled) {
                                break;  // Resume execution — don't terminate
                            }

                            // Preserve the contract traffic that led to the
                            // fault before the guest's top-level crash handler
                            // replaces the ring tail with reporting work.
                            hostcall_dump("before unhandled exception filter");
                            if (invoke_unhandled_exception_filter_32(
                                    cpu, memory, thunk_registry, process_mgr,
                                    erec_addr, ctx_addr, scratch_esp)) {
                                handled = true;
                                break;
                            }
                            
                            // No handler caught it — fall through to crash dump
                            VITREO_TRACE( "[seh32] No handler caught exception 0x%08x at EIP=0x%08x\n",
                                         status_code, static_cast<unsigned>(cpu.rip));
                        }
                    }

                    // SEH chain exhausted or empty — mimic Windows behavior:
                    // When UnhandledExceptionFilter has no custom filter, the OS
                    // terminates the process with the exception code.  This avoids
                    // a fatal crash dump for expected guest failures (e.g. protobuf
                    // CHECK failures that dereference NULL during error handling).
                    dump_trace();
                    u32 term_code = cpu::cpu_exception_to_status(ectx.exception);
                    std::fprintf(stderr,
                        "[vitreo] Unhandled %s at EIP=0x%08x fault_addr=0x%08x "
                        "— terminating guest (exit code 0x%08x)\n",
                        exception_name(ectx.exception),
                        static_cast<unsigned>(cpu.rip),
                        static_cast<unsigned>(ectx.exception_addr),
                        term_code);
                    // Hex dump 16 bytes at EIP for triage of decoder failures.
                    {
                        u8 ibuf[16] = {};
                        for (int bi = 0; bi < 16; ++bi) {
                            auto rb = memory.read_u8(static_cast<GuestAddr>(cpu.rip + bi));
                            ibuf[bi] = rb.is_ok() ? rb.value() : 0;
                        }
                        std::fprintf(stderr,
                            "[vitreo] Bytes at EIP: %02x %02x %02x %02x %02x %02x %02x %02x "
                            "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                            ibuf[0],ibuf[1],ibuf[2],ibuf[3],ibuf[4],ibuf[5],ibuf[6],ibuf[7],
                            ibuf[8],ibuf[9],ibuf[10],ibuf[11],ibuf[12],ibuf[13],ibuf[14],ibuf[15]);
                    }
                    std::fprintf(stderr,
                        "[vitreo] Register state: EAX=%08x ECX=%08x EDX=%08x EBX=%08x "
                        "ESP=%08x EBP=%08x ESI=%08x EDI=%08x\n",
                        static_cast<unsigned>(cpu.gpr.rax),
                        static_cast<unsigned>(cpu.gpr.rcx),
                        static_cast<unsigned>(cpu.gpr.rdx),
                        static_cast<unsigned>(cpu.gpr.rbx),
                        static_cast<unsigned>(cpu.gpr.rsp),
                        static_cast<unsigned>(cpu.gpr.rbp),
                        static_cast<unsigned>(cpu.gpr.rsi),
                        static_cast<unsigned>(cpu.gpr.rdi));
                    dump_guest_fault_summary(
                        cpu, memory, thunk_registry, module_manager(),
                        ectx.exception);
                    dump_control_flow_fault_context(cpu, memory);
                    hostcall_dump("unhandled x86 guest exception");
                    vitreo_pctrace_dump_ring("unhandled-x86-fault");
                    process_mgr.exit_process(term_code);
                    result.exit_code = static_cast<int>(term_code);
                    result.success = false;
                    running = false;
                    break;
                }
                
                // Unhandled exception — dump and terminate
                dump_trace();
                hostcall_dump("unhandled CPU exception (run_loop)");
                result.exit_code = -1;
                result.error_message = "CPU exception at RIP=0x" +
                    to_hex_string(cpu.rip);
                result.success = false;
                running = false;
                // Always dump register state on CPU exception
                if (cpu.is_64bit()) {
                    std::fprintf(stderr,
                                 "[vitreo] FATAL: Unhandled %s at RIP=0x%016llx\n",
                                 exception_name(ectx.exception),
                                 static_cast<unsigned long long>(cpu.rip));
                } else {
                    std::fprintf(stderr, "[vitreo] FATAL: Unhandled %s at EIP=0x%08x\n",
                                 exception_name(ectx.exception),
                                 static_cast<unsigned>(cpu.rip));
                }
                {
                    std::fprintf(stderr, "[vitreo] Exception type: %s (%u)\n",
                                 exception_name(ectx.exception),
                                 static_cast<unsigned>(ectx.exception));
                }
                if (cpu.is_64bit()) {
                    std::fprintf(stderr,
                        "[vitreo] Register dump at exception:\n"
                        "  RAX=%016llx  RCX=%016llx  RDX=%016llx  RBX=%016llx\n"
                        "  RSP=%016llx  RBP=%016llx  RSI=%016llx  RDI=%016llx\n"
                        "  R8 =%016llx  R9 =%016llx  R10=%016llx  R11=%016llx\n"
                        "  R12=%016llx  R13=%016llx  R14=%016llx  R15=%016llx\n"
                        "  RIP=%016llx\n",
                        static_cast<unsigned long long>(cpu.gpr.rax),
                        static_cast<unsigned long long>(cpu.gpr.rcx),
                        static_cast<unsigned long long>(cpu.gpr.rdx),
                        static_cast<unsigned long long>(cpu.gpr.rbx),
                        static_cast<unsigned long long>(cpu.gpr.rsp),
                        static_cast<unsigned long long>(cpu.gpr.rbp),
                        static_cast<unsigned long long>(cpu.gpr.rsi),
                        static_cast<unsigned long long>(cpu.gpr.rdi),
                        static_cast<unsigned long long>(cpu.gpr.r8),
                        static_cast<unsigned long long>(cpu.gpr.r9),
                        static_cast<unsigned long long>(cpu.gpr.r10),
                        static_cast<unsigned long long>(cpu.gpr.r11),
                        static_cast<unsigned long long>(cpu.gpr.r12),
                        static_cast<unsigned long long>(cpu.gpr.r13),
                        static_cast<unsigned long long>(cpu.gpr.r14),
                        static_cast<unsigned long long>(cpu.gpr.r15),
                        static_cast<unsigned long long>(cpu.rip));
                } else {
                    std::fprintf(stderr,
                        "[vitreo] Register dump at exception:\n"
                        "  EAX=%08x  ECX=%08x  EDX=%08x  EBX=%08x\n"
                        "  ESP=%08x  EBP=%08x  ESI=%08x  EDI=%08x\n"
                        "  EIP=%08x\n",
                        static_cast<unsigned>(cpu.gpr.rax),
                        static_cast<unsigned>(cpu.gpr.rcx),
                        static_cast<unsigned>(cpu.gpr.rdx),
                        static_cast<unsigned>(cpu.gpr.rbx),
                        static_cast<unsigned>(cpu.gpr.rsp),
                        static_cast<unsigned>(cpu.gpr.rbp),
                        static_cast<unsigned>(cpu.gpr.rsi),
                        static_cast<unsigned>(cpu.gpr.rdi),
                        static_cast<unsigned>(cpu.rip));
                }

                // Dump instruction bytes at EIP/RIP
                {
                    u8 bytes[16] = {};
                    auto rd = memory.read_bytes(cpu.rip, bytes, sizeof(bytes));
                    if (rd.is_ok()) {
                        std::fprintf(stderr, "[vitreo] Bytes at RIP: ");
                        for (u8 b : bytes) {
                            std::fprintf(stderr, "%02x ", static_cast<unsigned>(b));
                        }
                        std::fprintf(stderr, "\n");
                    } else {
                        std::fprintf(stderr, "[vitreo] Bytes at RIP: <unmapped>\n");
                    }
                }

                // Dump stack
                {
                    if (cpu.is_64bit()) {
                        std::fprintf(stderr, "[vitreo] Stack dump (RSP=0x%016llx):\n",
                                     static_cast<unsigned long long>(cpu.gpr.rsp));
                        u64 rsp = cpu.gpr.rsp;
                        u64 start = (rsp >= 64) ? rsp - 64 : 0;
                        for (int i = 0; i < 24; ++i) {
                            u64 addr = start + static_cast<u64>(i) * 8;
                            auto val = memory.read_u64(addr);
                            const char* marker = (addr == rsp) ? " <-- RSP" : "";
                            if (val.is_ok()) {
                                std::fprintf(stderr, "  [0x%016llx] = 0x%016llx%s\n",
                                             static_cast<unsigned long long>(addr),
                                             static_cast<unsigned long long>(val.value()),
                                             marker);
                            } else {
                                std::fprintf(stderr, "  [0x%016llx] = <unmapped>%s\n",
                                             static_cast<unsigned long long>(addr), marker);
                            }
                        }
                    } else {
                        std::fprintf(stderr, "[vitreo] Stack dump (ESP=0x%08x):\n",
                                     static_cast<unsigned>(cpu.gpr.rsp));
                        u32 esp = static_cast<u32>(cpu.gpr.rsp);
                        u32 start = (esp >= 64) ? esp - 64 : 0;
                        for (int i = 0; i < 32; ++i) {
                            u32 addr = start + static_cast<u32>(i) * 4;
                            auto val = memory.read_u32(addr);
                            const char* marker = (addr == esp) ? " <-- ESP" : "";
                            if (val.is_ok()) {
                                std::fprintf(stderr, "  [0x%08x] = 0x%08x%s\n",
                                             addr, val.value(), marker);
                            } else {
                                std::fprintf(stderr, "  [0x%08x] = <unmapped>%s\n",
                                             addr, marker);
                            }
                        }
                    }
                }

                // Probe faulting address - check common register-based addressing
                {
                    auto probe_reg = [&](const char* name, u64 val) {
                        u32 v = static_cast<u32>(val);
                        bool m = memory.is_mapped(static_cast<GuestAddr>(v), 4);
                        std::fprintf(stderr, "  [fault probe] %s=0x%08x mapped=%s\n",
                                     name, v, m ? "true" : "false");
                    };
                    probe_reg("EAX", cpu.gpr.rax);
                    probe_reg("ECX", cpu.gpr.rcx);
                    probe_reg("EDX", cpu.gpr.rdx);
                    probe_reg("EBX", cpu.gpr.rbx);
                    probe_reg("EDI", cpu.gpr.rdi);
                    probe_reg("ESI", cpu.gpr.rsi);
                }

                // 64-bit: dump memory near key pointer registers to help diagnose
                // struct corruption causing the fault.
                if (cpu.is_64bit()) {
                    auto dump_region = [&](const char* name, u64 addr) {
                        if (addr == 0 || !memory.is_mapped(static_cast<GuestAddr>(addr), 8)) return;
                        std::fprintf(stderr, "[vitreo] Memory near %s=0x%016llx:\n",
                                     name, static_cast<unsigned long long>(addr));
                        u64 base = addr & ~static_cast<u64>(0x1F);
                        for (int i = 0; i < 8; ++i) {
                            u64 a = base + static_cast<u64>(i) * 8;
                            auto v = memory.read_u64(a);
                            if (v.is_ok()) {
                                std::fprintf(stderr, "  [0x%016llx] = 0x%016llx%s\n",
                                             static_cast<unsigned long long>(a),
                                             static_cast<unsigned long long>(v.value()),
                                             (a == (addr & ~static_cast<u64>(7))) ? " <-- target" : "");
                            } else {
                                std::fprintf(stderr, "  [0x%016llx] = <unmapped>\n",
                                             static_cast<unsigned long long>(a));
                            }
                        }
                    };
                    dump_region("RCX", cpu.gpr.rcx);
                    dump_region("RDX", cpu.gpr.rdx);
                    dump_region("R8",  cpu.gpr.r8);
                    dump_region("R10", cpu.gpr.r10);
                    dump_region("R11", cpu.gpr.r11);
                }

                // Walk EBP chain to reconstruct call stack
                if (!cpu.is_64bit()) {
                    std::fprintf(stderr, "[vitreo] Call stack (EBP chain walk):\n");
                    u32 frame = static_cast<u32>(cpu.gpr.rbp);
                    for (int depth = 0; depth < 32 && frame != 0; ++depth) {
                        auto ret_addr = memory.read_u32(frame + 4);
                        auto saved_ebp = memory.read_u32(frame);
                        if (ret_addr.is_err() || saved_ebp.is_err()) {
                            std::fprintf(stderr, "  [%2d] EBP=0x%08x <unmapped>\n", depth, frame);
                            break;
                        }
                        u32 ret = ret_addr.value();
                        u32 next_ebp = saved_ebp.value();
                        bool ret_mapped = memory.is_mapped(static_cast<GuestAddr>(ret), 1);
                        if (thunk_registry.is_in_thunk_region(ret)) {
                            std::string name = thunk_registry.resolve_name(ret);
                            std::fprintf(stderr, "  [%2d] EBP=0x%08x  RET=0x%08x  %s\n",
                                         depth, frame, ret, name.c_str());
                        } else {
                            std::fprintf(stderr, "  [%2d] EBP=0x%08x  RET=0x%08x%s\n",
                                         depth, frame, ret,
                                         ret_mapped ? "" : " <unmapped>");
                        }
                        if (next_ebp <= frame) break;  // prevent infinite loops
                        frame = next_ebp;
                    }
                }
                break;
            }

            case cpu::ExecResult::Syscall:
                if (dispatch_native_service(
                        cpu, thunk_registry, marshaler, suspend_guard) ==
                    NativeServiceDispatch::UnknownService) {
                    result.exit_code = -1;
                    result.error_message =
                        cpu.is_64bit()
                            ? ("Unknown x64 NT service number 0x" +
                               to_hex_string(cpu.gpr.get32(
                                   static_cast<u8>(cpu::RegIndex::RAX))))
                            : ("Unknown x86 SYSENTER NT service number 0x" +
                               to_hex_string(cpu.gpr.get32(
                                   static_cast<u8>(cpu::RegIndex::RAX))));
                    result.success = false;
                    running = false;
                }
                break;

            case cpu::ExecResult::IOPort:
                result.exit_code = -1;
                result.error_message = "Unhandled I/O port access";
                result.success = false;
                running = false;
                break;
        }
    }

    result.instructions_executed = count;

    // ====================================================================
    // Profiling report (VITREO_PROFILE=1)
    // ====================================================================
    if (profiling) {
        fprintf(stderr, "\n========== VITREO PROFILE REPORT ==========\n");
        fprintf(stderr, "Total instructions:        %llu\n", (unsigned long long)count);
        fprintf(stderr, "  JIT-executed:            %llu (%.1f%%)\n",
                (unsigned long long)jit_insn_count,
                count ? 100.0 * jit_insn_count / count : 0.0);
        fprintf(stderr, "  Interpreter (block):     %llu (%.1f%%)\n",
                (unsigned long long)interp_block_insn_count,
                count ? 100.0 * interp_block_insn_count / count : 0.0);
        fprintf(stderr, "  Interpreter (single):    %llu (%.1f%%)\n",
                (unsigned long long)interp_single_insn_count,
                count ? 100.0 * interp_single_insn_count / count : 0.0);
        fprintf(stderr, "  Thunk calls:             %llu\n", (unsigned long long)thunk_count);
        fprintf(stderr, "\nJIT chain dispatch:\n");
        fprintf(stderr, "  Chain entries:           %llu\n", (unsigned long long)jit_chain_entries);
        fprintf(stderr, "  Chain blocks executed:   %llu\n", (unsigned long long)jit_chain_blocks);
        fprintf(stderr, "  Avg chain length:        %.1f\n",
                jit_chain_entries ? (double)jit_chain_blocks / jit_chain_entries : 0.0);
        fprintf(stderr, "\nJIT compilation:\n");
        fprintf(stderr, "  Compile attempts:        %llu\n", (unsigned long long)jit_compile_attempts);
        fprintf(stderr, "  Compile successes:       %llu\n", (unsigned long long)jit_compile_successes);
        fprintf(stderr, "  Compile failures:        %llu\n",
                (unsigned long long)(jit_compile_attempts - jit_compile_successes));
        fprintf(stderr, "  Blocks compiled:         %llu\n",
                static_cast<unsigned long long>(jit_compiler.blocks_compiled()));
        fprintf(stderr, "  ARM64 bytes generated:   %llu\n",
                static_cast<unsigned long long>(jit_compiler.bytes_generated()));
        fprintf(stderr, "  Process arena usage:     %zu / %zu bytes (%.1f%%)\n",
                cpu::jit::JitCodeArena::arena_bytes_used(),
                cpu::jit::JitCodeArena::arena_budget(),
                cpu::jit::JitCodeArena::arena_budget()
                    ? 100.0 * cpu::jit::JitCodeArena::arena_bytes_used() /
                          cpu::jit::JitCodeArena::arena_budget()
                    : 0.0);
        fprintf(stderr, "  Process arena chunks:    %zu live / %zu "
                        "(retired=%llu freed=%llu pending=%zu)\n",
                cpu::jit::JitCodeArena::arena_segment_count(),
                cpu::jit::JitCodeArena::arena_chunk_count(),
                static_cast<unsigned long long>(
                    cpu::jit::JitCodeArena::arena_chunk_retirements()),
                static_cast<unsigned long long>(
                    cpu::jit::JitCodeArena::arena_chunk_frees()),
                cpu::jit::JitCodeArena::arena_pending_free_count());

        // ----------------------------------------------------------------
        // Why code is not running natively.
        //
        // The percentages above say HOW MUCH ran interpreted. They do not say
        // WHY, and every large application slowdown measured so far has come
        // down to a why: the arena filled and the gate refused every native
        // entry; a block was evicted from the per-thread LRU before it was
        // seen a second time; twelve threads each compiled their own copy of
        // the same code. Those counters existed, but in three different
        // reports behind three different environment variables, so nobody
        // ever read them together. One run, one attribution.
        // ----------------------------------------------------------------
        const auto memo = cpu::jit::JitCompileMemo::compile_memo_stats();
        fprintf(stderr, "\nWhy not native:\n");
        fprintf(stderr, "  Arena gate refusals:     %llu\n",
                static_cast<unsigned long long>(
                    cpu::jit::JitCodeArena::arena_gate_refusals()));
        fprintf(stderr, "  Arena gate abandons:     %llu\n",
                static_cast<unsigned long long>(
                    cpu::jit::JitCodeArena::arena_gate_abandons()));
        fprintf(stderr, "  Block cache evictions:   %llu (capacity %zu)\n",
                static_cast<unsigned long long>(block_cache.evictions()),
                block_cache.capacity());
        fprintf(stderr, "  Hot threshold:           %llu\n",
                static_cast<unsigned long long>(
                    cpu::jit::JitCompiler::jit_hot_threshold()));

        // Shared-table effectiveness.
        //
        // There are TWO sharing paths and they are alternatives, which is the
        // trap in reading these numbers. try_adopt_shared_block() runs on the
        // dispatcher cold path BEFORE compile() is ever called; when it
        // succeeds, compile() never runs, so `compile_hits` stays zero BY
        // CONSTRUCTION. A reader who takes compile_hits=0 as "sharing is
        // broken" is reading the wrong counter.
        //
        // The number that means something is the duplication rate over both
        // paths: how often a thread emitted code that some thread in this
        // process had already emitted. 0% is perfect sharing.
        const unsigned long long reused = memo.compile_hits + memo.cold_adoptions;
        const unsigned long long attempts = memo.fresh_emits + reused;
        fprintf(stderr, "\nShared compile table:\n");
        fprintf(stderr, "  Duplication rate:        %.1f%% "
                        "(%llu fresh emits of %llu block acquisitions)\n",
                attempts ? 100.0 * memo.fresh_emits / attempts : 0.0,
                static_cast<unsigned long long>(memo.fresh_emits), attempts);
        fprintf(stderr, "  Reused via cold adopt:   %llu of %llu probes"
                        " (hash mismatch %llu, demoted %llu)\n",
                static_cast<unsigned long long>(memo.cold_adoptions),
                static_cast<unsigned long long>(memo.cold_probes),
                static_cast<unsigned long long>(memo.cold_hash_mismatches),
                static_cast<unsigned long long>(memo.cold_demoted));
        fprintf(stderr, "  Reused via compile memo: %llu of %llu probes\n",
                static_cast<unsigned long long>(memo.compile_hits),
                static_cast<unsigned long long>(memo.compile_probes));
        fprintf(stderr, "  Records / SMC purges:    %zu / %llu\n",
                memo.records,
                static_cast<unsigned long long>(memo.smc_purges));

        // Sort blocks by total instruction count (hottest first)
        std::vector<std::pair<GuestAddr, BlockProfile>> sorted_blocks(
            block_profiles.begin(), block_profiles.end());
        std::sort(sorted_blocks.begin(), sorted_blocks.end(),
                  [](const auto& a, const auto& b) {
                      return a.second.insn_count > b.second.insn_count;
                  });

        fprintf(stderr, "\n--- Top 30 Hot Blocks (by total instructions) ---\n");
        fprintf(stderr, "%-20s %-10s %-12s %-6s %-5s %-5s\n",
                "Address", "Execs", "Insns", "BlkSz", "JIT", "Fail");
        u32 shown = 0;
        for (const auto& [addr, bp] : sorted_blocks) {
            if (shown >= 30) break;
            fprintf(stderr, "0x%016llx   %-10llu %-12llu %-6u %-5s %-5s\n",
                    static_cast<unsigned long long>(addr),
                    (unsigned long long)bp.exec_count,
                    (unsigned long long)bp.insn_count,
                    bp.block_insns,
                    bp.jit_compiled ? "YES" : "no",
                    bp.jit_failed ? "FAIL" : "");
            ++shown;
        }

        // Show blocks that failed JIT compilation (hot ones first)
        fprintf(stderr, "\n--- JIT Compilation Failures (hot first) ---\n");
        shown = 0;
        for (const auto& [addr, bp] : sorted_blocks) {
            if (!bp.jit_failed) continue;
            if (shown >= 20) break;
            fprintf(stderr, "0x%016llx  execs=%-10llu insns=%-12llu blk_size=%u\n",
                    static_cast<unsigned long long>(addr),
                    (unsigned long long)bp.exec_count,
                    (unsigned long long)bp.insn_count, bp.block_insns);

            // Dump the first few instruction bytes for analysis
            u8 ibytes[32] = {};
            auto rd = memory.read_bytes(addr, ibytes, sizeof(ibytes));
            if (rd.is_ok()) {
                fprintf(stderr, "  bytes:");
                for (u32 i = 0; i < 32; ++i)
                    fprintf(stderr, " %02x", ibytes[i]);
                fprintf(stderr, "\n");
            }
            ++shown;
        }

        // Interpreter-only blocks that are very hot (never got JIT'd)
        fprintf(stderr, "\n--- Hot Interpreter-Only Blocks (never compiled) ---\n");
        shown = 0;
        for (const auto& [addr, bp] : sorted_blocks) {
            if (bp.jit_compiled || bp.jit_failed) continue;
            if (bp.insn_count < 100000) continue;  // only show significant ones
            if (shown >= 20) break;
            fprintf(stderr, "0x%016llx  execs=%-10llu insns=%-12llu blk_size=%u\n",
                    static_cast<unsigned long long>(addr),
                    (unsigned long long)bp.exec_count,
                    (unsigned long long)bp.insn_count, bp.block_insns);
            ++shown;
        }

        fprintf(stderr, "=============================================\n\n");
    }

    if (config.verbose) {
        VITREO_TRACE( "[vitreo] Execution finished: %llu instructions, "
                     "exit_code=%d, %s\n",
                     static_cast<unsigned long long>(result.instructions_executed),
                     result.exit_code,
                     result.success ? "success" : "failure");
    }

    // Restore original signal handlers
    sigaction(SIGTERM, &old_term, nullptr);
    sigaction(SIGINT,  &old_int,  nullptr);

    if (g_stop_requested) {
        // Close the measurement window at the instant guest execution ended.
        // Nothing called perf_window_stop() on this path, so the window stayed
        // OPEN from SIGTERM until the report was built: every counter kept
        // accumulating across shutdown, and "Window" reported that span as run
        // time. Measured consequence -- a sled-JIT A/B (since removed)
        // recorded 48.5 s for one arm and 60.5 s for the other from an
        // identical run duration, because the second arm's guest took longer
        // to NOTICE the signal, and
        // the extra dispatcher entries it booked while shutting down read as a
        // 3% regression that was really just a longer window.
        diagnostics::perf_window_stop();
        // Host SIGTERM (often from peer TerminateProcess → kill(SIGTERM)).
        // 143 = 128+SIGTERM; not a guest ExitProcess code. Parent
        // GetExitCodeProcess uses ProcessObject::termination_exit_code.
        std::fprintf(
            stderr,
            "[vitreo] host SIGTERM/SIGINT — stopping guest "
            "(reported exit_code=143/0x8f; not a Windows ExitProcess code)\n");
        result.exit_code = 143;
        result.success = true;
        // atexit dump can run after ThreadManager is destroyed and abort
        // on a dead mutex. Snapshot while the process is still intact.
        vitreo::cpu::jit::hot_dump::dump();
        diagnostics::perf_window_dump("sigterm");
    }

    return result;
}

} // namespace vitreo::cli
