// SPDX-License-Identifier: LGPL-2.1-only

/// @file frame_profile.cpp
/// @brief VITREO_D3D12_PROFILE: one line a second of per-frame averages.

#include "graphics/common/frame_profile.hpp"
#include "graphics/common/gpu_busy.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace vitreo::graphics::frame_profile {
namespace {

std::atomic<int> g_enabled{-1};

constexpr usize kTimes = static_cast<usize>(Time::Count);
constexpr usize kCounters = static_cast<usize>(Counter::Count);
constexpr usize kReasons = 48;

/// One reason table: a fixed array claimed slot by slot with a CAS on the
/// label pointer, so recording never allocates or locks.
struct ReasonTable {
    std::array<std::atomic<const char*>, kReasons> label{};
    std::array<std::atomic<u64>, kReasons> count{};
    std::array<std::atomic<u64>, kReasons> ns{};
    std::atomic<u64> overflow{0};

    void note(const char* reason, u64 time) noexcept {
        for (usize i = 0; i < kReasons; ++i) {
            const char* seen = label[i].load(std::memory_order_acquire);
            if (seen == nullptr) {
                const char* expected = nullptr;
                if (label[i].compare_exchange_strong(expected, reason,
                                                     std::memory_order_acq_rel)) {
                    seen = reason;
                } else {
                    seen = expected;
                }
            }
            if (seen == reason) {
                count[i].fetch_add(1, std::memory_order_relaxed);
                ns[i].fetch_add(time, std::memory_order_relaxed);
                return;
            }
        }
        overflow.fetch_add(1, std::memory_order_relaxed);
    }

    /// "label=n" (or "label=n/ms") for each reason, heaviest first; resets.
    void format(char* out, usize size, bool with_time, double frames) noexcept {
        struct Row {
            const char* label;
            u64 count;
            u64 ns;
        };
        std::array<Row, kReasons> rows{};
        usize n = 0;
        for (usize i = 0; i < kReasons; ++i) {
            const char* l = label[i].load(std::memory_order_acquire);
            if (l == nullptr) continue;
            const u64 c = count[i].exchange(0, std::memory_order_relaxed);
            const u64 t = ns[i].exchange(0, std::memory_order_relaxed);
            if (c == 0) continue;
            rows[n++] = {l, c, t};
        }
        std::sort(rows.begin(), rows.begin() + static_cast<long>(n),
                  [&](const Row& a, const Row& b) {
                      return with_time ? a.ns > b.ns : a.count > b.count;
                  });
        usize at = 0;
        if (size != 0) out[0] = '\0';
        const double per = frames > 0.0 ? frames : 1.0;
        for (usize i = 0; i < n && at + 1 < size; ++i) {
            const int wrote =
                with_time
                    ? std::snprintf(out + at, size - at, "%s%s=%.1f/%.2fms",
                                    i == 0 ? "" : " ", rows[i].label,
                                    static_cast<double>(rows[i].count) / per,
                                    static_cast<double>(rows[i].ns) / 1e6 / per)
                    : std::snprintf(out + at, size - at, "%s%s=%.1f",
                                    i == 0 ? "" : " ", rows[i].label,
                                    static_cast<double>(rows[i].count) / per);
            if (wrote < 0) break;
            at = std::min(size - 1, at + static_cast<usize>(wrote));
        }
        const u64 lost = overflow.exchange(0, std::memory_order_relaxed);
        if (lost != 0 && at + 1 < size) {
            std::snprintf(out + at, size - at, " (other=%llu)",
                          static_cast<unsigned long long>(lost));
        }
    }
};

struct State {
    std::array<std::atomic<u64>, kTimes> times{};
    std::array<std::atomic<u64>, kCounters> counters{};
    ReasonTable breaks;
    ReasonTable waits;
    ReasonTable internal;
    ReasonTable exclusive_apis;
    std::atomic<u64> frames{0};
    std::atomic<u64> worst_frame_ns{0};
    std::atomic<u64> last_frame_ns{0};
    std::mutex print_mutex;
    u64 last_print_ns{0};
};

State& state() {
    static State s;
    return s;
}

}  // namespace

bool enabled() noexcept {
    int value = g_enabled.load(std::memory_order_relaxed);
    if (value < 0) [[unlikely]] {
        const char* env = std::getenv("VITREO_D3D12_PROFILE");
        value = env != nullptr && env[0] != '\0' && env[0] != '0' ? 1 : 0;
        g_enabled.store(value, std::memory_order_relaxed);
        if (value != 0) set_gpu_busy_wanted(true);
    }
    return value != 0;
}

void set_enabled_for_testing(bool on) noexcept {
    g_enabled.store(on ? 1 : 0, std::memory_order_relaxed);
    if (on) set_gpu_busy_wanted(true);
}

void add(Time time, u64 ns) noexcept {
    state().times[static_cast<usize>(time)].fetch_add(ns, std::memory_order_relaxed);
}

void add(Counter counter, u64 amount) noexcept {
    state().counters[static_cast<usize>(counter)].fetch_add(
        amount, std::memory_order_relaxed);
}

void note_break(const char* reason) noexcept {
    if (!enabled()) return;
    state().breaks.note(reason, 0);
}

void note_internal(const char* reason) noexcept {
    if (!enabled()) return;
    state().internal.note(reason, 0);
}

void note_wait(const char* reason, u64 ns) noexcept {
    if (!enabled()) return;
    State& s = state();
    s.waits.note(reason, ns);
    s.times[static_cast<usize>(Time::CpuWait)].fetch_add(ns, std::memory_order_relaxed);
}

void note_exclusive_api(const char* reason, u64 ns) noexcept {
    if (!enabled()) return;
    state().exclusive_apis.note(reason, ns);
}

Snapshot take() noexcept {
    State& s = state();
    Snapshot out{};
    for (usize i = 0; i < kTimes; ++i) {
        out.times[i] = s.times[i].exchange(0, std::memory_order_relaxed);
    }
    for (usize i = 0; i < kCounters; ++i) {
        out.counters[i] = s.counters[i].exchange(0, std::memory_order_relaxed);
    }
    out.frames = s.frames.exchange(0, std::memory_order_relaxed);
    return out;
}

void format_reasons(char* out, usize size) noexcept {
    if (size == 0) return;
    char breaks[2048];
    char waits[2048];
    state().breaks.format(breaks, sizeof(breaks), false, 1.0);
    state().waits.format(waits, sizeof(waits), true, 1.0);
    std::snprintf(out, size, "breaks{%s} waits{%s}", breaks, waits);
}

void tick() noexcept {
    if (!enabled()) return;
    State& s = state();
    const u64 now = now_ns();
    const u64 last = s.last_frame_ns.exchange(now, std::memory_order_relaxed);
    if (last != 0) {
        const u64 interval = now - last;
        u64 worst = s.worst_frame_ns.load(std::memory_order_relaxed);
        while (interval > worst &&
               !s.worst_frame_ns.compare_exchange_weak(worst, interval,
                                                      std::memory_order_relaxed)) {
        }
    }
    s.frames.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lock(s.print_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (s.last_print_ns == 0) {
        s.last_print_ns = now;
        return;
    }
    const u64 elapsed = now - s.last_print_ns;
    if (elapsed < 1'000'000'000ull) return;
    s.last_print_ns = now;
    const Snapshot snap = take();
    const u64 worst = s.worst_frame_ns.exchange(0, std::memory_order_relaxed);
    const double frames = snap.frames > 0 ? static_cast<double>(snap.frames) : 1.0;
    const auto ms = [&](Time t) {
        return static_cast<double>(snap.times[static_cast<usize>(t)]) / 1e6 / frames;
    };
    const auto per = [&](Counter c) {
        return static_cast<double>(snap.counters[static_cast<usize>(c)]) / frames;
    };
    const RecentGpuBusy gpu = recent_gpu_busy(gpu_busy_clock_s());
    const double seconds = static_cast<double>(elapsed) / 1e9;
    char breaks[1536];
    char waits[1536];
    char internal[1536];
    char exclusive_apis[1536];
    s.breaks.format(breaks, sizeof(breaks), false, frames);
    s.waits.format(waits, sizeof(waits), true, frames);
    s.internal.format(internal, sizeof(internal), false, frames);
    s.exclusive_apis.format(exclusive_apis, sizeof(exclusive_apis), true, frames);
    const u64 executes = snap.counters[static_cast<usize>(Counter::Executes)];
    std::fprintf(
        stderr,
        "[d3d12-profile] fps=%.1f frame=%.2fms worst=%.1fms gpu=%.1fms/s "
        "api{rec=%.2f exec=%.2f present=%.2f sync=%.2f pso=%.2f res=%.2f "
        "other=%.2f calls=%.0f} lock{wait=%.2fms contended=%.0f} "
        "replay{%.2fms lists=%.1f execs=%.1f max-per-exec=%.2fms flush=%.2fms} "
        "draws=%.0f disp=%.0f encode{%.2fms draws=%.0f disp=%.0f render=%.1f "
        "compute=%.1f blit=%.1f begun=%.1f merged=%.1f} breaks{%s} "
        "waits{%.2fms %s} variants{%.1f %.2fms} window{syncs=%.0f resolves=%.0f} "
        "tables=%.0f cbv-size=%.0f residency=%.0f upload{%.1fKB chunks=%.2f} "
        "present-wait=%.2fms internal{%s} exclusive-api{%s}\n",
        static_cast<double>(snap.frames) / seconds, seconds * 1e3 / frames,
        static_cast<double>(worst) / 1e6, gpu.busy_ms, ms(Time::ApiRecord),
        ms(Time::ApiExecute), ms(Time::ApiPresent), ms(Time::ApiSync),
        ms(Time::ApiPso), ms(Time::ApiResource), ms(Time::ApiOther),
        per(Counter::ApiCalls), ms(Time::LockWait), per(Counter::LockContended),
        ms(Time::Replay), per(Counter::ListsReplayed), per(Counter::Executes),
        executes != 0 ? static_cast<double>(snap.times[static_cast<usize>(Time::Replay)]) /
                            1e6 / static_cast<double>(executes)
                      : 0.0,
        ms(Time::BackendFlush), per(Counter::Draws), per(Counter::Dispatches),
        ms(Time::Encode), per(Counter::EncodedDraws),
        per(Counter::EncodedDispatches), per(Counter::RenderEncoders),
        per(Counter::ComputeEncoders), per(Counter::BlitEncoders),
        per(Counter::PassesBegun), per(Counter::PassesMerged), breaks,
        ms(Time::CpuWait), waits, per(Counter::VariantCompiles),
        ms(Time::VariantCompile), per(Counter::WindowSyncs),
        per(Counter::WindowResolves), per(Counter::TableSets),
        per(Counter::RootCbvSizeScans), per(Counter::ResidencyDecls),
        static_cast<double>(snap.counters[static_cast<usize>(Counter::UploadBytes)]) /
            1024.0 / frames,
        per(Counter::UploadChunks), ms(Time::PresentWait), internal, exclusive_apis);
}

}  // namespace vitreo::graphics::frame_profile
