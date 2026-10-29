// SPDX-License-Identifier: LGPL-2.1-only

/// @file input_latency.cpp
/// @brief Implementation of the input-latency probe. See input_latency.hpp.

#include "windowing/messages/input_latency.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

namespace vitreo::windowing::input_latency {

namespace {

constexpr u32 kKeyFirst   = 0x0100;
constexpr u32 kKeyLast    = 0x0109;
constexpr u32 kMouseFirst = 0x0200;
constexpr u32 kMouseLast  = 0x020E;

/// Per-message-id accumulator. Samples are kept so the report can give
/// percentiles: a mean hides exactly the stalls this probe exists to find.
/// Capped so a long session cannot grow without bound; once full the bucket
/// keeps counting (n) but stops sampling, and the report says so.
constexpr usize kMaxSamples = 200000;

struct Bucket {
    u64 n = 0;
    u64 dropped = 0;
    std::vector<u32> queue_us;
    std::vector<u32> proc_us;
    std::vector<u32> flush_us;   ///< subset of proc_us: Vitreo's own flush
};

struct State {
    std::mutex mutex;
    std::vector<u32> event_age_us;
    std::vector<u32> click_to_pixel_us;
    std::atomic<u64> pending_action_ns{0};
    std::array<Bucket, 0x20F> buckets{};   // indexed by message id
    std::FILE* out = nullptr;
    bool trace = false;
    bool reported = false;
};

State& state() {
    static State s;
    return s;
}

bool init() {
    State& s = state();
    const char* dest = std::getenv("VITREO_INPUT_LATENCY");
    if (dest == nullptr || dest[0] == '\0') return false;
    if (std::strcmp(dest, "1") == 0) {
        s.out = stderr;
    } else {
        // One file per process: several guest processes share this env var, and
        // interleaved writes from four of them would be unreadable.
        std::string path = dest;
        path += '.';
        path += std::to_string(static_cast<int>(::getpid()));
        s.out = std::fopen(path.c_str(), "w");
        if (s.out == nullptr) return false;
    }
    const char* trace = std::getenv("VITREO_INPUT_LATENCY_TRACE");
    s.trace = trace != nullptr && trace[0] == '1';
    std::atexit([] { report(); });
    return true;
}

/// The message id a thread last took off the queue, with its arrival time.
/// A Win32 pump peeks and dispatches on the same thread, so this reliably
/// pairs the two halves without threading a field through guest memory.
thread_local u32 t_pending_message = 0;
thread_local HWND t_pending_hwnd = NULL_HWND;
thread_local u64 t_pending_post_ns = 0;

u32 to_us(u64 ns) noexcept {
    const u64 us = ns / 1000u;
    return static_cast<u32>(us > 0xFFFFFFFFull ? 0xFFFFFFFFull : us);
}

const char* message_name(u32 id) noexcept {
    switch (id) {
        case 0x0100: return "WM_KEYDOWN";
        case 0x0101: return "WM_KEYUP";
        case 0x0102: return "WM_CHAR";
        case 0x0104: return "WM_SYSKEYDOWN";
        case 0x0105: return "WM_SYSKEYUP";
        case 0x0200: return "WM_MOUSEMOVE";
        case 0x0201: return "WM_LBUTTONDOWN";
        case 0x0202: return "WM_LBUTTONUP";
        case 0x0203: return "WM_LBUTTONDBLCLK";
        case 0x0204: return "WM_RBUTTONDOWN";
        case 0x0205: return "WM_RBUTTONUP";
        case 0x0207: return "WM_MBUTTONDOWN";
        case 0x0208: return "WM_MBUTTONUP";
        case 0x020A: return "WM_MOUSEWHEEL";
        case 0x020E: return "WM_MOUSEHWHEEL";
        default:     return "";
    }
}

u32 percentile(std::vector<u32>& v, double p) noexcept {
    if (v.empty()) return 0;
    const usize idx = std::min(v.size() - 1,
        static_cast<usize>(p * static_cast<double>(v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + static_cast<isize>(idx), v.end());
    return v[idx];
}

} // namespace

u64 now_ns() noexcept {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000000000ull +
           static_cast<u64>(ts.tv_nsec);
}

bool enabled() noexcept {
    static const bool on = init();
    return on;
}

bool is_input_message(u32 message) noexcept {
    return (message >= kKeyFirst && message <= kKeyLast) ||
           (message >= kMouseFirst && message <= kMouseLast);
}

void note_event_age(u64 age_ns) noexcept {
    if (!enabled()) return;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.event_age_us.size() < kMaxSamples) {
        s.event_age_us.push_back(to_us(age_ns));
    }
}

void note_action_event() noexcept {
    if (!enabled()) return;
    State& s = state();
    u64 expected = 0;
    // Keep the OLDEST unanswered action: the honest latency is measured from
    // the first input the frame owes a response to, not the most recent.
    const u64 timestamp = now_ns();
    s.pending_action_ns.compare_exchange_strong(expected, timestamp,
                                                std::memory_order_relaxed);
    if (s.trace && s.out != nullptr) {
        std::lock_guard<std::mutex> lock(s.mutex);
        // Publish before guest dispatch so an external profiler can capture
        // the work caused by this action, without a coordinated time window.
        std::fprintf(s.out, "action pid=%d monotonic_ns=%llu\n",
                     static_cast<int>(::getpid()), timestamp);
        std::fflush(s.out);
    }
}

u64 take_pending_action_age() noexcept {
    if (!enabled()) return 0;
    State& s = state();
    const u64 started = s.pending_action_ns.exchange(0, std::memory_order_relaxed);
    if (started == 0) return 0;
    const u64 t = now_ns();
    if (t <= started) return 0;
    const u64 age = t - started;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.click_to_pixel_us.size() < kMaxSamples) {
            s.click_to_pixel_us.push_back(to_us(age));
        }
    }
    return age;
}

void note_delivered(const MSG& msg) noexcept {
    if (!enabled() || !is_input_message(msg.message)) return;

    t_pending_message = msg.message;
    t_pending_hwnd = msg.hwnd;
    t_pending_post_ns = msg.host_post_ns;

    if (msg.host_post_ns == 0) return;   // never went through post_impl
    const u64 waited = now_ns() - msg.host_post_ns;

    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    Bucket& b = s.buckets[msg.message];
    ++b.n;
    if (b.queue_us.size() < kMaxSamples) {
        b.queue_us.push_back(to_us(waited));
    } else {
        ++b.dropped;
    }
}

void note_flush(const MSG& msg, u64 begin_ns) noexcept {
    if (begin_ns == 0) return;
    const u64 spent = now_ns() - begin_ns;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    Bucket& b = s.buckets[msg.message];
    if (b.flush_us.size() < kMaxSamples) b.flush_us.push_back(to_us(spent));
}

u64 note_dispatch_begin(const MSG& msg) noexcept {
    if (!enabled() || !is_input_message(msg.message)) return 0;
    return now_ns();
}

void note_dispatch_end(const MSG& msg, u64 begin_ns) noexcept {
    if (begin_ns == 0) return;
    const u64 end_ns = now_ns();
    const u64 proc = end_ns - begin_ns;

    // The queue half was recorded at peek time; pair it only when this is the
    // same message that thread took off the queue. A guest may DispatchMessage
    // something it synthesised itself, which has no queue time to speak of.
    const bool paired = t_pending_message == msg.message &&
                        t_pending_hwnd == msg.hwnd &&
                        t_pending_post_ns != 0;
    const u64 post_ns = paired ? t_pending_post_ns : 0;
    if (paired) t_pending_post_ns = 0;   // consume: do not pair twice

    State& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        Bucket& b = s.buckets[msg.message];
        if (b.proc_us.size() < kMaxSamples) b.proc_us.push_back(to_us(proc));
    }
    if (s.trace && s.out != nullptr) {
        std::fprintf(s.out,
            "msg=0x%03x %-16s hwnd=0x%x queue_us=%u proc_us=%u total_us=%u "
            "wparam=0x%llx lparam=0x%llx\n",
            msg.message, message_name(msg.message), msg.hwnd,
            paired ? to_us(begin_ns - post_ns) : 0u,
            to_us(proc),
            paired ? to_us(end_ns - post_ns) : 0u,
            static_cast<unsigned long long>(msg.wParam),
            static_cast<unsigned long long>(msg.lParam));
    }
}

void report() noexcept {
    State& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.reported || s.out == nullptr) return;
        s.reported = true;
    }
    report_to(s.out);
}

void report_to(std::FILE* out) noexcept {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (out == nullptr || s.out == nullptr) return;   // probe never armed

    std::fprintf(out,
        "\n=== input latency (pid %d) ===\n"
        "queue = post -> PeekMessage/GetMessage removed it (Vitreo's wait)\n"
        "proc  = DispatchMessage -> guest wndproc returned (the app's work)\n"
        "flush = the part of proc that is Vitreo's framebuffer flush, not the app\n"
        "%-18s %8s %9s %9s %9s %9s %9s %9s %9s %9s\n",
        static_cast<int>(::getpid()),
        "message", "n",
        "q_p50us", "q_p95us", "q_maxus",
        "p_p50us", "p_p95us", "p_maxus",
        "f_p50us", "f_p95us");

    if (!s.click_to_pixel_us.empty()) {
        std::fprintf(out,
            "CLICK TO PIXELS (action dequeued -> next frame presented):\n"
            "  n=%zu  p50=%u us  p95=%u us  max=%u us\n",
            s.click_to_pixel_us.size(),
            percentile(s.click_to_pixel_us, 0.50),
            percentile(s.click_to_pixel_us, 0.95),
            *std::max_element(s.click_to_pixel_us.begin(),
                              s.click_to_pixel_us.end()));
    }
    if (!s.event_age_us.empty()) {
        std::fprintf(out,
            "macOS event age at pump_events dequeue (BEFORE the queue):\n"
            "  n=%zu  p50=%u us  p95=%u us  max=%u us\n",
            s.event_age_us.size(),
            percentile(s.event_age_us, 0.50),
            percentile(s.event_age_us, 0.95),
            *std::max_element(s.event_age_us.begin(), s.event_age_us.end()));
    }

    bool any = false;
    for (u32 id = 0; id < s.buckets.size(); ++id) {
        Bucket& b = s.buckets[id];
        if (b.n == 0 && b.proc_us.empty() && b.flush_us.empty()) continue;
        any = true;
        char label[32];
        const char* name = message_name(id);
        if (name[0] != '\0') {
            std::snprintf(label, sizeof(label), "%s", name);
        } else {
            std::snprintf(label, sizeof(label), "0x%03x", id);
        }
        std::fprintf(out,
            "%-18s %8llu %9u %9u %9u %9u %9u %9u %9u %9u\n",
            label, static_cast<unsigned long long>(b.n),
            percentile(b.queue_us, 0.50), percentile(b.queue_us, 0.95),
            b.queue_us.empty()
                ? 0u : *std::max_element(b.queue_us.begin(), b.queue_us.end()),
            percentile(b.proc_us, 0.50), percentile(b.proc_us, 0.95),
            b.proc_us.empty()
                ? 0u : *std::max_element(b.proc_us.begin(), b.proc_us.end()),
            percentile(b.flush_us, 0.50), percentile(b.flush_us, 0.95));
        if (b.dropped != 0) {
            std::fprintf(out, "%-18s (%llu samples beyond the %zu cap were "
                                "counted but not sampled)\n",
                         "", static_cast<unsigned long long>(b.dropped),
                         kMaxSamples);
        }
    }
    if (!any) {
        std::fprintf(out, "(no input messages reached the guest)\n");
    }
    std::fflush(out);
}

} // namespace vitreo::windowing::input_latency
