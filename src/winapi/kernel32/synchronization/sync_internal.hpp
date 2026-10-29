// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file sync_internal.hpp
/// @brief Shared helpers for SyncManager translation units.

#include "sync.hpp"
#include "winapi/core/marshaler.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace vitreo::winapi {

[[nodiscard]] std::string make_sync_transfer_name(char kind);
/// The SyncManager key of a Win32 object name: "Local\\X" is X. A bare
/// "Local\\" stays as written rather than becoming an unnamed object.
[[nodiscard]] std::string sync_object_key(const std::string& name);
[[nodiscard]] std::string hex_encode_sync_name(std::string_view value);
[[nodiscard]] std::optional<std::string> hex_decode_sync_name(std::string_view value);
[[nodiscard]] std::optional<u32> parse_sync_u32(std::string_view value);
[[nodiscard]] std::string format_parent_chain(
    const std::array<GuestAddr, 4>& parents, u8 parent_count);

[[nodiscard]] SyncTraceContext make_sync_trace_context(
    Marshaler& m, std::string_view api, bool capture_parents);

struct WaitStat {
    std::atomic<u64> count{0};
    std::atomic<u64> total_ns{0};
    std::atomic<u64> timeout_count{0};
    std::atomic<u64> zero_ms_count{0}; // calls with ms=0 (poll)
};

struct WaitProfileTable {
    std::mutex mu;
    std::map<u32, WaitStat> per_handle; // keyed by Handle (low 32 bits)
    std::atomic<u64> sleep_count{0};
    std::atomic<u64> sleep_total_ns{0};
    std::atomic<u64> sleep_zero_count{0};
    bool enabled = false;
    bool live = false;
    std::atomic<u64> last_dump_second{0};
    std::chrono::steady_clock::time_point start;

    WaitProfileTable();
    ~WaitProfileTable();

    WaitStat& get(u32 h);
    void dump();
    void maybe_dump();
};

extern WaitProfileTable g_wait_profile;

} // namespace vitreo::winapi
