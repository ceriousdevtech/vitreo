// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_pipeline_diag.cpp
/// @brief The D3D11 skip-stat tally (VITREO_D3D11_SKIP_STATS) and the
///        per-draw pipeline trace (pipeline_diag, pipeline_trace_enabled).
///        Split out of d3d11_pipeline.cpp as verbatim code motion.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_pipeline_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_state_census.hpp"
#include "winapi/graphics/d3d11/d3d11_stdcall.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/delta_record.hpp"
#include "winapi/graphics/d3d11/d3d11_draw_memo.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "winapi/graphics/common/live_diag.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "winapi/graphics/common/d3d_sampler.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "graphics/shaders/gs_passthrough.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <vitreo/diagnostics.hpp>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <limits>
#include <map>
#include <tuple>
#include <mutex>
#include <vector>
#include <cctype>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <mach/mach_time.h>

namespace vitreo::winapi {

/// VITREO_D3D11_SKIP_STATS: count every diagnostic format that reports a
/// skipped draw, a REJECTed resource or a failed call, independent of the
/// (per-call, very noisy) VITREO_D3D11_DIAG switch, and print the tally
/// every 5 s. A draw that silently never reaches Metal otherwise leaves no
/// trace at playable frame rates.
/// The switch is the environment variable (read once) or the marker file
/// /tmp/vitreo-d3d11-skip-stats, which can be touched or removed while the
/// guest runs. The marker is re-probed at most once per second: every
/// d3d11_diag / pipeline_diag call reaches here, and an access() syscall per
/// call was 5% of the RHI thread with the switch off.
bool d3d11_skip_stats_enabled() noexcept {
    static const bool env = std::getenv("VITREO_D3D11_SKIP_STATS") != nullptr;
    if (env) return true;
    static const u64 recheck_ticks = [] {
        mach_timebase_info_data_t tb{};
        (void)mach_timebase_info(&tb);
        if (tb.numer == 0) return u64{24'000'000};
        return static_cast<u64>(1'000'000'000ull * tb.denom / tb.numer);
    }();
    static std::atomic<u64> next_probe{0};
    static std::atomic<bool> marker{false};
    // The commpage's approximate clock (a load, no counter read): this runs
    // several times per draw, and mach_absolute_time's ISB + counter read
    // was itself 2% of UE's RHI thread.
    const u64 now = mach_approximate_time();
    u64 due = next_probe.load(std::memory_order_relaxed);
    if (now >= due &&
        next_probe.compare_exchange_strong(due, now + recheck_ticks,
                                           std::memory_order_relaxed)) {
        marker.store(::access("/tmp/vitreo-d3d11-skip-stats", F_OK) == 0,
                     std::memory_order_relaxed);
    }
    return marker.load(std::memory_order_relaxed);
}

void d3d11_skip_stat_report();  // d3d11_internal.hpp (not includable here)

namespace {

/// d3d11_skip_stat's tally: one site per diagnostic format (a string
/// literal, so its address names it), classified once. Counted from process
/// start whether or not the switch is on -- a draw dropped once at startup (a
/// LUT baked once) must still show when the marker is created later -- and
/// printed only while it is on. Lock-free on the counting side: a format is
/// one hashed slot and one load; only a skip format counts.
struct SkipSite {
    std::atomic<const char*> format{nullptr};
    /// 0 being classified, 1 not a skip format, 2 a skip format.
    std::atomic<u8> kind{0};
    std::atomic<u64> count{0};
    /// Printed so far (under the report lock).
    u64 reported{0};
};
constexpr unsigned kSkipSiteBits = 10;
constexpr usize kSkipSites = usize{1} << kSkipSiteBits;
SkipSite g_skip_sites[kSkipSites];
std::atomic<u64> g_skip_events{0};
/// Skip events of formats that found no slot (more formats than slots).
std::atomic<u64> g_skip_unslotted{0};
u64 g_skip_unslotted_reported{0};

[[nodiscard]] bool is_skip_format(const char* format) {
    return std::strstr(format, "skipped") != nullptr ||
           std::strstr(format, "REJECT") != nullptr ||
           std::strstr(format, "failed") != nullptr ||
           std::strstr(format, "UNIMPLEMENTED") != nullptr;
}

[[nodiscard]] SkipSite* skip_site(const char* format) {
    const usize home = static_cast<usize>(
        (reinterpret_cast<std::uintptr_t>(format) * 0x9E3779B97F4A7C15ull) >>
        (64 - kSkipSiteBits));
    for (usize probe = 0; probe < 8; ++probe) {
        SkipSite& site = g_skip_sites[(home + probe) & (kSkipSites - 1)];
        const char* key = site.format.load(std::memory_order_acquire);
        if (key == format) return &site;
        if (key != nullptr) continue;
        if (site.format.compare_exchange_strong(key, format,
                                                std::memory_order_acq_rel)) {
            site.kind.store(is_skip_format(format) ? 2u : 1u,
                            std::memory_order_release);
            return &site;
        }
        if (key == format) return &site;
    }
    return nullptr;
}

}  // namespace

void d3d11_skip_stat(const char* format) {
    if (format == nullptr) return;
    SkipSite* const site = skip_site(format);
    u8 kind = site != nullptr ? site->kind.load(std::memory_order_acquire) : 0;
    if (kind == 0) kind = is_skip_format(format) ? 2u : 1u;
    if (kind != 2) return;
    g_skip_events.fetch_add(1, std::memory_order_relaxed);
    if (site != nullptr) {
        site->count.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_skip_unslotted.fetch_add(1, std::memory_order_relaxed);
    }
    if (d3d11_skip_stats_enabled()) d3d11_skip_stat_report();
}

void d3d11_skip_stat_report() {
    static std::mutex mu;
    static bool primed = false;
    static std::chrono::steady_clock::time_point last{};
    std::lock_guard<std::mutex> lk(mu);
    const auto now = std::chrono::steady_clock::now();
    if (primed && now - last < std::chrono::seconds(5)) return;
    // The first report after the switch is turned on is everything so far.
    const char* const since = primed ? "" : " (since process start)";
    for (SkipSite& site : g_skip_sites) {
        if (site.kind.load(std::memory_order_acquire) != 2) continue;
        const u64 count = site.count.load(std::memory_order_relaxed);
        if (count == site.reported) continue;
        std::fprintf(stderr, "[d3d11-skip] n=%llu fmt=%.160s%s\n",
                     static_cast<unsigned long long>(count - site.reported),
                     site.format.load(std::memory_order_relaxed), since);
        site.reported = count;
    }
    const u64 unslotted = g_skip_unslotted.load(std::memory_order_relaxed);
    if (unslotted != g_skip_unslotted_reported) {
        std::fprintf(stderr, "[d3d11-skip] n=%llu fmt=(formats past the "
                             "tally's capacity)%s\n",
                     static_cast<unsigned long long>(
                         unslotted - g_skip_unslotted_reported),
                     since);
        g_skip_unslotted_reported = unslotted;
    }
    primed = true;
    last = now;
}

u64 d3d11_skip_events() noexcept {
    return g_skip_events.load(std::memory_order_relaxed);
}

namespace {

constexpr usize kDiagOnly = 0;

/// Same comma-separated marker-file filter d3d11_diag uses. Duplicated rather
/// than shared because including d3d11_internal.hpp here collides with this
/// file's own IID constants; the marker contract is three lines and stable.
[[nodiscard]] bool pipeline_diag_line_allowed(const char* message) {
    static const std::string filter = [] {
        std::string text;
        if (std::FILE* file = std::fopen("/tmp/vitreo-d3d11-diag", "r")) {
            char buffer[256];
            if (std::fgets(buffer, sizeof(buffer), file) != nullptr) {
                text = buffer;
                while (!text.empty() &&
                       (text.back() == '\n' || text.back() == '\r')) {
                    text.pop_back();
                }
            }
            (void)std::fclose(file);
        }
        return text;
    }();
    if (filter.empty()) return true;
    const std::string_view text(message);
    std::string_view remaining(filter);
    while (!remaining.empty()) {
        const auto comma = remaining.find(',');
        const std::string_view token = remaining.substr(0, comma);
        if (!token.empty() && text.find(token) != std::string_view::npos) {
            return true;
        }
        if (comma == std::string_view::npos) break;
        remaining = remaining.substr(comma + 1);
    }
    return false;
}

} // namespace

/// Whether the per-draw pipeline trace is on. Same marker-file gate as
/// d3d11_diag: an env-only switch never reaches a store-launched guest, so
/// this whole diagnostic was silently off in exactly the process being
/// investigated — and "no draw lines" then reads as "the guest issues no
/// draws", which is a badly wrong conclusion. Callers that build a trace line
/// from extra work (counting bindings) test this first.
[[nodiscard]] bool pipeline_trace_enabled() {
    static const bool enabled = [] {
        const bool marker = ::access("/tmp/vitreo-d3d11-diag", F_OK) == 0;
        const char* value = std::getenv("VITREO_D3D11_DIAG");
        const bool on = marker || (value != nullptr && value[0] != '\0' &&
                                   std::strcmp(value, "0") != 0);
        if (on) {
            std::fprintf(stderr,
                         "[vitreo] WARNING: D3D11 pipeline trace is on (%s); "
                         "it logs and flushes every draw and slows rendering\n",
                         marker ? "/tmp/vitreo-d3d11-diag exists"
                                : "VITREO_D3D11_DIAG");
        }
        return on;
    }();
    return enabled;
}

void pipeline_diag(const char* format, ...) {
    d3d11_skip_stat(format);
    if (!pipeline_trace_enabled()) return;
    static std::FILE* const output = [] {
        const char* path = std::getenv("VITREO_D3D11_DIAG_FILE");
        if (path != nullptr && path[0] != '\0') {
            if (std::FILE* file = std::fopen(path, "a")) return file;
        }
        if (::access("/tmp/vitreo-d3d11-diag", F_OK) == 0) {
            char marker_path[128];
            std::snprintf(marker_path, sizeof(marker_path),
                          "/tmp/vitreo-d3d11-diag-%d.log",
                          static_cast<int>(::getpid()));
            if (std::FILE* file = std::fopen(marker_path, "a")) return file;
        }
        return stderr;
    }();
    char message[2048];
    {
        va_list probe;
        va_start(probe, format);
        (void)std::vsnprintf(message, sizeof(message), format, probe);
        va_end(probe);
    }
    if (!pipeline_diag_line_allowed(message)) return;

    // See d3d11_diag: a mutex, not the stdio lock, so a diagnostic can never
    // abort the process by locking an invalidated FILE.
    // See d3d11_diag: no explicit lock; stdio is thread-safe per FILE.
    if (output == nullptr) return;
    std::fprintf(output, "[d3d11-pipeline pid=%d] ",
                 static_cast<int>(::getpid()));
    va_list args;
    va_start(args, format);
    std::vfprintf(output, format, args);
    va_end(args);
    std::fputc('\n', output);
    std::fflush(output);
}

} // namespace vitreo::winapi
