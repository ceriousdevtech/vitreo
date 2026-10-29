/// @file time.cpp
/// @brief Implementation of kernel32 time and date APIs (WIN32-080)

#include "time.hpp"
#include "graphics/common/perf_diag.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/common/crt_dependencies_contract.hpp"

#include <vitreo/time_diag.hpp>

#include <chrono>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <mach/mach_time.h>
#include <unistd.h>

namespace vitreo::winapi {

// =============================================================================
// Helpers
// =============================================================================

/// Convert mach_continuous_time to milliseconds
static u64 mach_time_to_ms() {
    static mach_timebase_info_data_t s_info = []() {
        mach_timebase_info_data_t info;
        mach_timebase_info(&info);
        return info;
    }();

    u64 ticks = mach_continuous_time();
    // ticks * numer / denom -> nanoseconds, then / 1e6 -> milliseconds
    return (ticks * s_info.numer / s_info.denom) / 1000000ULL;
}

u64 tick_count_ms() noexcept {
    const u64 ms = mach_time_to_ms();
    // Covers GetTickCount, GetTickCount64 and the interpreter fast path, all
    // of which read this one function.
    time_diag_observe(TimeApi::GetTickCount, ms);
    return ms;
}

/// Fill a SystemTime from a struct tm + milliseconds
static SystemTime tm_to_system_time(const struct tm& t, u16 ms) {
    SystemTime st{};
    st.year         = static_cast<u16>(t.tm_year + 1900);
    st.month        = static_cast<u16>(t.tm_mon + 1);
    st.day_of_week  = static_cast<u16>(t.tm_wday);
    st.day          = static_cast<u16>(t.tm_mday);
    st.hour         = static_cast<u16>(t.tm_hour);
    st.minute       = static_cast<u16>(t.tm_min);
    st.second       = static_cast<u16>(t.tm_sec);
    st.milliseconds = ms;
    return st;
}

// FILETIME is 100ns since 1601-01-01. libc timegm/gmtime_r are Unix-epoch
// based and reject year 1601; C++ toward-zero division also mis-converts
// any pre-1970 FILETIME that is not an exact second. Civil day math covers
// the full Windows range without either problem.
constexpr i64 kTicksPerDay = 864000000000LL;
constexpr i64 kDays1601To1970 = 134774;

// Howard Hinnant civil calendar (public domain): days since 1970-01-01.
static i64 days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned adj = m > 2 ? m - 3 : m + 9;
    const unsigned doy = (153 * adj + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<i64>(era) * 146097 + static_cast<i64>(doe) - 719468;
}

static void civil_from_days(i64 z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const i64 era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
}

static unsigned days_in_month(int year, unsigned month) {
    static constexpr unsigned kDays[] = {
        0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2) {
        const bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        return leap ? 29u : 28u;
    }
    return kDays[month];
}

// =============================================================================
// TimeManager Implementation
// =============================================================================

u32 TimeManager::get_tick_count() const {
    return static_cast<u32>(mach_time_to_ms() & 0xFFFFFFFFULL);
}

u64 TimeManager::get_tick_count64() const {
    return mach_time_to_ms();
}

SystemTime TimeManager::get_system_time() const {
    auto now = std::chrono::system_clock::now();
    auto since_epoch = now.time_since_epoch();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
    auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch)
                - std::chrono::duration_cast<std::chrono::milliseconds>(secs);

    time_t tt = static_cast<time_t>(secs.count());
    struct tm utc{};
    gmtime_r(&tt, &utc);

    return tm_to_system_time(utc, static_cast<u16>(ms.count()));
}

SystemTime TimeManager::get_local_time() const {
    auto now = std::chrono::system_clock::now();
    auto since_epoch = now.time_since_epoch();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
    auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch)
                - std::chrono::duration_cast<std::chrono::milliseconds>(secs);

    time_t tt = static_cast<time_t>(secs.count());
    struct tm local{};
    localtime_r(&tt, &local);

    return tm_to_system_time(local, static_cast<u16>(ms.count()));
}

bool TimeManager::file_time_to_system_time(u64 file_time, SystemTime& out) const {
    // Learn: input must be less than 0x8000000000000000. Values through that
    // bound can land in year 30828 (Windows maxft → y=30828 success).
    if (file_time >= 0x8000000000000000ULL) {
        return false;
    }
    const u64 day_ticks = static_cast<u64>(kTicksPerDay);
    const u64 total_days = file_time / day_ticks;
    const u64 rem_ticks = file_time % day_ticks;
    const i64 unix_days =
        static_cast<i64>(total_days) - kDays1601To1970;

    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civil_from_days(unix_days, year, month, day);
    if (year < 1601 || year > 30828) {
        return false;
    }

    const u64 sec_of_day = rem_ticks / TICKS_PER_SECOND;
    const u64 rem_of_sec = rem_ticks % TICKS_PER_SECOND;
    i64 dow = (unix_days + 4) % 7;  // 1970-01-01 was Thursday
    if (dow < 0) dow += 7;

    out.year = static_cast<u16>(year);
    out.month = static_cast<u16>(month);
    out.day_of_week = static_cast<u16>(dow);
    out.day = static_cast<u16>(day);
    out.hour = static_cast<u16>(sec_of_day / 3600u);
    out.minute = static_cast<u16>((sec_of_day % 3600u) / 60u);
    out.second = static_cast<u16>(sec_of_day % 60u);
    out.milliseconds = static_cast<u16>(rem_of_sec / TICKS_PER_MS);
    return true;
}

bool TimeManager::system_time_to_file_time(const SystemTime& st, u64& out) const {
    // SYSTEMTIME wYear is valid for 1601–30827 (Windows: 30828 → FALSE).
    if (st.year < 1601 || st.year > 30827 || st.month < 1 || st.month > 12 ||
        st.day < 1 || st.day > days_in_month(st.year, st.month) ||
        st.hour > 23 || st.minute > 59 || st.second > 59 ||
        st.milliseconds > 999) {
        return false;
    }

    const i64 unix_days = days_from_civil(st.year, st.month, st.day);
    const i64 days_since_1601 = unix_days + kDays1601To1970;
    if (days_since_1601 < 0) {
        return false;
    }

    const u64 hms =
        static_cast<u64>(st.hour) * 3600ull +
        static_cast<u64>(st.minute) * 60ull +
        static_cast<u64>(st.second);
    out = static_cast<u64>(days_since_1601) * static_cast<u64>(kTicksPerDay) +
          hms * TICKS_PER_SECOND +
          static_cast<u64>(st.milliseconds) * TICKS_PER_MS;
    // Result must stay below the signed FILETIME bound.
    if (out >= 0x8000000000000000ULL) {
        return false;
    }
    return true;
}

bool TimeManager::file_time_to_local_file_time(u64 utc_ft, u64& local_ft) const {
    // Get local timezone offset via localtime_r
    time_t now = time(nullptr);
    struct tm local{};
    localtime_r(&now, &local);
    long tz_offset = local.tm_gmtoff;  // seconds east of UTC

    i64 offset_ticks = static_cast<i64>(tz_offset) * static_cast<i64>(TICKS_PER_SECOND);
    local_ft = static_cast<u64>(static_cast<i64>(utc_ft) + offset_ticks);
    return true;
}

bool TimeManager::local_file_time_to_file_time(u64 local_ft, u64& utc_ft) const {
    time_t now = time(nullptr);
    struct tm local{};
    localtime_r(&now, &local);
    long tz_offset = local.tm_gmtoff;

    i64 offset_ticks = static_cast<i64>(tz_offset) * static_cast<i64>(TICKS_PER_SECOND);
    utc_ft = static_cast<u64>(static_cast<i64>(local_ft) - offset_ticks);
    return true;
}

bool TimeManager::dos_date_time_to_file_time(u16 fat_date, u16 fat_time,
                                             u64& out_ft) const {
    // FAT date: yyyyyyymmmmddddd (year since 1980), time: hhhhhmmmmmmsssss (sec/2)
    const u16 day   = static_cast<u16>(fat_date & 0x1Fu);
    const u16 month = static_cast<u16>((fat_date >> 5) & 0x0Fu);
    const u16 year  = static_cast<u16>(1980u + ((fat_date >> 9) & 0x7Fu));
    const u16 sec   = static_cast<u16>((fat_time & 0x1Fu) * 2u);
    const u16 min   = static_cast<u16>((fat_time >> 5) & 0x3Fu);
    const u16 hour  = static_cast<u16>((fat_time >> 11) & 0x1Fu);

    if (day < 1 || day > 31 || month < 1 || month > 12 || hour > 23 ||
        min > 59 || sec > 59) {
        return false;
    }

    SystemTime st{};
    st.year = year;
    st.month = month;
    st.day = day;
    st.hour = hour;
    st.minute = min;
    st.second = sec;
    st.milliseconds = 0;
    return system_time_to_file_time(st, out_ft);
}

bool TimeManager::file_time_to_dos_date_time(u64 file_time, u16& out_date,
                                             u16& out_time) const {
    SystemTime st{};
    if (!file_time_to_system_time(file_time, st)) return false;
    if (st.year < 1980 || st.year > 2107) return false;

    out_date = static_cast<u16>(
        (static_cast<u16>(st.year - 1980) << 9) |
        (static_cast<u16>(st.month & 0x0F) << 5) |
        (st.day & 0x1F));
    out_time = static_cast<u16>(
        (static_cast<u16>(st.hour & 0x1F) << 11) |
        (static_cast<u16>(st.minute & 0x3F) << 5) |
        (static_cast<u16>(st.second / 2) & 0x1F));
    return true;
}

void TimeManager::sleep_ms(u32 milliseconds) const {
    if (milliseconds == 0) {
        return;
    }
    usleep(static_cast<useconds_t>(milliseconds) * 1000u);
}

u32 TimeManager::sleep_ex(u32 milliseconds, [[maybe_unused]] bool alertable) const {
    // Non-alertable helper for unit tests. The exported SleepEx lives in
    // sync.cpp and drains QueueUserAPC when alertable.
    sleep_ms(milliseconds);
    return 0;
}

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

void impl_GetTickCount(Marshaler& m, TimeManager& mgr) {
    perf::count(perf::Counter::k32_get_tick_count);
    u32 ticks = mgr.get_tick_count();
    static u32 tick_trace = 0;
    ++tick_trace;
    // Guests poll this harder than its cost suggests; TRACE every call turns
    // a 10 ms slideshow dwell into a multi-second log flood.
    if (tick_trace == 1 || (tick_trace & 0x3FFu) == 0) {
        VITREO_TRACE("[time] GetTickCount -> %u (#%u)\n", ticks, tick_trace);
    }
    m.set_return(static_cast<u64>(ticks));
}

void impl_GetTickCount64(Marshaler& m, TimeManager& mgr) {
    u64 ticks = mgr.get_tick_count64();
    VITREO_TRACE("[time] GetTickCount64 -> %llu\n", (unsigned long long)ticks);
    // ULONGLONG return: x86 delivers it in EDX:EAX, so the high dword must be
    // set too. Curl_now() divides this by 1000 as a full 64-bit value; a stale
    // EDX gave an 11-year tv_sec and made every connect appear to time out.
    m.set_return_i64(ticks);
}

void impl_GetLocalTime(Marshaler& m, TimeManager& mgr) {
    GuestAddr out_ptr = m.arg_ptr(0);
    SystemTime st = mgr.get_local_time();
    (void)m.write_struct<SystemTime>(out_ptr, st);
}

void impl_GetSystemTime(Marshaler& m, TimeManager& mgr) {
    GuestAddr out_ptr = m.arg_ptr(0);
    SystemTime st = mgr.get_system_time();
    (void)m.write_struct<SystemTime>(out_ptr, st);
}

void impl_FileTimeToSystemTime(Marshaler& m, TimeManager& mgr) {
    GuestAddr ft_ptr = m.arg_ptr(0);
    GuestAddr st_ptr = m.arg_ptr(1);
    if (ft_ptr == 0 || st_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    auto lo = m.read_u32(ft_ptr);
    auto hi = m.read_u32(ft_ptr + 4);
    if (!lo.is_ok() || !hi.is_ok()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    u64 file_time = (static_cast<u64>(hi.value()) << 32) | lo.value();
    SystemTime st{};
    bool ok = mgr.file_time_to_system_time(file_time, st);
    if (ok) {
        (void)m.write_struct<SystemTime>(st_ptr, st);
        m.set_return(1u);
        return;
    }
    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    m.set_return(0u);
}

void impl_SystemTimeToFileTime(Marshaler& m, TimeManager& mgr) {
    GuestAddr st_ptr = m.arg_ptr(0);
    GuestAddr ft_ptr = m.arg_ptr(1);
    if (st_ptr == 0 || ft_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    auto st_result = m.read_struct<SystemTime>(st_ptr);
    if (!st_result.is_ok()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    SystemTime st = st_result.value();
    u64 ft = 0;
    bool ok = mgr.system_time_to_file_time(st, ft);
    if (ok) {
        (void)m.write_u64(ft_ptr, ft);
        m.set_return(1u);
        return;
    }
    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    m.set_return(0u);
}

void impl_FileTimeToLocalFileTime(Marshaler& m, TimeManager& mgr) {
    GuestAddr utc_ptr   = m.arg_ptr(0);
    GuestAddr local_ptr = m.arg_ptr(1);
    auto lo = m.read_u32(utc_ptr);
    auto hi = m.read_u32(utc_ptr + 4);
    u64 utc_ft = lo.is_ok() && hi.is_ok()
        ? (static_cast<u64>(hi.value()) << 32) | lo.value()
        : 0;
    u64 local_ft = 0;
    bool ok = mgr.file_time_to_local_file_time(utc_ft, local_ft);
    if (ok) {
        (void)m.write_u64(local_ptr, local_ft);
    }
    m.set_return(ok ? 1u : 0u);
}

void impl_LocalFileTimeToFileTime(Marshaler& m, TimeManager& mgr) {
    GuestAddr local_ptr = m.arg_ptr(0);
    GuestAddr utc_ptr   = m.arg_ptr(1);
    auto lo = m.read_u32(local_ptr);
    auto hi = m.read_u32(local_ptr + 4);
    u64 local_ft = lo.is_ok() && hi.is_ok()
        ? (static_cast<u64>(hi.value()) << 32) | lo.value()
        : 0;
    u64 utc_ft = 0;
    bool ok = mgr.local_file_time_to_file_time(local_ft, utc_ft);
    if (ok) {
        (void)m.write_u64(utc_ptr, utc_ft);
    }
    m.set_return(ok ? 1u : 0u);
}

void impl_DosDateTimeToFileTime(Marshaler& m, TimeManager& mgr) {
    // BOOL DosDateTimeToFileTime(WORD wFatDate, WORD wFatTime, LPFILETIME)
    // WORD args occupy full 4-byte stack slots under stdcall.
    const u16 fat_date = static_cast<u16>(m.arg(0));
    const u16 fat_time = static_cast<u16>(m.arg(1));
    const GuestAddr ft_ptr = m.arg_ptr(2);
    if (ft_ptr == 0) {
        m.set_return(0);
        return;
    }
    u64 ft = 0;
    const bool ok = mgr.dos_date_time_to_file_time(fat_date, fat_time, ft);
    if (ok) {
        (void)m.write_u64(ft_ptr, ft);
    }
    VITREO_TRACE("[time] DosDateTimeToFileTime(date=0x%x time=0x%x) -> %s ft=%llu\n",
                 fat_date, fat_time, ok ? "TRUE" : "FALSE",
                 (unsigned long long)ft);
    m.set_return(ok ? 1u : 0u);
}

void impl_FileTimeToDosDateTime(Marshaler& m, TimeManager& mgr) {
    // BOOL FileTimeToDosDateTime(const FILETIME*, LPWORD lpFatDate, LPWORD lpFatTime)
    const GuestAddr ft_ptr = m.arg_ptr(0);
    const GuestAddr date_ptr = m.arg_ptr(1);
    const GuestAddr time_ptr = m.arg_ptr(2);
    if (ft_ptr == 0 || date_ptr == 0 || time_ptr == 0) {
        m.set_return(0);
        return;
    }
    const auto lo = m.read_u32(ft_ptr);
    const auto hi = m.read_u32(ft_ptr + 4);
    if (lo.is_err() || hi.is_err()) {
        m.set_return(0);
        return;
    }
    const u64 ft = (static_cast<u64>(hi.value()) << 32) | lo.value();
    u16 fat_date = 0;
    u16 fat_time = 0;
    const bool ok = mgr.file_time_to_dos_date_time(ft, fat_date, fat_time);
    if (ok) {
        (void)m.write_u16(date_ptr, fat_date);
        (void)m.write_u16(time_ptr, fat_time);
    }
    VITREO_TRACE("[time] FileTimeToDosDateTime(ft=%llu) -> %s date=0x%x time=0x%x\n",
                 (unsigned long long)ft, ok ? "TRUE" : "FALSE", fat_date, fat_time);
    m.set_return(ok ? 1u : 0u);
}

void impl_Sleep(Marshaler& m, TimeManager& mgr) {
    u32 ms = static_cast<u32>(m.arg(0));
    mgr.sleep_ms(ms);
}

void impl_SleepEx(Marshaler& m, TimeManager& mgr) {
    u32 ms = static_cast<u32>(m.arg(0));
    u32 ret = mgr.sleep_ex(ms, false);
    m.set_return(static_cast<u64>(ret));
}

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports create_time_thunks(TimeManager& mgr) {
    DllExports exports;
    exports.entries.reserve(10);

    // GetTickCount (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetTickCount", 1,
        [&mgr](Marshaler& m) { impl_GetTickCount(m, mgr); },
        0
    });
    // GetTickCount64 (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetTickCount64", 2,
        [&mgr](Marshaler& m) { impl_GetTickCount64(m, mgr); },
        0
    });
    // GetLocalTime (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GetLocalTime", 3,
        [&mgr](Marshaler& m) { impl_GetLocalTime(m, mgr); },
        4
    });
    // GetSystemTime (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GetSystemTime", 4,
        [&mgr](Marshaler& m) { impl_GetSystemTime(m, mgr); },
        4
    });
    // FileTimeToSystemTime (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "FileTimeToSystemTime", 5,
        [&mgr](Marshaler& m) { impl_FileTimeToSystemTime(m, mgr); },
        8
    });
    // SystemTimeToFileTime (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "SystemTimeToFileTime", 6,
        [&mgr](Marshaler& m) { impl_SystemTimeToFileTime(m, mgr); },
        8
    });
    // FileTimeToLocalFileTime (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "FileTimeToLocalFileTime", 7,
        [&mgr](Marshaler& m) { impl_FileTimeToLocalFileTime(m, mgr); },
        8
    });
    // LocalFileTimeToFileTime (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "LocalFileTimeToFileTime", 8,
        [&mgr](Marshaler& m) { impl_LocalFileTimeToFileTime(m, mgr); },
        8
    });
    // DosDateTimeToFileTime (3 args: WORD, WORD, FILETIME*, stdcall_bytes=12).
    // A missing-export stub returns 0xFFFFFFFF (BOOL TRUE) without writing
    // the FILETIME and often pops the wrong number of stdcall bytes.
    exports.entries.push_back(ExportEntry{
        "DosDateTimeToFileTime", 0,
        [&mgr](Marshaler& m) { impl_DosDateTimeToFileTime(m, mgr); },
        12
    });
    // FileTimeToDosDateTime (3 args: FILETIME*, WORD*, WORD*, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "FileTimeToDosDateTime", 0,
        [&mgr](Marshaler& m) { impl_FileTimeToDosDateTime(m, mgr); },
        12
    });
    // Sleep (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "Sleep", 9,
        [&mgr](Marshaler& m) { impl_Sleep(m, mgr); },
        4
    });
    // SleepEx is owned by create_sync_thunks (alertable APC delivery).

    // The native CRT imports these BOOL APIs. A missing-export marker is
    // nonzero and therefore looks like success. Until the complete speaker /
    // alertable-wait and privileged capsule-clock contracts are available,
    // reject the whole operation with the documented failure value.
    // Sources: Learn utilapiset!Beep and sysinfoapi!SetLocalTime.
    exports.entries.push_back(ExportEntry{
        crt_dependencies_contract::Beep_name, 0, [](Marshaler& m) {
            static std::atomic_flag reported;
            if (!reported.test_and_set(std::memory_order_relaxed)) {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!Beep: session tone and alertable wait\n");
            }
            set_last_error(crt_dependencies_contract::ERROR_CALL_NOT_IMPLEMENTED_value);
            m.set_return(crt_dependencies_contract::FALSE_value);
        }, crt_dependencies_contract::Beep_stdcall_bytes
    });
    exports.entries.push_back(ExportEntry{
        crt_dependencies_contract::SetLocalTime_name, 0, [](Marshaler& m) {
            static std::atomic_flag reported;
            if (!reported.test_and_set(std::memory_order_relaxed)) {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!SetLocalTime: privileged capsule clock update\n");
            }
            set_last_error(crt_dependencies_contract::ERROR_CALL_NOT_IMPLEMENTED_value);
            m.set_return(crt_dependencies_contract::FALSE_value);
        }, crt_dependencies_contract::SetLocalTime_stdcall_bytes
    });

    return exports;
}

} // namespace vitreo::winapi
