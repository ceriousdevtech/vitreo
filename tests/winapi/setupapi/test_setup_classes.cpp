/// @file test_setup_classes.cpp
/// @brief Device setup class lookups and CM_MapCrToWin32Err, replayed from the
///        Windows captures tools/oracle_probe/setup_class_probe.expected and
///        cm_map_cr_probe.expected (x86 == x64).
///
/// SetupDiClassGuidsFromName knew only "Display" and the W form did not exist,
/// so pyserial's list_ports import failed and every PyInstaller tool that
/// bundles it exited at start.

#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/system/setupapi/setupapi.hpp"

#include <array>
#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kName = 0x1100;
constexpr GuestAddr kList = 0x2000;      // up to 64 GUIDs
constexpr GuestAddr kRequired = 0x1080;
constexpr GuestAddr kOut = 0x3000;
constexpr GuestAddr kGuid = 0x1800;     // outside every area call() refills
constexpr GuestAddr kMachine = 0x1900;  // likewise
constexpr u32 kSentinel = 0xDEADu;

// Driver-installed classes on the oracle machine that the generated table
// leaves out (gen_setup_classes.py NOT_WINDOWS).
const std::set<std::string> kNotWindows = {"nvlddmkm", "PROCMON24", "VirtioSerial"};

std::array<u8, 16> parse_guid(const std::string& s) {
    // {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}
    unsigned d1, d2, d3, b[8];
    std::sscanf(s.c_str(), "{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &d1, &d2, &d3,
                &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]);
    std::array<u8, 16> g{};
    for (int i = 0; i < 4; ++i) g[i] = static_cast<u8>(d1 >> (8 * i));
    g[4] = static_cast<u8>(d2); g[5] = static_cast<u8>(d2 >> 8);
    g[6] = static_cast<u8>(d3); g[7] = static_cast<u8>(d3 >> 8);
    for (int i = 0; i < 8; ++i) g[8 + i] = static_cast<u8>(b[i]);
    return g;
}

class SetupClassTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(0x80000, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = 0x80800;
        marshaler.attach_cpu(cpu);
        exports = create_setupapi_thunks();
        cfgmgr = create_cfgmgr32_thunks();
    }

    struct Out { u32 ret; u32 err; u32 required; usize touched; };

    Out call(const DllExports& table, const char* fn, std::vector<u64> args) {
        for (usize i = 0; i < 0x400; ++i) (void)memory.write_u8(kList + i, 0xCC);
        for (usize i = 0; i < 0x100; ++i) (void)memory.write_u8(kOut + i, 0xCC);
        (void)memory.write_u32(kRequired, 0xCCCCCCCCu);
        for (usize i = 0; i < args.size(); ++i) {
            (void)memory.write_u32(cpu.sp() + 4 + static_cast<GuestAddr>(i) * 4,
                                   static_cast<u32>(args[i]));
        }
        set_last_error(kSentinel);
        bool found = false;
        for (auto& e : table.entries) {
            if (e.name == fn) { e.handler(marshaler); found = true; break; }
        }
        EXPECT_TRUE(found) << "missing export " << fn;
        usize touched = 0;
        for (usize i = 0; i < 0x400; ++i) {
            if (memory.read_u8(kList + i).value_or(0xCC) != 0xCC) touched = i + 1;
        }
        return {static_cast<u32>(cpu.gpr.rax), get_last_error(),
                memory.read_u32(kRequired).value_or(0), touched};
    }
    void wname(const std::u16string& s) {
        for (usize i = 0; i <= s.size(); ++i) {
            (void)memory.write_u16(kName + i * 2, i < s.size() ? s[i] : 0);
        }
    }
    void aname(const std::string& s) {
        for (usize i = 0; i <= s.size(); ++i) {
            (void)memory.write_u8(kName + i, i < s.size() ? static_cast<u8>(s[i]) : 0);
        }
    }
    std::array<u8, 16> guid_at(usize index) {
        std::array<u8, 16> g{};
        (void)memory.read_bytes(kList + index * 16, g.data(), g.size());
        return g;
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    DllExports exports;
    DllExports cfgmgr;
};

}  // namespace

TEST_F(SetupClassTest, EveryInstalledClassRoundTripsByNameAndGuid) {
    std::ifstream in(VITREO_ORACLE_PROBE_DIR "/setup_class_probe.expected");
    ASSERT_TRUE(in.good());
    const std::regex row(R"(^class (\{[0-9a-f-]+\}) (\S+)$)");
    std::string line;
    int checked = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // probe wrote CRLF
        std::smatch m;
        if (!std::regex_match(line, m, row)) continue;
        const std::string name = m[2];
        const auto guid = parse_guid(m[1]);
        if (kNotWindows.count(name) || name == "<none>") continue;
        ++checked;
        // GUID -> name (NameW ports: RequiredSize counts the NUL).
        (void)memory.write_bytes(kGuid, guid.data(), guid.size());
        const auto n = call(exports, "SetupDiClassNameFromGuidW", {kGuid, kOut, 64, kRequired});
        EXPECT_EQ(n.ret, 1u) << name;
        EXPECT_EQ(n.required, name.size() + 1) << name;
        std::string got;
        for (usize i = 0; i < name.size(); ++i) {
            got.push_back(static_cast<char>(memory.read_u16(kOut + i * 2).value_or(0)));
        }
        EXPECT_EQ(got, name);
        // name -> GUIDs includes this one.
        std::u16string wide(name.begin(), name.end());
        wname(wide);
        const auto g = call(exports, "SetupDiClassGuidsFromNameW", {kName, kList, 64, kRequired});
        EXPECT_EQ(g.ret, 1u) << name;
        bool present = false;
        for (u32 i = 0; i < g.required && i < 64; ++i) present |= guid_at(i) == guid;
        EXPECT_TRUE(present) << name;
    }
    EXPECT_EQ(checked, 123);
}

TEST_F(SetupClassTest, GuidsFromNameFollowsTheMeasuredRules) {
    const auto ports = parse_guid("{4d36e978-e325-11ce-bfc1-08002be10318}");
    // W Ports / ports: case-insensitive, ERROR_SUCCESS, one GUID.
    for (const char16_t* n : {u"Ports", u"ports"}) {
        wname(n);
        const auto o = call(exports, "SetupDiClassGuidsFromNameW", {kName, kList, 64, kRequired});
        EXPECT_EQ(o.ret, 1u);
        EXPECT_EQ(o.err, 0u);
        EXPECT_EQ(o.required, 1u);
        EXPECT_EQ(o.touched, 16u);
        EXPECT_EQ(guid_at(0), ports);
    }
    // W PORTS_query: size 0 -> 122, required 1, nothing written.
    wname(u"PORTS");
    auto o = call(exports, "SetupDiClassGuidsFromNameW", {kName, 0, 0, kRequired});
    EXPECT_EQ(o.ret, 0u);
    EXPECT_EQ(o.err, error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(o.required, 1u);
    // W NoSuchClass / _query / empty / trailing_space: TRUE with none.
    for (const char16_t* n : {u"NoSuchClass", u"", u"Ports "}) {
        wname(n);
        for (u32 size : {64u, 0u}) {
            o = call(exports, "SetupDiClassGuidsFromNameW", {kName, size ? kList : 0, size, kRequired});
            EXPECT_EQ(o.ret, 1u);
            EXPECT_EQ(o.err, 0u);
            EXPECT_EQ(o.required, 0u);
            EXPECT_EQ(o.touched, 0u);
        }
    }
    // W null: 87, RequiredSize untouched.
    o = call(exports, "SetupDiClassGuidsFromNameW", {0, kList, 64, kRequired});
    EXPECT_EQ(o.ret, 0u);
    EXPECT_EQ(o.err, error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(o.required, 0xCCCCCCCCu);
    // W Ports_nullreq: the GUID is written, then 87.
    wname(u"Ports");
    o = call(exports, "SetupDiClassGuidsFromNameW", {kName, kList, 64, 0});
    EXPECT_EQ(o.ret, 0u);
    EXPECT_EQ(o.err, error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(o.touched, 16u);
    // W fvevol_1 / fvevol_2: the GUIDs that fit, in registry order.
    wname(u"fvevol");
    o = call(exports, "SetupDiClassGuidsFromNameW", {kName, kList, 1, kRequired});
    EXPECT_EQ(o.ret, 0u);
    EXPECT_EQ(o.err, error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(o.required, 2u);
    EXPECT_EQ(o.touched, 16u);
    EXPECT_EQ(guid_at(0), parse_guid("{3163c566-d381-4467-87bc-a65a18d5b648}"));
    o = call(exports, "SetupDiClassGuidsFromNameW", {kName, kList, 2, kRequired});
    EXPECT_EQ(o.ret, 1u);
    EXPECT_EQ(o.required, 2u);
    EXPECT_EQ(guid_at(1), parse_guid("{3163c566-d381-4467-87bc-a65a18d5b649}"));
    // A Ports / NoSuchClass / null.
    aname("Ports");
    o = call(exports, "SetupDiClassGuidsFromNameA", {kName, kList, 64, kRequired});
    EXPECT_EQ(o.ret, 1u);
    EXPECT_EQ(o.required, 1u);
    aname("NoSuchClass");
    o = call(exports, "SetupDiClassGuidsFromNameA", {kName, kList, 64, kRequired});
    EXPECT_EQ(o.ret, 1u);
    EXPECT_EQ(o.required, 0u);
    o = call(exports, "SetupDiClassGuidsFromNameA", {0, kList, 64, kRequired});
    EXPECT_EQ(o.err, error_codes::ERROR_INVALID_PARAMETER);
    // ExW Ports / _emptymachine / _reserved; ExA Ports.
    wname(u"Ports");
    o = call(exports, "SetupDiClassGuidsFromNameExW", {kName, kList, 4, kRequired, 0, 0});
    EXPECT_EQ(o.ret, 1u);
    EXPECT_EQ(o.required, 1u);
    (void)memory.write_u16(kMachine, 0);
    o = call(exports, "SetupDiClassGuidsFromNameExW", {kName, kList, 4, kRequired, kMachine, 0});
    EXPECT_EQ(o.ret, 1u);
    o = call(exports, "SetupDiClassGuidsFromNameExW", {kName, kList, 4, kRequired, 0, 1});
    EXPECT_EQ(o.ret, 0u);
    EXPECT_EQ(o.err, error_codes::ERROR_INVALID_PARAMETER);
    aname("Ports");
    o = call(exports, "SetupDiClassGuidsFromNameExA", {kName, kList, 4, kRequired, 0, 0});
    EXPECT_EQ(o.ret, 1u);
}

TEST_F(SetupClassTest, NameFromGuidFollowsTheMeasuredRules) {
    const auto ports = parse_guid("{4d36e978-e325-11ce-bfc1-08002be10318}");
    (void)memory.write_bytes(kName, ports.data(), ports.size());
    // NameW ports_small: 122 after reporting 6.
    auto o = call(exports, "SetupDiClassNameFromGuidW", {kName, kOut, 3, kRequired});
    EXPECT_EQ(o.ret, 0u);
    EXPECT_EQ(o.err, error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(o.required, 6u);
    // NameW ports_nullreq / ports_small_nullreq.
    o = call(exports, "SetupDiClassNameFromGuidW", {kName, kOut, 64, 0});
    EXPECT_EQ(o.ret, 1u);
    EXPECT_EQ(o.err, 0u);
    o = call(exports, "SetupDiClassNameFromGuidW", {kName, kOut, 3, 0});
    EXPECT_EQ(o.err, error_codes::ERROR_INSUFFICIENT_BUFFER);
    // NameA ports.
    o = call(exports, "SetupDiClassNameFromGuidA", {kName, kOut, 64, kRequired});
    EXPECT_EQ(o.ret, 1u);
    EXPECT_EQ(o.required, 6u);
    std::string a;
    for (usize i = 0; i < 6; ++i) a.push_back(static_cast<char>(memory.read_u8(kOut + i).value_or(1)));
    EXPECT_EQ(a, std::string("Ports\0", 6));
    // NameExW ports.
    o = call(exports, "SetupDiClassNameFromGuidExW", {kName, kOut, 64, kRequired, 0, 0});
    EXPECT_EQ(o.ret, 1u);
    // NameW bogus: ERROR_INVALID_CLASS, RequiredSize untouched.
    const auto bogus = parse_guid("{12345678-1234-1234-0102-030405060708}");
    (void)memory.write_bytes(kName, bogus.data(), bogus.size());
    o = call(exports, "SetupDiClassNameFromGuidW", {kName, kOut, 64, kRequired});
    EXPECT_EQ(o.ret, 0u);
    EXPECT_EQ(o.err, 0xE0000206u);
    EXPECT_EQ(o.required, 0xCCCCCCCCu);
    // NameW nullguid: ERROR_INVALID_USER_BUFFER.
    o = call(exports, "SetupDiClassNameFromGuidW", {0, kOut, 64, kRequired});
    EXPECT_EQ(o.err, 1784u);
    EXPECT_EQ(o.required, 0xCCCCCCCCu);
}

TEST_F(SetupClassTest, MapCrToWin32ErrMatchesEveryMeasuredCode) {
    std::ifstream in(VITREO_ORACLE_PROBE_DIR "/cm_map_cr_probe.expected");
    ASSERT_TRUE(in.good());
    std::string line;
    int rows = 0;
    while (std::getline(in, line)) {
        unsigned cr = 0, win32 = 0, last = 0;
        if (std::sscanf(line.c_str(), "cr=%x -> %x last=%x", &cr, &win32, &last) != 3) continue;
        ++rows;
        const auto o = call(cfgmgr, "CM_MapCrToWin32Err", {cr, 0xDEAD});
        EXPECT_EQ(o.ret, win32) << "cr=" << cr;
        EXPECT_EQ(o.err, kSentinel) << "last error must be untouched, cr=" << cr;
    }
    EXPECT_EQ(rows, 66);
}
