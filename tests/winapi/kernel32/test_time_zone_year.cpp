// GetTimeZoneInformationForYear for named zones replays Windows' own output:
// tools/oracle_probe/tz_year_probe{32,64}.expected ("year <key> ..." lines),
// produced from the same registry database the generated table comes from.
// The current-zone lines depend on the test host's zone and are not replayed;
// "err=" is compared by rule below, since it differs only for the host's own
// zone key.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <capsule/vfs/vfs.hpp>

#include <cstdio>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kData = 0x00100000;
constexpr GuestAddr kStack = 0x00200000;
constexpr GuestAddr kDtzi = kData;
constexpr GuestAddr kOut = kData + 0x400;

struct Case {
    const char* key;
    u32 year;
    bool disabled;
};

const Case kCases[] = {
    {"Mountain Standard Time", 2024, false}, {"Mountain Standard Time", 2006, false},
    {"Pacific Standard Time", 2024, false},  {"US Mountain Standard Time", 2024, false},
    {"AUS Eastern Standard Time", 2024, false}, {"GMT Standard Time", 2024, false},
    {"UTC", 2024, false},                    {"Tokyo Standard Time", 2024, false},
    {"Morocco Standard Time", 2024, false},  {"mountain standard time", 2006, false},
    {"Mountain Standard Time", 2006, true},  {"Mountain Standard Time", 2024, true},
    {"Mountain Standard Time", 1990, false}, {"Mountain Standard Time", 2100, false},
    {"No Such Zone", 2024, false},           {"", 2024, false},
};

std::string wide_at(GuestMemory& mem, GuestAddr at) {
    std::string s;
    for (u32 i = 0; i < 32; ++i) {
        const u16 c = mem.read_u16(at + i * 2).value_or(0);
        if (c == 0) break;
        s.push_back(static_cast<char>(c));
    }
    return s;
}

std::string systemtime(GuestMemory& mem, GuestAddr at) {
    u16 w[8];
    for (u32 i = 0; i < 8; ++i) w[i] = mem.read_u16(at + i * 2).value_or(0);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%u/%u/%u/%u %u:%u:%u.%u", w[0], w[1], w[2], w[3], w[4], w[5],
                  w[6], w[7]);
    return buf;
}

std::string strip_err(const std::string& line) {
    return std::regex_replace(line, std::regex(" err=[0-9]+"), "");
}

void replay(bool is64) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    Kernel32ExtManager kernel(memory, vfs);
    cpu::CpuState cpu;
    if (is64) cpu.init_long_mode(); else cpu.init_protected_mode();
    cpu.gpr.rsp = kStack + 0x800;
    Marshaler m(memory);
    m.attach_cpu(cpu);

    std::ifstream file(std::string(VITREO_ORACLE_PROBE_DIR) + "/tz_year_probe" +
                       (is64 ? "64" : "32") + ".expected");
    ASSERT_TRUE(file.good());
    std::vector<std::string> expected;
    for (std::string line; std::getline(file, line);) {
        if (line.rfind("year ", 0) == 0) expected.push_back(line);
    }
    ASSERT_EQ(expected.size(), sizeof kCases / sizeof kCases[0]);

    for (usize i = 0; i < expected.size(); ++i) {
        const Case& c = kCases[i];
        std::vector<u8> zero(432, 0);
        ASSERT_TRUE(memory.write_bytes(kDtzi, zero.data(), zero.size()).is_ok());
        for (u32 k = 0; c.key[k] != 0; ++k) {
            ASSERT_TRUE(memory.write_u16(kDtzi + 172 + k * 2, static_cast<u8>(c.key[k])).is_ok());
        }
        ASSERT_TRUE(memory.write_u8(kDtzi + 428, c.disabled ? 1 : 0).is_ok());
        std::vector<u8> fill(172, 0xCC);
        ASSERT_TRUE(memory.write_bytes(kOut, fill.data(), fill.size()).is_ok());
        if (is64) {
            cpu.gpr.rcx = c.year;
            cpu.gpr.rdx = kDtzi;
            cpu.gpr.r8 = kOut;
        } else {
            ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, c.year).is_ok());
            ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, kDtzi).is_ok());
            ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, kOut).is_ok());
        }
        set_last_error(0xDEAD);
        kernel.impl_GetTimeZoneInformationForYear(m);
        const u32 ok = static_cast<u32>(cpu.gpr.rax);
        const u32 err = get_last_error();
        char head[160];
        std::snprintf(head, sizeof head, "year %s %u%s ok=%u err=%u", c.key, c.year,
                      c.disabled ? " disabled" : "", ok, err);
        std::string line = head;
        if (ok) {
            char tzi[96];
            std::snprintf(tzi, sizeof tzi, " Bias=%d StdBias=%d DltBias=%d",
                          static_cast<i32>(memory.read_u32(kOut).value_or(0)),
                          static_cast<i32>(memory.read_u32(kOut + 84).value_or(0)),
                          static_cast<i32>(memory.read_u32(kOut + 168).value_or(0)));
            line += tzi;
            line += " Std=" + systemtime(memory, kOut + 68);
            line += " Dlt=" + systemtime(memory, kOut + 152);
            line += " StdName=\"" + wide_at(memory, kOut + 4) + "\"";
            line += " DltName=\"" + wide_at(memory, kOut + 88) + "\"";
            // Success: last error untouched only for the host zone's exact key.
            EXPECT_TRUE(err == 0 || err == 0xDEAD) << line;
        } else {
            char first[32];
            std::snprintf(first, sizeof first, " first_dword=0x%08x",
                          memory.read_u32(kOut).value_or(0));
            line += first;
            EXPECT_EQ(err, error_codes::ERROR_FILE_NOT_FOUND) << line;
        }
        EXPECT_EQ(strip_err(line), strip_err(expected[i])) << (is64 ? "x64" : "x86");
    }
}

TEST(TimeZoneYearTest, NamedZonesMatchWindowsX86) { replay(false); }
TEST(TimeZoneYearTest, NamedZonesMatchWindowsX64) { replay(true); }

}  // namespace
