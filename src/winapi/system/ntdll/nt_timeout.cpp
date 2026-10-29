/// @file nt_timeout.cpp
/// @brief The NT wait timeout convention (see nt_timeout.hpp).

#include "nt_timeout.hpp"

#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <atomic>

namespace vitreo::winapi::nt_timeout {

namespace {
std::atomic<SystemTimeSource> g_time_source{nullptr};

/// 1601-01-01 to 1970-01-01 in 100 ns units.
constexpr i64 kUnixEpochAs100ns = 116444736000000000LL;

/// A relative interval is capped at 2^62 ns (about 146 years) so the
/// steady-clock deadline cannot overflow. Nothing observable separates that
/// from the 29,000 years -INT64_MAX names: either waits until something
/// else ends the wait (nt_alert_wait_probe wait_rel_max / wait_int64_min).
constexpr u64 kMaxRelativeNs = u64{1} << 62;
}  // namespace

void set_system_time_source_for_test(SystemTimeSource source) noexcept {
    g_time_source.store(source, std::memory_order_release);
}

i64 system_time_100ns() noexcept {
    if (auto source = g_time_source.load(std::memory_order_acquire)) return source();
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    return kUnixEpochAs100ns +
           std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count() / 100;
}

Deadline Deadline::none() noexcept { return Deadline{}; }

Deadline Deadline::from_value(i64 value) noexcept {
    Deadline d;
    if (value > 0) {
        d.m_kind = Kind::Absolute;
        d.m_absolute_100ns = value;
        return d;
    }
    d.m_kind = Kind::Relative;
    d.m_zero = value == 0;
    // |value| without overflowing on INT64_MIN.
    const u64 magnitude_100ns = u64{0} - static_cast<u64>(value);
    const u64 ns = magnitude_100ns > kMaxRelativeNs / 100 ? kMaxRelativeNs
                                                          : magnitude_100ns * 100;
    d.m_relative_end = std::chrono::steady_clock::now() +
                       std::chrono::nanoseconds(static_cast<i64>(ns));
    return d;
}

i64 Deadline::next_slice_ns() const noexcept {
    switch (m_kind) {
        case Kind::None:
            return -1;
        case Kind::Relative: {
            if (m_zero) return 0;
            const i64 left = std::chrono::duration_cast<std::chrono::nanoseconds>(
                m_relative_end - std::chrono::steady_clock::now()).count();
            return left > 0 ? left : 0;
        }
        case Kind::Absolute: {
            const i64 now = system_time_100ns();
            if (m_absolute_100ns <= now) return 0;
            const u64 left_100ns = static_cast<u64>(m_absolute_100ns) - static_cast<u64>(now);
            const u64 slice_100ns = u64{kAbsoluteSliceMs} * 10000u;
            return static_cast<i64>(std::min(left_100ns, slice_100ns) * 100u);
        }
    }
    return 0;
}

u32 Deadline::next_slice_ms() const noexcept {
    const i64 ns = next_slice_ns();
    if (ns < 0) return kInfiniteMs;
    const u64 ms = (static_cast<u64>(ns) + 999'999u) / 1'000'000u;
    return static_cast<u32>(std::min<u64>(ms, kInfiniteMs - 1u));
}

std::optional<Deadline> read_timeout(const GuestMemory& memory, GuestAddr ptr) {
    if (ptr == 0) return Deadline::none();
    const auto value = memory.read_u64(ptr);
    if (!value.is_ok()) return std::nullopt;
    return Deadline::from_value(static_cast<i64>(value.value()));
}

}  // namespace vitreo::winapi::nt_timeout
