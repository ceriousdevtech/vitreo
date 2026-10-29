// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include "translation_store_stats.hpp"

#include <vitreo/diagnostics.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <mach/mach_time.h>
#include <sys/time.h>
#include <unistd.h>

namespace vitreo::cpu::jit {

namespace {

std::atomic<int> g_mode_override{-1};

TranslationStoreMode parse_mode() noexcept {
    const char* v = std::getenv("VITREO_TRANSLATION_STORE");
    if (v == nullptr || v[0] == '\0') return TranslationStoreMode::Warm;
    if (std::strcmp(v, "off") == 0 || std::strcmp(v, "0") == 0) {
        return TranslationStoreMode::Off;
    }
    if (std::strcmp(v, "measure") == 0) return TranslationStoreMode::Measure;
    if (std::strcmp(v, "warm") == 0) return TranslationStoreMode::Warm;
    std::fprintf(stderr,
                 "[tstore] VITREO_TRANSLATION_STORE=%s IGNORED: expected off, "
                 "measure or warm\n",
                 v);
    return TranslationStoreMode::Off;
}

}  // namespace

TranslationStoreMode translation_store_mode() noexcept {
    if (const int forced = g_mode_override.load(std::memory_order_relaxed);
        forced >= 0) {
        return static_cast<TranslationStoreMode>(forced);
    }
    static const TranslationStoreMode mode = parse_mode();
    return mode;
}

void set_translation_store_mode_for_test(bool force,
                                         TranslationStoreMode mode) noexcept {
    g_mode_override.store(force ? static_cast<int>(mode) : -1,
                          std::memory_order_relaxed);
}

namespace tstats {
namespace {

constexpr usize kOutcomes = 5;
constexpr usize kEvents = static_cast<usize>(Event::Count);

std::atomic<u64> g_compiles[kOutcomes]{};
std::atomic<u64> g_compile_ns[kOutcomes]{};
std::atomic<u64> g_decodes{0};
std::atomic<u64> g_decode_ns{0};
std::atomic<u64> g_events[kEvents]{};
std::atomic<ShutdownHook> g_shutdown_hook{nullptr};
std::atomic<bool> g_shutdown_done{false};

constexpr const char* kOutcomeName[kOutcomes] = {"none", "memo", "store", "verified",
                                                 "fresh"};
constexpr const char* kEventName[kEvents] = {
    "lookup",        "hit",           "miss",          "miss_index_not_ready",
    "rej_mac",       "rej_bytes",     "rej_decode",    "rej_shape",
    "rej_reloc",     "verify_ok",     "verify_bad",    "produced",
    "produced_bytes", "skip_not_image", "skip_seen",   "skip_delegate",
    "skip_pinless",  "skip_unreloc",  "skip_host_addr", "skip_full",
    "skip_diag",     "produced_relocs", "rej_stub",    "skip_dup",
};
static_assert(sizeof(kEventName) / sizeof(kEventName[0]) == kEvents);

u64 realtime_ms() noexcept {
    timeval tv{};
    gettimeofday(&tv, nullptr);
    return static_cast<u64>(tv.tv_sec) * 1000u + static_cast<u64>(tv.tv_usec) / 1000u;
}

/// The CSV sampler: one per process, started by the first counted event.
struct Sampler {
    std::mutex mutex;
    std::FILE* file{nullptr};
    bool started{false};
    u64 start_ticks{0};
    u64 last_line_sum{~u64{0}};
    u64 last_report_ticks{0};

    void write_line_locked(const char* mark) noexcept {
        if (file == nullptr) return;
        const Totals t = totals();
        const u64 mono_ms = ticks_to_ns(now_ticks() - start_ticks) / 1000000u;
        if (mark != nullptr) {
            std::fprintf(file, "#mark,%s,%llu,%llu\n", mark,
                         static_cast<unsigned long long>(realtime_ms()),
                         static_cast<unsigned long long>(mono_ms));
        }
        std::fprintf(file, "%llu,%llu", static_cast<unsigned long long>(realtime_ms()),
                     static_cast<unsigned long long>(mono_ms));
        for (usize i = 0; i < kOutcomes; ++i) {
            std::fprintf(file, ",%llu,%llu", static_cast<unsigned long long>(t.compiles[i]),
                         static_cast<unsigned long long>(t.compile_ns[i]));
        }
        std::fprintf(file, ",%llu,%llu", static_cast<unsigned long long>(t.decodes),
                     static_cast<unsigned long long>(t.decode_ns));
        for (usize i = 0; i < kEvents; ++i) {
            std::fprintf(file, ",%llu",
                         static_cast<unsigned long long>(
                             g_events[i].load(std::memory_order_relaxed)));
        }
        std::fputc('\n', file);
        std::fflush(file);
    }

    void open_locked() noexcept {
        start_ticks = now_ticks();
        const char* dir = std::getenv("VITREO_LOG_DIR");
        if (dir != nullptr && dir[0] != '\0') {
            char path[1024];
            std::snprintf(path, sizeof path, "%s/tstore_%d.csv", dir,
                          static_cast<int>(::getpid()));
            file = std::fopen(path, "a");
        }
        if (file != nullptr) {
            std::fprintf(file, "realtime_ms,mono_ms");
            for (usize i = 0; i < kOutcomes; ++i) {
                std::fprintf(file, ",%s_n,%s_ns", kOutcomeName[i], kOutcomeName[i]);
            }
            std::fprintf(file, ",decode_n,decode_ns");
            for (usize i = 0; i < kEvents; ++i) std::fprintf(file, ",%s", kEventName[i]);
            std::fputc('\n', file);
            write_line_locked("tstore_start");
        }
        std::fprintf(stderr, "[tstore] measuring pid=%d mode=%s csv=%s\n",
                     static_cast<int>(::getpid()),
                     translation_store_mode() == TranslationStoreMode::Warm ? "warm"
                                                                             : "measure",
                     file != nullptr ? "yes" : "no (VITREO_LOG_DIR unset)");
    }

    void tick() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        u64 sum = g_decodes.load(std::memory_order_relaxed);
        for (usize i = 0; i < kOutcomes; ++i) {
            sum += g_compiles[i].load(std::memory_order_relaxed);
        }
        if (sum != last_line_sum) {
            last_line_sum = sum;
            write_line_locked(nullptr);
        }
        const u64 now = now_ticks();
        if (ticks_to_ns(now - last_report_ticks) >= 10'000'000'000ull) {
            last_report_ticks = now;
            report_locked("periodic");
        }
    }

    void report_locked(const char* why) noexcept {
        const Totals t = totals();
        char buf[2048];
        int n = std::snprintf(buf, sizeof buf, "[tstore] %s pid=%d", why,
                              static_cast<int>(::getpid()));
        for (usize i = 0; i < kOutcomes && n > 0 && n < static_cast<int>(sizeof buf); ++i) {
            n += std::snprintf(buf + n, sizeof buf - static_cast<usize>(n),
                               " %s=%llu/%.1fms", kOutcomeName[i],
                               static_cast<unsigned long long>(t.compiles[i]),
                               static_cast<double>(t.compile_ns[i]) / 1e6);
        }
        if (n > 0 && n < static_cast<int>(sizeof buf)) {
            n += std::snprintf(buf + n, sizeof buf - static_cast<usize>(n),
                               " decode=%llu/%.1fms",
                               static_cast<unsigned long long>(t.decodes),
                               static_cast<double>(t.decode_ns) / 1e6);
        }
        for (usize i = 0; i < kEvents && n > 0 && n < static_cast<int>(sizeof buf); ++i) {
            const u64 v = g_events[i].load(std::memory_order_relaxed);
            if (v == 0) continue;
            n += std::snprintf(buf + n, sizeof buf - static_cast<usize>(n), " %s=%llu",
                               kEventName[i], static_cast<unsigned long long>(v));
        }
        std::fprintf(stderr, "%s\n", buf);
    }
};

Sampler& sampler() noexcept {
    static Sampler* s = new Sampler();
    return *s;
}

void on_phase(diagnostics::LoadPhase phase) noexcept {
    Sampler& s = sampler();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.write_line_locked(diagnostics::phase_name(phase));
}

void ensure_started() noexcept {
    static std::once_flag once;
    std::call_once(once, [] {
        Sampler& s = sampler();
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.open_locked();
            s.started = true;
            s.last_report_ticks = now_ticks();
        }
        diagnostics::phase_set_observer(&on_phase);
        std::atexit([] { shutdown(); });
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                sampler().tick();
            }
        }).detach();
    });
}

}  // namespace

u64 now_ticks() noexcept { return mach_absolute_time(); }

u64 ticks_to_ns(u64 ticks) noexcept {
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return info;
    }();
    return static_cast<u64>((static_cast<__uint128_t>(ticks) * tb.numer) / tb.denom);
}

void note_decode(u64 ns) noexcept {
    if (!measuring()) return;
    ensure_started();
    g_decodes.fetch_add(1, std::memory_order_relaxed);
    g_decode_ns.fetch_add(ns, std::memory_order_relaxed);
}

void note_compile(TranslationOutcome outcome, u64 ns) noexcept {
    if (!measuring()) return;
    ensure_started();
    const auto i = static_cast<usize>(outcome);
    g_compiles[i].fetch_add(1, std::memory_order_relaxed);
    g_compile_ns[i].fetch_add(ns, std::memory_order_relaxed);
}

void note(Event event, u64 n) noexcept {
    if (!measuring()) return;
    g_events[static_cast<usize>(event)].fetch_add(n, std::memory_order_relaxed);
}

u64 count(Event event) noexcept {
    return g_events[static_cast<usize>(event)].load(std::memory_order_relaxed);
}

Totals totals() noexcept {
    Totals t;
    for (usize i = 0; i < kOutcomes; ++i) {
        t.compiles[i] = g_compiles[i].load(std::memory_order_relaxed);
        t.compile_ns[i] = g_compile_ns[i].load(std::memory_order_relaxed);
    }
    t.decodes = g_decodes.load(std::memory_order_relaxed);
    t.decode_ns = g_decode_ns.load(std::memory_order_relaxed);
    return t;
}

void report(const char* why) noexcept {
    Sampler& s = sampler();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.report_locked(why);
}

void flush() noexcept {
    Sampler& s = sampler();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.write_line_locked(nullptr);
}

void set_shutdown_hook(ShutdownHook hook) noexcept {
    g_shutdown_hook.store(hook, std::memory_order_release);
}

void shutdown() noexcept {
    if (g_shutdown_done.exchange(true, std::memory_order_acq_rel)) return;
    if (ShutdownHook hook = g_shutdown_hook.load(std::memory_order_acquire)) hook();
    Sampler& s = sampler();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.started) return;
    s.write_line_locked("exit");
    s.report_locked("exit");
}

void reset_for_test() noexcept {
    for (usize i = 0; i < kOutcomes; ++i) {
        g_compiles[i].store(0, std::memory_order_relaxed);
        g_compile_ns[i].store(0, std::memory_order_relaxed);
    }
    g_decodes.store(0, std::memory_order_relaxed);
    g_decode_ns.store(0, std::memory_order_relaxed);
    for (auto& e : g_events) e.store(0, std::memory_order_relaxed);
}

}  // namespace tstats
}  // namespace vitreo::cpu::jit
