#pragma once

/// @file nt_timeout.hpp
/// @brief The NT wait timeout convention, shared by every native wait.
///
/// Learn, ZwWaitForSingleObject `Timeout`: "An optional pointer to a time-out
/// value that specifies the absolute or relative time at which the wait is to
/// be completed. A negative value specifies an interval relative to the
/// current time. The value should be expressed in units of 100 nanoseconds.
/// Absolute expiration times track any changes in the system time. Relative
/// expiration times are not affected by system time changes." A NULL pointer
/// is no time-out at all, and zero tests the wait conditions without waiting.
///
/// Measured (tools/oracle_probe/nt_alert_wait_probe.expected): an absolute
/// time in the past (even 1, in 1601) expires at once; INT64_MAX absolute,
/// -INT64_MAX relative and INT64_MIN all wait until something else ends the
/// wait; an unreadable pointer is STATUS_ACCESS_VIOLATION.

#include <vitreo/common.hpp>

#include <chrono>
#include <optional>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi::nt_timeout {

/// Current system time as a FILETIME value (100 ns units since 1601-01-01
/// UTC), the clock an absolute NT timeout is measured against.
using SystemTimeSource = i64 (*)();

/// Test seam: replace the system clock absolute timeouts are measured
/// against (nullptr restores the host clock).
void set_system_time_source_for_test(SystemTimeSource source) noexcept;

/// The system time absolute timeouts are measured against.
[[nodiscard]] i64 system_time_100ns() noexcept;

/// The host has no wait that tracks wall-clock changes, so an absolute wait
/// parks at most this long before it re-reads the system time. A clock
/// change is therefore noticed within this bound rather than at once.
constexpr u32 kAbsoluteSliceMs = 100;

/// No-time-out marker for next_slice_ms().
constexpr u32 kInfiniteMs = 0xFFFFFFFFu;

/// One NT timeout, evaluated against the right clock each time it is asked.
class Deadline {
public:
    /// NULL Timeout: no time-out.
    [[nodiscard]] static Deadline none() noexcept;
    /// A LARGE_INTEGER Timeout value, read when the wait began.
    [[nodiscard]] static Deadline from_value(i64 value) noexcept;

    [[nodiscard]] bool infinite() const noexcept { return m_kind == Kind::None; }
    /// Timeout 0: test the wait conditions without waiting.
    [[nodiscard]] bool is_zero() const noexcept { return m_zero; }
    /// The time-out has passed (always true for a zero time-out).
    [[nodiscard]] bool expired() const noexcept { return next_slice_ns() == 0; }

    /// How long the next host park may last, in nanoseconds: -1 for no
    /// time-out, 0 once expired, otherwise the time left, cut to
    /// kAbsoluteSliceMs for an absolute time-out.
    [[nodiscard]] i64 next_slice_ns() const noexcept;
    /// The same in whole milliseconds, rounded up (a wait never ends early):
    /// kInfiniteMs for no time-out, at most kInfiniteMs - 1 otherwise.
    [[nodiscard]] u32 next_slice_ms() const noexcept;

private:
    enum class Kind { None, Relative, Absolute };
    Kind m_kind = Kind::None;
    bool m_zero = false;
    std::chrono::steady_clock::time_point m_relative_end{};
    i64 m_absolute_100ns = 0;
};

/// Read the PLARGE_INTEGER Timeout at `ptr` (0 = no time-out); nullopt when
/// it is unreadable (the caller returns STATUS_ACCESS_VIOLATION).
[[nodiscard]] std::optional<Deadline> read_timeout(const GuestMemory& memory,
                                                   GuestAddr ptr);

}  // namespace vitreo::winapi::nt_timeout
