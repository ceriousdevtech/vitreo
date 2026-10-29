/// @file test_crt_time.cpp
/// @brief Tests for the UCRT time API set (api-ms-win-crt-time)

#include <gtest/gtest.h>
#include <winapi/msvcrt/system/crt_time.hpp>
#include <winapi/msvcrt/text/crt_locale.hpp>
#include <winapi/msvcrt/memory/crt_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <string>

using namespace vitreo;
using namespace vitreo::winapi;

class CrtTimeTest : public ::testing::Test {
protected:
    static constexpr GuestAddr ARENA = 0x20000000;
    static constexpr GuestAddr SCRATCH = 0x10000;
    static constexpr GuestAddr STACK = SCRATCH;

    GuestMemory memory;
    CrtMemoryManager memory_manager{memory, ARENA, 0x20000};
    CrtLocaleManager locale_manager{memory, memory_manager};
    CrtTimeManager manager{memory, memory_manager, locale_manager};
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    DllExports exports = create_crt_time_thunks(manager);

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(SCRATCH, 0x10000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = STACK;
        marshaler.attach_cpu(cpu);
    }

    /// Place x86 cdecl arguments starting just above the return address.
    void set_args(std::initializer_list<u32> args) {
        GuestAddr slot = STACK + 4;
        for (u32 value : args) {
            ASSERT_TRUE(memory.write_u32(slot, value).is_ok());
            slot += 4;
        }
    }

    const ExportEntry& entry(const char* name) {
        auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& e) { return e.name == name; });
        EXPECT_NE(it, exports.entries.end()) << "missing export " << name;
        return *it;
    }

    void call(const char* name) { entry(name).handler(marshaler); }
};

// -----------------------------------------------------------------------------
// Calendar conversion
// -----------------------------------------------------------------------------

TEST_F(CrtTimeTest, GmtimeBreaksDownAKnownInstant) {
    // 2001-09-09T01:46:40Z — the "one billion seconds" instant.
    GuestTm out{};
    ASSERT_TRUE(CrtTimeManager::gmtime(1000000000LL, out));
    EXPECT_EQ(out.tm_year, 101);   // 2001 - 1900
    EXPECT_EQ(out.tm_mon, 8);      // September
    EXPECT_EQ(out.tm_mday, 9);
    EXPECT_EQ(out.tm_hour, 1);
    EXPECT_EQ(out.tm_min, 46);
    EXPECT_EQ(out.tm_sec, 40);
    EXPECT_EQ(out.tm_wday, 0);     // Sunday
    EXPECT_EQ(out.tm_yday, 251);
    EXPECT_EQ(out.tm_isdst, 0);
}

TEST_F(CrtTimeTest, GmtimeRejectsTimesOutsideTheCrtRange) {
    GuestTm out{};
    EXPECT_FALSE(CrtTimeManager::gmtime(-1, out));
    EXPECT_FALSE(CrtTimeManager::gmtime(CrtTimeManager::MAX_TIME64 + 1, out));
    EXPECT_TRUE(CrtTimeManager::gmtime(CrtTimeManager::MAX_TIME64, out));
}

TEST_F(CrtTimeTest, MktimeNormalisesInPlaceAndRoundTrips) {
    // 2001-09-09 with an out-of-range hour that must normalise into the day.
    GuestTm value{};
    value.tm_year  = 101;
    value.tm_mon   = 8;
    value.tm_mday  = 8;
    value.tm_hour  = 25;   // rolls into the 9th
    value.tm_isdst = -1;
    const i64 result = CrtTimeManager::mktime(value);
    ASSERT_NE(result, -1);
    EXPECT_EQ(value.tm_mday, 9);
    EXPECT_EQ(value.tm_hour, 1);
    EXPECT_EQ(value.tm_wday, 0);   // mktime fills in the weekday
}

// -----------------------------------------------------------------------------
// Guest-visible thunks
// -----------------------------------------------------------------------------

TEST_F(CrtTimeTest, Gmtime64WritesAPerThreadTmAndReturnsIt) {
    constexpr GuestAddr time_ptr = SCRATCH + 0x1000;
    ASSERT_TRUE(memory.write_u64(time_ptr, 1000000000ULL).is_ok());
    set_args({static_cast<u32>(time_ptr)});

    call("_gmtime64");
    const auto result = static_cast<GuestAddr>(cpu.gpr.rax);
    ASSERT_NE(result, 0u);

    GuestTm out{};
    ASSERT_TRUE(manager.read_tm(result, out));
    EXPECT_EQ(out.tm_year, 101);
    EXPECT_EQ(out.tm_mon, 8);
    EXPECT_EQ(out.tm_mday, 9);

    // The buffer is a per-thread static, so a second call reuses it.
    call("_gmtime64");
    EXPECT_EQ(static_cast<GuestAddr>(cpu.gpr.rax), result);
}

TEST_F(CrtTimeTest, Gmtime64ReturnsNullAndSetsErrnoOutOfRange) {
    constexpr GuestAddr time_ptr = SCRATCH + 0x1000;
    ASSERT_TRUE(memory.write_u64(
        time_ptr, static_cast<u64>(CrtTimeManager::MAX_TIME64 + 1)).is_ok());
    set_args({static_cast<u32>(time_ptr)});

    call("_gmtime64");
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(memory.read_u32(memory_manager.errno_addr()).value(), 22u);
}

TEST_F(CrtTimeTest, Gmtime64SFillsDestinationAndReportsSuccess) {
    constexpr GuestAddr dest = SCRATCH + 0x1000;
    constexpr GuestAddr time_ptr = SCRATCH + 0x1100;
    ASSERT_TRUE(memory.write_u64(time_ptr, 1000000000ULL).is_ok());
    set_args({static_cast<u32>(dest), static_cast<u32>(time_ptr)});

    call("_gmtime64_s");
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    GuestTm out{};
    ASSERT_TRUE(manager.read_tm(dest, out));
    EXPECT_EQ(out.tm_year, 101);
}

TEST_F(CrtTimeTest, Gmtime64SFillsDestinationWithMinusOneOnFailure) {
    constexpr GuestAddr dest = SCRATCH + 0x1000;
    constexpr GuestAddr time_ptr = SCRATCH + 0x1100;
    ASSERT_TRUE(memory.write_u64(time_ptr, ~0ULL).is_ok());
    set_args({static_cast<u32>(dest), static_cast<u32>(time_ptr)});

    call("_gmtime64_s");
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 22u);
    GuestTm out{};
    ASSERT_TRUE(manager.read_tm(dest, out));
    EXPECT_EQ(out.tm_year, -1);
    EXPECT_EQ(out.tm_sec, -1);
}

TEST_F(CrtTimeTest, Time64ReturnsInEdxEaxOnX86) {
    // A __time64_t return must land in EDX:EAX, not EAX alone: a stale EDX is
    // how the guest's 64-bit time arithmetic silently gains an offset.
    cpu.gpr.rdx = 0xDEADBEEF;
    set_args({0});
    call("_time64");
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rdx), 0u);
    EXPECT_GT(static_cast<u32>(cpu.gpr.rax), 1600000000u);
}

TEST_F(CrtTimeTest, Time64WritesThroughItsOutParameter) {
    constexpr GuestAddr out_ptr = SCRATCH + 0x1000;
    ASSERT_TRUE(memory.write_u64(out_ptr, 0).is_ok());
    set_args({static_cast<u32>(out_ptr)});
    call("_time64");
    const u64 written = memory.read_u64(out_ptr).value();
    EXPECT_EQ(written, cpu.gpr.rax);
    EXPECT_GT(written, 1600000000ULL);
}

TEST_F(CrtTimeTest, Mktime64ReturnsMinusOneInEdxEaxOnFailure) {
    constexpr GuestAddr tm_ptr = SCRATCH + 0x1000;
    GuestTm value{};
    value.tm_year = -4000;   // long before the CRT's epoch
    value.tm_mday = 1;
    ASSERT_TRUE(manager.write_tm(tm_ptr, value));
    set_args({static_cast<u32>(tm_ptr)});

    call("_mktime64");
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0xFFFFFFFFu);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rdx), 0xFFFFFFFFu);
}

TEST_F(CrtTimeTest, StrftimeFormatsIntoTheGuestBuffer) {
    constexpr GuestAddr dest = SCRATCH + 0x1000;
    constexpr GuestAddr fmt = SCRATCH + 0x1100;
    constexpr GuestAddr tm_ptr = SCRATCH + 0x1200;

    GuestTm value{};
    ASSERT_TRUE(CrtTimeManager::gmtime(1000000000LL, value));
    ASSERT_TRUE(manager.write_tm(tm_ptr, value));
    ASSERT_TRUE(marshaler.write_ansi_string(fmt, "%Y-%m-%d %H:%M:%S").is_ok());
    set_args({static_cast<u32>(dest), 64, static_cast<u32>(fmt),
              static_cast<u32>(tm_ptr)});

    call("strftime");
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 19u);
    EXPECT_EQ(marshaler.read_ansi_string(dest).value(), "2001-09-09 01:46:40");
}

TEST_F(CrtTimeTest, StrftimeReportsZeroWhenTheBufferIsTooSmall) {
    constexpr GuestAddr dest = SCRATCH + 0x1000;
    constexpr GuestAddr fmt = SCRATCH + 0x1100;
    constexpr GuestAddr tm_ptr = SCRATCH + 0x1200;

    GuestTm value{};
    ASSERT_TRUE(CrtTimeManager::gmtime(1000000000LL, value));
    ASSERT_TRUE(manager.write_tm(tm_ptr, value));
    ASSERT_TRUE(marshaler.write_ansi_string(fmt, "%Y-%m-%d %H:%M:%S").is_ok());
    set_args({static_cast<u32>(dest), 8, static_cast<u32>(fmt),
              static_cast<u32>(tm_ptr)});

    call("strftime");
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(memory.read_u32(memory_manager.errno_addr()).value(), 34u);
}

TEST_F(CrtTimeTest, AsctimeProducesTheFixedTwentySixCharacterForm) {
    constexpr GuestAddr tm_ptr = SCRATCH + 0x1000;
    GuestTm value{};
    ASSERT_TRUE(CrtTimeManager::gmtime(1000000000LL, value));
    ASSERT_TRUE(manager.write_tm(tm_ptr, value));
    set_args({static_cast<u32>(tm_ptr)});

    call("asctime");
    const auto result = static_cast<GuestAddr>(cpu.gpr.rax);
    ASSERT_NE(result, 0u);
    const std::string text = marshaler.read_ansi_string(result).value();
    EXPECT_EQ(text, "Sun Sep  9 01:46:40 2001\n");
    EXPECT_EQ(text.size(), 25u);  // 26 including the terminator
}

TEST_F(CrtTimeTest, TzsetAppliesGuestTzEnvironment) {
    manager.set_tz_reader([]() {
        return std::optional<std::string>("PST8PDT");
    });
    set_args({});
    call("_tzset");

    constexpr GuestAddr out_ptr = SCRATCH + 0x1000;
    set_args({static_cast<u32>(out_ptr)});
    call("_get_timezone");
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(static_cast<i32>(memory.read_u32(out_ptr).value()), 8 * 3600);

    call("_get_daylight");
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(static_cast<i32>(memory.read_u32(out_ptr).value()), 1);

    call("_get_dstbias");
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(static_cast<i32>(memory.read_u32(out_ptr).value()), -3600);

    // __tzname returns char** — first entry "PST", second "PDT".
    set_args({});
    call("__tzname");
    const auto table = static_cast<GuestAddr>(cpu.gpr.rax);
    ASSERT_NE(table, 0u);
    const auto std_name =
        static_cast<GuestAddr>(memory.read_u32(table).value());
    const auto dst_name =
        static_cast<GuestAddr>(memory.read_u32(table + 4).value());
    EXPECT_EQ(marshaler.read_ansi_string(std_name).value(), "PST");
    EXPECT_EQ(marshaler.read_ansi_string(dst_name).value(), "PDT");

    // Refresh with no DST zone.
    manager.set_tz_reader([]() {
        return std::optional<std::string>("EST5");
    });
    call("_tzset");
    set_args({static_cast<u32>(out_ptr)});
    call("_get_timezone");
    EXPECT_EQ(static_cast<i32>(memory.read_u32(out_ptr).value()), 5 * 3600);
    call("_get_daylight");
    EXPECT_EQ(static_cast<i32>(memory.read_u32(out_ptr).value()), 0);
}

TEST_F(CrtTimeTest, TzsetAppliesNegativeOffsetForEastOfUtc) {
    manager.set_tz_reader([]() {
        return std::optional<std::string>("GST-1GDT");
    });
    set_args({});
    call("_tzset");

    constexpr GuestAddr out_ptr = SCRATCH + 0x1400;
    set_args({static_cast<u32>(out_ptr)});
    call("_get_timezone");
    EXPECT_EQ(static_cast<i32>(memory.read_u32(out_ptr).value()), -3600);
    call("_get_daylight");
    EXPECT_EQ(static_cast<i32>(memory.read_u32(out_ptr).value()), 1);
}

TEST_F(CrtTimeTest, Ftime64DstflagIsZeroWhenZoneHasNoDst) {
    manager.set_tz_reader([]() {
        return std::optional<std::string>("EST5");
    });
    set_args({});
    call("_tzset");

    constexpr GuestAddr out = SCRATCH + 0x1800;
    set_args({static_cast<u32>(out)});
    call("_ftime64");
    // __timeb64: time@0 (8), millitm@8, timezone@10, dstflag@12
    EXPECT_EQ(static_cast<i16>(memory.read_u16(out + 10).value()),
              static_cast<i16>(5 * 60));
    EXPECT_EQ(static_cast<i16>(memory.read_u16(out + 12).value()), 0);
}

TEST_F(CrtTimeTest, Ftime64DstflagFollowsUsRulesWhenTzHasDst) {
    manager.set_tz_reader([]() {
        return std::optional<std::string>("PST8PDT");
    });
    set_args({});
    call("_tzset");

    constexpr GuestAddr out = SCRATCH + 0x1900;
    set_args({static_cast<u32>(out)});
    call("_ftime64");
    EXPECT_EQ(static_cast<i16>(memory.read_u16(out + 10).value()),
              static_cast<i16>(8 * 60));
    const auto dstflag =
        static_cast<i16>(memory.read_u16(out + 12).value());
    EXPECT_TRUE(dstflag == 0 || dstflag == 1);
    // Zone observes DST, so _daylight is 1 — but dstflag is "in effect now".
    constexpr GuestAddr day_out = SCRATCH + 0x1A00;
    set_args({static_cast<u32>(day_out)});
    call("_get_daylight");
    EXPECT_EQ(static_cast<i32>(memory.read_u32(day_out).value()), 1);
}

TEST_F(CrtTimeTest, StrftimeSupportsMsvcHashFlagAndCLocale) {
    constexpr GuestAddr dest = SCRATCH + 0x2000;
    constexpr GuestAddr fmt = SCRATCH + 0x2100;
    constexpr GuestAddr tm_ptr = SCRATCH + 0x2200;

    GuestTm value{};
    ASSERT_TRUE(CrtTimeManager::gmtime(1000000000LL, value));
    ASSERT_TRUE(manager.write_tm(tm_ptr, value));

    auto check = [&](const char* format, const char* expected) {
        ASSERT_TRUE(marshaler.write_ansi_string(fmt, format).is_ok());
        set_args({static_cast<u32>(dest), 128, static_cast<u32>(fmt),
                  static_cast<u32>(tm_ptr)});
        call("strftime");
        EXPECT_EQ(marshaler.read_ansi_string(dest).value(), expected)
            << format;
    };

    check("%c", "Sun Sep  9 01:46:40 2001");
    check("%#c", "Sunday, September 09, 2001 01:46:40");
    check("%x", "09/09/01");
    check("%#x", "Sunday, September 09, 2001");
    check("%d", "09");
    check("%#d", "9");
    check("%H", "01");
    check("%#H", "1");
}

TEST_F(CrtTimeTest, TimezoneQueriesRejectNullOutput) {
    set_args({0});
    call("_get_timezone");
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 22u);
}

// _Gettnames / _W_Gettnames hand each caller a NEW block it frees (MSVCP's
// time facets do): the "C" locale's __crt_lc_time_data with every string
// packed after it, refcount 0. Every value comes from
// tests/oracle_data/gettnames_probe{32,64}.expected (Windows 11 UCRT). A
// shared block was freed once per facet, releasing memory the heap had
// already reused (LibreOffice's SfxItemPool table).
TEST_F(CrtTimeTest, GettnamesReturnsAFreshCallerOwnedBlockPerCall) {
    for (const usize ptr_size : {usize{4}, usize{8}}) {
        SCOPED_TRACE(ptr_size == 8 ? "x64" : "x86");
        const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                          "oracle_data" /
                          (ptr_size == 8 ? "gettnames_probe64.expected" : "gettnames_probe32.expected");
        std::ifstream in(path);
        ASSERT_TRUE(in) << path;

        const GuestAddr block = manager.allocate_lc_time_data(ptr_size);
        const GuestAddr second = manager.allocate_lc_time_data(ptr_size);
        ASSERT_NE(block, 0u);
        ASSERT_NE(second, 0u);
        EXPECT_NE(block, second);  // "two _Gettnames calls distinct=1"

        auto read_ptr = [&](GuestAddr at) -> GuestAddr {
            return ptr_size == 8 ? static_cast<GuestAddr>(memory.read_u64(at).value_or(0))
                                 : static_cast<GuestAddr>(memory.read_u32(at).value_or(0));
        };
        const std::regex header_re(R"re(== _Gettnames sizeof_header=(\d+) msize=(\d+) caltype=(\d+) refcount=(\d+))re");
        const std::regex field_re(R"re((\S+) off=(\d+) (L?)"(.*)")re");
        usize header = 0;
        usize field = 0;
        std::string line;
        while (std::getline(in, line)) {
            std::smatch mt;
            if (std::regex_match(line, mt, header_re)) {
                header = std::stoul(mt[1]);
                EXPECT_EQ(header, 43 * ptr_size + 8 + 44 * ptr_size);
                EXPECT_EQ(memory_manager.crt_block_size(block), std::stoul(mt[2]));  // _msize
                const GuestAddr caltype_at = block + static_cast<GuestAddr>(43 * ptr_size);
                EXPECT_EQ(memory.read_u32(caltype_at).value_or(99), std::stoul(mt[3]));
                EXPECT_EQ(memory.read_u32(caltype_at + 4).value_or(99), std::stoul(mt[4]));
                continue;
            }
            if (!std::regex_match(line, mt, field_re) || header == 0) continue;
            SCOPED_TRACE(line);
            const bool wide = mt[3] == "L";
            const usize slot = field < 43 ? field * ptr_size : 43 * ptr_size + 8 + (field - 43) * ptr_size;
            ++field;
            const GuestAddr str = read_ptr(block + static_cast<GuestAddr>(slot));
            EXPECT_EQ(str, block + static_cast<GuestAddr>(std::stoul(mt[2])));
            const std::string want = mt[4];
            std::string got;
            for (usize i = 0;; ++i) {
                const u16 ch = wide ? memory.read_u16(str + static_cast<GuestAddr>(i * 2)).value_or(0)
                                    : memory.read_u8(str + static_cast<GuestAddr>(i)).value_or(0);
                if (ch == 0 || i > 64) break;
                got.push_back(static_cast<char>(ch));
            }
            EXPECT_EQ(got, want);
        }
        EXPECT_EQ(field, 87u);  // 43 narrow + 44 wide strings

        // The caller frees it ("freed ok"); the other block is untouched.
        memory_manager.crt_free(block);
        EXPECT_NE(read_ptr(second), 0u);
        memory_manager.crt_free(second);
    }
}

TEST_F(CrtTimeTest, GettnamesExportsReturnDistinctBlocks) {
    set_args({});
    call("_Gettnames");
    const auto first = static_cast<GuestAddr>(cpu.gpr.rax);
    call("_W_Gettnames");
    const auto second = static_cast<GuestAddr>(cpu.gpr.rax);
    ASSERT_NE(first, 0u);
    ASSERT_NE(second, 0u);
    EXPECT_NE(first, second);  // "_W_Gettnames distinct=1"
    EXPECT_EQ(marshaler.read_ansi_string(static_cast<GuestAddr>(memory.read_u32(first).value())).value(), "Sun");
}
