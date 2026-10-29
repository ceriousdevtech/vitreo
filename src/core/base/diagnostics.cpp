/// @file diagnostics.cpp
/// @brief Storage for the runtime diagnostic hooks declared in
///        include/vitreo/diagnostics.hpp.

#include <vitreo/diagnostics.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace vitreo::diagnostics {
namespace {

/// The hook is genuinely process-global: it is read from guest threads at
/// arbitrary points, including from a JIT fault handler, so it is stored as an
/// atomic rather than guarded by a lock that a fault path could not take.
/// Relaxed ordering is sufficient — the pointer is installed once at startup,
/// and a dump that raced with installation would simply be the no-op.
std::atomic<HostcallDumpFn> g_hostcall_dump{nullptr};

} // namespace

void set_hostcall_dump(HostcallDumpFn fn) noexcept {
    g_hostcall_dump.store(fn, std::memory_order_relaxed);
}

void dump_hostcalls(const char* reason) noexcept {
    if (auto fn = g_hostcall_dump.load(std::memory_order_relaxed)) {
        fn(reason);
    }
}

// -----------------------------------------------------------------------------
// Trace gates
// -----------------------------------------------------------------------------

namespace {

/// Release/acquire rather than relaxed: a reader that observes the latch must
/// also observe whatever the setting thread recorded before setting it, since
/// the point of the gate is that the earlier state is now worth tracing.
std::atomic<bool> g_trace_milestone{false};

/// Describes the current thread's guest stack, so it is per-thread. Plain int:
/// only the owning thread ever touches it.
thread_local int t_module_init_depth = 0;

} // namespace

void set_trace_milestone_reached(bool reached) noexcept {
    g_trace_milestone.store(reached, std::memory_order_release);
}

bool trace_milestone_reached() noexcept {
    return g_trace_milestone.load(std::memory_order_acquire);
}

void enter_module_init() noexcept { ++t_module_init_depth; }

void leave_module_init() noexcept {
    // Clamped at zero so an unbalanced leave — a guest that longjmps out of
    // DllMain, say — cannot drive the depth negative and disable the gate for
    // the rest of the thread's life.
    if (t_module_init_depth > 0) --t_module_init_depth;
}

int module_init_depth() noexcept { return t_module_init_depth; }

// -----------------------------------------------------------------------------
// Load-phase clock
// -----------------------------------------------------------------------------

namespace {

constexpr const char* kPhaseName[] = {
    "process_start",
    "pe_entry",
    "first_window",
    "first_device",
    "first_draw",
    "first_present",
    "present_10",
    "present_60",
    "present_300",
    "present_1000",
    "present_5000",
};

static_assert(sizeof(kPhaseName) / sizeof(kPhaseName[0]) ==
                  static_cast<unsigned>(LoadPhase::Present5000) + 1,
              "LoadPhase name table must match the enum");

std::atomic<u64> g_phase_origin_ns{0};
std::atomic<PhaseObserver> g_phase_observer{nullptr};
std::atomic<u32> g_phase_marked{0};
std::atomic<u32> g_phase_presents{0};
std::atomic<u32> g_phase_draws{0};
std::atomic<u64> g_phase_sse_helpers{0};
std::atomic<u64> g_phase_finds{0};
std::atomic<u64> g_phase_present_ns{0};
std::atomic<u64> g_phase_present_ns_after_first{0};
std::atomic<u64> g_phase_present_submit_ns{0};
std::atomic<u64> g_phase_sleep_ns{0};
std::atomic<u64> g_phase_sleep_main_ns{0};
std::atomic<u64> g_phase_wfso_ns{0};
std::atomic<u64> g_phase_wfso_main_ns{0};
std::atomic<u64> g_phase_heartbeat_slot{0};
std::atomic<u32> g_phase_heartbeat_presents{0};
std::atomic<u64> g_phase_guest_rip{0};
std::atomic<u32> g_phase_guest_eax{0};
std::atomic<u32> g_phase_guest_ecx{0};

u64 now_ns() noexcept {
    using Clock = std::chrono::steady_clock;
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

u64 origin_or_start() noexcept {
    u64 origin = g_phase_origin_ns.load(std::memory_order_relaxed);
    if (origin != 0) {
        return origin;
    }
    const u64 stamped = now_ns();
    if (g_phase_origin_ns.compare_exchange_strong(
            origin, stamped, std::memory_order_relaxed)) {
        return stamped;
    }
    return origin;
}

void print_phase(LoadPhase phase, u64 origin_ns) noexcept {
    const auto idx = static_cast<unsigned>(phase);
    const u64 elapsed_ns = now_ns() - origin_ns;
    const double ms = static_cast<double>(elapsed_ns) / 1000000.0;
    std::fprintf(stderr, "[load-phase] %s +%.0f ms\n", kPhaseName[idx], ms);
    if (PhaseObserver observer = g_phase_observer.load(std::memory_order_acquire)) {
        observer(phase);
    }
}

} // namespace

void phase_set_observer(PhaseObserver observer) noexcept {
    g_phase_observer.store(observer, std::memory_order_release);
}

const char* phase_name(LoadPhase phase) noexcept {
    const auto idx = static_cast<unsigned>(phase);
    return idx < sizeof(kPhaseName) / sizeof(kPhaseName[0]) ? kPhaseName[idx] : "?";
}

void phase_clock_start() noexcept {
    const u64 origin = origin_or_start();
    const u32 bit = 1u << static_cast<u32>(LoadPhase::ProcessStart);
    const u32 prev = g_phase_marked.fetch_or(bit, std::memory_order_relaxed);
    if ((prev & bit) != 0) {
        return;
    }
    print_phase(LoadPhase::ProcessStart, origin);
    perf_window_on_phase(LoadPhase::ProcessStart);
}

void phase_mark(LoadPhase phase) noexcept {
    const u64 origin = g_phase_origin_ns.load(std::memory_order_relaxed);
    if (origin == 0) {
        return;
    }
    const u32 bit = 1u << static_cast<u32>(phase);
    const u32 prev = g_phase_marked.fetch_or(bit, std::memory_order_relaxed);
    if ((prev & bit) != 0) {
        return;
    }
    print_phase(phase, origin);
    perf_window_on_phase(phase);
}

void phase_note_present() noexcept {
    if (g_phase_origin_ns.load(std::memory_order_relaxed) == 0) {
        return;
    }
    const u32 n = g_phase_presents.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) {
        phase_mark(LoadPhase::FirstPresent);
    } else if (n == 10) {
        phase_mark(LoadPhase::Present10);
    } else if (n == 60) {
        phase_mark(LoadPhase::Present60);
    } else if (n == 300) {
        phase_mark(LoadPhase::Present300);
    } else if (n == 1000) {
        phase_mark(LoadPhase::Present1000);
    } else if (n == 5000) {
        phase_mark(LoadPhase::Present5000);
    }
    phase_maybe_heartbeat();
    perf_add_present();
}

void phase_add_present_wall(u64 ns) noexcept {
    if (ns == 0 ||
        g_phase_origin_ns.load(std::memory_order_relaxed) == 0) {
        return;
    }
    g_phase_present_ns.fetch_add(ns, std::memory_order_relaxed);
    const u32 marked = g_phase_marked.load(std::memory_order_relaxed);
    if ((marked & (1u << static_cast<u32>(LoadPhase::FirstPresent))) != 0) {
        g_phase_present_ns_after_first.fetch_add(ns, std::memory_order_relaxed);
    }
}

void phase_add_present_submit_wall(u64 ns) noexcept {
    if (ns == 0 ||
        g_phase_origin_ns.load(std::memory_order_relaxed) == 0) {
        return;
    }
    g_phase_present_submit_ns.fetch_add(ns, std::memory_order_relaxed);
}

void phase_add_sleep_wall(u64 ns) noexcept {
    if (ns == 0 ||
        g_phase_origin_ns.load(std::memory_order_relaxed) == 0) {
        return;
    }
    g_phase_sleep_ns.fetch_add(ns, std::memory_order_relaxed);
#if defined(__APPLE__)
    if (pthread_main_np() != 0) {
        g_phase_sleep_main_ns.fetch_add(ns, std::memory_order_relaxed);
    }
#endif
}

void phase_add_wfso_wall(u64 ns) noexcept {
    if (ns == 0 ||
        g_phase_origin_ns.load(std::memory_order_relaxed) == 0) {
        return;
    }
    g_phase_wfso_ns.fetch_add(ns, std::memory_order_relaxed);
#if defined(__APPLE__)
    if (pthread_main_np() != 0) {
        g_phase_wfso_main_ns.fetch_add(ns, std::memory_order_relaxed);
    }
#endif
}

void phase_note_draw() noexcept {
    if (g_phase_origin_ns.load(std::memory_order_relaxed) == 0) return;
    g_phase_draws.fetch_add(1, std::memory_order_relaxed);
    perf_add_draw();
}

void phase_note_sse_helper() noexcept {
    if (g_phase_origin_ns.load(std::memory_order_relaxed) == 0) return;
    g_phase_sse_helpers.fetch_add(1, std::memory_order_relaxed);
}

void phase_note_find() noexcept {
    if (g_phase_origin_ns.load(std::memory_order_relaxed) == 0) return;
    g_phase_finds.fetch_add(1, std::memory_order_relaxed);
}

void phase_note_guest_exec(u64 rip, u32 eax, u32 ecx) noexcept {
    if (g_phase_origin_ns.load(std::memory_order_relaxed) == 0) return;
    g_phase_guest_rip.store(rip, std::memory_order_relaxed);
    g_phase_guest_eax.store(eax, std::memory_order_relaxed);
    g_phase_guest_ecx.store(ecx, std::memory_order_relaxed);
}

void phase_maybe_heartbeat() noexcept {
    const u64 origin = g_phase_origin_ns.load(std::memory_order_relaxed);
    if (origin == 0) {
        return;
    }
    const u64 elapsed_ns = now_ns() - origin;
    // Heartbeat period. The 5 s default is too coarse to attribute load-phase
    // work: milestones bucket to the preceding heartbeat, so nothing below
    // ~33 % of a 15 s window is resolvable and every configuration looks
    // identical. VITREO_HEARTBEAT_MS tightens it for measurement runs.
    static const u64 s_period_ns = [] {
        if (const char* e = std::getenv("VITREO_HEARTBEAT_MS")) {
            const long v = std::strtol(e, nullptr, 10);
            if (v > 0) return static_cast<u64>(v) * 1000000ull;
        }
        return 5000000000ull;
    }();
    const u64 slot = elapsed_ns / s_period_ns;
    if (slot == 0) {
        return;
    }
    u64 last = g_phase_heartbeat_slot.load(std::memory_order_relaxed);
    if (last >= slot) {
        return;
    }
    if (!g_phase_heartbeat_slot.compare_exchange_strong(
            last, slot, std::memory_order_relaxed)) {
        return;
    }
    const u32 presents = g_phase_presents.load(std::memory_order_relaxed);
    const u32 draws = g_phase_draws.load(std::memory_order_relaxed);
    const u64 sse = g_phase_sse_helpers.load(std::memory_order_relaxed);
    const u64 finds = g_phase_finds.load(std::memory_order_relaxed);
    const u64 present_ns = g_phase_present_ns.load(std::memory_order_relaxed);
    const u64 present_ns_first =
        g_phase_present_ns_after_first.load(std::memory_order_relaxed);
    const u64 present_submit_ns =
        g_phase_present_submit_ns.load(std::memory_order_relaxed);
    const u64 sleep_ns = g_phase_sleep_ns.load(std::memory_order_relaxed);
    const u64 sleep_main_ns =
        g_phase_sleep_main_ns.load(std::memory_order_relaxed);
    const u64 wfso_ns = g_phase_wfso_ns.load(std::memory_order_relaxed);
    const u64 wfso_main_ns =
        g_phase_wfso_main_ns.load(std::memory_order_relaxed);
    const double ms = static_cast<double>(elapsed_ns) / 1000000.0;
    const u32 prev_presents =
        g_phase_heartbeat_presents.exchange(presents,
                                            std::memory_order_relaxed);
    const u64 guest_rip = g_phase_guest_rip.load(std::memory_order_relaxed);
    const u32 guest_eax = g_phase_guest_eax.load(std::memory_order_relaxed);
    const u32 guest_ecx = g_phase_guest_ecx.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[load-phase] heartbeat +%.0f ms presents=%u draws=%u "
                 "sse=%llu finds=%llu present_ms=%.0f submit_ms=%.0f "
                 "sleep_ms=%.0f sleep_main_ms=%.0f wfso_ms=%.0f "
                 "wfso_main_ms=%.0f since_first=%.0f",
                 ms, presents, draws,
                 static_cast<unsigned long long>(sse),
                 static_cast<unsigned long long>(finds),
                 static_cast<double>(present_ns) / 1000000.0,
                 static_cast<double>(present_submit_ns) / 1000000.0,
                 static_cast<double>(sleep_ns) / 1000000.0,
                 static_cast<double>(sleep_main_ns) / 1000000.0,
                 static_cast<double>(wfso_ns) / 1000000.0,
                 static_cast<double>(wfso_main_ns) / 1000000.0,
                 static_cast<double>(present_ns_first) / 1000000.0);
    if (guest_rip != 0 &&
        (presents == prev_presents || prev_presents == 0)) {
        std::fprintf(stderr, " rip=0x%llx eax=0x%x ecx=0x%x",
                     static_cast<unsigned long long>(guest_rip),
                     guest_eax, guest_ecx);
    }
    std::fputc('\n', stderr);
    perf_window_poll_threads();
}

void phase_clock_reset_for_tests() noexcept {
    g_phase_origin_ns.store(0, std::memory_order_relaxed);
    g_phase_marked.store(0, std::memory_order_relaxed);
    g_phase_presents.store(0, std::memory_order_relaxed);
    g_phase_draws.store(0, std::memory_order_relaxed);
    g_phase_sse_helpers.store(0, std::memory_order_relaxed);
    g_phase_finds.store(0, std::memory_order_relaxed);
    g_phase_present_ns.store(0, std::memory_order_relaxed);
    g_phase_present_ns_after_first.store(0, std::memory_order_relaxed);
    g_phase_present_submit_ns.store(0, std::memory_order_relaxed);
    g_phase_sleep_ns.store(0, std::memory_order_relaxed);
    g_phase_sleep_main_ns.store(0, std::memory_order_relaxed);
    g_phase_wfso_ns.store(0, std::memory_order_relaxed);
    g_phase_wfso_main_ns.store(0, std::memory_order_relaxed);
    g_phase_heartbeat_slot.store(0, std::memory_order_relaxed);
    g_phase_heartbeat_presents.store(0, std::memory_order_relaxed);
    g_phase_guest_rip.store(0, std::memory_order_relaxed);
    g_phase_guest_eax.store(0, std::memory_order_relaxed);
    g_phase_guest_ecx.store(0, std::memory_order_relaxed);
}

} // namespace vitreo::diagnostics
