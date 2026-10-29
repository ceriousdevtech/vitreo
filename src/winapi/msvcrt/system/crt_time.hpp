// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file crt_time.hpp
/// @brief UCRT time API set (api-ms-win-crt-time-l1-1-0) implementation

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <chrono>
#include <ctime>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace vitreo {

class GuestMemory;

namespace winapi {

class CrtMemoryManager;
class CrtLocaleManager;

/// Guest-visible `struct tm`: nine 32-bit ints on both x86 and x64 Windows.
struct GuestTm {
    i32 tm_sec;
    i32 tm_min;
    i32 tm_hour;
    i32 tm_mday;
    i32 tm_mon;
    i32 tm_year;
    i32 tm_wday;
    i32 tm_yday;
    i32 tm_isdst;
};
static_assert(sizeof(GuestTm) == 36, "guest struct tm must be 9 x 32-bit ints");

/// Owns the guest-side state behind the CRT time functions: the per-thread
/// `struct tm` and string buffers that `gmtime`/`localtime`/`ctime`/`asctime`
/// return pointers into, plus the cached timezone state set up by `_tzset`.
class CrtTimeManager {
public:
    CrtTimeManager(GuestMemory& memory,
                   CrtMemoryManager& memory_manager,
                   CrtLocaleManager& locale_manager);

    CrtTimeManager(const CrtTimeManager&) = delete;
    CrtTimeManager& operator=(const CrtTimeManager&) = delete;

    /// UCRT's inclusive upper bound for a __time64_t accepted by gmtime and
    /// friends: 23:59:59 on 3000-12-31 UTC.
    static constexpr i64 MAX_TIME64 = 32535215999LL;

    /// Break @p t down as UTC. Returns false (leaving @p out untouched) when
    /// @p t is outside the range UCRT accepts.
    [[nodiscard]] static bool gmtime(i64 t, GuestTm& out) noexcept;

    /// Break @p t down as local time, honouring the host timezone.
    [[nodiscard]] static bool localtime(i64 t, GuestTm& out) noexcept;

    /// Convert a broken-down local time back to a __time64_t, normalising
    /// @p value in place exactly as `_mktime64` does. Returns -1 on failure.
    [[nodiscard]] static i64 mktime(GuestTm& value) noexcept;

    /// Per-thread `struct tm` returned by gmtime/localtime. Zero if the guest
    /// CRT arena is exhausted.
    [[nodiscard]] GuestAddr thread_tm_buffer();

    /// Per-thread 26-byte buffer returned by asctime/ctime (32 bytes reserved).
    [[nodiscard]] GuestAddr thread_string_buffer(bool wide);

    /// Write a GuestTm into guest memory at @p addr.
    [[nodiscard]] bool write_tm(GuestAddr addr, const GuestTm& value);

    /// Read a GuestTm from guest memory at @p addr.
    [[nodiscard]] bool read_tm(GuestAddr addr, GuestTm& out) const;

    /// Set the CRT errno slot (shared with the other CRT managers).
    void set_errno(u32 value);

    /// Seconds west of UTC for the applied zone (after `_tzset`).
    [[nodiscard]] i32 timezone_seconds() const noexcept;

    /// Seconds of DST bias (the CRT's `_dstbias`, normally -3600 when DST
    /// is configured).
    [[nodiscard]] i32 dst_bias_seconds() const noexcept;

    /// Non-zero when a daylight-saving-time zone is configured.
    [[nodiscard]] i32 daylight_flag() const noexcept;

    /// Non-zero when DST is *currently* in effect (Learn `_ftime` `dstflag`).
    [[nodiscard]] i32 dst_in_effect_now() const noexcept;

    /// Learn `_tzset`: read guest `TZ` (via @p tz_reader) or fall back to the
    /// host OS zone, then refresh `_daylight` / `_timezone` / `_dstbias` /
    /// `_tzname` guest cells.
    using TzReader = std::function<std::optional<std::string>()>;
    void set_tz_reader(TzReader reader) { m_tz_reader = std::move(reader); }
    void apply_tzset();

    /// Host-zone fallback used before the first `_tzset` and when TZ is unset.
    [[nodiscard]] static i32 host_timezone_seconds() noexcept;
    [[nodiscard]] static i32 host_dst_bias_seconds() noexcept;
    [[nodiscard]] static i32 host_daylight_flag() noexcept;

    /// Guest pointer to the tzname[] array (two char* entries).
    /// Pointer to the guest-side `char *tzname[2]` array. The entries are
    /// pointer-sized, so the block differs between bitnesses and is cached per
    /// pointer size.
    [[nodiscard]] GuestAddr tzname_addr(usize ptr_size);

    /// Pointers to the guest-side `_daylight`, `_timezone` and `_dstbias`
    /// variables. The UCRT exposes each through an accessor that returns the
    /// variable's address, so they must live in guest memory.
    [[nodiscard]] GuestAddr daylight_addr();
    [[nodiscard]] GuestAddr timezone_addr();
    [[nodiscard]] GuestAddr dstbias_addr();

    /// What `_Gettnames` / `_W_Gettnames` return: a NEW heap block per call,
    /// owned by the caller, who frees it (MSVCP's time facets do). It holds the
    /// "C" locale's `__crt_lc_time_data` followed by every string it points to
    /// (UCRT `_W_Gettnames`, measured in gettnames_probe{32,64}.expected).
    /// Returns 0 when the allocation fails.
    /// @param ptr_size 4 on x86, 8 on x64 — the struct is an array of pointers.
    [[nodiscard]] GuestAddr allocate_lc_time_data(usize ptr_size);

    [[nodiscard]] CrtLocaleManager& locale() noexcept { return m_locale_manager; }

    /// Milliseconds since this manager was constructed (CRT/process start).
    /// Learn clock: elapsed since CRT initialization; returns (clock_t)-1
    /// after LONG_MAX ms.
    [[nodiscard]] u32 clock_ms() const noexcept;

private:
    [[nodiscard]] GuestAddr per_thread_slot(
        std::unordered_map<u32, GuestAddr>& table, usize size);

    GuestMemory&      m_memory;
    CrtMemoryManager& m_memory_manager;
    CrtLocaleManager& m_locale_manager;

    std::mutex m_lock;
    std::unordered_map<u32, GuestAddr> m_tm_buffers;
    std::unordered_map<u32, GuestAddr> m_str_buffers;
    std::unordered_map<u32, GuestAddr> m_wstr_buffers;
    GuestAddr m_tzname_addr32{0};
    GuestAddr m_tzname_addr64{0};
    GuestAddr m_daylight_addr{0};
    GuestAddr m_timezone_addr{0};
    GuestAddr m_dstbias_addr{0};
    std::chrono::steady_clock::time_point m_clock_start{
        std::chrono::steady_clock::now()};

    TzReader m_tz_reader;
    /// Applied `_tzset` state (Learn globals). Initialised from the host zone.
    i32 m_timezone{0};
    i32 m_daylight{0};
    i32 m_dstbias{0};
    std::string m_tzname0{"UTC"};
    std::string m_tzname1;
    bool m_zone_applied{false};
    /// True when the applied zone came from a parsed guest `TZ` string.
    bool m_tz_from_env{false};

    void publish_zone_locked();
    void apply_host_zone_locked();
    [[nodiscard]] bool parse_tz_locked(std::string_view tz);
};

/// Build the DllExports for the CRT time API set.
DllExports create_crt_time_thunks(CrtTimeManager& mgr);

} // namespace winapi
} // namespace vitreo
