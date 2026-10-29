// SPDX-License-Identifier: LGPL-2.1-only

#include "winapi/core/host_gui_pump.hpp"
#include <vitreo/diagnostics.hpp>

#include <chrono>
#include <mutex>
#include <pthread.h>

namespace vitreo::winapi {
namespace {

std::mutex g_pump_mutex;
HostGuiPump g_pump;
std::chrono::steady_clock::time_point g_last_pump{};
thread_local int g_host_gui_pump_depth = 0;

} // namespace

void set_host_gui_pump(HostGuiPump pump) {
    std::lock_guard<std::mutex> lock(g_pump_mutex);
    g_pump = std::move(pump);
}

void clear_host_gui_pump() {
    std::lock_guard<std::mutex> lock(g_pump_mutex);
    g_pump = {};
    g_last_pump = {};
}

bool in_host_gui_pump() noexcept {
    return g_host_gui_pump_depth > 0;
}

void pump_host_gui() {
    if (pthread_main_np() == 0) {
        return;
    }

    HostGuiPump pump;
    {
        std::lock_guard<std::mutex> lock(g_pump_mutex);
        if (!g_pump) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (g_last_pump.time_since_epoch().count() != 0 &&
            now - g_last_pump < std::chrono::milliseconds(16)) {
            return;
        }
        g_last_pump = now;
        pump = g_pump;
    }
    diagnostics::phase_maybe_heartbeat();
    // Invoke outside the lock. The callback may enqueue Win32 input, but must
    // not retrieve or dispatch guest messages.
    ++g_host_gui_pump_depth;
    struct DepthGuard {
        ~DepthGuard() { --g_host_gui_pump_depth; }
    } depth_guard;
    pump();
}

} // namespace vitreo::winapi
