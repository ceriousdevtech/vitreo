// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
/// @file crt_time.cpp
/// @brief UCRT time API set (api-ms-win-crt-time-l1-1-0) implementation
///
/// The UCRT's time functions are ordinary C library contracts, so the host
/// implementations are used for the calendar arithmetic. What is *not*
/// delegated is the Windows-visible shape of the API: the per-thread static
/// `struct tm` that `gmtime`/`localtime` return a pointer into, the CRT's
/// accepted `__time64_t` range, the `errno`/`_invalid_parameter` behaviour on
/// out-of-range input, and the `EDX:EAX` return of the 64-bit functions on x86.

#include "winapi/msvcrt/system/crt_time.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/text/crt_locale.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/cpu/state/cpu_state.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 EINVAL_VALUE = 22;
constexpr u32 ERANGE_VALUE = 34;

/// Size of the buffer `asctime`/`ctime` write into (26 bytes used).
constexpr usize ASCTIME_BUFFER_CHARS = 32;

void guest_tm_to_host(const GuestTm& src, std::tm& dst) noexcept {
    std::memset(&dst, 0, sizeof(dst));
    dst.tm_sec   = src.tm_sec;
    dst.tm_min   = src.tm_min;
    dst.tm_hour  = src.tm_hour;
    dst.tm_mday  = src.tm_mday;
    dst.tm_mon   = src.tm_mon;
    dst.tm_year  = src.tm_year;
    dst.tm_wday  = src.tm_wday;
    dst.tm_yday  = src.tm_yday;
    dst.tm_isdst = src.tm_isdst;
}

void host_tm_to_guest(const std::tm& src, GuestTm& dst) noexcept {
    dst.tm_sec   = src.tm_sec;
    dst.tm_min   = src.tm_min;
    dst.tm_hour  = src.tm_hour;
    dst.tm_mday  = src.tm_mday;
    dst.tm_mon   = src.tm_mon;
    dst.tm_year  = src.tm_year;
    dst.tm_wday  = src.tm_wday;
    dst.tm_yday  = src.tm_yday;
    dst.tm_isdst = src.tm_isdst;
}

/// UTC offsets in seconds for standard and daylight time in the host zone.
struct ZoneOffsets {
    i32 standard{0};
    i32 daylight{0};
};

ZoneOffsets host_zone_offsets() noexcept {
    // Sample January and July of the current year. DST, where observed, always
    // moves the offset east, so the smaller offset is standard time.
    const std::time_t now = std::time(nullptr);
    std::tm probe{};
    if (localtime_r(&now, &probe) == nullptr) return {};

    std::tm jan{};
    jan.tm_year  = probe.tm_year;
    jan.tm_mon   = 0;
    jan.tm_mday  = 15;
    jan.tm_hour  = 12;
    jan.tm_isdst = -1;
    std::tm jul = jan;
    jul.tm_mon   = 6;

    const std::time_t jan_t = std::mktime(&jan);
    const std::time_t jul_t = std::mktime(&jul);
    if (jan_t == static_cast<std::time_t>(-1) ||
        jul_t == static_cast<std::time_t>(-1)) {
        return {};
    }

    const auto a = static_cast<i32>(jan.tm_gmtoff);
    const auto b = static_cast<i32>(jul.tm_gmtoff);
    return ZoneOffsets{std::min(a, b), std::max(a, b)};
}

/// Format a broken-down time the way `asctime` does:
/// "Www Mmm dd hh:mm:ss yyyy\n\0" — exactly 26 characters, day-of-month
/// space-padded. Returns false if the fields cannot produce that shape.
bool format_asctime(const GuestTm& t, char (&out)[ASCTIME_BUFFER_CHARS]) noexcept {
    static const char* const WDAY[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* const MON[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    if (t.tm_wday < 0 || t.tm_wday > 6) return false;
    if (t.tm_mon  < 0 || t.tm_mon  > 11) return false;
    if (t.tm_mday < 1 || t.tm_mday > 31) return false;
    if (t.tm_hour < 0 || t.tm_hour > 23) return false;
    if (t.tm_min  < 0 || t.tm_min  > 59) return false;
    if (t.tm_sec  < 0 || t.tm_sec  > 60) return false;
    const i64 year = static_cast<i64>(t.tm_year) + 1900;
    if (year < 1000 || year > 9999) return false;

    std::snprintf(out, sizeof(out), "%s %s %2d %02d:%02d:%02d %4lld\n",
                  WDAY[t.tm_wday], MON[t.tm_mon], t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec,
                  static_cast<long long>(year));
    return true;
}

/// MSVC UCRT "C" locale `strftime` (oracle: crt_strftime_probe.expected).
/// Supports the `#` flag (remove leading zeros / long date forms) that BSD
/// `strftime` rejects. Returns false on invalid conversion / range failure.
bool format_strftime_msvc(const GuestTm& t, std::string_view fmt,
                          std::string& out) {
    static const char* const WDAY_ABBR[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* const WDAY[7] = {
        "Sunday", "Monday", "Tuesday", "Wednesday",
        "Thursday", "Friday", "Saturday"};
    static const char* const MON_ABBR[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static const char* const MON[12] = {
        "January", "February", "March", "April", "May", "June", "July",
        "August", "September", "October", "November", "December"};

    if (t.tm_wday < 0 || t.tm_wday > 6) return false;
    if (t.tm_mon < 0 || t.tm_mon > 11) return false;
    if (t.tm_mday < 1 || t.tm_mday > 31) return false;
    if (t.tm_hour < 0 || t.tm_hour > 23) return false;
    if (t.tm_min < 0 || t.tm_min > 59) return false;
    if (t.tm_sec < 0 || t.tm_sec > 60) return false;

    const i32 year = t.tm_year + 1900;
    const i32 yy = ((year % 100) + 100) % 100;

    auto append_num = [&](i32 v, int width, bool hash) {
        char buf[32];
        if (hash) {
            std::snprintf(buf, sizeof(buf), "%d", v);
        } else if (width > 0) {
            std::snprintf(buf, sizeof(buf), "%0*d", width, v);
        } else {
            std::snprintf(buf, sizeof(buf), "%d", v);
        }
        out += buf;
    };

    out.clear();
    for (usize i = 0; i < fmt.size();) {
        if (fmt[i] != '%') {
            out.push_back(fmt[i++]);
            continue;
        }
        ++i;
        if (i >= fmt.size()) return false;
        bool hash = false;
        if (fmt[i] == '#') {
            hash = true;
            ++i;
            if (i >= fmt.size()) return false;
        }
        const char code = fmt[i++];
        switch (code) {
        case '%':
            out.push_back('%');
            break;
        case 'a':
            out += WDAY_ABBR[t.tm_wday];
            break;
        case 'A':
            out += WDAY[t.tm_wday];
            break;
        case 'b':
        case 'h':
            out += MON_ABBR[t.tm_mon];
            break;
        case 'B':
            out += MON[t.tm_mon];
            break;
        case 'c':
            // Learn %#c: long date/time. Plain %c in UCRT "C" = asctime w/o NL.
            if (hash) {
                char buf[80];
                std::snprintf(buf, sizeof(buf),
                              "%s, %s %02d, %04d %02d:%02d:%02d",
                              WDAY[t.tm_wday], MON[t.tm_mon], t.tm_mday, year,
                              t.tm_hour, t.tm_min, t.tm_sec);
                out += buf;
            } else {
                char buf[40];
                std::snprintf(buf, sizeof(buf),
                              "%s %s %2d %02d:%02d:%02d %04d",
                              WDAY_ABBR[t.tm_wday], MON_ABBR[t.tm_mon],
                              t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, year);
                out += buf;
            }
            break;
        case 'C':
            append_num(year / 100, 2, hash);
            break;
        case 'd':
            append_num(t.tm_mday, 2, hash);
            break;
        case 'D':
            if (hash) {
                append_num(t.tm_mon + 1, 0, true);
                out.push_back('/');
                append_num(t.tm_mday, 0, true);
                out.push_back('/');
                append_num(yy, 0, true);
            } else {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%02d/%02d/%02d",
                              t.tm_mon + 1, t.tm_mday, yy);
                out += buf;
            }
            break;
        case 'e':
            if (hash) {
                append_num(t.tm_mday, 0, true);
            } else {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%2d", t.tm_mday);
                out += buf;
            }
            break;
        case 'F':
            if (hash) {
                append_num(year, 0, true);
                out.push_back('-');
                append_num(t.tm_mon + 1, 0, true);
                out.push_back('-');
                append_num(t.tm_mday, 0, true);
            } else {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                              year, t.tm_mon + 1, t.tm_mday);
                out += buf;
            }
            break;
        case 'H':
            append_num(t.tm_hour, 2, hash);
            break;
        case 'I': {
            i32 h = t.tm_hour % 12;
            if (h == 0) h = 12;
            append_num(h, 2, hash);
            break;
        }
        case 'j':
            append_num(t.tm_yday + 1, 3, hash);
            break;
        case 'm':
            append_num(t.tm_mon + 1, 2, hash);
            break;
        case 'M':
            append_num(t.tm_min, 2, hash);
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'p':
            out += (t.tm_hour >= 12) ? "PM" : "AM";
            break;
        case 'r': {
            i32 h = t.tm_hour % 12;
            if (h == 0) h = 12;
            char buf[24];
            if (hash) {
                std::snprintf(buf, sizeof(buf), "%d:%d:%d %s", h, t.tm_min,
                              t.tm_sec, t.tm_hour >= 12 ? "PM" : "AM");
            } else {
                std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d %s", h, t.tm_min,
                              t.tm_sec, t.tm_hour >= 12 ? "PM" : "AM");
            }
            out += buf;
            break;
        }
        case 'R':
            if (hash) {
                append_num(t.tm_hour, 0, true);
                out.push_back(':');
                append_num(t.tm_min, 0, true);
            } else {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
                out += buf;
            }
            break;
        case 'S':
            append_num(t.tm_sec, 2, hash);
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'T':
            if (hash) {
                append_num(t.tm_hour, 0, true);
                out.push_back(':');
                append_num(t.tm_min, 0, true);
                out.push_back(':');
                append_num(t.tm_sec, 0, true);
            } else {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                              t.tm_hour, t.tm_min, t.tm_sec);
                out += buf;
            }
            break;
        case 'u': {
            // ISO weekday: Monday=1 .. Sunday=7
            const i32 u = t.tm_wday == 0 ? 7 : t.tm_wday;
            append_num(u, 0, false);
            break;
        }
        case 'U': {
            // Week number, Sunday-first week 1.
            const i32 week = (t.tm_yday + 7 - t.tm_wday) / 7;
            append_num(week, 2, hash);
            break;
        }
        case 'V': {
            // ISO week — approximate via host for correctness of edge cases.
            std::tm host{};
            guest_tm_to_host(t, host);
            char buf[8]{};
            if (std::strftime(buf, sizeof(buf), "%V", &host) == 0) return false;
            if (hash) {
                // Strip leading zeros.
                const char* p = buf;
                while (*p == '0' && p[1] != '\0') ++p;
                out += p;
            } else {
                out += buf;
            }
            break;
        }
        case 'W': {
            const i32 monday = (t.tm_wday + 6) % 7;
            const i32 week = (t.tm_yday + 7 - monday) / 7;
            append_num(week, 2, hash);
            break;
        }
        case 'g':
        case 'G': {
            std::tm host{};
            guest_tm_to_host(t, host);
            char buf[16]{};
            const char* f = (code == 'g') ? "%g" : "%G";
            if (std::strftime(buf, sizeof(buf), f, &host) == 0) return false;
            if (hash) {
                const char* p = buf;
                while (*p == '0' && p[1] != '\0') ++p;
                out += p;
            } else {
                out += buf;
            }
            break;
        }
        case 'w':
            append_num(t.tm_wday, 0, false);
            break;
        case 'x':
            if (hash) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%s, %s %02d, %04d",
                              WDAY[t.tm_wday], MON[t.tm_mon], t.tm_mday, year);
                out += buf;
            } else {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%02d/%02d/%02d",
                              t.tm_mon + 1, t.tm_mday, yy);
                out += buf;
            }
            break;
        case 'X':
            // %#X ignored per Learn.
            {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                              t.tm_hour, t.tm_min, t.tm_sec);
                out += buf;
            }
            break;
        case 'y':
            append_num(yy, 2, hash);
            break;
        case 'Y':
            append_num(year, 4, hash);
            break;
        case 'Z':
        case 'z':
            // Leave empty when zone unknown (gmtime case); no host zone inject.
            break;
        default:
            // Unknown / unsupported conversion — Learn: invalid → EINVAL.
            return false;
        }
    }
    return true;
}

/// Read a 64-bit value passed *by value*: one register/stack slot on x64, two
/// consecutive 32-bit slots (low then high) on x86.
i64 arg_i64(Marshaler& m, usize index) {
    if (m.cpu_is_64bit()) {
        return static_cast<i64>(m.arg(index));
    }
    const u64 lo = m.arg(index) & 0xFFFFFFFFULL;
    const u64 hi = m.arg(index + 1) & 0xFFFFFFFFULL;
    return static_cast<i64>(lo | (hi << 32));
}

/// Number of argument slots a by-value 64-bit parameter occupies.
usize i64_slots(Marshaler& m) { return m.cpu_is_64bit() ? 1 : 2; }

/// Return a `double` the way the CRT's ABI does: ST(0) on x86, XMM0 on x64.
void return_double(Marshaler& m, double value) {
    if (!m.has_cpu()) return;
    if (m.cpu_is_64bit()) {
        u64 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        m.cpu()->sse.xmm[0].u64_[0] = bits;
        m.cpu()->sse.xmm[0].u64_[1] = 0;
    } else {
        m.cpu()->fpu.push(cpu::Float80(value));
    }
}

/// Read a __time64_t through a guest pointer.
bool read_time64(Marshaler& m, GuestAddr ptr, i64& out) {
    auto r = m.read_u64(ptr);
    if (!r.is_ok()) return false;
    out = static_cast<i64>(r.value());
    return true;
}

} // namespace

// =============================================================================
// CrtTimeManager
// =============================================================================

CrtTimeManager::CrtTimeManager(GuestMemory& memory,
                               CrtMemoryManager& memory_manager,
                               CrtLocaleManager& locale_manager)
    : m_memory(memory)
    , m_memory_manager(memory_manager)
    , m_locale_manager(locale_manager)
    , m_clock_start(std::chrono::steady_clock::now())
{
    apply_host_zone_locked();
    m_zone_applied = true;
}

u32 CrtTimeManager::clock_ms() const noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - m_clock_start)
                             .count();
    // Learn clock: after LONG_MAX ms return (clock_t)-1 instead of wrapping.
    if (elapsed < 0 || elapsed > static_cast<i64>(0x7fffffff)) {
        return 0xFFFFFFFFu;
    }
    return static_cast<u32>(elapsed);
}

bool CrtTimeManager::gmtime(i64 t, GuestTm& out) noexcept {
    if (t < 0 || t > MAX_TIME64) return false;
    const auto tt = static_cast<std::time_t>(t);
    std::tm host{};
    if (gmtime_r(&tt, &host) == nullptr) return false;
    host_tm_to_guest(host, out);
    out.tm_isdst = 0;  // UTC never reports DST
    return true;
}

bool CrtTimeManager::localtime(i64 t, GuestTm& out) noexcept {
    if (t < 0 || t > MAX_TIME64) return false;
    const auto tt = static_cast<std::time_t>(t);
    std::tm host{};
    if (localtime_r(&tt, &host) == nullptr) return false;
    host_tm_to_guest(host, out);
    return true;
}

i64 CrtTimeManager::mktime(GuestTm& value) noexcept {
    std::tm host{};
    guest_tm_to_host(value, host);
    const std::time_t result = std::mktime(&host);
    if (result == static_cast<std::time_t>(-1)) return -1;
    // The CRT rejects times it could not hand back to gmtime/localtime.
    if (static_cast<i64>(result) < 0 || static_cast<i64>(result) > MAX_TIME64) {
        return -1;
    }
    host_tm_to_guest(host, value);  // mktime normalises in place
    return static_cast<i64>(result);
}

GuestAddr CrtTimeManager::per_thread_slot(
    std::unordered_map<u32, GuestAddr>& table, usize size) {
    const u32 tid = ThreadManager::get_current_thread_id_static();
    std::lock_guard<std::mutex> guard(m_lock);
    auto it = table.find(tid);
    if (it != table.end()) return it->second;

    const GuestAddr addr = m_memory_manager.crt_malloc(size);
    if (addr == 0) return 0;
    for (usize off = 0; off < size; off += 4) {
        (void)m_memory.write_u32(addr + static_cast<GuestAddr>(off), 0);
    }
    table.emplace(tid, addr);
    return addr;
}

GuestAddr CrtTimeManager::thread_tm_buffer() {
    return per_thread_slot(m_tm_buffers, sizeof(GuestTm));
}

GuestAddr CrtTimeManager::thread_string_buffer(bool wide) {
    return wide
        ? per_thread_slot(m_wstr_buffers, ASCTIME_BUFFER_CHARS * 2)
        : per_thread_slot(m_str_buffers, ASCTIME_BUFFER_CHARS);
}

bool CrtTimeManager::write_tm(GuestAddr addr, const GuestTm& value) {
    if (addr == 0) return false;
    const i32 fields[9] = {
        value.tm_sec, value.tm_min, value.tm_hour, value.tm_mday, value.tm_mon,
        value.tm_year, value.tm_wday, value.tm_yday, value.tm_isdst};
    for (usize i = 0; i < 9; ++i) {
        if (!m_memory.write_u32(addr + static_cast<GuestAddr>(i * 4),
                                static_cast<u32>(fields[i])).is_ok()) {
            return false;
        }
    }
    return true;
}

bool CrtTimeManager::read_tm(GuestAddr addr, GuestTm& out) const {
    if (addr == 0) return false;
    i32 fields[9]{};
    for (usize i = 0; i < 9; ++i) {
        auto r = m_memory.read_u32(addr + static_cast<GuestAddr>(i * 4));
        if (!r.is_ok()) return false;
        fields[i] = static_cast<i32>(r.value());
    }
    out.tm_sec   = fields[0];
    out.tm_min   = fields[1];
    out.tm_hour  = fields[2];
    out.tm_mday  = fields[3];
    out.tm_mon   = fields[4];
    out.tm_year  = fields[5];
    out.tm_wday  = fields[6];
    out.tm_yday  = fields[7];
    out.tm_isdst = fields[8];
    return true;
}

void CrtTimeManager::set_errno(u32 value) {
    (void)m_memory.write_u32(m_memory_manager.errno_addr(), value);
}

i32 CrtTimeManager::host_timezone_seconds() noexcept {
    return -host_zone_offsets().standard;
}

i32 CrtTimeManager::host_dst_bias_seconds() noexcept {
    const ZoneOffsets z = host_zone_offsets();
    return -(z.daylight - z.standard);
}

i32 CrtTimeManager::host_daylight_flag() noexcept {
    const ZoneOffsets z = host_zone_offsets();
    return z.daylight != z.standard ? 1 : 0;
}

i32 CrtTimeManager::timezone_seconds() const noexcept {
    return m_timezone;
}

i32 CrtTimeManager::dst_bias_seconds() const noexcept {
    return m_dstbias;
}

i32 CrtTimeManager::daylight_flag() const noexcept {
    return m_daylight;
}

namespace {

/// Second Sunday of March / first Sunday of November (US DST since 2007).
/// Returns day-of-month (1..31) for the given year (years since 1900).
i32 nth_weekday_of_month(i32 year_since_1900, i32 month0, i32 weekday,
                         i32 nth) noexcept {
    // Zeller-ish: find weekday of the 1st, then step to the nth occurrence.
    std::tm probe{};
    probe.tm_year = year_since_1900;
    probe.tm_mon = month0;
    probe.tm_mday = 1;
    probe.tm_hour = 12;
    probe.tm_isdst = 0;
    const std::time_t t = timegm(&probe);
    if (t == static_cast<std::time_t>(-1)) return 1;
    std::tm out{};
    if (gmtime_r(&t, &out) == nullptr) return 1;
    i32 first = out.tm_wday;
    i32 day = 1 + ((weekday - first + 7) % 7) + (nth - 1) * 7;
    return day;
}

/// Learn `_tzset`: with a DST zone in `TZ`, UCRT uses United States DST rules.
bool us_dst_in_effect(std::time_t utc, i32 timezone_west_sec) noexcept {
    // Local standard time = UTC − _timezone (seconds).
    const std::time_t local_std =
        utc - static_cast<std::time_t>(timezone_west_sec);
    std::tm lt{};
    if (gmtime_r(&local_std, &lt) == nullptr) return false;

    const i32 year = lt.tm_year;
    const i32 start_day = nth_weekday_of_month(year, 2, 0, 2);  // Mar, Sun #2
    const i32 end_day = nth_weekday_of_month(year, 10, 0, 1);   // Nov, Sun #1

    const i32 yday = lt.tm_yday;
    // Approximate yday for start/end without full calendar: build dates.
    auto yday_of = [&](i32 month0, i32 mday) -> i32 {
        std::tm p{};
        p.tm_year = year;
        p.tm_mon = month0;
        p.tm_mday = mday;
        p.tm_hour = 12;
        p.tm_isdst = 0;
        const std::time_t tt = timegm(&p);
        std::tm g{};
        if (tt == static_cast<std::time_t>(-1) || gmtime_r(&tt, &g) == nullptr) {
            return -1;
        }
        return g.tm_yday;
    };
    const i32 start_yday = yday_of(2, start_day);
    const i32 end_yday = yday_of(10, end_day);
    if (start_yday < 0 || end_yday < 0) return false;

    // Transition at 02:00 local standard time (start) / 02:00 local daylight
    // (end). Compare as (yday, hour) in local standard time; for the end
    // transition, 02:00 daylight == 01:00 standard.
    const i32 hour = lt.tm_hour;
    if (yday < start_yday) return false;
    if (yday == start_yday && hour < 2) return false;
    if (yday > end_yday) return false;
    if (yday == end_yday && hour >= 1) return false;  // 01:00 std == 02:00 DST
    return true;
}

} // namespace

i32 CrtTimeManager::dst_in_effect_now() const noexcept {
    if (m_daylight == 0) return 0;
    const std::time_t now = std::time(nullptr);
    if (m_tz_from_env) {
        return us_dst_in_effect(now, m_timezone) ? 1 : 0;
    }
    std::tm host{};
    if (localtime_r(&now, &host) == nullptr) return 0;
    return host.tm_isdst > 0 ? 1 : 0;
}

void CrtTimeManager::apply_host_zone_locked() {
    m_timezone = host_timezone_seconds();
    m_daylight = host_daylight_flag();
    m_dstbias = host_dst_bias_seconds();
    m_tz_from_env = false;

    const std::time_t now = std::time(nullptr);
    std::tm host{};
    m_tzname0 = "UTC";
    m_tzname1.clear();
    if (localtime_r(&now, &host) != nullptr && host.tm_zone != nullptr) {
        m_tzname0 = host.tm_zone;
    }
    // When the host observes DST, leave a non-empty DST name placeholder so
    // _tzname[1] is distinguishable; US-style default when host has no name.
    if (m_daylight != 0 && m_tzname1.empty()) {
        m_tzname1 = m_tzname0;
    }
}

bool CrtTimeManager::parse_tz_locked(std::string_view tz) {
    // Learn _tzset: TZ=tzn[+|-]hh[:mm[:ss]][dzn]
    auto is_name_char = [](char c) {
        return std::isalpha(static_cast<unsigned char>(c)) != 0;
    };
    usize i = 0;
    while (i < tz.size() &&
           std::isspace(static_cast<unsigned char>(tz[i])) != 0) {
        ++i;
    }
    if (i >= tz.size() || !is_name_char(tz[i])) return false;

    const usize std_begin = i;
    while (i < tz.size() && is_name_char(tz[i])) ++i;
    std::string std_name(tz.substr(std_begin, i - std_begin));
    if (std_name.empty()) return false;

    if (i >= tz.size()) {
        // Name only — treat as UTC with no DST (UCRT default when offset missing
        // is not fully documented; require an offset for a usable TZ).
        return false;
    }

    i32 sign = 1;
    if (tz[i] == '+') {
        ++i;
    } else if (tz[i] == '-') {
        sign = -1;
        ++i;
    }
    if (i >= tz.size() ||
        std::isdigit(static_cast<unsigned char>(tz[i])) == 0) {
        return false;
    }

    auto read_num = [&](i32& out) -> bool {
        if (i >= tz.size() ||
            std::isdigit(static_cast<unsigned char>(tz[i])) == 0) {
            return false;
        }
        i32 v = 0;
        while (i < tz.size() &&
               std::isdigit(static_cast<unsigned char>(tz[i])) != 0) {
            v = v * 10 + (tz[i] - '0');
            ++i;
        }
        out = v;
        return true;
    };

    i32 hh = 0;
    i32 mm = 0;
    i32 ss = 0;
    if (!read_num(hh)) return false;
    if (i < tz.size() && tz[i] == ':') {
        ++i;
        if (!read_num(mm)) return false;
        if (i < tz.size() && tz[i] == ':') {
            ++i;
            if (!read_num(ss)) return false;
        }
    }

    std::string dst_name;
    if (i < tz.size() && is_name_char(tz[i])) {
        const usize dst_begin = i;
        while (i < tz.size() && is_name_char(tz[i])) ++i;
        dst_name.assign(tz.substr(dst_begin, i - dst_begin));
    }

    // Learn: offset is from local to UTC (positive west of UTC).
    const i32 offset_sec = sign * (hh * 3600 + mm * 60 + ss);
    m_timezone = offset_sec;
    m_tzname0 = std::move(std_name);
    m_tzname1 = std::move(dst_name);
    m_daylight = m_tzname1.empty() ? 0 : 1;
    // US DST rules: one hour ahead of standard when DST is configured.
    m_dstbias = m_daylight != 0 ? -3600 : 0;
    m_tz_from_env = true;
    return true;
}

void CrtTimeManager::publish_zone_locked() {
    if (m_daylight_addr != 0) {
        (void)m_memory.write_u32(m_daylight_addr,
                                 static_cast<u32>(m_daylight));
    }
    if (m_timezone_addr != 0) {
        (void)m_memory.write_u32(m_timezone_addr,
                                 static_cast<u32>(m_timezone));
    }
    if (m_dstbias_addr != 0) {
        (void)m_memory.write_u32(m_dstbias_addr,
                                 static_cast<u32>(m_dstbias));
    }

    auto rewrite_tzname = [&](GuestAddr block, usize ptr_size) {
        if (block == 0) return;
        constexpr usize NAME_CHARS = 64;
        const usize header = ptr_size * 2;
        const GuestAddr std_name = block + header;
        const GuestAddr dst_name = std_name + NAME_CHARS;

        auto write_name = [&](GuestAddr addr, const std::string& s) {
            for (usize i = 0; i < NAME_CHARS; ++i) {
                const u8 ch = i < s.size() ? static_cast<u8>(s[i]) : 0;
                (void)m_memory.write_u8(addr + static_cast<GuestAddr>(i), ch);
            }
        };
        write_name(std_name, m_tzname0);
        write_name(dst_name, m_tzname1);

        if (ptr_size == 8) {
            (void)m_memory.write_u64(block, static_cast<u64>(std_name));
            (void)m_memory.write_u64(block + 8, static_cast<u64>(dst_name));
        } else {
            (void)m_memory.write_u32(block, static_cast<u32>(std_name));
            (void)m_memory.write_u32(block + 4, static_cast<u32>(dst_name));
        }
    };
    rewrite_tzname(m_tzname_addr32, 4);
    rewrite_tzname(m_tzname_addr64, 8);
}

void CrtTimeManager::apply_tzset() {
    std::optional<std::string> tz;
    if (m_tz_reader) {
        tz = m_tz_reader();
    }

    std::lock_guard<std::mutex> guard(m_lock);
    if (tz && !tz->empty()) {
        if (!parse_tz_locked(*tz)) {
            // Malformed TZ — fall back to host zone (same as unset).
            apply_host_zone_locked();
        }
    } else {
        apply_host_zone_locked();
    }
    m_zone_applied = true;
    publish_zone_locked();
}

GuestAddr CrtTimeManager::tzname_addr(usize ptr_size) {
    std::lock_guard<std::mutex> guard(m_lock);
    GuestAddr& cached = ptr_size == 8 ? m_tzname_addr64 : m_tzname_addr32;
    if (cached != 0) return cached;

    // Two char* entries followed by two NAME_CHARS name buffers.
    constexpr usize NAME_CHARS = 64;
    const usize header = ptr_size * 2;
    const GuestAddr block =
        m_memory_manager.crt_malloc(header + NAME_CHARS * 2);
    if (block == 0) return 0;

    const GuestAddr std_name = block + header;
    const GuestAddr dst_name = std_name + NAME_CHARS;

    auto write_name = [&](GuestAddr addr, const std::string& s) {
        for (usize i = 0; i < NAME_CHARS; ++i) {
            const u8 ch = i < s.size() ? static_cast<u8>(s[i]) : 0;
            (void)m_memory.write_u8(addr + static_cast<GuestAddr>(i), ch);
        }
    };
    write_name(std_name, m_tzname0);
    write_name(dst_name, m_tzname1);

    if (ptr_size == 8) {
        (void)m_memory.write_u64(block, static_cast<u64>(std_name));
        (void)m_memory.write_u64(block + 8, static_cast<u64>(dst_name));
    } else {
        (void)m_memory.write_u32(block, static_cast<u32>(std_name));
        (void)m_memory.write_u32(block + 4, static_cast<u32>(dst_name));
    }

    cached = block;
    return cached;
}

GuestAddr CrtTimeManager::daylight_addr() {
    std::lock_guard<std::mutex> guard(m_lock);
    if (m_daylight_addr != 0) return m_daylight_addr;
    const GuestAddr block = m_memory_manager.crt_malloc(sizeof(u32));
    if (block == 0) return 0;
    (void)m_memory.write_u32(block, static_cast<u32>(m_daylight));
    m_daylight_addr = block;
    return m_daylight_addr;
}

GuestAddr CrtTimeManager::timezone_addr() {
    std::lock_guard<std::mutex> guard(m_lock);
    if (m_timezone_addr != 0) return m_timezone_addr;
    const GuestAddr block = m_memory_manager.crt_malloc(sizeof(u32));
    if (block == 0) return 0;
    (void)m_memory.write_u32(block, static_cast<u32>(m_timezone));
    m_timezone_addr = block;
    return m_timezone_addr;
}

GuestAddr CrtTimeManager::dstbias_addr() {
    std::lock_guard<std::mutex> guard(m_lock);
    if (m_dstbias_addr != 0) return m_dstbias_addr;
    const GuestAddr block = m_memory_manager.crt_malloc(sizeof(u32));
    if (block == 0) return 0;
    (void)m_memory.write_u32(block, static_cast<u32>(m_dstbias));
    m_dstbias_addr = block;
    return m_dstbias_addr;
}

GuestAddr CrtTimeManager::allocate_lc_time_data(usize ptr_size) {
    // The "C" locale's time names (gettnames_probe{32,64}.expected).
    static const char* const WDAY_ABBR[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* const WDAY[7] = {
        "Sunday", "Monday", "Tuesday", "Wednesday",
        "Thursday", "Friday", "Saturday"};
    static const char* const MON_ABBR[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static const char* const MON[12] = {
        "January", "February", "March", "April", "May", "June", "July",
        "August", "September", "October", "November", "December"};
    static const char* const AMPM[2] = {"AM", "PM"};
    static const char* const SDATEFMT = "MM/dd/yy";
    static const char* const LDATEFMT = "dddd, MMMM dd, yyyy";
    static const char* const TIMEFMT  = "HH:mm:ss";
    static const char* const LOCALE_NAME = "en-US";
    constexpr i32 CALTYPE_GREGORIAN = 1;

    // __crt_lc_time_data (UCRT corecrt_internal.h): 43 narrow pointers,
    // int ww_caltype, long refcount, 44 wide pointers — 356 bytes on x86,
    // 704 on x64. UCRT _W_Gettnames allocates it and every string in ONE
    // block: the header, then the narrow strings back to back, then the wide
    // strings each aligned to 2 bytes, in field order; refcount is 0.
    const char* const* const narrow_arrays[] = {WDAY_ABBR, WDAY, MON_ABBR, MON, AMPM};
    const usize array_counts[] = {7, 7, 12, 12, 2};
    std::vector<const char*> fields;
    for (usize a = 0; a < 5; ++a)
        for (usize i = 0; i < array_counts[a]; ++i) fields.push_back(narrow_arrays[a][i]);
    fields.push_back(SDATEFMT);
    fields.push_back(LDATEFMT);
    fields.push_back(TIMEFMT);
    constexpr usize NARROW_PTRS = 43;
    const usize header = NARROW_PTRS * ptr_size + 8 + (NARROW_PTRS + 1) * ptr_size;

    usize total = header;
    for (const char* f : fields) total += std::strlen(f) + 1;
    for (const char* f : fields) {
        total += total & 1u;
        total += (std::strlen(f) + 1) * 2;
    }
    total += total & 1u;
    total += (std::strlen(LOCALE_NAME) + 1) * 2;

    const GuestAddr block = m_memory_manager.crt_malloc(total);
    if (block == 0) return 0;
    std::vector<u8> bytes(total, 0);
    auto put_ptr = [&](usize at, GuestAddr value) {
        for (usize i = 0; i < ptr_size; ++i)
            bytes[at + i] = static_cast<u8>(static_cast<u64>(value) >> (8 * i));
    };
    usize slot = 0;
    usize cursor = header;
    for (const char* f : fields) {
        put_ptr(slot, block + static_cast<GuestAddr>(cursor));
        slot += ptr_size;
        const usize len = std::strlen(f);
        std::memcpy(bytes.data() + cursor, f, len + 1);
        cursor += len + 1;
    }
    // ww_caltype, refcount.
    for (usize i = 0; i < 4; ++i)
        bytes[slot + i] = static_cast<u8>(static_cast<u32>(CALTYPE_GREGORIAN) >> (8 * i));
    slot += 8;
    auto put_wide = [&](const char* f) {
        cursor += cursor & 1u;
        put_ptr(slot, block + static_cast<GuestAddr>(cursor));
        slot += ptr_size;
        for (const char* c = f; ; ++c) {
            bytes[cursor] = static_cast<u8>(*c);
            bytes[cursor + 1] = 0;
            cursor += 2;
            if (*c == 0) break;
        }
    };
    for (const char* f : fields) put_wide(f);
    put_wide(LOCALE_NAME);

    if (m_memory.write_bytes(block, reinterpret_cast<const Byte*>(bytes.data()), total).is_err()) {
        m_memory_manager.crt_free(block);
        return 0;
    }
    return block;
}

// =============================================================================
// Thunks
// =============================================================================

DllExports create_crt_time_thunks(CrtTimeManager& mgr) {
    DllExports exports;
    exports.entries.reserve(40);

    // ---- current time -------------------------------------------------------

    // __time64_t _time64(__time64_t* dest)
    auto time64 = [&mgr](Marshaler& m) {
        const GuestAddr out_ptr = m.arg_ptr(0);
        const auto now = static_cast<i64>(std::time(nullptr));
        if (out_ptr != 0) {
            (void)m.write_u64(out_ptr, static_cast<u64>(now));
        }
        (void)mgr;
        m.set_return_i64(static_cast<u64>(now));
    };
    exports.entries.push_back({"_time64", 0, time64});

    // __time32_t _time32(__time32_t* dest)
    exports.entries.push_back({"_time32", 0, [](Marshaler& m) {
        const GuestAddr out_ptr = m.arg_ptr(0);
        const auto now = static_cast<u32>(std::time(nullptr));
        if (out_ptr != 0) {
            (void)m.write_u32(out_ptr, now);
        }
        m.set_return(now);
    }});

    // clock_t clock(void) — milliseconds since CRT init (CLOCKS_PER_SEC=1000)
    exports.entries.push_back({"clock", 0, [&mgr](Marshaler& m) {
        m.set_return(mgr.clock_ms());
    }});

    // ---- gmtime / localtime -------------------------------------------------

    // struct tm* _gmtime64(const __time64_t* t) / _localtime64
    auto breakdown_ptr = [&mgr](Marshaler& m, bool local) {
        const GuestAddr t_ptr = m.arg_ptr(0);
        i64 t = 0;
        if (t_ptr == 0 || !read_time64(m, t_ptr, t)) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        GuestTm tmv{};
        const bool ok = local ? CrtTimeManager::localtime(t, tmv)
                              : CrtTimeManager::gmtime(t, tmv);
        if (!ok) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        const GuestAddr buf = mgr.thread_tm_buffer();
        if (buf == 0 || !mgr.write_tm(buf, tmv)) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        m.set_return(buf);
    };
    exports.entries.push_back({"_gmtime64", 0,
        [breakdown_ptr](Marshaler& m) { breakdown_ptr(m, false); }});
    exports.entries.push_back({"_localtime64", 0,
        [breakdown_ptr](Marshaler& m) { breakdown_ptr(m, true); }});

    // struct tm* _gmtime32(const __time32_t*) / _localtime32
    auto breakdown_ptr32 = [&mgr](Marshaler& m, bool local) {
        const GuestAddr t_ptr = m.arg_ptr(0);
        if (t_ptr == 0) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        auto raw = m.read_u32(t_ptr);
        if (!raw.is_ok()) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        const auto t = static_cast<i64>(static_cast<i32>(raw.value()));
        GuestTm tmv{};
        const bool ok = local ? CrtTimeManager::localtime(t, tmv)
                              : CrtTimeManager::gmtime(t, tmv);
        const GuestAddr buf = ok ? mgr.thread_tm_buffer() : GuestAddr{0};
        if (!ok || buf == 0 || !mgr.write_tm(buf, tmv)) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        m.set_return(buf);
    };
    exports.entries.push_back({"_gmtime32", 0,
        [breakdown_ptr32](Marshaler& m) { breakdown_ptr32(m, false); }});
    exports.entries.push_back({"_localtime32", 0,
        [breakdown_ptr32](Marshaler& m) { breakdown_ptr32(m, true); }});

    // errno_t _gmtime64_s(struct tm* dest, const __time64_t* t) / _localtime64_s
    auto breakdown_s = [&mgr](Marshaler& m, bool local) {
        const GuestAddr dest  = m.arg_ptr(0);
        const GuestAddr t_ptr = m.arg_ptr(1);
        if (dest == 0) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        // On failure the CRT fills the output with -1 before returning.
        const GuestTm invalid{-1, -1, -1, -1, -1, -1, -1, -1, -1};
        i64 t = 0;
        if (t_ptr == 0 || !read_time64(m, t_ptr, t)) {
            (void)mgr.write_tm(dest, invalid);
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        GuestTm tmv{};
        const bool ok = local ? CrtTimeManager::localtime(t, tmv)
                              : CrtTimeManager::gmtime(t, tmv);
        if (!ok) {
            (void)mgr.write_tm(dest, invalid);
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        if (!mgr.write_tm(dest, tmv)) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        m.set_return(0);
    };
    exports.entries.push_back({"_gmtime64_s", 0,
        [breakdown_s](Marshaler& m) { breakdown_s(m, false); }});
    exports.entries.push_back({"_localtime64_s", 0,
        [breakdown_s](Marshaler& m) { breakdown_s(m, true); }});

    // ---- mktime -------------------------------------------------------------

    // __time64_t _mktime64(struct tm* t) — also _mkgmtime64 (UTC variant)
    exports.entries.push_back({"_mktime64", 0, [&mgr](Marshaler& m) {
        const GuestAddr ptr = m.arg_ptr(0);
        GuestTm tmv{};
        if (ptr == 0 || !mgr.read_tm(ptr, tmv)) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return_i64(static_cast<u64>(-1LL));
            return;
        }
        const i64 result = CrtTimeManager::mktime(tmv);
        if (result < 0) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return_i64(static_cast<u64>(-1LL));
            return;
        }
        (void)mgr.write_tm(ptr, tmv);
        m.set_return_i64(static_cast<u64>(result));
    }});

    exports.entries.push_back({"_mktime32", 0, [&mgr](Marshaler& m) {
        const GuestAddr ptr = m.arg_ptr(0);
        GuestTm tmv{};
        if (ptr == 0 || !mgr.read_tm(ptr, tmv)) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        const i64 result = CrtTimeManager::mktime(tmv);
        if (result < 0 || result > 0x7FFFFFFFLL) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        (void)mgr.write_tm(ptr, tmv);
        m.set_return(static_cast<u32>(result));
    }});

    // __time64_t _mkgmtime64(struct tm*) — interpret the fields as UTC
    exports.entries.push_back({"_mkgmtime64", 0, [&mgr](Marshaler& m) {
        const GuestAddr ptr = m.arg_ptr(0);
        GuestTm tmv{};
        if (ptr == 0 || !mgr.read_tm(ptr, tmv)) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return_i64(static_cast<u64>(-1LL));
            return;
        }
        std::tm host{};
        guest_tm_to_host(tmv, host);
        const std::time_t result = timegm(&host);
        if (result == static_cast<std::time_t>(-1) ||
            static_cast<i64>(result) < 0 ||
            static_cast<i64>(result) > CrtTimeManager::MAX_TIME64) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return_i64(static_cast<u64>(-1LL));
            return;
        }
        host_tm_to_guest(host, tmv);
        (void)mgr.write_tm(ptr, tmv);
        m.set_return_i64(static_cast<u64>(result));
    }});

    // ---- difftime -----------------------------------------------------------

    // double _difftime64(__time64_t later, __time64_t earlier)
    exports.entries.push_back({"_difftime64", 0, [](Marshaler& m) {
        const i64 later   = arg_i64(m, 0);
        const i64 earlier = arg_i64(m, i64_slots(m));
        return_double(m, static_cast<double>(later - earlier));
    }});
    exports.entries.push_back({"_difftime32", 0, [](Marshaler& m) {
        const auto later   = static_cast<i32>(m.arg(0));
        const auto earlier = static_cast<i32>(m.arg(1));
        return_double(m, static_cast<double>(later) -
                         static_cast<double>(earlier));
    }});

    // ---- asctime / ctime ----------------------------------------------------

    // char* asctime(const struct tm*) / wchar_t* _wasctime(const struct tm*)
    auto asctime_fn = [&mgr](Marshaler& m, bool wide) {
        const GuestAddr ptr = m.arg_ptr(0);
        GuestTm tmv{};
        char text[ASCTIME_BUFFER_CHARS];
        if (ptr == 0 || !mgr.read_tm(ptr, tmv) || !format_asctime(tmv, text)) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        const GuestAddr buf = mgr.thread_string_buffer(wide);
        if (buf == 0) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        const bool ok = wide ? m.write_wide_string(buf, text).is_ok()
                             : m.write_ansi_string(buf, text).is_ok();
        if (!ok) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        m.set_return(buf);
    };
    exports.entries.push_back({"asctime", 0,
        [asctime_fn](Marshaler& m) { asctime_fn(m, false); }});
    exports.entries.push_back({"_wasctime", 0,
        [asctime_fn](Marshaler& m) { asctime_fn(m, true); }});

    // errno_t asctime_s(char* buf, size_t size, const struct tm*)
    exports.entries.push_back({"asctime_s", 0, [&mgr](Marshaler& m) {
        const GuestAddr buf  = m.arg_ptr(0);
        const auto      size = static_cast<usize>(m.arg(1));
        const GuestAddr ptr  = m.arg_ptr(2);
        GuestTm tmv{};
        char text[ASCTIME_BUFFER_CHARS];
        if (buf == 0 || size < 26) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        if (ptr == 0 || !mgr.read_tm(ptr, tmv) || !format_asctime(tmv, text)) {
            (void)m.write_ansi_string(buf, "");
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        m.set_return(m.write_ansi_string(buf, text).is_ok() ? 0 : EINVAL_VALUE);
    }});

    // char* _ctime64(const __time64_t*) — localtime + asctime
    auto ctime_fn = [&mgr](Marshaler& m, bool wide) {
        const GuestAddr t_ptr = m.arg_ptr(0);
        i64 t = 0;
        GuestTm tmv{};
        char text[ASCTIME_BUFFER_CHARS];
        if (t_ptr == 0 || !read_time64(m, t_ptr, t) ||
            !CrtTimeManager::localtime(t, tmv) || !format_asctime(tmv, text)) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        const GuestAddr buf = mgr.thread_string_buffer(wide);
        if (buf == 0) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        const bool ok = wide ? m.write_wide_string(buf, text).is_ok()
                             : m.write_ansi_string(buf, text).is_ok();
        m.set_return(ok ? buf : 0);
    };
    exports.entries.push_back({"_ctime64", 0,
        [ctime_fn](Marshaler& m) { ctime_fn(m, false); }});
    exports.entries.push_back({"_wctime64", 0,
        [ctime_fn](Marshaler& m) { ctime_fn(m, true); }});

    // errno_t _wctime64_s(wchar_t* buf, size_t sizeInWords, const __time64_t* time)
    exports.entries.push_back({"_wctime64_s", 0, [&mgr](Marshaler& m) {
        const GuestAddr buf = m.arg_ptr(0);
        const usize size_in_words = static_cast<usize>(m.arg(1));
        const GuestAddr t_ptr = m.arg_ptr(2);
        constexpr usize needed = 26;  // asctime produces 26 chars including NUL
        if (buf == 0 || size_in_words == 0 || t_ptr == 0) {
            if (buf != 0 && size_in_words != 0) (void)m.write_u16(buf, 0);
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        if (size_in_words < needed) {
            (void)m.write_u16(buf, 0);
            mgr.set_errno(ERANGE_VALUE);
            m.set_return(ERANGE_VALUE);
            return;
        }
        i64 t = 0;
        GuestTm tmv{};
        char text[ASCTIME_BUFFER_CHARS];
        if (!read_time64(m, t_ptr, t) ||
            !CrtTimeManager::localtime(t, tmv) || !format_asctime(tmv, text)) {
            (void)m.write_u16(buf, 0);
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        if (m.write_wide_string(buf, text).is_err()) {
            (void)m.write_u16(buf, 0);
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        m.set_return(0);
    }});

    // ---- strftime -----------------------------------------------------------

    // size_t strftime(char* dest, size_t max, const char* fmt, const struct tm*)
    exports.entries.push_back({"strftime", 0, [&mgr](Marshaler& m) {
        const GuestAddr dest = m.arg_ptr(0);
        const auto      max  = static_cast<usize>(m.arg(1));
        const GuestAddr fmt  = m.arg_ptr(2);
        const GuestAddr tptr = m.arg_ptr(3);

        if (dest == 0 || max == 0 || fmt == 0 || tptr == 0) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        // Guard against max near SIZE_MAX (would overflow host buffers).
        if (max > static_cast<usize>(0x10000000)) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        auto fmt_str = m.read_ansi_string(fmt);
        GuestTm tmv{};
        if (!fmt_str.is_ok() || !mgr.read_tm(tptr, tmv)) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }

        std::string formatted;
        if (!format_strftime_msvc(tmv, fmt_str.value(), formatted)) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        // Need room for characters + terminating NUL within max.
        if (formatted.size() + 1 > max) {
            (void)m.write_u8(dest, 0);
            mgr.set_errno(ERANGE_VALUE);
            m.set_return(0);
            return;
        }
        for (usize i = 0; i < formatted.size(); ++i) {
            if (!m.write_u8(dest + static_cast<GuestAddr>(i),
                            static_cast<u8>(formatted[i])).is_ok()) {
                m.set_return(0);
                return;
            }
        }
        if (!m.write_u8(dest + static_cast<GuestAddr>(formatted.size()), 0)
                 .is_ok()) {
            m.set_return(0);
            return;
        }
        m.set_return(static_cast<u64>(formatted.size()));
    }});

    // size_t wcsftime(wchar_t* dest, size_t max, const wchar_t* fmt, const tm*)
    exports.entries.push_back({"wcsftime", 0, [&mgr](Marshaler& m) {
        const GuestAddr dest = m.arg_ptr(0);
        const auto      max  = static_cast<usize>(m.arg(1));
        const GuestAddr fmt  = m.arg_ptr(2);
        const GuestAddr tptr = m.arg_ptr(3);

        if (dest == 0 || max == 0 || fmt == 0 || tptr == 0) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        if (max > static_cast<usize>(0x10000000)) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        auto fmt_str = m.read_wide_string(fmt);
        GuestTm tmv{};
        if (!fmt_str.is_ok() || !mgr.read_tm(tptr, tmv)) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }

        std::string formatted;
        if (!format_strftime_msvc(tmv, fmt_str.value(), formatted)) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        if (formatted.size() + 1 > max) {
            (void)m.write_u16(dest, 0);
            mgr.set_errno(ERANGE_VALUE);
            m.set_return(0);
            return;
        }
        for (usize i = 0; i < formatted.size(); ++i) {
            const u16 ch = static_cast<u8>(formatted[i]);
            if (!m.write_u16(dest + static_cast<GuestAddr>(i * 2), ch).is_ok()) {
                m.set_return(0);
                return;
            }
        }
        if (!m.write_u16(
                 dest + static_cast<GuestAddr>(formatted.size() * 2), 0)
                 .is_ok()) {
            m.set_return(0);
            return;
        }
        m.set_return(static_cast<u64>(formatted.size()));
    }});

    // ---- timezone state -----------------------------------------------------

    // void _tzset(void) — Learn: assign `_daylight`/`_timezone`/`_tzname` from
    // the guest `TZ` environment variable (or the host OS zone when unset).
    exports.entries.push_back({"_tzset", 0, [&mgr](Marshaler&) {
        mgr.apply_tzset();
    }});

    // errno_t _get_timezone(long* out) — seconds west of UTC (standard time)
    exports.entries.push_back({"_get_timezone", 0, [&mgr](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(0);
        if (out == 0) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        const bool ok = m.write_u32(
            out, static_cast<u32>(mgr.timezone_seconds())).is_ok();
        m.set_return(ok ? 0 : EINVAL_VALUE);
    }});

    // errno_t _get_dstbias(long* out) — seconds added by DST, negated
    exports.entries.push_back({"_get_dstbias", 0, [&mgr](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(0);
        if (out == 0) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        const bool ok = m.write_u32(
            out, static_cast<u32>(mgr.dst_bias_seconds())).is_ok();
        m.set_return(ok ? 0 : EINVAL_VALUE);
    }});

    // errno_t _get_daylight(int* out)
    exports.entries.push_back({"_get_daylight", 0, [&mgr](Marshaler& m) {
        const GuestAddr out = m.arg_ptr(0);
        if (out == 0) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        const bool ok = m.write_u32(
            out, static_cast<u32>(mgr.daylight_flag())).is_ok();
        m.set_return(ok ? 0 : EINVAL_VALUE);
    }});

    // char** __p__tzname(void) — pointer to the tzname[2] array
    exports.entries.push_back({"__p__tzname", 0, [&mgr](Marshaler& m) {
        m.set_return(mgr.tzname_addr(m.cpu_is_64bit() ? 8u : 4u));
    }});

    // The UCRT spells these accessors without the `_p_` infix, and callers use
    // them as data: `_tzname` is `*__tzname()`, so the returned pointer is
    // immediately dereferenced and indexed. A missing-export stub here returns
    // 0xFFFFFFFF, which the caller then indexes as an array base.
    exports.entries.push_back({"__tzname", 0, [&mgr](Marshaler& m) {
        m.set_return(mgr.tzname_addr(m.cpu_is_64bit() ? 8u : 4u));
    }});
    exports.entries.push_back({"__daylight", 0, [&mgr](Marshaler& m) {
        m.set_return(mgr.daylight_addr());
    }});
    exports.entries.push_back({"__timezone", 0, [&mgr](Marshaler& m) {
        m.set_return(mgr.timezone_addr());
    }});
    exports.entries.push_back({"__dstbias", 0, [&mgr](Marshaler& m) {
        m.set_return(mgr.dstbias_addr());
    }});

    // errno_t _get_tzname(size_t* written, char* buffer, size_t size, int index)
    exports.entries.push_back({"_get_tzname", 0, [&mgr](Marshaler& m) {
        const GuestAddr written = m.arg_ptr(0);
        const GuestAddr buffer  = m.arg_ptr(1);
        const auto      size    = static_cast<usize>(m.arg(2));
        const auto      index   = static_cast<i32>(m.arg(3));
        if (written == 0 || buffer == 0 || (index != 0 && index != 1)) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }

        const GuestAddr ptr_size = m.cpu_is_64bit() ? 8u : 4u;
        const GuestAddr table = mgr.tzname_addr(ptr_size);
        std::string name;
        if (table != 0) {
            auto entry = m.read_ptr(
                table + static_cast<GuestAddr>(index) * ptr_size);
            if (entry.is_ok() && entry.value() != 0) {
                auto text = m.read_ansi_string(
                    static_cast<GuestAddr>(entry.value()));
                if (text.is_ok()) name = text.value();
            }
        }

        const usize needed = name.size() + 1;
        (void)m.write_ptr(written, needed);
        if (size < needed) {
            mgr.set_errno(ERANGE_VALUE);
            m.set_return(ERANGE_VALUE);
            return;
        }
        m.set_return(m.write_ansi_string(buffer, name).is_ok()
            ? 0u : EINVAL_VALUE);
    }});

    // size_t _Strftime(char* dest, size_t max, const char* fmt,
    //                  const struct tm* t, void* lc_time_arg)
    //
    // MSVCP's <locale> time facets call this instead of strftime; it is the
    // same contract with an explicit locale-data argument appended.
    exports.entries.push_back({"_Strftime", 0, [&mgr](Marshaler& m) {
        const GuestAddr dest = m.arg_ptr(0);
        const auto      max  = static_cast<usize>(m.arg(1));
        const GuestAddr fmt  = m.arg_ptr(2);
        const GuestAddr tptr = m.arg_ptr(3);
        (void)m.arg_ptr(4);  // lc_time_arg — Vitreo publishes only "C"

        if (dest == 0 || max == 0 || fmt == 0 || tptr == 0) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        auto fmt_str = m.read_ansi_string(fmt);
        GuestTm tmv{};
        if (!fmt_str.is_ok() || !mgr.read_tm(tptr, tmv)) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        std::tm host{};
        guest_tm_to_host(tmv, host);

        std::vector<char> out(max + 1, '\0');
        const usize written =
            std::strftime(out.data(), max + 1, fmt_str.value().c_str(), &host);
        if (written == 0 || written > max) {
            (void)m.write_u8(dest, 0);
            mgr.set_errno(ERANGE_VALUE);
            m.set_return(0);
            return;
        }
        for (usize i = 0; i <= written; ++i) {
            if (!m.write_u8(dest + static_cast<GuestAddr>(i),
                            static_cast<u8>(out[i])).is_ok()) {
                m.set_return(0);
                return;
            }
        }
        m.set_return(static_cast<u64>(written));
    }});

    // size_t _Wcsftime(wchar_t* dest, size_t max, const wchar_t* fmt,
    //                  const struct tm* t, void* lc_time_arg)
    exports.entries.push_back({"_Wcsftime", 0, [&mgr](Marshaler& m) {
        const GuestAddr dest = m.arg_ptr(0);
        const auto      max  = static_cast<usize>(m.arg(1));
        const GuestAddr fmt  = m.arg_ptr(2);
        const GuestAddr tptr = m.arg_ptr(3);
        (void)m.arg_ptr(4);  // lc_time_arg

        if (dest == 0 || max == 0 || fmt == 0 || tptr == 0) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        auto fmt_str = m.read_wide_string(fmt);
        GuestTm tmv{};
        if (!fmt_str.is_ok() || !mgr.read_tm(tptr, tmv)) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0);
            return;
        }
        std::tm host{};
        guest_tm_to_host(tmv, host);

        std::vector<char> out(max * 4 + 1, '\0');
        const usize written =
            std::strftime(out.data(), out.size(), fmt_str.value().c_str(), &host);
        if (written == 0 || written > max) {
            (void)m.write_u16(dest, 0);
            mgr.set_errno(ERANGE_VALUE);
            m.set_return(0);
            return;
        }
        for (usize i = 0; i <= written; ++i) {
            const u16 ch = i < written ? static_cast<u8>(out[i]) : 0;
            if (!m.write_u16(dest + static_cast<GuestAddr>(i * 2), ch).is_ok()) {
                m.set_return(0);
                return;
            }
        }
        m.set_return(static_cast<u64>(written));
    }});

    // ---- _Gettnames ---------------------------------------------------------
    //
    // MSVCP's <locale> time facets call these for the current locale's
    // `__crt_lc_time_data` and free() the result: each call returns a fresh
    // block the caller owns (UCRT _Gettnames forwards to _W_Gettnames;
    // gettnames_probe: two calls give distinct blocks). A shared block was
    // freed by every facet and then by the next one too, releasing whatever
    // the heap had handed that memory to in between.
    auto gettnames = [&mgr](Marshaler& m) {
        m.set_return(mgr.allocate_lc_time_data(m.cpu_is_64bit() ? 8 : 4));
    };
    exports.entries.push_back({"_Gettnames", 0, gettnames});
    exports.entries.push_back({"_W_Gettnames", 0, gettnames});

    // ---- void _ftime(struct _timeb*) ---------------------------------------
    //
    //   struct _timeb { time_t time; unsigned short millitm;
    //                   short timezone; short dstflag; };
    // time_t is 32-bit in msvcrt.dll, so the struct is 12 bytes with tail
    // padding. _ftime64 uses a 64-bit time and a 16-byte struct.
    const auto ftime_fn = [&mgr](Marshaler& m, bool wide_time) {
        const GuestAddr out = m.arg_ptr(0);
        if (out == 0) {
            mgr.set_errno(EINVAL_VALUE);
            return;
        }
        const auto now = std::chrono::system_clock::now();
        const auto since_epoch = now.time_since_epoch();
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
        const auto millis =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                since_epoch - seconds);

        GuestAddr cursor = out;
        if (wide_time) {
            (void)m.write_u64(cursor, static_cast<u64>(seconds.count()));
            cursor += 8;
        } else {
            (void)m.write_u32(cursor, static_cast<u32>(seconds.count()));
            cursor += 4;
        }
        (void)m.write_u16(cursor, static_cast<u16>(millis.count()));
        (void)m.write_u16(cursor + 2, static_cast<u16>(
            static_cast<i16>(mgr.timezone_seconds() / 60)));
        // Learn `_ftime` dstflag: DST currently in effect, not merely observed.
        (void)m.write_u16(cursor + 4, static_cast<u16>(
            static_cast<i16>(mgr.dst_in_effect_now() != 0 ? 1 : 0)));
    };
    exports.entries.push_back({"_ftime", 0,
        [ftime_fn](Marshaler& m) { ftime_fn(m, false); }});
    exports.entries.push_back({"_ftime32", 0,
        [ftime_fn](Marshaler& m) { ftime_fn(m, false); }});
    exports.entries.push_back({"_ftime64", 0,
        [ftime_fn](Marshaler& m) { ftime_fn(m, true); }});

    // ---- legacy msvcrt.dll names -------------------------------------------
    //
    // msvcrt.dll predates the __time64_t split, so its unsuffixed exports use
    // a 32-bit time_t and are the same functions as the _*32 variants. Alias
    // rather than reimplement so the two can never drift apart.
    struct TimeAlias { const char* legacy; const char* implemented; };
    static constexpr TimeAlias kLegacyAliases[] = {
        {"gmtime",   "_gmtime32"},
        {"localtime", "_localtime32"},
        {"mktime",   "_mktime32"},
        {"mkgmtime", "_mkgmtime64"},
        {"difftime", "_difftime32"},
        {"ctime",    "_ctime64"},
        {"_wctime",  "_wctime64"},
    };
    for (const auto& alias : kLegacyAliases) {
        const auto found = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [&](const ExportEntry& e) { return e.name == alias.implemented; });
        if (found == exports.entries.end()) continue;
        exports.entries.push_back({alias.legacy, 0, found->handler,
                                   found->stdcall_bytes});
    }

    return exports;
}

} // namespace vitreo::winapi
