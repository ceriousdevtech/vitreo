#pragma once

/// @file time.hpp
/// @brief kernel32 time and date APIs (WIN32-080)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

class Marshaler;

// =============================================================================
// SYSTEMTIME structure (matches Windows layout)
// =============================================================================

struct SystemTime {
    u16 year;
    u16 month;
    u16 day_of_week;
    u16 day;
    u16 hour;
    u16 minute;
    u16 second;
    u16 milliseconds;
};

/// Milliseconds since boot, the value behind GetTickCount/GetTickCount64.
///
/// Exposed as a free function so the interpreter's fast-thunk path can answer
/// GetTickCount without a full host-call transition. Guests poll this far
/// harder than its cost suggests. On Windows it is a plain read of
/// KUSER_SHARED_DATA and effectively free.
[[nodiscard]] u64 tick_count_ms() noexcept;

/// Low 32 bits of `tick_count_ms()`, the DWORD GetTickCount / MSG.time /
/// TIMERPROC dwTime / timeGetTime share. Guest-visible Win32 timestamps
/// must use this, not `steady_clock::time_since_epoch()`.
[[nodiscard]] inline u32 tick_count32() noexcept {
    return static_cast<u32>(tick_count_ms() & 0xFFFFFFFFULL);
}

// =============================================================================
// TimeManager
// =============================================================================

class TimeManager {
public:
    TimeManager() = default;

    /// GetTickCount (lower 32 bits of ms since boot)
    [[nodiscard]] u32 get_tick_count() const;

    /// GetTickCount64
    [[nodiscard]] u64 get_tick_count64() const;

    /// Get UTC system time
    [[nodiscard]] SystemTime get_system_time() const;

    /// Get local time
    [[nodiscard]] SystemTime get_local_time() const;

    /// FileTimeToSystemTime: convert 100ns-since-1601 to SystemTime
    [[nodiscard]] bool file_time_to_system_time(u64 file_time, SystemTime& out) const;

    /// SystemTimeToFileTime
    [[nodiscard]] bool system_time_to_file_time(const SystemTime& st, u64& out) const;

    /// FileTimeToLocalFileTime (apply TZ offset)
    [[nodiscard]] bool file_time_to_local_file_time(u64 utc_ft, u64& local_ft) const;

    /// LocalFileTimeToFileTime
    [[nodiscard]] bool local_file_time_to_file_time(u64 local_ft, u64& utc_ft) const;

    /// DosDateTimeToFileTime — FAT date/time words → FILETIME (UTC, no TZ).
    [[nodiscard]] bool dos_date_time_to_file_time(u16 fat_date, u16 fat_time,
                                                  u64& out_ft) const;

    /// FileTimeToDosDateTime — FILETIME → FAT date/time words.
    [[nodiscard]] bool file_time_to_dos_date_time(u64 file_time, u16& out_date,
                                                  u16& out_time) const;

    /// Sleep
    void sleep_ms(u32 milliseconds) const;

    /// SleepEx (returns 0, no APCs)
    u32 sleep_ex(u32 milliseconds, bool alertable) const;

private:
    static constexpr u64 EPOCH_OFFSET = 116444736000000000ULL;
    static constexpr u64 TICKS_PER_SECOND = 10000000ULL;
    static constexpr u64 TICKS_PER_MS = 10000ULL;
};

/// Register time thunks
DllExports create_time_thunks(TimeManager& mgr);

} // namespace vitreo::winapi
