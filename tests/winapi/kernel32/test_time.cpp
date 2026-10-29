/// @file test_time.cpp
/// @brief Tests for TimeManager (WIN32-080)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/system/time.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/common/crt_dependencies_contract.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <capsule/vfs/vfs.hpp>

#include <thread>
#include <chrono>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class TimeTest : public ::testing::Test {
protected:
    TimeManager mgr;
};

// =============================================================================
// GetTickCount Tests
// =============================================================================

TEST_F(TimeTest, GetTickCount_ReturnsNonZero) {
    u32 ticks = mgr.get_tick_count();
    EXPECT_NE(ticks, 0u);
}

TEST_F(TimeTest, GetTickCount64_ReturnsNonZero) {
    u64 ticks = mgr.get_tick_count64();
    EXPECT_NE(ticks, 0u);
}

TEST_F(TimeTest, GetTickCount64_GreaterOrEqualGetTickCount) {
    u64 tc64 = mgr.get_tick_count64();
    u32 tc32 = mgr.get_tick_count();
    // The 64-bit value truncated to 32 bits should be close to the 32-bit value
    // (they are called nearly simultaneously)
    EXPECT_GE(tc64, static_cast<u64>(tc32));
}

TEST_F(TimeTest, GetTickCount64_AdvancesAfterDelay) {
    u64 t1 = mgr.get_tick_count64();
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    u64 t2 = mgr.get_tick_count64();
    EXPECT_GT(t2, t1);
}

TEST_F(TimeTest, TickCount32MatchesGetTickCount) {
    const u32 a = tick_count32();
    const u32 b = mgr.get_tick_count();
    const u32 c = tick_count32();
    EXPECT_LE(b - a, 5u);
    EXPECT_LE(c - b, 5u);
}

// =============================================================================
// GetLocalTime Tests
// =============================================================================

TEST_F(TimeTest, GetLocalTime_ValidYear) {
    SystemTime st = mgr.get_local_time();
    EXPECT_GE(st.year, 2024u);
}

TEST_F(TimeTest, GetLocalTime_ValidMonth) {
    SystemTime st = mgr.get_local_time();
    EXPECT_GE(st.month, 1u);
    EXPECT_LE(st.month, 12u);
}

TEST_F(TimeTest, GetLocalTime_ValidDay) {
    SystemTime st = mgr.get_local_time();
    EXPECT_GE(st.day, 1u);
    EXPECT_LE(st.day, 31u);
}

// =============================================================================
// GetSystemTime Tests
// =============================================================================

TEST_F(TimeTest, GetSystemTime_ValidUTCFields) {
    SystemTime st = mgr.get_system_time();
    EXPECT_GE(st.year, 2024u);
    EXPECT_GE(st.month, 1u);
    EXPECT_LE(st.month, 12u);
    EXPECT_GE(st.day, 1u);
    EXPECT_LE(st.day, 31u);
    EXPECT_LE(st.hour, 23u);
    EXPECT_LE(st.minute, 59u);
    EXPECT_LE(st.second, 59u);
    EXPECT_LT(st.milliseconds, 1000u);
}

// =============================================================================
// FileTimeToSystemTime / SystemTimeToFileTime Tests
// =============================================================================

TEST_F(TimeTest, FileTimeSystemTime_RoundTrip) {
    // Start with a known SystemTime: 2024-06-15 12:30:45.123
    SystemTime orig{};
    orig.year = 2024;
    orig.month = 6;
    orig.day = 15;
    orig.hour = 12;
    orig.minute = 30;
    orig.second = 45;
    orig.milliseconds = 123;

    u64 ft = 0;
    ASSERT_TRUE(mgr.system_time_to_file_time(orig, ft));

    SystemTime back{};
    ASSERT_TRUE(mgr.file_time_to_system_time(ft, back));

    EXPECT_EQ(back.year, orig.year);
    EXPECT_EQ(back.month, orig.month);
    EXPECT_EQ(back.day, orig.day);
    EXPECT_EQ(back.hour, orig.hour);
    EXPECT_EQ(back.minute, orig.minute);
    EXPECT_EQ(back.second, orig.second);
    EXPECT_EQ(back.milliseconds, orig.milliseconds);
}

TEST_F(TimeTest, FileTimeToSystemTime_KnownEpoch) {
    // EPOCH_OFFSET = 116444736000000000 corresponds to 1970-01-01 00:00:00 UTC
    constexpr u64 EPOCH_OFFSET = 116444736000000000ULL;

    SystemTime st{};
    ASSERT_TRUE(mgr.file_time_to_system_time(EPOCH_OFFSET, st));

    EXPECT_EQ(st.year, 1970u);
    EXPECT_EQ(st.month, 1u);
    EXPECT_EQ(st.day, 1u);
    EXPECT_EQ(st.hour, 0u);
    EXPECT_EQ(st.minute, 0u);
    EXPECT_EQ(st.second, 0u);
}

TEST_F(TimeTest, SystemTimeToFileTime_UnixEpochOffset) {
    // 1970-01-01 00:00:00 UTC -> file time should equal EPOCH_OFFSET
    constexpr u64 EPOCH_OFFSET = 116444736000000000ULL;

    SystemTime st{};
    st.year = 1970;
    st.month = 1;
    st.day = 1;
    st.hour = 0;
    st.minute = 0;
    st.second = 0;
    st.milliseconds = 0;

    u64 ft = 0;
    ASSERT_TRUE(mgr.system_time_to_file_time(st, ft));
    EXPECT_EQ(ft, EPOCH_OFFSET);
}

TEST_F(TimeTest, FileTimeYearBoundsMatchWindows) {
    SystemTime st{};
    // Max legal FILETIME → year 30828 succeeds (Windows).
    EXPECT_TRUE(mgr.file_time_to_system_time(0x7FFFFFFFFFFFFFFFULL, st));
    EXPECT_EQ(st.year, 30828u);
    // Signed bound rejected.
    EXPECT_FALSE(mgr.file_time_to_system_time(0x8000000000000000ULL, st));
    // SystemTimeToFileTime: 30827 ok, 30828 fail.
    SystemTime in{};
    in.year = 30827; in.month = 1; in.day = 1;
    u64 ft = 0;
    EXPECT_TRUE(mgr.system_time_to_file_time(in, ft));
    in.year = 30828;
    EXPECT_FALSE(mgr.system_time_to_file_time(in, ft));
}

TEST_F(TimeTest, FileTimeToSystemTime_WindowsEpoch) {
    SystemTime st{};
    ASSERT_TRUE(mgr.file_time_to_system_time(0, st));
    EXPECT_EQ(st.year, 1601u);
    EXPECT_EQ(st.month, 1u);
    EXPECT_EQ(st.day, 1u);
    EXPECT_EQ(st.hour, 0u);
    EXPECT_EQ(st.minute, 0u);
    EXPECT_EQ(st.second, 0u);
    EXPECT_EQ(st.milliseconds, 0u);
    EXPECT_EQ(st.day_of_week, 1u);  // Monday
}

TEST_F(TimeTest, FileTimeToSystemTime_PreUnixFractionalSecond) {
    // 500 ms before 1970-01-01. Toward-zero division used to land on 1970.
    constexpr u64 EPOCH_OFFSET = 116444736000000000ULL;
    SystemTime st{};
    ASSERT_TRUE(mgr.file_time_to_system_time(EPOCH_OFFSET - 5000000ULL, st));
    EXPECT_EQ(st.year, 1969u);
    EXPECT_EQ(st.month, 12u);
    EXPECT_EQ(st.day, 31u);
    EXPECT_EQ(st.hour, 23u);
    EXPECT_EQ(st.minute, 59u);
    EXPECT_EQ(st.second, 59u);
    EXPECT_EQ(st.milliseconds, 500u);
}

TEST_F(TimeTest, FileTimeToSystemTime_SubMillisecondWindowsEpoch) {
    // 1000 ticks = 0.1 ms after 1601-01-01; Windows reports 0 ms.
    SystemTime st{};
    ASSERT_TRUE(mgr.file_time_to_system_time(1000, st));
    EXPECT_EQ(st.year, 1601u);
    EXPECT_EQ(st.month, 1u);
    EXPECT_EQ(st.day, 1u);
    EXPECT_EQ(st.hour, 0u);
    EXPECT_EQ(st.minute, 0u);
    EXPECT_EQ(st.second, 0u);
    EXPECT_EQ(st.milliseconds, 0u);
}

TEST_F(TimeTest, SystemTimeToFileTime_WindowsEpoch) {
    SystemTime st{};
    st.year = 1601;
    st.month = 1;
    st.day = 1;
    u64 ft = 0xDEADBEEFULL;
    ASSERT_TRUE(mgr.system_time_to_file_time(st, ft));
    EXPECT_EQ(ft, 0u);
}

// =============================================================================
// FileTimeToLocalFileTime / LocalFileTimeToFileTime Tests
// =============================================================================

TEST_F(TimeTest, FileTimeLocalFileTime_RoundTrip) {
    constexpr u64 EPOCH_OFFSET = 116444736000000000ULL;
    u64 utc_ft = EPOCH_OFFSET + 100000000000ULL;  // some time after epoch

    u64 local_ft = 0;
    ASSERT_TRUE(mgr.file_time_to_local_file_time(utc_ft, local_ft));

    u64 back = 0;
    ASSERT_TRUE(mgr.local_file_time_to_file_time(local_ft, back));
    EXPECT_EQ(back, utc_ft);
}

// =============================================================================
// DosDateTime ↔ FileTime (FAT date/time used by cab extractors)
// =============================================================================

TEST_F(TimeTest, DosDateTime_RoundTrip) {
    // 2004-09-15 14:30:00 → FAT date/time
    // year 2004-1980=24, month 9, day 15 → (24<<9)|(9<<5)|15 = 0x3130F
    // hour 14, min 30, sec 0 → (14<<11)|(30<<5)|0 = 0x73C0
    constexpr u16 date = static_cast<u16>((24u << 9) | (9u << 5) | 15u);
    constexpr u16 time = static_cast<u16>((14u << 11) | (30u << 5) | 0u);

    u64 ft = 0;
    ASSERT_TRUE(mgr.dos_date_time_to_file_time(date, time, ft));
    EXPECT_NE(ft, 0u);

    u16 out_date = 0;
    u16 out_time = 0;
    ASSERT_TRUE(mgr.file_time_to_dos_date_time(ft, out_date, out_time));
    EXPECT_EQ(out_date, date);
    EXPECT_EQ(out_time, time);
}

TEST_F(TimeTest, DosDateTime_InvalidDateRejected) {
    u64 ft = 0;
    // month = 0 is invalid
    EXPECT_FALSE(mgr.dos_date_time_to_file_time(0x0001, 0, ft));
}

TEST_F(TimeTest, CreateTimeThunks_IncludesDosDateTime) {
    DllExports exports = create_time_thunks(mgr);
    bool found_to_ft = false;
    bool found_from_ft = false;
    for (const auto& e : exports.entries) {
        if (e.name == "DosDateTimeToFileTime") {
            found_to_ft = true;
            EXPECT_EQ(e.stdcall_bytes, 12u);
        }
        if (e.name == "FileTimeToDosDateTime") {
            found_from_ft = true;
            EXPECT_EQ(e.stdcall_bytes, 12u);
        }
    }
    EXPECT_TRUE(found_to_ft);
    EXPECT_TRUE(found_from_ft);
}

TEST_F(TimeTest, UnsupportedToneAndClockUpdateReturnBoolFailureInBothAbis) {
    // Learn's Beep/SetLocalTime return-value contracts require BOOL FALSE on
    // failure. The unavailable implementation reports the SDK's explicit
    // ERROR_CALL_NOT_IMPLEMENTED, never the nonzero missing-export marker.
    const auto exports = create_time_thunks(mgr);
    for (bool x64 : {false, true}) {
        GuestMemory memory;
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode();
        else cpu.init_protected_mode();
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        u32 found = 0;
        for (const auto& entry : exports.entries) {
            if (entry.name != "Beep" && entry.name != "SetLocalTime") continue;
            ++found;
            cpu.gpr.rax = 0xffffffffu;
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            entry.handler(marshaler);
            EXPECT_EQ(cpu.gpr.rax, 0u) << entry.name << " x64=" << x64;
            EXPECT_EQ(get_last_error(), error_codes::ERROR_CALL_NOT_IMPLEMENTED);
            EXPECT_EQ(entry.stdcall_bytes, entry.name == "Beep"
                ? crt_dependencies_contract::Beep_stdcall_bytes
                : crt_dependencies_contract::SetLocalTime_stdcall_bytes);
            EXPECT_EQ(entry.ordinal, 0u); // Do not invent a Windows ordinal.
        }
        EXPECT_EQ(found, crt_dependencies_contract::api_count);
    }
}

// =============================================================================
// Sleep Tests
// =============================================================================

TEST_F(TimeTest, Sleep_ZeroReturnsImmediately) {
    auto start = std::chrono::steady_clock::now();
    mgr.sleep_ms(0);
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    EXPECT_LT(ms, 50);  // Should be essentially instant
}

TEST_F(TimeTest, Sleep_10ms_DelaysAtLeast5ms) {
    auto start = std::chrono::steady_clock::now();
    mgr.sleep_ms(10);
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    EXPECT_GE(ms, 5);
}

TEST_F(TimeTest, SleepEx_ReturnsZero) {
    u32 ret = mgr.sleep_ex(0, false);
    EXPECT_EQ(ret, 0u);
}

// Audit 06 H5. GetTimeZoneInformation must report the host zone (Bias,
// DaylightBias, current STANDARD/DAYLIGHT id), not an all-zero UTC stub.
// Measured on Windows America/Denver equivalent: Bias=420, DltBias=-60, id=2
// in September; this host is America/Boise (same offsets).
TEST(TimeZoneInformationTest, ReportsHostBiasAndDaylightId) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    Kernel32ExtManager ext(memory, vfs);
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = 0x10800;
    Marshaler m(memory);
    m.attach_cpu(cpu);
    const GuestAddr buf = 0x10100;
    cpu.gpr.rcx = buf;
    set_last_error(0xDEAD);
    ext.impl_GetTimeZoneInformation(m);
    EXPECT_EQ(get_last_error(), 0u);
    const u32 id = static_cast<u32>(cpu.gpr.rax);
    EXPECT_TRUE(id == 1u || id == 2u);  // STANDARD or DAYLIGHT
    i32 bias = 0, dlt = 0;
    ASSERT_TRUE(memory.read_bytes(buf, reinterpret_cast<u8*>(&bias), 4).is_ok());
    ASSERT_TRUE(memory.read_bytes(buf + 168, reinterpret_cast<u8*>(&dlt), 4).is_ok());
    // Mountain: Bias 420, DaylightBias -60. Accept any zone with a non-zero
    // Bias or a DST delta so UTC hosts (Bias 0, no DST) still pass as STANDARD.
    if (dlt != 0) {
        EXPECT_EQ(dlt, -60);
        EXPECT_NE(bias, 0);
    }
    // StandardName should be non-empty UTF-16.
    u16 first = 0;
    ASSERT_TRUE(memory.read_bytes(buf + 4, reinterpret_cast<u8*>(&first), 2).is_ok());
    EXPECT_NE(first, 0u);
}

TEST(TimeZoneInformationTest, SystemTimeToTzSpecificLocalTimeAppliesBias) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    Kernel32ExtManager ext(memory, vfs);
    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler m(memory);
    m.attach_cpu(cpu);

    // UTC noon on 2024-01-15 (standard time for Mountain).
    u16 utc[8] = {2024, 1, 0, 15, 12, 0, 0, 0};
    const GuestAddr utc_addr = 0x10200;
    const GuestAddr local_addr = 0x10220;
    ASSERT_TRUE(memory.write_bytes(utc_addr, reinterpret_cast<const u8*>(utc), 16).is_ok());
    cpu.gpr.rcx = 0;  // NULL TZI → active zone
    cpu.gpr.rdx = utc_addr;
    cpu.gpr.r8 = local_addr;
    ext.impl_SystemTimeToTzSpecificLocalTime(m);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    u16 local[8] = {};
    ASSERT_TRUE(memory.read_bytes(local_addr, reinterpret_cast<u8*>(local), 16).is_ok());
    // Mountain Standard: UTC-7 → local hour 5.
    EXPECT_EQ(local[0], 2024u);
    EXPECT_EQ(local[1], 1u);
    EXPECT_EQ(local[3], 15u);
    EXPECT_EQ(local[4], 5u);
}

TEST(TimeZoneInformationTest, TzSpecificLocalTimeToSystemTimeRoundTrip) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    Kernel32ExtManager ext(memory, vfs);
    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler m(memory);
    m.attach_cpu(cpu);

    // Local 05:00 on 2024-01-15 Mountain Standard → UTC 12:00.
    u16 local[8] = {2024, 1, 0, 15, 5, 0, 0, 0};
    const GuestAddr local_addr = 0x10200;
    const GuestAddr utc_addr = 0x10220;
    ASSERT_TRUE(memory.write_bytes(local_addr, reinterpret_cast<const u8*>(local), 16).is_ok());
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = local_addr;
    cpu.gpr.r8 = utc_addr;
    ext.impl_TzSpecificLocalTimeToSystemTime(m);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    u16 utc[8] = {};
    ASSERT_TRUE(memory.read_bytes(utc_addr, reinterpret_cast<u8*>(utc), 16).is_ok());
    EXPECT_EQ(utc[0], 2024u);
    EXPECT_EQ(utc[1], 1u);
    EXPECT_EQ(utc[3], 15u);
    EXPECT_EQ(utc[4], 12u);
}

// Audit 06 M10 — recompute weekday; validate calendar; reject exclusive flags.
TEST(TimeZoneInformationTest, GetDateFormatWRecomputesWeekdayAndValidates) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000, 0x2000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    Kernel32ExtManager ext(memory, vfs);
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = 0x11000;
    Marshaler m(memory);
    m.attach_cpu(cpu);

    const GuestAddr date = 0x10200;
    const GuestAddr out = 0x10300;
    const GuestAddr fmt = 0x10400;
    // Write picture "dddd"
    const char16_t pic[] = u"dddd";
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(memory.write_u16(fmt + static_cast<u64>(i) * 2,
                                     static_cast<u16>(pic[i])).is_ok());
    }
    // 2024-01-01 is Monday; lie with wDayOfWeek=3 (Wednesday).
    u16 st[8] = {2024, 1, 3, 1, 0, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(date, reinterpret_cast<const u8*>(st), 16).is_ok());

    // Stack args for x64: arg4=out, arg5=cch after rcx/rdx/r8/r9
    cpu.gpr.rcx = 0x0409;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = date;
    cpu.gpr.r9 = fmt;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, out).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, 64).is_ok());
    ext.impl_GetDateFormatW(m);
    EXPECT_GT(cpu.gpr.rax, 0u);
    // Expect "Monday"
    EXPECT_EQ(memory.read_u16(out).value(), u'M');
    EXPECT_EQ(memory.read_u16(out + 2).value(), u'o');
    EXPECT_EQ(memory.read_u16(out + 4).value(), u'n');

    // Feb 30 → ERROR_INVALID_PARAMETER
    st[1] = 2; st[3] = 30;
    ASSERT_TRUE(memory.write_bytes(date, reinterpret_cast<const u8*>(st), 16).is_ok());
    set_last_error(0);
    cpu.gpr.rcx = 0x0409;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = date;
    cpu.gpr.r9 = 0;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, out).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, 64).is_ok());
    ext.impl_GetDateFormatW(m);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    // LONGDATE|SHORTDATE → ERROR_INVALID_FLAGS
    st[1] = 3; st[3] = 15; st[0] = 2024;
    ASSERT_TRUE(memory.write_bytes(date, reinterpret_cast<const u8*>(st), 16).is_ok());
    set_last_error(0);
    cpu.gpr.rcx = 0x0409;
    cpu.gpr.rdx = 0x1 | 0x2; // SHORT|LONG
    cpu.gpr.r8 = date;
    cpu.gpr.r9 = 0;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, out).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, 64).is_ok());
    ext.impl_GetDateFormatW(m);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
}
