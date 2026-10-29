#import <Metal/Metal.h>

#include "graphics/metal/metal_gpu_watch.hpp"
#include "graphics/common/gpu_busy.hpp"

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace vitreo::graphics::gpu_watch {

namespace {

using Clock = std::chrono::steady_clock;

struct Entry {
    Clock::time_point committed;
    std::string summary;
    std::vector<EventWait> waits;
    bool reported{false};
    /// The command buffer itself (retained), so the watchdog can ask Metal
    /// whether the GPU really is still on it or only its completion handler
    /// has not been delivered.
    void* buffer{nullptr};
};

std::mutex g_mutex;
std::map<u64, Entry> g_inflight;  // ordered by commit serial
u64 g_next_serial = 1;
// Per-second GPU utilization (VITREO_GPU_WATCH_STATS=1): GPU-busy seconds and
// command buffers retired in the current one-second window.
double g_window_busy = 0.0;
double g_window_slowest = 0.0;
u64 g_window_count = 0;
Clock::time_point g_window_start = Clock::now();

void watchdog_loop() {
    // Age is measured from when a command buffer became the OLDEST one in
    // flight, not from its commit. A queue executes in order, so time spent
    // waiting behind a slow predecessor is not this buffer's GPU time; timing
    // from commit killed the guest for a buffer that had merely queued
    // behind one legitimately heavy 3.4 s frame.
    u64 head_serial = 0;
    Clock::time_point head_since{};
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_inflight.empty()) continue;
        auto& [serial, entry] = *g_inflight.begin();
        if (serial != head_serial) {
            head_serial = serial;
            head_since = std::max(entry.committed, Clock::now() -
                                  std::chrono::milliseconds(200));
        }
        const double age =
            std::chrono::duration<double>(Clock::now() - head_since).count();
        // Completion handlers of one queue are delivered serially. If the
        // GPU finished this buffer but its handler has not run, a handler
        // ahead of it is blocked on the host (e.g. on a lock held by a thread
        // that is itself waiting for a completion): a HOST deadlock, not a
        // GPU hang. Killing the process for it would hide the real bug and
        // lose the game; dump the guest threads instead and keep watching.
        MTLCommandBufferStatus status = MTLCommandBufferStatusNotEnqueued;
        if (entry.buffer != nullptr) {
            status = ((__bridge id<MTLCommandBuffer>)entry.buffer).status;
        }
        const bool gpu_done = status == MTLCommandBufferStatusCompleted ||
                              status == MTLCommandBufferStatusError;
        if (age >= 2.0 && gpu_done) {
            static bool dumped = false;
            if (!dumped) {
                dumped = true;
                std::fprintf(stderr,
                             "[metal] GPU WATCHDOG: command buffer #%llu "
                             "FINISHED on the GPU (status %ld) %.1f s ago "
                             "but its completion handler has not run: a "
                             "completion handler is blocked on the host. "
                             "Dumping guest threads (SIGUSR1).\n",
                             static_cast<unsigned long long>(serial),
                             static_cast<long>(status), age);
                std::fflush(stderr);
                ::kill(::getpid(), SIGUSR1);
            }
            continue;
        }
        if (age >= 2.0 && !entry.reported) {
            entry.reported = true;
            std::fprintf(stderr,
                         "[metal] GPU WATCHDOG: command buffer #%llu not "
                         "complete after %.1f s (%zu queued behind it), "
                         "status %ld (2=committed: blocked before the GPU "
                         "started it, 3=scheduled: running). Contents: %s\n",
                         static_cast<unsigned long long>(serial), age,
                         g_inflight.size() - 1, static_cast<long>(status),
                         entry.summary.c_str());
            for (const EventWait& wait : entry.waits) {
                id<MTLSharedEvent> event =
                    (__bridge id<MTLSharedEvent>)wait.event;
                unsigned long long now = 0;
                if ([event respondsToSelector:@selector(signaledValue)]) {
                    now = static_cast<unsigned long long>(event.signaledValue);
                }
                std::fprintf(stderr,
                             "[metal] GPU WATCHDOG:   waits for event %p to "
                             "reach %llu; it is at %llu%s\n",
                             wait.event,
                             static_cast<unsigned long long>(wait.value), now,
                             now < wait.value ? "  <-- UNSATISFIED" : "");
            }
            std::fflush(stderr);
        }
        // Windows' TDR resets the device after 2 s on one packet; a command
        // buffer here holds many. The loop guard already bounds every shader
        // loop, so this is only the backstop that keeps a wedged GPU from
        // freezing WindowServer indefinitely.
        if (age >= 8.0) {
            std::fprintf(stderr,
                         "[metal] GPU WATCHDOG: command buffer #%llu status "
                         "%ld (1=enqueued 2=committed 3=scheduled); "
                         "terminating the process so it stops queueing GPU "
                         "work behind it\n",
                         static_cast<unsigned long long>(serial),
                         static_cast<long>(status));
            std::fflush(stderr);
            ::_exit(0x1C);
        }
    }
}

}  // namespace

bool enabled() noexcept {
    static const bool on = [] {
        const char* text = std::getenv("VITREO_GPU_WATCHDOG");
        return text == nullptr || text[0] != '0';
    }();
    return on;
}

void note(std::string& summary, const char* label) {
    if (label == nullptr || label[0] == '\0') return;
    constexpr usize kMax = 4096;
    if (summary.size() >= kMax) return;
    // Skip an immediate repeat: a thousand draws of one pipeline are one entry.
    const usize last = summary.rfind(' ');
    const char* tail = summary.c_str() + (last == std::string::npos ? 0 : last + 1);
    if (std::strcmp(tail, label) == 0) return;
    if (!summary.empty()) summary.push_back(' ');
    summary.append(label);
}

void time_busy(void* command_buffer) {
    if (!gpu_busy_wanted() || command_buffer == nullptr) return;
    id<MTLCommandBuffer> buf = (__bridge id<MTLCommandBuffer>)command_buffer;
    [buf addCompletedHandler:^(id<MTLCommandBuffer> done) {
      note_gpu_interval(done.GPUStartTime, done.GPUEndTime,
                        static_cast<u64>(done.device.currentAllocatedSize));
    }];
}

void track(void* command_buffer, std::string summary,
           std::vector<EventWait> waits) {
    time_busy(command_buffer);
    if (!enabled() || command_buffer == nullptr) return;
    for (EventWait& wait : waits) {
        if (wait.event != nullptr) CFRetain(wait.event);
    }
    static std::once_flag started;
    std::call_once(started, [] { std::thread(watchdog_loop).detach(); });
    u64 serial = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        serial = g_next_serial++;
        g_inflight.emplace(serial,
                           Entry{Clock::now(), std::move(summary),
                                 std::move(waits), false,
                                 const_cast<void*>(CFRetain(command_buffer))});
    }
    id<MTLCommandBuffer> buf = (__bridge id<MTLCommandBuffer>)command_buffer;
    [buf addCompletedHandler:^(id<MTLCommandBuffer> done) {
      const double gpu_seconds = done.GPUEndTime - done.GPUStartTime;
      std::lock_guard<std::mutex> lock(g_mutex);
      static const bool stats = std::getenv("VITREO_GPU_WATCH_STATS") != nullptr;
      if (stats) {
          g_window_busy += gpu_seconds > 0 ? gpu_seconds : 0.0;
          g_window_slowest = std::max(g_window_slowest, gpu_seconds);
          ++g_window_count;
          const double elapsed = std::chrono::duration<double>(
              Clock::now() - g_window_start).count();
          if (elapsed >= 1.0) {
              std::fprintf(stderr,
                           "[metal] gpu-stats %.0f ms busy / %.0f ms, %llu "
                           "command buffers, slowest %.1f ms, %zu in flight\n",
                           g_window_busy * 1000.0, elapsed * 1000.0,
                           static_cast<unsigned long long>(g_window_count),
                           g_window_slowest * 1000.0, g_inflight.size());
              g_window_busy = g_window_slowest = 0.0;
              g_window_count = 0;
              g_window_start = Clock::now();
          }
      }
      auto it = g_inflight.find(serial);
      if (it == g_inflight.end()) return;
      static const double slow_seconds = [] {
          const char* text = std::getenv("VITREO_GPU_WATCH_SLOW_MS");
          return text != nullptr ? std::atof(text) / 1000.0 : 1.0;
      }();
      if (gpu_seconds > slow_seconds ||
          done.status == MTLCommandBufferStatusError) {
          std::fprintf(stderr,
                       "[metal] slow/failed command buffer #%llu: gpu=%.2f s "
                       "status=%ld. Contents: %s\n",
                       static_cast<unsigned long long>(serial), gpu_seconds,
                       static_cast<long>(done.status),
                       it->second.summary.c_str());
      }
      if (it->second.buffer != nullptr) CFRelease(it->second.buffer);
      for (const EventWait& wait : it->second.waits) {
          if (wait.event != nullptr) CFRelease(wait.event);
      }
      g_inflight.erase(it);
    }];
}

}  // namespace vitreo::graphics::gpu_watch
