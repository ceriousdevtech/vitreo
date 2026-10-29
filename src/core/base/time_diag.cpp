/// @file time_diag.cpp
/// @brief VITREO_TIME_DIAG implementation. See vitreo/time_diag.hpp.

#include "vitreo/time_diag.hpp"

#include "vitreo/virtual_tsc.hpp"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include <pthread.h>

namespace vitreo {

namespace {

/// Both millisecond APIs are DWORD-wide at the guest boundary and wrap every
/// ~49.7 days. A reading below its predecessor is far more likely to be that
/// wrap than a clock running backwards, so the wrap is folded back in before
/// anything is called an anomaly.
constexpr u64 kMsWrapPeriod = 1ULL << 32;

constexpr u64 kNsPerMs = 1'000'000ULL;

/// Default forward step between adjacent reads at one call site that is worth
/// reporting. Simulation code polls its clock far more often than this, so a
/// second of apparent elapsed time is already several hundred frames. A guest
/// thread that legitimately blocks for longer than this will trip it, so
/// `VITREO_TIME_DIAG=<ms>` raises the bar for runs where that is expected.
constexpr u64 kDefaultJumpNs = 1'000'000'000ULL;

/// Fixed interval used for rate-limiting drift reports, independent of the
/// tunable jump threshold.
constexpr u64 kDriftReportIntervalNs = 5'000'000'000ULL;

/// Drift is only meaningful once both domains have run long enough for their
/// rates to separate; below this the comparison is dominated by the offset
/// between the two first samples.
constexpr u64 kDriftMinElapsedNs = 2'000'000'000ULL;

/// Report when the two domains disagree by more than this fraction of elapsed
/// time. Generous: the failure this is built to catch is a ~40x scaling error,
/// not a few parts per million of oscillator skew.
constexpr double kDriftTolerance = 0.05;

/// Cap output so a wedged clock cannot fill the disk. Once reached, the
/// summary still prints, so the run is not left silent.
constexpr u32 kMaxReports = 500;
/// Separate budget for cross-domain drift, which must never be crowded out
/// by jump-report noise.
constexpr u64 kMaxDriftReports = 20;

constexpr usize kMaxSites = 256;

[[nodiscard]] bool is_millisecond_api(TimeApi api) noexcept {
    return api == TimeApi::GetTickCount || api == TimeApi::GetTickCount64 ||
           api == TimeApi::TimeGetTime;
}

/// True for the APIs the guest sees as a 32-bit DWORD, which are the only ones
/// that can legitimately wrap within a session.
[[nodiscard]] bool is_32bit_ms_api(TimeApi api) noexcept {
    return api == TimeApi::GetTickCount || api == TimeApi::TimeGetTime;
}

[[nodiscard]] const char* api_name(TimeApi api) noexcept {
    switch (api) {
    case TimeApi::GetTickCount:   return "GetTickCount";
    case TimeApi::GetTickCount64: return "GetTickCount64";
    case TimeApi::TimeGetTime:    return "timeGetTime";
    case TimeApi::Qpc:            return "QueryPerformanceCounter";
    case TimeApi::Rdtsc:          return "RDTSC";
    }
    return "?";
}

/// Ticks per second of the guest-visible value each API returns. These are
/// three different units — the millisecond APIs, the 10 MHz QPC domain and the
/// GHz TSC — so nothing may be compared until it has been through `to_ns`.
[[nodiscard]] u64 api_hz(TimeApi api) noexcept {
    switch (api) {
    case TimeApi::GetTickCount:
    case TimeApi::GetTickCount64:
    case TimeApi::TimeGetTime:
        return 1'000ULL;
    case TimeApi::Qpc:
        return kQpcHz;
    case TimeApi::Rdtsc:
        return guest_tsc_hz();
    }
    return 1ULL;
}

/// Convert a raw reading to nanoseconds. The 128-bit intermediate matters:
/// a TSC reading is already ~1e18 at GHz rates, so `ticks * kNsPerSecond`
/// overflows 64 bits.
[[nodiscard]] u64 to_ns(TimeApi api, u64 ticks) noexcept {
    const auto scaled =
        static_cast<unsigned __int128>(ticks) * kNsPerSecond / api_hz(api);
    return static_cast<u64>(scaled);
}

[[nodiscard]] u64 current_thread_id() noexcept {
    u64 tid = 0;
    (void)pthread_threadid_np(nullptr, &tid);
    return tid;
}

struct Site {
    TimeApi api{};
    u64 caller{0};
    /// Part of the key. Guests read these clocks from many threads at once,
    /// and two threads interleaving reads of one monotonic clock make the
    /// shared slot appear to move backwards. Continuity is only meaningful
    /// within a single thread's own sequence of reads.
    u64 thread_id{0};
    u64 previous_raw{0};
    /// Accumulated wrap correction, so a 32-bit ms clock keeps a monotonic
    /// normalised value across the 49.7-day boundary.
    u64 wrap_bias{0};
    u64 samples{0};
    u64 anomalies{0};
    bool initialised{false};
};

struct DiagState {
    std::mutex lock;
    std::array<Site, kMaxSites> sites{};
    usize site_count{0};
    u32 reports{0};
    bool sites_exhausted{false};

    /// First observation in each domain, used as the drift anchor. Both are
    /// normalised to nanoseconds.
    u64 ms_anchor_ns{0};
    u64 qpc_anchor_ns{0};
    bool ms_anchored{false};
    bool qpc_anchored{false};
    u64 last_drift_report_ns{0};
    u64 drift_reports{0};
};

DiagState& state() noexcept {
    // Deliberately leaked. The atexit summary and any guest thread still
    // running at shutdown can both reach this after a function-local static
    // would have been destroyed, and locking a destroyed mutex terminates the
    // process. A diagnostic must not be able to change how the run ends.
    static DiagState* s = new DiagState();
    return *s;
}

/// Locate or create the per-(api, caller, thread) slot. Caller holds the lock.
Site* find_site(DiagState& s, TimeApi api, u64 caller, u64 tid) noexcept {
    for (usize i = 0; i < s.site_count; ++i) {
        if (s.sites[i].api == api && s.sites[i].caller == caller &&
            s.sites[i].thread_id == tid) {
            return &s.sites[i];
        }
    }
    if (s.site_count >= kMaxSites) {
        s.sites_exhausted = true;
        return nullptr;
    }
    Site* site = &s.sites[s.site_count++];
    site->api = api;
    site->caller = caller;
    site->thread_id = tid;
    return site;
}

/// Reporting threshold in nanoseconds, from `VITREO_TIME_DIAG` when it parses
/// as a positive millisecond count.
u64 jump_threshold_ns() noexcept {
    static const u64 threshold = [] {
        const char* value = std::getenv("VITREO_TIME_DIAG");
        if (value == nullptr) return kDefaultJumpNs;
        char* end = nullptr;
        const unsigned long long ms = std::strtoull(value, &end, 10);
        if (end == value || ms == 0) return kDefaultJumpNs;
        return static_cast<u64>(ms) * kNsPerMs;
    }();
    return threshold;
}

}  // namespace

bool time_diag_enabled() noexcept {
    // The summary is registered here rather than at a call site so that every
    // run which instruments anything also reports what it saw. A silent log is
    // otherwise indistinguishable from instrumentation that never executed.
    static const bool enabled = [] {
        if (std::getenv("VITREO_TIME_DIAG") == nullptr) return false;
        (void)std::atexit([] { time_diag_report_summary(); });
        return true;
    }();
    return enabled;
}

void time_diag_record(TimeApi api, u64 raw_value, u64 caller) noexcept {
    DiagState& s = state();
    const u64 tid = current_thread_id();

    std::lock_guard<std::mutex> guard(s.lock);

    Site* site = find_site(s, api, caller, tid);
    if (site == nullptr) return;

    ++site->samples;

    // Fold a DWORD wrap back in before comparing, so the wrap itself is not
    // reported as a backwards clock.
    if (site->initialised && is_32bit_ms_api(api) &&
        raw_value < site->previous_raw &&
        (site->previous_raw - raw_value) > (kMsWrapPeriod / 2)) {
        site->wrap_bias += kMsWrapPeriod;
    }

    const u64 now_ns = to_ns(api, raw_value + site->wrap_bias);

    // Anchor each domain on its first sample so drift is measured between two
    // elapsed intervals rather than between two unrelated epochs.
    if (is_millisecond_api(api)) {
        if (!s.ms_anchored) {
            s.ms_anchor_ns = now_ns;
            s.ms_anchored = true;
        }
    } else if (!s.qpc_anchored) {
        s.qpc_anchor_ns = now_ns;
        s.qpc_anchored = true;
    }

    if (site->initialised) {
        const u64 previous_ns =
            to_ns(api, site->previous_raw + site->wrap_bias);
        const bool backwards = now_ns < previous_ns;
        const u64 delta_ns = backwards ? (previous_ns - now_ns)
                                       : (now_ns - previous_ns);
        const bool jumped = !backwards && delta_ns > jump_threshold_ns();

        if ((backwards || jumped) && s.reports < kMaxReports) {
            ++s.reports;
            ++site->anomalies;
            std::fprintf(
                stderr,
                "[time-diag] %s thread=0x%" PRIx64 " caller=0x%" PRIx64
                " value=%" PRIu64 " %s delta=%" PRIu64 "ns (%.3f ms)\n",
                api_name(api), tid, caller, raw_value,
                backwards ? "BACKWARDS" : "JUMP", delta_ns,
                static_cast<double>(delta_ns) / 1e6);
        }
    }

    site->previous_raw = raw_value;
    site->initialised = true;

    // Cross-domain drift. Only checkable once both domains have been sampled.
    if (!s.ms_anchored || !s.qpc_anchored) return;
    if (!is_millisecond_api(api)) {
        // Compare on QPC samples only, so one comparison uses one fresh
        // reading from each domain rather than two stale ones.
        return;
    }
    const u64 ms_elapsed = now_ns - s.ms_anchor_ns;
    if (ms_elapsed < kDriftMinElapsedNs) return;
    if (s.last_drift_report_ns != 0 &&
        ms_elapsed - s.last_drift_report_ns < kDriftReportIntervalNs) {
        return;
    }

    // The other domain's most recent reading, normalised the same way.
    u64 qpc_latest_ns = 0;
    for (usize i = 0; i < s.site_count; ++i) {
        const Site& other = s.sites[i];
        if (other.initialised && !is_millisecond_api(other.api)) {
            const u64 value = to_ns(other.api,
                                    other.previous_raw + other.wrap_bias);
            if (value > qpc_latest_ns) qpc_latest_ns = value;
        }
    }
    if (qpc_latest_ns <= s.qpc_anchor_ns) return;

    const u64 qpc_elapsed = qpc_latest_ns - s.qpc_anchor_ns;
    // Both domains must have run long enough to be comparable. Without this a
    // guest that reads QPC twice in quick succession but polls GetTickCount
    // for seconds yields a near-zero denominator and a meaningless ratio.
    if (qpc_elapsed < kDriftMinElapsedNs) return;
    const double ratio = static_cast<double>(ms_elapsed) /
                         static_cast<double>(qpc_elapsed);
    if (ratio > 1.0 + kDriftTolerance || ratio < 1.0 - kDriftTolerance) {
        s.last_drift_report_ns = ms_elapsed;
        ++s.drift_reports;
        // Drift gets its OWN budget rather than sharing the jump budget.
        // Jump reports are dominated by ordinary 1-4 ms scheduling jitter and
        // exhaust kMaxReports quickly; sharing meant the rarer and far more
        // important signal was counted in the summary but its detail line
        // never printed. A silenced signal reads exactly like an absent one.
        if (s.drift_reports <= kMaxDriftReports) {
            std::fprintf(
                stderr,
                "[time-diag] DRIFT ms-domain elapsed=%.3f s vs "
                "qpc-domain elapsed=%.3f s ratio=%.4f\n",
                static_cast<double>(ms_elapsed) / 1e9,
                static_cast<double>(qpc_elapsed) / 1e9, ratio);
        }
    }
}

void time_diag_report_summary() noexcept {
    if (!time_diag_enabled()) return;
    DiagState& s = state();
    std::lock_guard<std::mutex> guard(s.lock);

    std::fprintf(stderr, "[time-diag] summary: %zu call sites, %u reports%s\n",
                 s.site_count, s.reports,
                 s.sites_exhausted ? " (site table full)" : "");
    for (usize i = 0; i < s.site_count; ++i) {
        const Site& site = s.sites[i];
        std::fprintf(stderr,
                     "[time-diag]   %-24s thread=0x%" PRIx64
                     " caller=0x%" PRIx64 " samples=%" PRIu64
                     " anomalies=%" PRIu64 " last=%" PRIu64 "\n",
                     api_name(site.api), site.thread_id, site.caller,
                     site.samples, site.anomalies, site.previous_raw);
    }
    if (s.drift_reports != 0) {
        std::fprintf(stderr,
                     "[time-diag]   cross-domain drift detections: %" PRIu64
                     "\n",
                     s.drift_reports);
    }
}

}  // namespace vitreo
