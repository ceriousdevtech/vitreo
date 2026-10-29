// SPDX-License-Identifier: LGPL-2.1-only

/// @file timezone.cpp
/// @brief Time-zone and date-format APIs on Kernel32ExtManager.
///
/// GetTimeZoneInformation, GetDateFormatA/W, and
/// SystemTimeToTzSpecificLocalTime. Distinct from system/time.cpp,
/// which owns the TimeManager GetTickCount / FILETIME surface.
///
/// Closed cut from kernel32_ext.cpp: nothing these functions define is used
/// elsewhere in that file, and they use nothing file-local from elsewhere —
/// verified in both directions with tools/architecture/plan_extraction.py
/// before moving.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/synchronization/critical_section.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "ipc/process_session.hpp"
#include "core/pe/loader/teb_setup.hpp"
#include "core/pe/parser/manifest_info.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "platform/macos/elevation_authorization.hpp"
#include "windowing/resources/resource_walker.hpp"

#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/win_path.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <limits>
#include <random>
#include <sys/sysctl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <libproc.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <ctime>
#include <string_view>

#include <CoreFoundation/CoreFoundation.h>

namespace vitreo::winapi {

namespace {

constexpr u32 kTimeZoneIdUnknown = 0;
constexpr u32 kTimeZoneIdStandard = 1;
constexpr u32 kTimeZoneIdDaylight = 2;
constexpr u32 kTimeZoneInformationBytes = 172;

/// Absolute time for noon UTC on the 15th of @p month in a fixed year that
/// straddles a northern-hemisphere DST change (2024).
CFAbsoluteTime noon_utc_on(int month) {
    CFGregorianDate gd{};
    gd.year = 2024;
    gd.month = month;
    gd.day = 15;
    gd.hour = 12;
    CFTimeZoneRef gmt = CFTimeZoneCreateWithTimeIntervalFromGMT(kCFAllocatorDefault, 0);
    CFAbsoluteTime t = CFGregorianDateGetAbsoluteTime(gd, gmt);
    CFRelease(gmt);
    return t;
}

void write_wchar32(u16* dst, const char* utf8) {
    std::memset(dst, 0, 32 * sizeof(u16));
    if (utf8 == nullptr) return;
    CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, utf8,
                                              kCFStringEncodingUTF8);
    if (!s) return;
    CFStringGetBytes(s, CFRangeMake(0, CFStringGetLength(s)),
                     kCFStringEncodingUTF16LE, 0, false,
                     reinterpret_cast<UInt8*>(dst), 31 * sizeof(u16), nullptr);
    CFRelease(s);
}

void write_systemtime_words(u16* dst, CFAbsoluteTime at, CFTimeZoneRef tz) {
    CFGregorianDate gd = CFAbsoluteTimeGetGregorianDate(at, tz);
    // Relative form (wYear=0) is not filled here — absolute transition stamps
    // are enough for GetTimeZoneInformation consumers that only need Bias.
    dst[0] = 0;                                    // wYear
    dst[1] = static_cast<u16>(gd.month);           // wMonth
    dst[2] = 0;                                    // wDayOfWeek
    dst[3] = static_cast<u16>(gd.day);             // wDay
    dst[4] = static_cast<u16>(gd.hour);            // wHour
    dst[5] = static_cast<u16>(gd.minute);          // wMinute
    dst[6] = 0;
    dst[7] = 0;
}

/// Fallback when the host zone has no Windows key: a TIME_ZONE_INFORMATION
/// (172 bytes) derived from the macOS zone. Returns TIME_ZONE_ID_*.
u32 fill_time_zone_information_from_host(u8* out172) {
    std::memset(out172, 0, kTimeZoneInformationBytes);
    CFTimeZoneRef tz = CFTimeZoneCopySystem();
    if (!tz) return kTimeZoneIdUnknown;

    const CFAbsoluteTime jan = noon_utc_on(1);
    const CFAbsoluteTime jul = noon_utc_on(7);
    const CFTimeInterval jan_east = CFTimeZoneGetSecondsFromGMT(tz, jan);
    const CFTimeInterval jul_east = CFTimeZoneGetSecondsFromGMT(tz, jul);
    // The more-east offset is DST; the other is standard. Works for both
    // northern and southern hemisphere zones.
    const bool jul_is_dst = jul_east > jan_east;
    const CFTimeInterval std_east = jul_is_dst ? jan_east : jul_east;
    const CFTimeInterval dlt_east = jul_is_dst ? jul_east : jan_east;
    const CFAbsoluteTime std_sample = jul_is_dst ? jan : jul;
    const CFAbsoluteTime dlt_sample = jul_is_dst ? jul : jan;
    // Windows Bias = minutes west of UTC for standard time.
    const i32 bias = -static_cast<i32>(std_east / 60.0);
    const i32 daylight_bias =
        -static_cast<i32>(dlt_east / 60.0) - bias;  // typically -60
    const i32 standard_bias = 0;

    std::memcpy(out172 + 0, &bias, 4);
    std::memcpy(out172 + 84, &standard_bias, 4);
    std::memcpy(out172 + 168, &daylight_bias, 4);

    // Names: prefer CF abbreviations (MST/MDT); fall back to zone name.
    CFStringRef std_abbr = CFTimeZoneCopyAbbreviation(tz, std_sample);
    CFStringRef dlt_abbr = CFTimeZoneCopyAbbreviation(tz, dlt_sample);
    char std_buf[64] = {};
    char dlt_buf[64] = {};
    if (std_abbr) {
        CFStringGetCString(std_abbr, std_buf, sizeof std_buf, kCFStringEncodingUTF8);
        CFRelease(std_abbr);
    }
    if (dlt_abbr) {
        CFStringGetCString(dlt_abbr, dlt_buf, sizeof dlt_buf, kCFStringEncodingUTF8);
        CFRelease(dlt_abbr);
    }
    if (std_buf[0] == 0) {
        CFStringRef name = CFTimeZoneGetName(tz);
        if (name) {
            CFStringGetCString(name, std_buf, sizeof std_buf, kCFStringEncodingUTF8);
        }
    }
    if (dlt_buf[0] == 0) {
        std::memcpy(dlt_buf, std_buf, sizeof dlt_buf);
    }
    write_wchar32(reinterpret_cast<u16*>(out172 + 4), std_buf);
    write_wchar32(reinterpret_cast<u16*>(out172 + 88), dlt_buf);

    // Transition stamps: next DST transitions around "now", when the zone
    // observes daylight saving (daylight_bias != 0).
    if (daylight_bias != 0) {
        CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
        CFAbsoluteTime next = CFTimeZoneGetNextDaylightSavingTimeTransition(tz, now);
        CFAbsoluteTime prev = CFTimeZoneGetNextDaylightSavingTimeTransition(tz, next - 400 * 24 * 3600.0);
        // Whichever of prev/next is the spring forward (offset increases east /
        // Bias decreases) is DaylightDate; the other is StandardDate.
        const CFTimeInterval before_next =
            CFTimeZoneGetSecondsFromGMT(tz, next - 3600.0);
        const CFTimeInterval after_next =
            CFTimeZoneGetSecondsFromGMT(tz, next + 3600.0);
        CFAbsoluteTime daylight_at = next;
        CFAbsoluteTime standard_at = prev;
        if (after_next > before_next) {
            // next is spring-forward → DaylightDate
            daylight_at = next;
            standard_at = prev;
        } else {
            daylight_at = prev;
            standard_at = next;
        }
        write_systemtime_words(reinterpret_cast<u16*>(out172 + 68), standard_at, tz);
        write_systemtime_words(reinterpret_cast<u16*>(out172 + 152), daylight_at, tz);
    }

    const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    u32 id = kTimeZoneIdUnknown;
    if (daylight_bias != 0) {
        id = CFTimeZoneIsDaylightSavingTime(tz, now) ? kTimeZoneIdDaylight
                                                     : kTimeZoneIdStandard;
    } else if (bias != 0 || std_buf[0] != 0) {
        id = kTimeZoneIdStandard;
    }
    CFRelease(tz);
    return id;
}

// -----------------------------------------------------------------------------
// Windows time zones from open data. Windows names each zone by a key
// ("Mountain Standard Time"); Unicode CLDR's windowsZones mapping (shipped in
// the host's ICU) links each key to an IANA zone, and the host's IANA tz
// database gives that zone's offsets and transitions for any year. No Windows
// database is carried. Expected behaviour is Windows' observed output:
// tools/oracle_probe/tz_year_probe.c -> tz_year_probe{32,64}.expected.
// -----------------------------------------------------------------------------

}  // namespace
}  // namespace vitreo::winapi

// libicucore C API (ICU4C ucal.h / uenum.h).
extern "C" {
struct UCalendar;
struct UEnumeration;
int32_t ucal_getWindowsTimeZoneID(const char16_t* id, int32_t len, char16_t* winid,
                                  int32_t winid_capacity, int* status);
int32_t ucal_getTimeZoneIDForWindowsID(const char16_t* winid, int32_t len, const char* region,
                                       char16_t* id, int32_t id_capacity, int* status);
UEnumeration* ucal_openTimeZones(int* status);
const char16_t* uenum_unext(UEnumeration* en, int32_t* len, int* status);
void uenum_close(UEnumeration* en);
UCalendar* ucal_open(const char16_t* zone_id, int32_t len, const char* locale, int type,
                     int* status);
void ucal_close(UCalendar* cal);
void ucal_setMillis(UCalendar* cal, double millis, int* status);
int32_t ucal_get(const UCalendar* cal, int field, int* status);
signed char ucal_getTimeZoneTransitionDate(const UCalendar* cal, int type, double* transition,
                                           int* status);
}

namespace vitreo::winapi {
namespace {

constexpr int kUcalGregorian = 1;
constexpr int kUcalZoneOffset = 15;
constexpr int kUcalDstOffset = 16;
constexpr int kUcalTransitionNext = 0;

struct WindowsZone {
    std::u16string key;   // canonical spelling of the Windows key
    std::u16string iana;  // CLDR's representative ("001") IANA zone
};

/// One year's rules, in REG_TZI terms: biases in minutes west of UTC and the
/// two transition SYSTEMTIMEs (all zero without daylight time).
struct ZoneRules {
    i32 bias = 0;
    i32 standard_bias = 0;
    i32 daylight_bias = 0;
    u16 standard_date[8] = {};
    u16 daylight_date[8] = {};
};

[[nodiscard]] bool key_equal_ci(std::u16string_view a, std::u16string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        char16_t x = a[i], y = b[i];
        if (x >= u'A' && x <= u'Z') x = static_cast<char16_t>(x - u'A' + u'a');
        if (y >= u'A' && y <= u'Z') y = static_cast<char16_t>(y - u'A' + u'a');
        if (x != y) return false;
    }
    return true;
}

/// Every Windows key CLDR maps an IANA zone to.
[[nodiscard]] const std::vector<std::u16string>& windows_zone_keys() {
    static const std::vector<std::u16string> keys = [] {
        std::vector<std::u16string> out;
        int status = 0;
        UEnumeration* zones = ucal_openTimeZones(&status);
        if (zones == nullptr || status > 0) return out;
        for (;;) {
            int32_t len = 0;
            const char16_t* id = uenum_unext(zones, &len, &status);
            if (id == nullptr || status > 0) break;
            char16_t win[128] = {};
            int map_status = 0;
            const int32_t n = ucal_getWindowsTimeZoneID(id, len, win, 127, &map_status);
            if (map_status > 0 || n <= 0) continue;
            std::u16string key(win, static_cast<usize>(n));
            if (std::find(out.begin(), out.end(), key) == out.end()) out.push_back(std::move(key));
        }
        uenum_close(zones);
        return out;
    }();
    return keys;
}

/// The zone for a Windows key, compared case-insensitively like Windows.
[[nodiscard]] std::optional<WindowsZone> find_time_zone(std::u16string_view key) {
    if (key.empty()) return std::nullopt;
    for (const auto& known : windows_zone_keys()) {
        if (!key_equal_ci(known, key)) continue;
        char16_t iana[128] = {};
        int status = 0;
        const int32_t n = ucal_getTimeZoneIDForWindowsID(
            known.data(), static_cast<int32_t>(known.size()), "001", iana, 127, &status);
        if (status > 0 || n <= 0) return std::nullopt;
        return WindowsZone{known, std::u16string(iana, static_cast<usize>(n))};
    }
    return std::nullopt;
}

/// The host zone's Windows key, or nullopt when CLDR has no mapping.
[[nodiscard]] std::optional<WindowsZone> host_time_zone() {
    CFTimeZoneRef tz = CFTimeZoneCopySystem();
    if (!tz) return std::nullopt;
    CFStringRef name = CFTimeZoneGetName(tz);
    std::u16string iana;
    if (name) {
        iana.resize(static_cast<usize>(CFStringGetLength(name)));
        CFStringGetCharacters(name, CFRangeMake(0, CFStringGetLength(name)),
                              reinterpret_cast<UniChar*>(iana.data()));
    }
    CFRelease(tz);
    char16_t win[128] = {};
    int status = 0;
    const int32_t n = ucal_getWindowsTimeZoneID(iana.data(), static_cast<int32_t>(iana.size()),
                                                win, 127, &status);
    if (status <= 0 && n > 0) {
        if (auto zone = find_time_zone(std::u16string_view(win, static_cast<usize>(n)))) {
            return zone;
        }
    }
    static std::once_flag once;
    std::call_once(once, [] {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!time zones: host zone has no Windows time-zone key; using rules derived from the macOS zone\n");
    });
    return std::nullopt;
}

[[nodiscard]] u32 current_utc_year() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    return static_cast<u32>(utc.tm_year + 1900);
}

/// Day-in-month SYSTEMTIME (wYear 0; wDay = occurrence 1..5 of wDayOfWeek,
/// 5 = last) of the local wall-clock time @p local_seconds.
void occurrence_date(i64 local_seconds, u16 out[8]) {
    const std::time_t t = static_cast<std::time_t>(local_seconds);
    std::tm d{};
    gmtime_r(&t, &d);
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int year = d.tm_year + 1900;
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    const int month_days = kDays[d.tm_mon] + (d.tm_mon == 1 && leap ? 1 : 0);
    out[0] = 0;
    out[1] = static_cast<u16>(d.tm_mon + 1);
    out[2] = static_cast<u16>(d.tm_wday);
    out[3] = static_cast<u16>(d.tm_mday + 7 > month_days ? 5 : (d.tm_mday - 1) / 7 + 1);
    out[4] = static_cast<u16>(d.tm_hour);
    out[5] = static_cast<u16>(d.tm_min);
    out[6] = static_cast<u16>(d.tm_sec);
    out[7] = 0;
}

/// @p zone's rules for @p year from the IANA database. Windows' standard
/// time is the year's smaller total UTC offset and daylight time the larger
/// (so a zone whose IANA "DST" is negative, like Morocco's Ramadan time, is
/// expressed the way Windows expresses it). Each transition is given in the
/// wall clock of the period it ends. Without two such transitions the zone
/// has no daylight rules; Windows still reports DaylightBias -60 then, except
/// for the UTC key (0).
[[nodiscard]] ZoneRules zone_rules(const WindowsZone& zone, u32 year) {
    ZoneRules rules;
    rules.daylight_bias = key_equal_ci(zone.key, u"UTC") ? 0 : -60;
    int status = 0;
    UCalendar* cal = ucal_open(zone.iana.data(), static_cast<int32_t>(zone.iana.size()), "",
                               kUcalGregorian, &status);
    if (cal == nullptr || status > 0) {
        if (cal) ucal_close(cal);
        return rules;
    }
    auto offset_at = [&](double millis) {
        int st = 0;
        ucal_setMillis(cal, millis, &st);
        return ucal_get(cal, kUcalZoneOffset, &st) + ucal_get(cal, kUcalDstOffset, &st);
    };
    std::tm jan1{};
    jan1.tm_year = static_cast<int>(year) - 1900;
    jan1.tm_mday = 1;
    const double year_start = static_cast<double>(timegm(&jan1)) * 1000.0;
    jan1.tm_year += 1;
    const double year_end = static_cast<double>(timegm(&jan1)) * 1000.0;
    struct Change { double at; int before; int after; };
    std::vector<Change> changes;
    double cursor = year_start - 2 * 86400000.0;
    while (changes.size() < 8) {
        int st = 0;
        ucal_setMillis(cal, cursor, &st);
        double next = 0;
        if (!ucal_getTimeZoneTransitionDate(cal, kUcalTransitionNext, &next, &st) || st > 0 ||
            next >= year_end + 2 * 86400000.0) {
            break;
        }
        const int before = offset_at(next - 1);
        const int after = offset_at(next);
        if (before != after) changes.push_back({next, before, after});
        cursor = next;
    }
    rules.bias = -offset_at(year_start) / 60000;
    const Change* to_daylight = nullptr;
    const Change* to_standard = nullptr;
    for (const auto& c : changes) {
        // A transition counts toward the year its local wall clock falls in.
        const double local = c.at + c.before;
        if (local < year_start || local >= year_end) continue;
        if (c.after > c.before && to_daylight == nullptr) to_daylight = &c;
        if (c.after < c.before && to_standard == nullptr) to_standard = &c;
    }
    if (to_daylight != nullptr && to_standard != nullptr &&
        to_daylight->before == to_standard->after && to_daylight->after == to_standard->before) {
        const int standard = to_daylight->before, daylight = to_daylight->after;
        rules.bias = -standard / 60000;
        rules.daylight_bias = -(daylight - standard) / 60000;
        occurrence_date(static_cast<i64>((to_daylight->at + standard) / 1000.0), rules.daylight_date);
        occurrence_date(static_cast<i64>((to_standard->at + daylight) / 1000.0), rules.standard_date);
    }
    ucal_close(cal);
    return rules;
}

/// StandardName / DaylightName (WCHAR[32]) as Windows reports them: the key,
/// and the key with "Standard" read as "Daylight"; the UTC key's names are
/// "Coordinated Universal Time" (tz_year_probe).
void write_zone_names(u8* out172, const WindowsZone& zone) {
    std::u16string standard = key_equal_ci(zone.key, u"UTC") ? std::u16string(u"Coordinated Universal Time")
                                                               : zone.key;
    std::u16string daylight = standard;
    if (const auto at = daylight.find(u"Standard"); at != std::u16string::npos) {
        daylight.replace(at, 8, u"Daylight");
    }
    auto put = [](u8* dst, const std::u16string& name) {
        std::memset(dst, 0, 64);
        for (usize i = 0; i < 31 && i < name.size(); ++i) {
            dst[i * 2] = static_cast<u8>(name[i]);
            dst[i * 2 + 1] = static_cast<u8>(name[i] >> 8);
        }
    };
    put(out172 + 4, standard);
    put(out172 + 88, daylight);
}

/// TIME_ZONE_INFORMATION (172 bytes) for @p zone and @p rules.
void lay_out_tzi(u8* out172, const WindowsZone& zone, const ZoneRules& rules) {
    std::memset(out172, 0, kTimeZoneInformationBytes);
    auto put32 = [&](u32 at, i32 v) { std::memcpy(out172 + at, &v, 4); };
    auto put_date = [&](u32 at, const u16 date[8]) {
        for (u32 i = 0; i < 8; ++i) {
            out172[at + i * 2] = static_cast<u8>(date[i]);
            out172[at + i * 2 + 1] = static_cast<u8>(date[i] >> 8);
        }
    };
    put32(0, rules.bias);
    write_zone_names(out172, zone);
    put_date(68, rules.standard_date);
    put32(84, rules.standard_bias);
    put_date(152, rules.daylight_date);
    put32(168, rules.daylight_bias);
}

[[nodiscard]] u16 word_at(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
[[nodiscard]] i32 long_at(const u8* p) {
    return static_cast<i32>(static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
                            (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24));
}

/// Seconds since the Unix epoch of a transition rule (SYSTEMTIME at @p rule)
/// in @p year, expressed in local time offset @p bias_minutes west of UTC.
/// wYear == 0: day-in-month form (wDay = occurrence 1..5, 5 = last
/// wDayOfWeek of the month); otherwise an absolute date (Learn
/// TIME_ZONE_INFORMATION).
[[nodiscard]] i64 transition_utc(const u8* rule, u32 year, i32 bias_minutes) {
    const u16 month = word_at(rule + 2);
    const u16 dow = word_at(rule + 4);
    const u16 day = word_at(rule + 6);
    std::tm t{};
    t.tm_year = static_cast<int>((word_at(rule) != 0 ? word_at(rule) : year) - 1900);
    t.tm_mon = month - 1;
    t.tm_mday = 1;
    if (word_at(rule) != 0) {
        t.tm_mday = day;
    } else {
        const i64 first = timegm(&t);
        std::tm f{};
        const std::time_t ft = static_cast<std::time_t>(first);
        gmtime_r(&ft, &f);
        int mday = 1 + ((dow - f.tm_wday + 7) % 7) + 7 * (day - 1);
        std::tm probe = t;
        probe.tm_mday = mday;
        const std::time_t pt = timegm(&probe);
        std::tm check{};
        gmtime_r(&pt, &check);
        if (check.tm_mon != t.tm_mon) mday -= 7;  // occurrence 5 = last
        t.tm_mday = mday;
    }
    t.tm_hour = word_at(rule + 8);
    t.tm_min = word_at(rule + 10);
    t.tm_sec = word_at(rule + 12);
    return static_cast<i64>(timegm(&t)) + static_cast<i64>(bias_minutes) * 60;
}

/// TIME_ZONE_ID_* for @p tzi at the current instant: UNKNOWN without
/// daylight rules, otherwise STANDARD or DAYLIGHT.
[[nodiscard]] u32 time_zone_id(const u8* tzi) {
    const u8* std_date = tzi + 68;
    const u8* dlt_date = tzi + 152;
    if (word_at(std_date + 2) == 0 || word_at(dlt_date + 2) == 0) return kTimeZoneIdUnknown;
    const i32 bias = long_at(tzi + 0);
    const u32 year = current_utc_year();
    // DaylightDate is local standard time; StandardDate local daylight time.
    const i64 dst_start = transition_utc(dlt_date, year, bias + long_at(tzi + 84));
    const i64 dst_end = transition_utc(std_date, year, bias + long_at(tzi + 168));
    const i64 now = static_cast<i64>(std::time(nullptr));
    const bool daylight = dst_start < dst_end ? (now >= dst_start && now < dst_end)
                                              : (now >= dst_start || now < dst_end);
    return daylight ? kTimeZoneIdDaylight : kTimeZoneIdStandard;
}

/// The current zone for the current year. Returns TIME_ZONE_ID_* and the
/// zone (nullopt on the host-derived fallback).
u32 fill_time_zone_information(u8* out172, std::optional<WindowsZone>* zone_out = nullptr) {
    auto zone = host_time_zone();
    if (zone_out != nullptr) *zone_out = zone;
    if (!zone) return fill_time_zone_information_from_host(out172);
    lay_out_tzi(out172, *zone, zone_rules(*zone, current_utc_year()));
    return time_zone_id(out172);
}

}  // namespace

// =============================================================================
// Time / Date
// =============================================================================

void Kernel32ExtManager::impl_GetDynamicTimeZoneInformation(Marshaler& m) {
    // DWORD GetDynamicTimeZoneInformation(PDYNAMIC_TIME_ZONE_INFORMATION):
    // TIME_ZONE_INFORMATION (172), TimeZoneKeyName[128], then
    // DynamicDaylightTimeDisabled (sizeof 432, tz_year_probe line 1).
    const GuestAddr out = m.arg_ptr(0);
    if (out == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0xFFFFFFFFu);  // TIME_ZONE_ID_INVALID
        return;
    }
    u8 host[432] = {};
    std::optional<WindowsZone> zone;
    const u32 id = fill_time_zone_information(host, &zone);
    if (zone) {
        for (usize i = 0; i < 127 && i < zone->key.size(); ++i) {
            host[172 + i * 2] = static_cast<u8>(zone->key[i]);
            host[172 + i * 2 + 1] = static_cast<u8>(zone->key[i] >> 8);
        }
    }
    (void)m_memory.write_bytes(out, host, sizeof host);
    m.set_return(id);
}

void Kernel32ExtManager::impl_GetTimeZoneInformationForYear(Marshaler& m) {
    // BOOL GetTimeZoneInformationForYear(USHORT wYear,
    //     PDYNAMIC_TIME_ZONE_INFORMATION pdtzi, LPTIME_ZONE_INFORMATION ptzi)
    // Measured (tz_year_probe): wYear 0 is the current year; NULL pdtzi is
    // the current zone; key names compare case-insensitively; an unknown or
    // empty key fails with ERROR_FILE_NOT_FOUND and leaves ptzi untouched;
    // success leaves the last error alone for the current zone's exact key
    // and sets 0 for any other. A NULL ptzi faults on Windows; Vitreo fails
    // with ERROR_INVALID_PARAMETER instead.
    const u32 year_arg = static_cast<u32>(m.arg(0) & 0xFFFF);
    const GuestAddr dtzi = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    const u32 year = year_arg == 0 ? current_utc_year() : year_arg;
    const auto current = host_time_zone();
    u8 host[kTimeZoneInformationBytes];
    if (dtzi == 0) {
        if (!current) {
            (void)fill_time_zone_information_from_host(host);
        } else {
            lay_out_tzi(host, *current, zone_rules(*current, year));
        }
        (void)m_memory.write_bytes(out, host, sizeof host);
        m.set_return(1u);
        return;
    }
    std::u16string key;
    for (u32 i = 0; i < 128; ++i) {
        const u16 c = m_memory.read_u16(dtzi + 172 + i * 2).value_or(0);
        if (c == 0) break;
        key.push_back(static_cast<char16_t>(c));
    }
    const bool disabled = m_memory.read_u8(dtzi + 428).value_or(0) != 0;
    const auto zone = find_time_zone(key);
    if (!zone) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        m.set_return(0u);
        return;
    }
    // DynamicDaylightTimeDisabled: the zone's standing (current) rules
    // instead of that year's.
    lay_out_tzi(host, *zone, zone_rules(*zone, disabled ? current_utc_year() : year));
    (void)m_memory.write_bytes(out, host, sizeof host);
    if (!current || key != current->key) {
        set_last_error(error_codes::ERROR_SUCCESS);
    }
    m.set_return(1u);
}

void Kernel32ExtManager::impl_GetTimeZoneInformation(Marshaler& m) {
    // DWORD GetTimeZoneInformation(LPTIME_ZONE_INFORMATION lpTimeZoneInformation)
    // Learn + measured on Windows (C:\oracle\mem\tz64.exe): Bias/names/DST
    // come from the active zone; return TIME_ZONE_ID_STANDARD or _DAYLIGHT.
    GuestAddr tz_ptr = m.arg_ptr(0);
    u8 host[kTimeZoneInformationBytes];
    const u32 id = fill_time_zone_information(host);
    if (tz_ptr != 0) {
        (void)m_memory.write_bytes(tz_ptr, host, kTimeZoneInformationBytes);
    }
    // Measured on Windows 11: success sets ERROR_SUCCESS
    // (last_error_on_success_probe.expected).
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(id);
}

namespace {

constexpr u32 DATE_SHORTDATE   = 0x00000001;
constexpr u32 DATE_LONGDATE    = 0x00000002;
constexpr u32 DATE_YEARMONTH   = 0x00000008;
constexpr u32 DATE_LTRREADING  = 0x00000010;
constexpr u32 DATE_RTLREADING  = 0x00000020;
constexpr u32 DATE_MONTHDAY    = 0x00000080;

const char* const kDayNames[7] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                  "Thursday", "Friday", "Saturday"};
const char* const kMonthNames[12] = {"January", "February", "March", "April",
                                     "May", "June", "July", "August",
                                     "September", "October", "November",
                                     "December"};

void append_number(std::string& out, u32 value, int min_digits) {
    std::string digits = std::to_string(value);
    while (static_cast<int>(digits.size()) < min_digits) digits.insert(0, "0");
    out += digits;
}

/// Gregorian weekday: 0=Sunday … 6=Saturday (SYSTEMTIME / tm_wday).
u32 gregorian_day_of_week(u32 year, u32 month, u32 day) {
    // Sakamoto's methods — matches Windows calendar weekday for GetDateFormat.
    static const u32 t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    u32 y = year;
    if (month < 3) --y;
    return (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;
}

bool is_leap_year(u32 year) {
    return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
}

bool valid_calendar_date(u32 year, u32 month, u32 day) {
    // GetDateFormatEx: earliest supported date is 1601-01-01.
    if (year < 1601 || month < 1 || month > 12 || day < 1) return false;
    static const u32 kDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    u32 max_day = kDays[month];
    if (month == 2 && is_leap_year(year)) max_day = 29;
    return day <= max_day;
}

/// Expand a Windows date picture string against a SYSTEMTIME. Supports the
/// documented d/M/y groups plus 'gg' (era) and single-quoted literals; any
/// other character is copied through verbatim, as Windows does.
std::string format_date(const std::string& picture, u32 year, u32 month,
                        u32 day, u32 day_of_week) {
    std::string out;
    usize i = 0;
    while (i < picture.size()) {
        const char c = picture[i];
        if (c == '\'') {
            // Quoted literal; '' inside a literal is an escaped quote.
            ++i;
            while (i < picture.size()) {
                if (picture[i] == '\'') {
                    if (i + 1 < picture.size() && picture[i + 1] == '\'') {
                        out += '\'';
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                out += picture[i++];
            }
            continue;
        }
        if (c != 'd' && c != 'M' && c != 'y' && c != 'g') {
            out += c;
            ++i;
            continue;
        }
        usize run = 0;
        while (i + run < picture.size() && picture[i + run] == c) ++run;
        switch (c) {
        case 'd':
            if (run == 1 || run == 2) {
                append_number(out, day, static_cast<int>(run));
            } else {
                const char* name = kDayNames[day_of_week % 7];
                out += (run == 3) ? std::string(name).substr(0, 3) : name;
            }
            break;
        case 'M':
            if (run == 1 || run == 2) {
                append_number(out, month, static_cast<int>(run));
            } else {
                const u32 index = (month >= 1 && month <= 12) ? month - 1 : 0;
                const char* name = kMonthNames[index];
                out += (run == 3) ? std::string(name).substr(0, 3) : name;
            }
            break;
        case 'y':
            if (run <= 2) {
                append_number(out, year % 100, 2);
            } else {
                append_number(out, year, 4);
            }
            break;
        case 'g':
            out += "A.D.";
            break;
        default:
            break;
        }
        i += run;
    }
    return out;
}

} // namespace

void Kernel32ExtManager::get_date_format_common(Marshaler& m, bool wide) {
    // int GetDateFormatA/W(LCID Locale, DWORD dwFlags, const SYSTEMTIME* lpDate,
    //                      LPCSTR/W lpFormat, LPSTR/W lpDateStr, int cchDate)
    (void)m.arg(0); // Locale: Vitreo reports en-US everywhere.
    const u32 flags = static_cast<u32>(m.arg(1));
    const GuestAddr date_ptr = m.arg_ptr(2);
    const GuestAddr fmt_ptr = m.arg_ptr(3);
    const GuestAddr out_ptr = m.arg_ptr(4);
    const int cch = static_cast<int>(m.arg(5));

    if (cch < 0 || (cch > 0 && out_ptr == 0)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    // Mutually exclusive style / reading flags (Windows → ERROR_INVALID_FLAGS).
    const u32 style = flags & (DATE_SHORTDATE | DATE_LONGDATE | DATE_YEARMONTH |
                               DATE_MONTHDAY);
    const u32 style_bits = (style & DATE_SHORTDATE ? 1u : 0u) +
                           (style & DATE_LONGDATE ? 1u : 0u) +
                           (style & DATE_YEARMONTH ? 1u : 0u) +
                           (style & DATE_MONTHDAY ? 1u : 0u);
    if (style_bits > 1 ||
        ((flags & DATE_LTRREADING) && (flags & DATE_RTLREADING)) ||
        (flags & DATE_MONTHDAY) != 0) {
        // DATE_MONTHDAY: Windows en-US returns ERROR_INVALID_FLAGS.
        set_last_error(error_codes::ERROR_INVALID_FLAGS);
        m.set_return(0);
        return;
    }

    // SYSTEMTIME: wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond,
    // wMilliseconds — all u16. NULL means "use the current local date".
    u32 year = 0, month = 0, day = 0, day_of_week = 0;
    if (date_ptr != 0) {
        const auto y = m.read_u16(date_ptr + 0);
        const auto mo = m.read_u16(date_ptr + 2);
        const auto dw = m.read_u16(date_ptr + 4);
        const auto d = m.read_u16(date_ptr + 6);
        if (y.is_err() || mo.is_err() || dw.is_err() || d.is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        year = y.value();
        month = mo.value();
        day = d.value();
        (void)dw; // caller's wDayOfWeek is ignored — recompute below
        if (!valid_calendar_date(year, month, day)) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        day_of_week = gregorian_day_of_week(year, month, day);
    } else {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        year = static_cast<u32>(local.tm_year) + 1900;
        month = static_cast<u32>(local.tm_mon) + 1;
        day = static_cast<u32>(local.tm_mday);
        day_of_week = static_cast<u32>(local.tm_wday);
    }

    // An explicit picture string wins; otherwise the flags select a locale
    // default. These are the en-US patterns.
    std::string picture;
    if (fmt_ptr != 0) {
        auto fmt = wide ? m.read_wide_string(fmt_ptr) : m.read_ansi_string(fmt_ptr);
        if (fmt.is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        picture = fmt.value();
    } else if (flags & DATE_LONGDATE) {
        picture = "dddd, MMMM d, yyyy";
    } else if (flags & DATE_YEARMONTH) {
        picture = "MMMM yyyy";
    } else {
        (void)DATE_SHORTDATE; // the default when no style flag is set
        picture = "M/d/yyyy";
    }

    const std::string formatted =
        format_date(picture, year, month, day, day_of_week);
    // Documented return counts UTF-16 code units including the NUL (W) or
    // ANSI bytes including NUL (A) — not UTF-8 byte length of the host string.
    const int required = wide
        ? static_cast<int>(utf8_to_utf16(formatted).size() + 1)
        : static_cast<int>(formatted.size() + 1);

    if (cch == 0) {
        m.set_return(static_cast<u64>(required));
        return;
    }
    if (cch < required) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0);
        return;
    }
    const auto written = wide ? m.write_wide_string(out_ptr, formatted)
                              : m.write_ansi_string(out_ptr, formatted);
    if (written.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    m.set_return(static_cast<u64>(required));
}

void Kernel32ExtManager::impl_GetDateFormatA(Marshaler& m) {
    get_date_format_common(m, /*wide=*/false);
}

void Kernel32ExtManager::impl_GetDateFormatW(Marshaler& m) {
    get_date_format_common(m, /*wide=*/true);
}

void Kernel32ExtManager::impl_SystemTimeToTzSpecificLocalTime(Marshaler& m) {
    // BOOL SystemTimeToTzSpecificLocalTime(
    //   const TIME_ZONE_INFORMATION* lpTimeZoneInformation,
    //   const SYSTEMTIME* lpUniversalTime,
    //   LPSYSTEMTIME lpLocalTime)
    // Learn: NULL zone uses the active zone; DST is applied.
    const GuestAddr tz_ptr = m.arg_ptr(0);
    const GuestAddr utc_time = m.arg_ptr(1);
    const GuestAddr local_time = m.arg_ptr(2);
    if (utc_time == 0 || local_time == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    u16 st[8] = {};
    if (m_memory.read_bytes(utc_time, reinterpret_cast<u8*>(st), 16).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }

    // Build a UTC broken-down time → time_t.
    std::tm utc_tm{};
    utc_tm.tm_year = static_cast<int>(st[0]) - 1900;
    utc_tm.tm_mon = static_cast<int>(st[1]) - 1;
    utc_tm.tm_mday = static_cast<int>(st[3]);
    utc_tm.tm_hour = static_cast<int>(st[4]);
    utc_tm.tm_min = static_cast<int>(st[5]);
    utc_tm.tm_sec = static_cast<int>(st[6]);
    const time_t utc_tt = timegm(&utc_tm);
    if (utc_tt == static_cast<time_t>(-1)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    i32 bias_west = 0;
    i32 daylight_bias = 0;
    bool use_dst = false;
    if (tz_ptr == 0) {
        u8 host[kTimeZoneInformationBytes];
        (void)fill_time_zone_information(host);
        std::memcpy(&bias_west, host + 0, 4);
        std::memcpy(&daylight_bias, host + 168, 4);
        CFTimeZoneRef tz = CFTimeZoneCopySystem();
        if (tz) {
            // CFAbsoluteTime epoch is 2001-01-01; Unix is 1970-01-01.
            const CFAbsoluteTime at =
                static_cast<CFAbsoluteTime>(utc_tt) - kCFAbsoluteTimeIntervalSince1970;
            use_dst = CFTimeZoneIsDaylightSavingTime(tz, at);
            CFRelease(tz);
        }
    } else {
        if (m_memory.read_bytes(tz_ptr, reinterpret_cast<u8*>(&bias_west), 4).is_err() ||
            m_memory.read_bytes(tz_ptr + 168, reinterpret_cast<u8*>(&daylight_bias), 4)
                .is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
        // Without evaluating StandardDate/DaylightDate transition rules,
        // apply DaylightBias when the active host zone is in DST and the
        // supplied TZI has a non-zero DaylightBias (covers the NULL-equivalent
        // "use current settings" path Mono takes with an explicit buffer).
        if (daylight_bias != 0) {
            CFTimeZoneRef tz = CFTimeZoneCopySystem();
            if (tz) {
                const CFAbsoluteTime at =
                    static_cast<CFAbsoluteTime>(utc_tt) -
                    kCFAbsoluteTimeIntervalSince1970;
                use_dst = CFTimeZoneIsDaylightSavingTime(tz, at);
                CFRelease(tz);
            }
        }
    }

    const i32 total_west = bias_west + (use_dst ? daylight_bias : 0);
    const time_t local_tt = utc_tt - static_cast<time_t>(total_west) * 60;
    std::tm local_tm{};
    gmtime_r(&local_tt, &local_tm);  // civil fields in the target zone

    u16 out[8] = {};
    out[0] = static_cast<u16>(local_tm.tm_year + 1900);
    out[1] = static_cast<u16>(local_tm.tm_mon + 1);
    out[2] = static_cast<u16>(local_tm.tm_wday);
    out[3] = static_cast<u16>(local_tm.tm_mday);
    out[4] = static_cast<u16>(local_tm.tm_hour);
    out[5] = static_cast<u16>(local_tm.tm_min);
    out[6] = static_cast<u16>(local_tm.tm_sec);
    out[7] = st[7];  // milliseconds preserved
    if (m_memory.write_bytes(local_time, reinterpret_cast<const u8*>(out), 16)
            .is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

void Kernel32ExtManager::impl_TzSpecificLocalTimeToSystemTime(Marshaler& m) {
    // BOOL TzSpecificLocalTimeToSystemTime(
    //   const TIME_ZONE_INFORMATION* lpTimeZoneInformation,
    //   const SYSTEMTIME* lpLocalTime,
    //   LPSYSTEMTIME lpUniversalTime)
    // Inverse of SystemTimeToTzSpecificLocalTime.
    const GuestAddr tz_ptr = m.arg_ptr(0);
    const GuestAddr local_time = m.arg_ptr(1);
    const GuestAddr utc_time = m.arg_ptr(2);
    if (local_time == 0 || utc_time == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    u16 st[8] = {};
    if (m_memory.read_bytes(local_time, reinterpret_cast<u8*>(st), 16).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }

    time_t utc_tt = static_cast<time_t>(-1);
    if (tz_ptr == 0) {
        // NULL zone: let CF interpret the civil fields in the system zone
        // (handles DST for that calendar date, including ambiguous times as DST).
        CFTimeZoneRef tz = CFTimeZoneCopySystem();
        if (!tz) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        CFGregorianDate gd{};
        gd.year = st[0];
        gd.month = st[1];
        gd.day = st[3];
        gd.hour = st[4];
        gd.minute = st[5];
        gd.second = st[6];
        const CFAbsoluteTime at = CFGregorianDateGetAbsoluteTime(gd, tz);
        CFRelease(tz);
        utc_tt = static_cast<time_t>(at + kCFAbsoluteTimeIntervalSince1970);
    } else {
        i32 bias_west = 0;
        i32 daylight_bias = 0;
        if (m_memory.read_bytes(tz_ptr, reinterpret_cast<u8*>(&bias_west), 4).is_err() ||
            m_memory.read_bytes(tz_ptr + 168, reinterpret_cast<u8*>(&daylight_bias), 4)
                .is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
        std::tm local_as_utc{};
        local_as_utc.tm_year = static_cast<int>(st[0]) - 1900;
        local_as_utc.tm_mon = static_cast<int>(st[1]) - 1;
        local_as_utc.tm_mday = static_cast<int>(st[3]);
        local_as_utc.tm_hour = static_cast<int>(st[4]);
        local_as_utc.tm_min = static_cast<int>(st[5]);
        local_as_utc.tm_sec = static_cast<int>(st[6]);
        const time_t pretend = timegm(&local_as_utc);
        if (pretend == static_cast<time_t>(-1)) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        // Decide DST from the supplied local calendar date via the host zone
        // (transition rules in the TZI itself are not yet evaluated).
        bool use_dst = false;
        if (daylight_bias != 0) {
            CFTimeZoneRef tz = CFTimeZoneCopySystem();
            if (tz) {
                CFGregorianDate gd{};
                gd.year = st[0];
                gd.month = st[1];
                gd.day = st[3];
                gd.hour = st[4];
                gd.minute = st[5];
                gd.second = st[6];
                const CFAbsoluteTime at = CFGregorianDateGetAbsoluteTime(gd, tz);
                use_dst = CFTimeZoneIsDaylightSavingTime(tz, at);
                CFRelease(tz);
            }
        }
        const i32 total_west = bias_west + (use_dst ? daylight_bias : 0);
        utc_tt = pretend + static_cast<time_t>(total_west) * 60;
    }

    if (utc_tt == static_cast<time_t>(-1)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    std::tm utc_tm{};
    gmtime_r(&utc_tt, &utc_tm);

    u16 out[8] = {};
    out[0] = static_cast<u16>(utc_tm.tm_year + 1900);
    out[1] = static_cast<u16>(utc_tm.tm_mon + 1);
    out[2] = static_cast<u16>(utc_tm.tm_wday);
    out[3] = static_cast<u16>(utc_tm.tm_mday);
    out[4] = static_cast<u16>(utc_tm.tm_hour);
    out[5] = static_cast<u16>(utc_tm.tm_min);
    out[6] = static_cast<u16>(utc_tm.tm_sec);
    out[7] = st[7];
    if (m_memory.write_bytes(utc_time, reinterpret_cast<const u8*>(out), 16).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

} // namespace vitreo::winapi
