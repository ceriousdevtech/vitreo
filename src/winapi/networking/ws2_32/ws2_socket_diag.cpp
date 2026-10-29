// SPDX-License-Identifier: LGPL-2.1-only

/// @file ws2_socket_diag.cpp
/// @brief Per-socket byte counters and recv diagnostics for Winsock.

#include "ws2_32_internal.hpp"

#include <vitreo/diagnostics.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace vitreo::winapi {

// =============================================================================
// Per-socket byte counters (always-on diagnostics)
// Uses atomic counters to avoid mutex overhead on the recv/send hot path.
// =============================================================================
std::mutex s_socket_stats_mutex;  // Only held for socket map iteration (dump/diagnose)
struct SocketStats {
    std::atomic<uint64_t> recv_bytes{0};
    std::atomic<uint64_t> send_bytes{0};
};
std::unordered_map<u32, std::unique_ptr<SocketStats>> s_socket_stats;  // socket → stats


[[nodiscard]] bool ws2_trace_enabled() {
    static const bool enabled = (std::getenv("VITREO_TRACE") != nullptr);
    return enabled;
}

void track_recv(u32 guest_socket, ssize_t bytes) {
    if (bytes <= 0 || !ws2_trace_enabled()) return;
    std::lock_guard<std::mutex> lk(s_socket_stats_mutex);
    auto it = s_socket_stats.find(guest_socket);
    if (it != s_socket_stats.end()) {
        it->second->recv_bytes.fetch_add(static_cast<uint64_t>(bytes), std::memory_order_relaxed);
    } else {
        auto stats = std::make_unique<SocketStats>();
        stats->recv_bytes.store(static_cast<uint64_t>(bytes), std::memory_order_relaxed);
        s_socket_stats[guest_socket] = std::move(stats);
    }
}

/// Log significant recv events (EOF, error) on high-traffic sockets.
void diagnose_recv(u32 guest_socket, ssize_t rc, int err) {
    if (!ws2_trace_enabled()) return;  // Skip lock entirely when not tracing
    std::lock_guard<std::mutex> lk(s_socket_stats_mutex);
    auto it = s_socket_stats.find(guest_socket);
    uint64_t total_recv = (it != s_socket_stats.end()) ? it->second->recv_bytes.load(std::memory_order_relaxed) : 0;
    // Only log for sockets that have already received significant data.
    if (total_recv < 100000) return;
    if (rc == 0) {
        VITREO_TRACE("[ws2_32] DOWNLOAD DIAG: socket 0x%x EOF after %llu bytes recv'd\n",
                     guest_socket, total_recv);
    } else if (rc < 0 && err != EAGAIN && err != EWOULDBLOCK) {
        VITREO_TRACE("[ws2_32] DOWNLOAD DIAG: socket 0x%x recv error=%d after %llu bytes recv'd\n",
                     guest_socket, err, total_recv);
    }
}

void track_send(u32 guest_socket, ssize_t bytes) {
    if (bytes <= 0 || !ws2_trace_enabled()) return;
    std::lock_guard<std::mutex> lk(s_socket_stats_mutex);
    auto it = s_socket_stats.find(guest_socket);
    if (it != s_socket_stats.end()) {
        it->second->send_bytes.fetch_add(static_cast<uint64_t>(bytes), std::memory_order_relaxed);
    } else {
        auto stats = std::make_unique<SocketStats>();
        stats->send_bytes.store(static_cast<uint64_t>(bytes), std::memory_order_relaxed);
        s_socket_stats[guest_socket] = std::move(stats);
    }
}

void dump_socket_stats(u32 guest_socket) {
    std::lock_guard<std::mutex> lk(s_socket_stats_mutex);
    auto it = s_socket_stats.find(guest_socket);
    if (it != s_socket_stats.end()) {
        uint64_t rb = it->second->recv_bytes.load(std::memory_order_relaxed);
        uint64_t sb = it->second->send_bytes.load(std::memory_order_relaxed);
        if (rb > 0 || sb > 0) {
            VITREO_TRACE("[ws2_32] socket 0x%x closed - recv=%llu bytes, sent=%llu bytes\n",
                         guest_socket, rb, sb);
        }
        s_socket_stats.erase(it);
    }
}

} // namespace vitreo::winapi
