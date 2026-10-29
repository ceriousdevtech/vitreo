/// @file test_uxtheme_oracle.cpp
/// @brief uxtheme against Windows 11 Aero, replayed from the oracle captures.
///
/// tests/oracle_data/uxtheme_aero_probe{32,64}.expected is
/// tools/oracle_probe/uxtheme_aero_probe.c run on winoracle's interactive
/// desktop; uxtheme_exports.expected is the real DLL's export table with
/// uxtheme.lib's stdcall sizes. Every expected value below is read from
/// those files; each x86 line is checked through the x86 calling
/// convention and each x64 line through the x64 one.

#include <gtest/gtest.h>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <windowing/model/hwnd_manager.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/graphics/gdi32/gdi32.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/ui/uxtheme/theme_render.hpp>
#include <winapi/ui/uxtheme/uxtheme.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::windowing::WindowData;

namespace {

constexpr GuestAddr kMem = 0x10000;
constexpr GuestAddr kStr = kMem + 0x100;
constexpr GuestAddr kOut = kMem + 0x1000;
constexpr GuestAddr kRect = kMem + 0x3000;
constexpr GuestAddr kRect2 = kMem + 0x3020;
constexpr GuestAddr kStack = 0x70000;
constexpr GuestAddr kRet = 0xDEAD0000;
constexpr u32 kSentinel = 0xCCCCCCCCu;

std::vector<std::string> expected_lines(const std::string& file) {
    std::ifstream in(std::string(VITREO_ORACLE_PROBE_DIR) + "/" + file);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> words(const std::string& s) {
    std::istringstream in(s);
    std::vector<std::string> w;
    for (std::string t; in >> t;) w.push_back(t);
    return w;
}

std::map<std::string, std::string> fields(const std::string& s) {
    std::map<std::string, std::string> f;
    static const std::regex re(R"((!?\w+)=(\S+))");
    for (std::sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it) f[(*it)[1]] = (*it)[2];
    return f;
}

std::vector<i32> nums(const std::string& s) {
    std::vector<i32> v;
    std::stringstream in(s);
    for (std::string t; std::getline(in, t, ',');) v.push_back(static_cast<i32>(std::stol(t)));
    return v;
}

u32 hex(const std::string& s) { return static_cast<u32>(std::stoul(s.substr(0, 8), nullptr, 16)); }

class UxThemeOracleTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(kMem, 0x40000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(kStack - 0x8000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(GdiManager::GDI_DIB_SCRATCH_BASE, GdiManager::GDI_DIB_SCRATCH_SIZE,
                                       MemoryProtection::ReadWrite).is_ok());
        gdi_.set_guest_memory(&memory_);
        UxThemeServices s;
        s.hwnd_mgr = &hwnd_mgr_;
        s.gdi = &gdi_;
        s.send_message = [this](u32 hwnd, u32 msg, u64, u64) -> u64 {
            messages_.push_back({hwnd, msg});
            // DefWindowProc: WM_CTLCOLORSTATIC answers a (system colour) brush.
            return msg == 0x0138 ? 0x1000000Fu : 0;
        };
        s.system_metric = [](i32 index) -> i32 {
            switch (index) {  // GetSystemMetrics on winoracle (probe "gsm=" column)
            case 2: case 3: case 20: case 21: return 17;
            case 4: return 23;
            case 5: case 6: return 1;
            case 15: return 20;
            case 30: return 36;
            case 31: case 52: case 53: return 22;
            case 32: return 8;
            case 54: case 55: return 19;
            default: return 0;
            }
        };
        s.parent = [this](u32 hwnd) -> u32 {
            const auto it = parents_.find(hwnd);
            return it == parents_.end() ? 0 : it->second;
        };
        s.map_point = [](u32, u32, i32& x, i32& y) {
            x += 10;
            y += 5;
            return true;
        };
        exports_ = create_uxtheme_thunks(s);
        GuestAddr bits = 0;
        dib_ = gdi_.create_runtime_dib_section(200, 100, false, 32, &bits);
        dc_ = gdi_.create_compatible_dc(0);
        (void)gdi_.select_object(dc_, dib_);
    }

    const ExportEntry* entry(const std::string& name) const {
        for (const auto& e : exports_.entries)
            if (e.name == name) return &e;
        return nullptr;
    }

    /// Call an export through the x86 (stdcall) or x64 convention; returns EAX/RAX.
    u64 call(const std::string& name, const std::vector<u64>& args) {
        cpu_ = cpu::CpuState{};
        if (is64_) {
            cpu_.mode = cpu::CpuMode::Long;
            cpu_.gpr.rsp = kStack;
            const u64 r[4] = {args.size() > 0 ? args[0] : 0, args.size() > 1 ? args[1] : 0,
                              args.size() > 2 ? args[2] : 0, args.size() > 3 ? args[3] : 0};
            cpu_.gpr.rcx = r[0];
            cpu_.gpr.rdx = r[1];
            cpu_.gpr.r8 = r[2];
            cpu_.gpr.r9 = r[3];
            (void)memory_.write_u64(kStack, kRet);
            for (usize i = 4; i < args.size(); ++i) (void)memory_.write_u64(kStack + 0x28 + (i - 4) * 8, args[i]);
        } else {
            cpu_.init_protected_mode();
            cpu_.set_sp(kStack);
            (void)memory_.write_u32(kStack, static_cast<u32>(kRet));
            for (usize i = 0; i < args.size(); ++i) (void)memory_.write_u32(kStack + 4 + i * 4, static_cast<u32>(args[i]));
        }
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        const ExportEntry* e = entry(name);
        EXPECT_NE(e, nullptr) << name;
        if (e == nullptr) return 0;
        e->handler(m);
        return is64_ ? cpu_.gpr.rax : static_cast<u32>(cpu_.gpr.rax);
    }
    u32 hr(const std::string& name, const std::vector<u64>& args) { return static_cast<u32>(call(name, args)); }

    GuestAddr wstr(GuestAddr at, const std::string& s) {
        for (usize i = 0; i < s.size(); ++i) (void)memory_.write_u16(at + i * 2, static_cast<u8>(s[i]));
        (void)memory_.write_u16(at + s.size() * 2, 0);
        return at;
    }
    void rect(GuestAddr at, i32 l, i32 t, i32 r, i32 b) {
        (void)memory_.write_u32(at, static_cast<u32>(l));
        (void)memory_.write_u32(at + 4, static_cast<u32>(t));
        (void)memory_.write_u32(at + 8, static_cast<u32>(r));
        (void)memory_.write_u32(at + 12, static_cast<u32>(b));
    }
    void fill(GuestAddr at, usize n) {
        for (usize i = 0; i < n; i += 4) (void)memory_.write_u32(at + i, kSentinel);
    }
    i32 rd(GuestAddr at) { return static_cast<i32>(memory_.read_u32(at).value_or(0)); }
    std::string read_size(u32 h) {
        return hex8(h) + ":" + (rd(kOut) == static_cast<i32>(kSentinel) ? std::string("untouched")
                                                                         : std::to_string(rd(kOut)) + "," +
                                                                               std::to_string(rd(kOut + 4)));
    }
    std::string read_rect4(u32 h) {
        if (rd(kOut) == static_cast<i32>(kSentinel)) return hex8(h) + ":untouched";
        return hex8(h) + ":" + std::to_string(rd(kOut)) + "," + std::to_string(rd(kOut + 4)) + "," +
               std::to_string(rd(kOut + 8)) + "," + std::to_string(rd(kOut + 12));
    }
    static std::string hex8(u32 v) {
        char b[16];
        std::snprintf(b, sizeof b, "%08x", v);
        return b;
    }

    u64 open(const std::string& cls, u64 hwnd = 0) { return call("OpenThemeData", {hwnd, wstr(kStr, cls)}); }

    std::vector<std::string> lines(const char* bits32or64) {
        return expected_lines(std::string("uxtheme_aero_probe") + bits32or64 + ".expected");
    }

    GuestMemory memory_;
    cpu::CpuState cpu_;
    HandleTable handles_;
    GdiManager gdi_{handles_};
    windowing::HwndManager hwnd_mgr_;
    DllExports exports_;
    Handle dib_ = 0, dc_ = 0;
    bool is64_ = true;
    std::vector<std::pair<u32, u32>> messages_;
    std::map<u32, u32> parents_;
};

// The export table: every named Windows 11 export, at its ordinal, with
// uxtheme.lib's x86 stdcall size, and nothing else.
TEST_F(UxThemeOracleTest, ExportTableMatchesWindows) {
    usize n = 0;
    for (const auto& line : expected_lines("uxtheme_exports.expected")) {
        if (line.empty() || line[0] == '#') continue;
        const auto w = words(line);
        ASSERT_EQ(w.size(), 3u) << line;
        const ExportEntry* e = entry(w[1]);
        ASSERT_NE(e, nullptr) << "missing export " << w[1];
        EXPECT_EQ(e->ordinal, std::stoi(w[0])) << w[1];
        EXPECT_EQ(e->stdcall_bytes, std::stoi(w[2])) << w[1];
        ++n;
    }
    EXPECT_EQ(n, 85u);
    EXPECT_EQ(exports_.entries.size(), n);
}

// Every [part, state] section of every class Aero defines for the classes
// drawn here: defined/transparent flags, TS_MIN/TS_TRUE/TS_DRAW with and
// without a rectangle, content rectangle, extent, margins and the region's
// availability.
TEST_F(UxThemeOracleTest, PartMetricsMatchAeroBothBitnesses) {
    for (const char* bits : {"32", "64"}) {
        is64_ = std::string(bits) == "64";
        std::map<std::string, u64> themes;
        usize checked = 0;
        std::vector<std::string> bad;
        for (const auto& line : lines(bits)) {
            const auto w = words(line);
            if (w.size() < 4 || w[0] != "ps") continue;
            auto& h = themes[w[1]];
            if (h == 0) h = open(w[1]);
            ASSERT_NE(h, 0u) << w[1];
            const u64 part = std::stoul(w[2]), state = std::stoul(w[3]);
            auto f = fields(line);
            auto check = [&](const std::string& key, const std::string& got) {
                if (f[key] != got) bad.push_back(std::string(bits) + " " + w[1] + " " + w[2] + " " + w[3] + " " + key +
                                                 " want " + f[key] + " got " + got);
            };
            check("def", std::to_string(call("IsThemePartDefined", {h, part, state})));
            check("pt", std::to_string(call("IsThemeBackgroundPartiallyTransparent", {h, part, state})));
            const struct { const char* key; u64 kind; int rect; } sizes[] = {
                {"min", 0, 0}, {"true", 1, 0}, {"draw", 2, 0}, {"trueA", 1, 1}, {"drawA", 2, 1}, {"trueB", 1, 2},
                {"drawB", 2, 2}};
            rect(kRect, 0, 0, 100, 30);
            rect(kRect2, 0, 0, 8, 8);
            for (const auto& s : sizes) {
                fill(kOut, 8);
                const u32 r = hr("GetThemePartSize",
                                 {h, 0, part, state, s.rect == 0 ? 0 : s.rect == 1 ? kRect : kRect2, s.kind, kOut});
                check(s.key, read_size(r));
            }
            fill(kOut, 16);
            check("content", read_rect4(hr("GetThemeBackgroundContentRect", {h, 0, part, state, kRect, kOut})));
            fill(kOut, 16);
            check("contentB", read_rect4(hr("GetThemeBackgroundContentRect", {h, 0, part, state, kRect2, kOut})));
            rect(kRect2, 10, 10, 90, 20);
            fill(kOut, 16);
            check("extent", read_rect4(hr("GetThemeBackgroundExtent", {h, 0, part, state, kRect2, kOut})));
            fill(kOut, 16);
            check("margc", read_rect4(hr("GetThemeMargins", {h, 0, part, state, 3602, 0, kOut})));
            fill(kOut, 16);
            check("margs", read_rect4(hr("GetThemeMargins", {h, 0, part, state, 3601, 0, kOut})));
            (void)memory_.write_u64(kOut, 0x0BADBEEFu);
            const u32 rg = hr("GetThemeBackgroundRegion", {h, 0, part, state, kRect, kOut});
            if (hex8(rg) != f["rgn"].substr(0, 8)) bad.push_back(std::string(bits) + " " + line.substr(0, 30) + " rgn");
            ++checked;
        }
        EXPECT_EQ(checked, 3677u) << bits;
        for (usize i = 0; i < bad.size() && i < 25; ++i) ADD_FAILURE() << bad[i];
        EXPECT_TRUE(bad.empty()) << bad.size() << " mismatches";
    }
}

// Every property Aero reports for those sections: its origin and, for the
// declared type, its value; the other getters succeed or fail as measured
// (Windows' getters do not check the type).
TEST_F(UxThemeOracleTest, PropertiesMatchAeroBothBitnesses) {
    for (const char* bits : {"32", "64"}) {
        is64_ = std::string(bits) == "64";
        std::map<std::string, u64> themes;
        std::vector<std::string> bad;
        usize checked = 0;
        for (const auto& line : lines(bits)) {
            const auto w = words(line);
            if (w.size() < 5 || w[0] != "prop") continue;
            auto& h = themes[w[1]];
            if (h == 0) h = open(w[1]);
            const u64 part = std::stoul(w[2]), state = std::stoul(w[3]), id = std::stoul(w[4]);
            auto f = fields(line);
            auto fail = [&](const std::string& what) { bad.push_back(std::string(bits) + " " + line + " :: " + what); };
            fill(kOut, 4);
            if (hr("GetThemePropertyOrigin", {h, part, state, id, kOut}) != 0 || std::to_string(rd(kOut)) != f["o"])
                fail("origin " + std::to_string(rd(kOut)));
            const std::string t = f["t"];
            auto scalar = [&](const char* fn) {
                fill(kOut, 4);
                const u32 r = hr(fn, {h, part, state, id, kOut});
                return r == 0 ? std::to_string(rd(kOut)) : "hr" + hex8(r);
            };
            if (t == "int") {
                if (scalar("GetThemeInt") != f["int"]) fail("int");
                fill(kOut, 4);
                if (hr("GetThemeMetric", {h, 0, part, state, id, kOut}) != 0 || std::to_string(rd(kOut)) != f["metric"])
                    fail("metric");
            } else if (t == "bool") {
                if (scalar("GetThemeBool") != f["bool"]) fail("bool");
            } else if (t == "enum") {
                if (scalar("GetThemeEnumValue") != f["enum"]) fail("enum");
            } else if (t == "color") {
                fill(kOut, 4);
                if (hr("GetThemeColor", {h, part, state, id, kOut}) != 0) fail("color");
            } else if (t == "margins") {
                fill(kOut, 16);
                const u32 r = hr("GetThemeMargins", {h, 0, part, state, id, 0, kOut});
                const std::string got = read_rect4(r);
                if (got != "00000000:" + f["margins"]) fail("margins " + got);
            } else if (t == "position") {
                fill(kOut, 8);
                const u32 r = hr("GetThemePosition", {h, part, state, id, kOut});
                if (read_size(r) != "00000000:" + f["pos"]) fail("pos");
            } else if (t == "font") {
                fill(kOut, 92);
                if (hr("GetThemeFont", {h, 0, part, state, id, kOut}) != 0) fail("font hr");
                const auto v = nums(f["font"].substr(0, f["font"].rfind(',')));
                if (rd(kOut) != v[0] || rd(kOut + 4) != v[1] || rd(kOut + 16) != v[2]) fail("font value");
            } else if (t == "intlist") {
                fill(kOut, 1612);
                if (hr("GetThemeIntList", {h, part, state, id, kOut}) != 0) fail("intlist hr");
                const auto colon = f["intlist"].find(':');
                const i32 n = std::stoi(f["intlist"].substr(0, colon));
                if (rd(kOut) != n) fail("intlist count");
                const auto v = nums(f["intlist"].substr(colon + 1));
                for (i32 i = 0; i < n && i < static_cast<i32>(v.size()); ++i)
                    if (rd(kOut + 4 + static_cast<GuestAddr>(i) * 4) != v[static_cast<usize>(i)]) {
                        fail("intlist value");
                        break;
                    }
            }
            // Failing getters, measured the first time an id appears in a class.
            for (const auto& [key, value] : f) {
                if (key[0] != '!') continue;
                const std::string g = key.substr(1);
                u32 r = 0;
                fill(kOut, 1612);
                if (g == "font") r = hr("GetThemeFont", {h, 0, part, state, id, kOut});
                else if (g == "intlist") {
                    if (t == "color") continue;  // reads the COLORREF as the count: colours are Vitreo's own
                    r = hr("GetThemeIntList", {h, part, state, id, kOut});
                } else continue;
                if (hex8(r) != value) fail("!" + g + " " + hex8(r));
            }
            ++checked;
        }
        EXPECT_EQ(checked, 3582u) << bits;
        for (usize i = 0; i < bad.size() && i < 25; ++i) ADD_FAILURE() << bad[i];
        EXPECT_TRUE(bad.empty()) << bad.size() << " mismatches";
    }
}

// OpenThemeDataForDpi: Aero's sizes and layout ints at 120, 144 and 192 DPI.
TEST_F(UxThemeOracleTest, DpiMetricsMatchAeroBothBitnesses) {
    for (const char* bits : {"32", "64"}) {
        is64_ = std::string(bits) == "64";
        std::map<std::string, u64> themes;
        std::vector<std::string> bad;
        for (const auto& line : lines(bits)) {
            const auto w = words(line);
            if (w.size() < 6 || (w[0] != "dpi" && w[0] != "dprop") || line.find("open=null") != std::string::npos)
                continue;
            auto& h = themes[w[1] + "@" + w[2]];
            if (h == 0) h = call("OpenThemeDataForDpi", {0, wstr(kStr, w[1]), std::stoul(w[2])});
            ASSERT_NE(h, 0u) << line;
            const u64 part = std::stoul(w[3]), state = std::stoul(w[4]);
            auto f = fields(line);
            auto check = [&](const std::string& key, const std::string& got) {
                if (f.count(key) && f[key] != got) bad.push_back(std::string(bits) + " " + line + " :: " + key + " " + got);
            };
            if (w[0] == "dpi") {
                rect(kRect, 0, 0, 100, 30);
                const struct { const char* key; u64 kind; bool r; } sizes[] = {
                    {"min", 0, false}, {"true", 1, false}, {"trueA", 1, true}, {"drawA", 2, true}};
                for (const auto& s : sizes) {
                    fill(kOut, 8);
                    check(s.key, read_size(hr("GetThemePartSize", {h, 0, part, state, s.r ? kRect : 0, s.kind, kOut})));
                }
                fill(kOut, 16);
                check("content", read_rect4(hr("GetThemeBackgroundContentRect", {h, 0, part, state, kRect, kOut})));
                fill(kOut, 16);
                check("margc", read_rect4(hr("GetThemeMargins", {h, 0, part, state, 3602, 0, kOut})));
                fill(kOut, 16);
                check("margs", read_rect4(hr("GetThemeMargins", {h, 0, part, state, 3601, 0, kOut})));
            } else {
                const u64 id = std::stoul(w[5]);
                fill(kOut, 16);
                if (f.count("int")) check("int", hr("GetThemeInt", {h, part, state, id, kOut}) ? "x" : std::to_string(rd(kOut)));
                fill(kOut, 16);
                if (f.count("metric"))
                    check("metric", hr("GetThemeMetric", {h, 0, part, state, id, kOut}) ? "x" : std::to_string(rd(kOut)));
                fill(kOut, 16);
                if (f.count("margins")) {
                    const std::string m = read_rect4(hr("GetThemeMargins", {h, 0, part, state, id, 0, kOut}));
                    check("margins", m.substr(9));
                }
                fill(kOut, 16);
                if (f.count("pos")) check("pos", read_size(hr("GetThemePosition", {h, part, state, id, kOut})).substr(9));
            }
        }
        for (usize i = 0; i < bad.size() && i < 25; ++i) ADD_FAILURE() << bad[i];
        EXPECT_TRUE(bad.empty()) << bad.size() << " mismatches";
    }
}

// OpenThemeData's class-list parsing, case folding, sub-applications and
// last errors; OpenThemeDataEx/ForDpi; handle sharing and CloseThemeData.
TEST_F(UxThemeOracleTest, OpenContractsMatchAeroBothBitnesses) {
    for (const char* bits : {"32", "64"}) {
        is64_ = std::string(bits) == "64";
        WindowData data{};
        const u32 hw = hwnd_mgr_.create(data);
        for (const auto& line : lines(bits)) {
            auto f = fields(line);
            if (line.rfind("open ", 0) == 0) {
                const bool null_list = line.rfind("open NULL", 0) == 0;
                std::string label;
                if (!null_list) label = line.substr(6, line.find('"', 6) - 6);
                const bool with_hwnd = line.find("\" hwnd ") != std::string::npos;
                set_last_error(0xdead);
                const u64 h = call("OpenThemeData", {with_hwnd ? hw : 0u, null_list ? 0 : wstr(kStr, label)});
                EXPECT_EQ(h != 0 ? "ok" : "null", f["ret"]) << bits << " " << line;
                EXPECT_EQ(hex8(get_last_error()), f["err"]) << bits << " " << line;
                if (h) (void)call("CloseThemeData", {h});
            } else if (line.rfind("openex ", 0) == 0) {
                const std::string label = line.substr(8, line.find('"', 8) - 8);
                set_last_error(0xdead);
                const u64 h = call("OpenThemeDataEx", {f["hwnd"] == "1" ? hw : 0u, wstr(kStr, label), std::stoul(f["flags"])});
                EXPECT_EQ(h != 0 ? "ok" : "null", f["ret"]) << line;
                EXPECT_EQ(hex8(get_last_error()), f["err"]) << line;
                if (h) (void)call("CloseThemeData", {h});
            } else if (line.rfind("openfordpi ", 0) == 0) {
                set_last_error(0xdead);
                const u64 h = call("OpenThemeDataForDpi", {0, wstr(kStr, "Button"), std::stoul(f["dpi"])});
                EXPECT_EQ(h != 0 ? "ok" : "null", f["ret"]) << line;
                EXPECT_EQ(hex8(get_last_error()), f["err"]) << line;
                if (h) {
                    fill(kOut, 8);
                    EXPECT_EQ(read_size(hr("GetThemePartSize", {h, 0, 1, 1, 0, 1, kOut})), f["true31"]) << line;
                    fill(kOut, 8);
                    EXPECT_EQ(read_size(hr("GetThemePartSize", {h, 0, 3, 1, 0, 1, kOut})), f["chk"]) << line;
                    (void)call("CloseThemeData", {h});
                }
            }
        }
        // samehandle twice=1 case=1 hwnd=1 list=1; close valid / second S_OK; NULL / bogus E_HANDLE.
        const u64 h1 = open("Button"), h2 = open("Button"), h3 = open("BUTTON"), h4 = open("Button", hw),
                  h5 = open("Button;Edit");
        EXPECT_TRUE(h1 == h2 && h1 == h3 && h1 == h4 && h1 == h5);
        set_last_error(0xdead);
        EXPECT_EQ(hr("CloseThemeData", {h1}), 0u);
        EXPECT_EQ(hr("CloseThemeData", {h1}), 0u);
        EXPECT_EQ(get_last_error(), 0xdeadu);
        for (u64 h : {h3, h4, h5}) (void)call("CloseThemeData", {h});
        EXPECT_EQ(hr("CloseThemeData", {0}), 0x80070006u);
        EXPECT_EQ(hr("CloseThemeData", {0x1234}), 0x80070006u);
        // useafterclose partsize=80070006:untouched
        fill(kOut, 8);
        EXPECT_EQ(read_size(hr("GetThemePartSize", {h1, 0, 1, 1, 0, 1, kOut})), "80070006:untouched");
    }
}

// The probe's parameter matrix for every HTHEME export: NULL and bogus
// handles, NULL outputs, unknown part/state/property and a property of
// another type, with BUTTON part 1 state 1.
TEST_F(UxThemeOracleTest, ApiMatrixMatchesAeroBothBitnesses) {
    struct Api { const char* name; u64 defprop, wrongprop; };
    const std::map<std::string, Api> apis = {
        {"GetThemeBool", {"GetThemeBool", 2201, 3803}}, {"GetThemeColor", {"GetThemeColor", 3803, 3602}},
        {"GetThemeEnumValue", {"GetThemeEnumValue", 4001, 3803}}, {"GetThemeInt", {"GetThemeInt", 2401, 3803}},
        {"GetThemeIntList", {"GetThemeIntList", 6000, 3803}}, {"GetThemeMargins", {"GetThemeMargins", 3602, 3803}},
        {"GetThemeMetric", {"GetThemeMetric", 2403, 3803}}, {"GetThemePosition", {"GetThemePosition", 3401, 3803}},
        {"GetThemeRect", {"GetThemeRect", 5002, 3803}}, {"GetThemeFont", {"GetThemeFont", 210, 3803}},
        {"GetThemeFilename", {"GetThemeFilename", 3002, 3803}}, {"GetThemeString", {"GetThemeString", 3201, 3803}},
        {"GetThemeBitmap", {"GetThemeBitmap", 2, 3803}}, {"GetThemePropertyOrigin", {"GetThemePropertyOrigin", 3002, 0}},
        {"GetThemeTransitionDuration", {"GetThemeTransitionDuration", 6000, 3803}}};
    for (const char* bits : {"32", "64"}) {
        is64_ = std::string(bits) == "64";
        const u64 button = open("Button");
        rect(kRect, 0, 0, 100, 30);
        std::vector<std::string> bad;
        for (const auto& line : lines(bits)) {
            const auto w = words(line);
            if (w.size() < 3 || w[0] != "api") continue;
            const std::string& name = w[1];
            const std::string& v = w[2];
            // x86 GetThemeTransitionDuration on TMT_TEXTCOLOR reads a COLORREF
            // as an int list: the outcome is whatever memory follows it.
            if (!is64_ && name == "GetThemeTransitionDuration" && v == "wrongtype") continue;
            // GetThemeIntList on TMT_TEXTCOLOR takes the COLORREF as the count;
            // the colours are Vitreo's (AppKit's), so the count is not Aero's.
            if (name == "GetThemeIntList" && v == "wrongtype") continue;
            auto f = fields(line);
            const u64 h = v == "nullh" ? 0 : v == "bogus" ? 0x1234 : button;
            const u64 p = v == "part99" ? 99 : 1, s = v == "state99" ? 99 : 1;
            const auto api = apis.find(name);
            const u64 prop = api == apis.end() ? 0 : v == "prop9999" ? 9999 : v == "wrongtype" ? api->second.wrongprop
                                                                                               : api->second.defprop;
            const bool nullout = v == "nullout";
            const GuestAddr out = nullout ? 0 : kOut;
            fill(kOut, 2048);
            set_last_error(0xdead);
            u32 r = 0;
            bool skip = false;
            if (name == "DrawThemeBackground" || name == "DrawThemeBackgroundEx")
                r = hr(name, {h, dc_, p, s, nullout ? 0 : kRect, 0});
            else if (name == "DrawThemeEdge") r = hr(name, {h, dc_, p, s, nullout ? 0 : kRect, 5, 0x200F, kOut});
            else if (name == "DrawThemeText")
                r = hr(name, {h, dc_, p, s, wstr(kStr, "Probe"), static_cast<u64>(-1) & 0xFFFFFFFFu, 0, 0, nullout ? 0 : kRect});
            else if (name == "DrawThemeTextEx")
                r = hr(name, {h, dc_, p, s, wstr(kStr, "Probe"), static_cast<u64>(-1) & 0xFFFFFFFFu, 0, nullout ? 0 : kRect, 0});
            else if (name == "GetThemeBackgroundContentRect" || name == "GetThemeBackgroundExtent" ||
                     name == "GetThemeBackgroundRegion")
                r = hr(name, {h, 0, p, s, kRect, out});
            else if (name == "GetThemePartSize") r = hr(name, {h, 0, p, s, 0, 1, out});
            else if (name == "GetThemeMargins" || name == "GetThemeMetric" || name == "GetThemeFont") {
                if (name == "GetThemeMargins") r = hr(name, {h, 0, p, s, prop, 0, out});
                else r = hr(name, {h, 0, p, s, prop, out});
            } else if (name == "GetThemeFilename" || name == "GetThemeString") r = hr(name, {h, p, s, prop, out, 260});
            else if (name == "GetThemeBitmap") r = hr(name, {h, p, s, prop, 1, out});
            else if (name == "GetThemeTransitionDuration") r = hr(name, {h, p, s, s + 1, prop, out});
            else if (name == "HitTestThemeBackground") {
                const u64 pt = (u64{15} << 32) | 50u;
                r = is64_ ? hr(name, {h, 0, p, s, 0, kRect, 0, pt, out}) : hr(name, {h, 0, p, s, 0, kRect, 0, 50, 15, out});
            } else if (name == "IsThemeBackgroundPartiallyTransparent" || name == "IsThemePartDefined") {
                r = 0;
                const u64 ret = call(name, {h, p, s});
                if (std::to_string(ret) != f["ret"]) bad.push_back(line + " ret " + std::to_string(ret));
            } else if (name == "GetThemeTextExtent") r = hr(name, {h, dc_, p, s, wstr(kStr, "Probe"), 0xFFFFFFFFu, 0, 0, out});
            else if (name == "GetThemeTextMetrics") r = hr(name, {h, dc_, p, s, out});
            else if (name == "GetThemeAnimationProperty") r = hr(name, {h, p, s, 0, out, 4, kOut + 0x700});
            else if (name == "GetThemeAnimationTransform") r = hr(name, {h, p, s, 0, out, 2048, kOut + 0x700});
            else if (name == "GetThemeTimingFunction") r = hr(name, {h, p, out, 2048, kOut + 0x700});
            else if (name == "GetThemeStream") skip = true;  // theme-file data streams are not shipped
            else if (name == "DrawThemeIcon") r = hr(name, {h, dc_, p, s, kRect, 0, 0});
            else if (api != apis.end()) r = hr(name, {h, p, s, prop, out});
            else skip = true;
            if (skip) continue;
            if (hex8(r) != f["hr"]) bad.push_back(std::string(bits) + " " + line + " :: hr " + hex8(r));
            if (f.count("err") && hex8(get_last_error()) != f["err"])
                bad.push_back(std::string(bits) + " " + line + " :: err " + hex8(get_last_error()));
            if (f.count("out") && !nullout && name != "DrawThemeEdge") {
                const bool touched = rd(kOut) != static_cast<i32>(kSentinel) ||
                                     (name == "GetThemeFilename" || name == "GetThemeString" || name == "GetThemeFont"
                                          ? memory_.read_u16(kOut).value_or(0) != 0xCCCC
                                          : false);
                if ((touched ? "touched" : "untouched") != f["out"])
                    bad.push_back(std::string(bits) + " " + line + " :: out");
            }
        }
        for (usize i = 0; i < bad.size() && i < 30; ++i) ADD_FAILURE() << bad[i];
        EXPECT_TRUE(bad.empty()) << bad.size() << " mismatches";
    }
}

// DrawThemeEdge's content rectangle, the sys-metric/font/bool/int exports,
// transition durations, GetCurrentThemeName and dialog textures.
TEST_F(UxThemeOracleTest, EdgeSysAndMiscContractsMatchAeroBothBitnesses) {
    for (const char* bits : {"32", "64"}) {
        is64_ = std::string(bits) == "64";
        const u64 button = open("Button");
        const u64 win = open("Window");
        WindowData data{};
        const u32 hw = hwnd_mgr_.create(data);
        for (const auto& line : lines(bits)) {
            const auto w = words(line);
            if (w.empty()) continue;
            auto f = fields(line);
            if (w[0] == "edge" && w[1].rfind("p=", 0) == 0) {
                rect(kRect, 0, 0, 50, 20);
                fill(kOut, 16);
                const u32 r = hr("DrawThemeEdge", {button, dc_, std::stoul(f["p"]), 1, kRect, std::stoul(f["edge"]),
                                                   std::stoul(f["flags"], nullptr, 16), kOut});
                EXPECT_EQ(read_rect4(r), f["hr"]) << bits << " " << line;
            } else if (w[0] == "syssize") {
                const u64 h = w[1] == "null" ? 0 : w[1] == "button" ? button : win;
                set_last_error(0xdead);
                const i32 id = std::stoi(w[2]);
                EXPECT_EQ(std::to_string(static_cast<i32>(call("GetThemeSysSize", {h, static_cast<u32>(id)}))), f["ret"])
                    << bits << " " << line;
                EXPECT_EQ(hex8(get_last_error()), f["err"]) << bits << " " << line;
            } else if (w[0] == "sysbool") {
                const u64 h = w[1] == "null" ? 0 : w[1] == "button" ? button : win;
                set_last_error(0xdead);
                EXPECT_EQ(std::to_string(call("GetThemeSysBool", {h, std::stoul(w[2])})), f["ret"]) << line;
                EXPECT_EQ(hex8(get_last_error()), f["err"]) << line;
            } else if (w[0] == "sysint") {
                const u64 h = w[1] == "null" ? 0 : w[1] == "button" ? button : win;
                fill(kOut, 4);
                const u32 r = hr("GetThemeSysInt", {h, std::stoul(w[2]), kOut});
                EXPECT_EQ(hex8(r), f["hr"]) << line;
                if (r == 0) EXPECT_EQ(std::to_string(rd(kOut)), f["v"]) << line;
            } else if (w[0] == "sysfont") {
                const u64 h = w[1] == "null" ? 0 : w[1] == "button" ? button : win;
                fill(kOut, 92);
                const u32 r = hr("GetThemeSysFont", {h, std::stoul(w[2]), kOut});
                EXPECT_EQ(hex8(r), f["f"].substr(0, 8)) << line;
                const std::string want = f["f"].substr(9);
                if (want == "untouched") EXPECT_EQ(rd(kOut), static_cast<i32>(kSentinel)) << line;
                else EXPECT_EQ(std::to_string(rd(kOut)) + "," + std::to_string(rd(kOut + 4)) + "," +
                                   std::to_string(rd(kOut + 16)), want.substr(0, want.find(',', want.find(',', want.find(',') + 1) + 1)))
                    << line;
            } else if (w[0] == "transition") {
                const auto arrow = w[3].find("->");
                const u64 from = std::stoul(w[3].substr(0, arrow)), to = std::stoul(w[3].substr(arrow + 2));
                fill(kOut, 4);
                const u32 r = hr("GetThemeTransitionDuration", {button, std::stoul(w[2]), from, to, std::stoul(f["prop"]), kOut});
                EXPECT_EQ(hex8(r), f["hr"]) << line;
                EXPECT_EQ(f["d"] == "untouched" ? rd(kOut) == static_cast<i32>(kSentinel)
                                                : std::to_string(rd(kOut)) == f["d"], true) << line;
            } else if (w[0] == "intlist") {
                fill(kOut, 1612);
                EXPECT_EQ(hr("GetThemeIntList", {button, std::stoul(w[2]), std::stoul(w[3]), 6000, kOut}), 0u) << line;
                EXPECT_EQ(std::to_string(rd(kOut)), f["n"].substr(0, f["n"].find(':'))) << line;
            } else if (w[0] == "currentthemename" && f.count("name")) {
                const u32 cn = std::stoul(f["name"]), cc = std::stoul(f["color"]);
                fill(kOut, 2048);
                const u32 r = hr("GetCurrentThemeName", {kOut, cn, kOut + 0x400, cc, kOut + 0x600, 260});
                EXPECT_EQ(hex8(r), f["hr"]) << line;
            } else if (w[0] == "dlgtexture" && f.count("flags")) {
                set_last_error(0xdead);
                const u32 r = hr("EnableThemeDialogTexture", {hw, std::stoul(f["flags"], nullptr, 16)});
                EXPECT_EQ(hex8(r), f["hr"]) << line;
                EXPECT_EQ(std::to_string(call("IsThemeDialogTextureEnabled", {hw})), f["enabled"]) << line;
            } else if (w[0] == "dlgtexture" && w[1] == "nullhwnd") {
                set_last_error(0xdead);
                EXPECT_EQ(hex8(hr("EnableThemeDialogTexture", {0, 6})), f["hr"]);
                EXPECT_EQ(hex8(get_last_error()), f["err"]);
            } else if (w[0] == "appprops" && f.count("set")) {
                (void)call("SetThemeAppProperties", {std::stoul(f["set"], nullptr, 16)});
                const u64 h = open("Button");
                EXPECT_EQ(h ? "ok" : "null", f["open"]) << line;
                if (h) (void)call("CloseThemeData", {h});
            } else if (w[0] == "appprops" && f.count("restored")) {
                (void)call("SetThemeAppProperties", {std::stoul(f["restored"], nullptr, 16)});
            }
        }
        EXPECT_EQ(call("GetThemeAppProperties", {}), 3u);  // "appprops 3"
        // The theme name strings themselves: Aero's file path and its colour/size names.
        fill(kOut, 2048);
        ASSERT_EQ(hr("GetCurrentThemeName", {kOut, 260, kOut + 0x400, 260, kOut + 0x600, 260}), 0u);
        auto len = [&](GuestAddr a) { usize n = 0; while (memory_.read_u16(a + n * 2).value_or(0)) ++n; return n; };
        EXPECT_EQ(len(kOut), 46u);
        EXPECT_EQ(len(kOut + 0x400), 11u);
        EXPECT_EQ(len(kOut + 0x600), 10u);
    }
}

// SetWindowTheme / GetWindowTheme: sub-app and id-list substitution, the
// empty list that turns theming off, WM_THEMECHANGED, and which handle a
// window remembers.
TEST_F(UxThemeOracleTest, WindowThemeContractsMatchAeroBothBitnesses) {
    for (const char* bits : {"32", "64"}) {
        is64_ = std::string(bits) == "64";
        WindowData data{};
        const u32 w = hwnd_mgr_.create(data);
        const struct { const char* name; const char* app; const char* ids; } cases[] = {
            {"null_null", nullptr, nullptr},          {"empty_empty", "", ""},
            {"explorer_null", "Explorer", nullptr},   {"null_null2", nullptr, nullptr},
            {"bogus_null", "Bogus", nullptr},         {"null_treeview", nullptr, "TreeView"},
            {"explorer_empty", "Explorer", ""},       {"darkmode_null", "DarkMode_Explorer", nullptr}};
        std::map<std::string, std::map<std::string, std::string>> want;
        for (const auto& line : lines(bits))
            if (line.rfind("setwindowtheme ", 0) == 0) want[words(line)[1]] = fields(line);
        for (const auto& c : cases) {
            messages_.clear();
            set_last_error(0xdead);
            const u32 r = hr("SetWindowTheme", {w, c.app ? wstr(kStr, c.app) : 0, c.ids ? wstr(kStr + 0x200, c.ids) : 0});
            auto& f = want[c.name];
            EXPECT_EQ(hex8(r), f["hr"]) << c.name;
            EXPECT_EQ(std::to_string(messages_.size()), f["themechanged_sync"]) << c.name;
            set_last_error(0xdead);
            const u64 hb = open("Button", w);
            const u64 tv = open("TreeView", w);
            const u64 ex = open("Explorer::TreeView"), pl = open("TreeView"), dk = open("DarkMode_Explorer::TreeView");
            EXPECT_EQ(hb ? "ok" : "null", f["open_button"]) << c.name;
            EXPECT_EQ(tv ? "ok" : "null", f["open_treeview"]) << c.name;
            EXPECT_EQ(std::to_string(tv && tv == ex), f["tv_eq_explorer"]) << c.name;
            EXPECT_EQ(std::to_string(tv && tv == pl), f["tv_eq_plain"]) << c.name;
            EXPECT_EQ(std::to_string(tv && tv == dk), f["tv_eq_dark"]) << c.name;
        }
        (void)hr("SetWindowTheme", {w, 0, 0});
        EXPECT_EQ(hr("SetWindowTheme", {0, wstr(kStr, "Explorer"), 0}), 0x80070006u);
        // getwindowtheme before=null after1_eq1=1 after2_eq1=0 after2_eq2=1
        const u32 g = hwnd_mgr_.create(data);
        EXPECT_EQ(call("GetWindowTheme", {g}), 0u);
        const u64 h1 = open("Button", g);
        EXPECT_EQ(call("GetWindowTheme", {g}), h1);
        const u64 h2 = open("Edit", g);
        EXPECT_EQ(call("GetWindowTheme", {g}), h2);
        // afterclose2 eq2=1: the window keeps the handle it last opened.
        (void)call("CloseThemeData", {h2});
        EXPECT_EQ(call("GetWindowTheme", {g}), h2);
        set_last_error(0xdead);
        EXPECT_EQ(call("GetWindowTheme", {0}), 0u);
        EXPECT_EQ(get_last_error(), 0x80070006u);
        (void)open("Bogus", g);
        EXPECT_EQ(call("GetWindowTheme", {g}), 0u);  // afterbogusopen null=1
    }
}

// DrawThemeBackgroundEx's DTBGOPTS size check, DrawThemeText/TextEx
// parameters and DrawThemeParentBackground(Ex)'s messages and results.
TEST_F(UxThemeOracleTest, DrawContractsMatchAeroBothBitnesses) {
    for (const char* bits : {"32", "64"}) {
        is64_ = std::string(bits) == "64";
        const u64 button = open("Button");
        rect(kRect, 0, 0, 100, 30);
        for (const auto& line : lines(bits)) {
            const auto w = words(line);
            if (w.empty()) continue;
            auto f = fields(line);
            if (w[0] == "dtbex" && f.count("size")) {
                (void)memory_.write_u32(kOut, std::stoul(f["size"]));
                (void)memory_.write_u32(kOut + 4, std::stoul(f["flags"], nullptr, 16));
                rect(kOut + 8, 0, 0, 50, 15);
                EXPECT_EQ(hex8(hr("DrawThemeBackgroundEx", {button, dc_, 1, 1, kRect, kOut})), f["hr"]) << line;
            } else if (w[0] == "dtb") {
                const std::string& n = w[1];
                const bool clip = w[2] == "clip";
                rect(kRect2, 0, 0, 50, 15);
                GuestAddr rc = kRect;
                if (n == "nullrect") rc = 0;
                if (n == "empty") { rc = kRect + 0x40; rect(rc, 10, 10, 10, 10); }
                if (n == "inverted") { rc = kRect + 0x40; rect(rc, 50, 30, 0, 0); }
                EXPECT_EQ(hex8(hr("DrawThemeBackground", {button, n == "nullhdc" ? 0 : dc_, 1, 1, rc, clip ? kRect2 : 0})),
                          f["hr"]) << line;
            } else if (w[0] == "parentbg" || w[0] == "parentbgex") {
                WindowData data{};
                const u32 parent = hwnd_mgr_.create(data);
                const u32 child = hwnd_mgr_.create(data);
                const std::string& kind = w[1];
                u32 target = child;
                if (kind == "nullhwnd") target = 0;
                else if (kind == "toplevel") target = parent;
                else parents_[child] = parent;
                messages_.clear();
                set_last_error(0xdead);
                u32 r;
                if (w[0] == "parentbg") r = hr("DrawThemeParentBackground", {target, dc_, kind == "child_rect" ? kRect : 0});
                else r = hr("DrawThemeParentBackgroundEx", {target, dc_, std::stoul(f["flags"]), 0});
                EXPECT_EQ(hex8(r), f["hr"]) << line;
                EXPECT_EQ(hex8(get_last_error()), f["err"]) << line;
                std::string msgs;
                for (const auto& [hwnd, msg] : messages_) {
                    char b[8];
                    std::snprintf(b, sizeof b, "%04x", msg);
                    msgs += (msgs.empty() ? "" : ",") + std::string(b);
                }
                EXPECT_EQ(msgs, f.count("msgs") ? f["msgs"] : "") << line;
                parents_.clear();
            }
        }
        // dtt: NULL hdc E_HANDLE, NULL text E_POINTER, NULL text with cch 0
        // HRESULT_FROM_WIN32(last error), drawn text sets the last error to 0.
        set_last_error(0xdead);
        EXPECT_EQ(hr("DrawThemeText", {button, 0, 1, 1, wstr(kStr, "Probe"), 0xFFFFFFFFu, 0, 0, kRect}), 0x80070006u);
        EXPECT_EQ(hr("DrawThemeText", {button, dc_, 1, 1, 0, 0xFFFFFFFFu, 0, 0, kRect}), 0x80004003u);
        EXPECT_EQ(hr("DrawThemeText", {button, dc_, 1, 1, 0, 0, 0, 0, kRect}), 0x8007deadu);
        EXPECT_EQ(hr("DrawThemeText", {button, dc_, 1, 1, wstr(kStr, "Probe"), 0xFFFFFFFFu, 0, 0, 0}), 0x80004003u);
        set_last_error(0xdead);
        EXPECT_EQ(hr("DrawThemeText", {button, dc_, 1, 1, wstr(kStr, "Probe"), 0, 0, 0, kRect}), 0u);
        EXPECT_EQ(get_last_error(), 0xdeadu);  // "dtt cch0 err=0000dead"
        // dttex: DTTOPTS.dwSize must be the structure's size (64 x86 / 72 x64).
        (void)memory_.write_u32(kOut, 0);
        EXPECT_EQ(hr("DrawThemeTextEx", {button, dc_, 1, 1, wstr(kStr, "Probe"), 0xFFFFFFFFu, 0, kRect, kOut}), 0x80004005u);
        for (u32 i = 0; i < 72; i += 4) (void)memory_.write_u32(kOut + i, 0);
        (void)memory_.write_u32(kOut, is64_ ? 72u : 64u);
        EXPECT_EQ(hr("DrawThemeTextEx", {button, dc_, 1, 1, wstr(kStr, "Probe"), 0xFFFFFFFFu, 0, kRect, kOut}), 0u);
        (void)memory_.write_u32(kOut, is64_ ? 64u : 72u);
        EXPECT_EQ(hr("DrawThemeTextEx", {button, dc_, 1, 1, wstr(kStr, "Probe"), 0xFFFFFFFFu, 0, kRect, kOut}), 0x80004005u);
    }
}

// Buffered paint: buffer formats, DC origin and the handle contracts.
TEST_F(UxThemeOracleTest, BufferedPaintMatchesAeroBothBitnesses) {
    for (const char* bits : {"32", "64"}) {
        is64_ = std::string(bits) == "64";
        const GuestAddr phdc = kOut + 0x800, pbits = kOut + 0x810, prow = kOut + 0x820;
        rect(kRect, 5, 7, 45, 27);
        for (u64 fmt = 0; fmt <= 3; ++fmt) {
            (void)memory_.write_u64(phdc, 0xBADBEEF);
            set_last_error(0xdead);
            const u64 pb = call("BeginBufferedPaint", {dc_, kRect, fmt, 0, phdc});
            ASSERT_NE(pb, 0u) << fmt;
            EXPECT_EQ(get_last_error(), 0u);
            const u64 paint_dc = is64_ ? memory_.read_u64(phdc).value() : memory_.read_u32(phdc).value();
            EXPECT_EQ(call("GetBufferedPaintDC", {pb}), paint_dc);
            EXPECT_EQ(call("GetBufferedPaintTargetDC", {pb}), dc_);
            fill(kOut, 16);
            EXPECT_EQ(read_rect4(hr("GetBufferedPaintTargetRect", {pb, kOut})), "00000000:5,7,45,27");
            const u32 bits_hr = hr("GetBufferedPaintBits", {pb, pbits, prow});
            EXPECT_EQ(bits_hr, fmt == 0 ? 0x80004005u : 0u) << fmt;
            if (fmt != 0) EXPECT_EQ(rd(prow), fmt == 3 ? 64 : 40) << fmt;  // bits=..:nonnull:40 / :64
            EXPECT_EQ(hr("BufferedPaintClear", {pb, 0}), 0u);
            EXPECT_EQ(hr("BufferedPaintSetAlpha", {pb, 0, 255}), (fmt == 0 || fmt == 3) ? 0x80004005u : 0u) << fmt;
            EXPECT_EQ(hr("EndBufferedPaint", {pb, 1}), 0u);
            EXPECT_EQ(hr("EndBufferedPaint", {pb, 1}), 0x80070057u);  // doubleend second
        }
        // fmt=4/7, cbSize 0, NULL target/rect, empty/inverted rect: NULL with
        // *phdc = NULL and the last error 0; a NULL phdc leaves the error.
        auto begin_fails = [&](u64 target, GuestAddr rc, u64 fmt, GuestAddr params) {
            (void)memory_.write_u64(phdc, 0xBADBEEF);
            set_last_error(0xdead);
            EXPECT_EQ(call("BeginBufferedPaint", {target, rc, fmt, params, phdc}), 0u);
            EXPECT_EQ(rd(phdc), 0);
            EXPECT_EQ(get_last_error(), 0u);
        };
        begin_fails(dc_, kRect, 4, 0);
        begin_fails(dc_, kRect, 7, 0);
        begin_fails(0, kRect, 1, 0);
        begin_fails(dc_, 0, 1, 0);
        rect(kRect2, 10, 10, 10, 10);
        begin_fails(dc_, kRect2, 1, 0);
        (void)memory_.write_u32(kOut + 0x900, 0);  // cbSize 0
        begin_fails(dc_, kRect, 1, kOut + 0x900);
        set_last_error(0xdead);
        EXPECT_EQ(call("BeginBufferedPaint", {dc_, kRect, 1, 0, 0}), 0u);
        EXPECT_EQ(get_last_error(), 0xdeadu);
        // nullhandle dc=null target=null targetrect/bits/clear/alpha E_FAIL end E_INVALIDARG
        EXPECT_EQ(call("GetBufferedPaintDC", {0}), 0u);
        EXPECT_EQ(call("GetBufferedPaintTargetDC", {0}), 0u);
        EXPECT_EQ(hr("GetBufferedPaintTargetRect", {0, kOut}), 0x80004005u);
        EXPECT_EQ(hr("GetBufferedPaintBits", {0, pbits, prow}), 0x80004005u);
        EXPECT_EQ(hr("BufferedPaintClear", {0, 0}), 0x80004005u);
        EXPECT_EQ(hr("BufferedPaintSetAlpha", {0, 0, 0}), 0x80004005u);
        EXPECT_EQ(hr("EndBufferedPaint", {0, 1}), 0x80070057u);
        // bp init/uninit balance, uninit_extra E_FAIL
        EXPECT_EQ(hr("BufferedPaintInit", {}), 0u);
        EXPECT_EQ(hr("BufferedPaintUnInit", {}), 0u);
        EXPECT_EQ(hr("BufferedPaintUnInit", {}), 0x80004005u);
        // banim: BPAS_NONE / no duration have no "from" DC; a bad style or
        // cbSize fails; EndBufferedAnimation(NULL) E_INVALIDARG.
        const GuestAddr anim = kOut + 0xA00, pfrom = kOut + 0xA20, pto = kOut + 0xA30;
        auto banim = [&](u32 cb, u32 style, u32 duration) {
            (void)memory_.write_u32(anim, cb);
            (void)memory_.write_u32(anim + 4, 0);
            (void)memory_.write_u32(anim + 8, style);
            (void)memory_.write_u32(anim + 12, duration);
            (void)memory_.write_u64(pfrom, 0xBADBEEF);
            (void)memory_.write_u64(pto, 0xBADBEEF);
            return call("BeginBufferedAnimation", {0, dc_, kRect, 1, 0, anim, pfrom, pto});
        };
        u64 a = banim(16, 1, 200);
        EXPECT_NE(a, 0u);
        EXPECT_NE(rd(pfrom), 0);
        EXPECT_NE(rd(pto), 0);
        EXPECT_EQ(hr("EndBufferedAnimation", {a, 1}), 0u);
        a = banim(16, 0, 200);
        EXPECT_NE(a, 0u);
        EXPECT_EQ(rd(pfrom), 0);
        EXPECT_NE(rd(pto), 0);
        EXPECT_EQ(hr("EndBufferedAnimation", {a, 1}), 0u);
        EXPECT_EQ(banim(0, 1, 200), 0u);
        EXPECT_EQ(banim(16, 9, 200), 0u);
        EXPECT_EQ(hr("EndBufferedAnimation", {0, 1}), 0x80070057u);
        EXPECT_EQ(call("BufferedPaintRenderAnimation", {0, 0}), 0u);
        EXPECT_EQ(hr("BufferedPaintStopAllAnimations", {0}), 0u);
    }
}

// --- render tests -------------------------------------------------------

/// The straight colour of canvas pixel (x, y), 0xAARRGGBB.
u32 pixel(const uxtheme::PartCanvas& c, i32 x, i32 y) {
    const u8* p = &c.bgra[(static_cast<usize>(y) * static_cast<usize>(c.width) + static_cast<usize>(x)) * 4u];
    const u32 a = p[3];
    if (a == 0) return 0;
    auto un = [&](u8 v) { return std::min<u32>(255u, (v * 255u + a / 2) / a); };
    return (a << 24) | (un(p[2]) << 16) | (un(p[1]) << 8) | un(p[0]);
}

bool any_pixels(const uxtheme::PartCanvas& c) {
    for (usize i = 3; i < c.bgra.size(); i += 4)
        if (c.bgra[i] != 0) return true;
    return false;
}

// Every [part, state] Aero defines for the drawn classes renders without
// failing; the parts Aero draws (not "partially transparent" spacers) put
// pixels down, in both appearances.
TEST(UxThemeRender, EveryMeasuredPartRendersInBothAppearances) {
    std::map<std::string, uxtheme::ThemeClass> classes;
    for (usize i = 0; i < uxtheme::kThemeClassCount; ++i) {
        std::string n;
        for (char16_t ch : uxtheme::kThemeClasses[i].name) n.push_back(static_cast<char>(ch));
        classes[n] = static_cast<uxtheme::ThemeClass>(i);
    }
    usize rendered = 0, drawn = 0;
    for (const auto& line : expected_lines("uxtheme_aero_probe64.expected")) {
        const auto w = words(line);
        if (w.size() < 4 || w[0] != "ps" || w[1].find("::") != std::string::npos) continue;
        const auto cls = classes.at(w[1]);
        const i32 part = std::stoi(w[2]), state = std::stoi(w[3]);
        for (bool dark : {false, true}) {
            const auto palette = uxtheme::default_palette(dark ? windowing::ThemeAppearance::Dark
                                                               : windowing::ThemeAppearance::Light);
            uxtheme::PartCanvas c;
            const bool ok = uxtheme::render_part(cls, part, state, 40, 24, 1.0, palette, dark, {}, c);
            ++rendered;
            if (part == 0) continue;  // the class section has no artwork of its own in most classes
            EXPECT_TRUE(ok) << line;
            if (ok && any_pixels(c)) ++drawn;
        }
    }
    EXPECT_GT(rendered, 2000u);
    EXPECT_GT(drawn, rendered / 2);
}

// macOS look: a checked check box and radio button are filled with the
// accent colour and carry a white mark; unchecked ones are a light bezel.
TEST(UxThemeRender, CheckedButtonsUseTheAccentColour) {
    const auto light = uxtheme::default_palette(windowing::ThemeAppearance::Light);
    const u32 accent = light[windowing::ThemeColor::Accent] & 0xFFFFFFu;
    for (i32 part : {uxtheme::button::BP_CHECKBOX, uxtheme::button::BP_RADIOBUTTON}) {
        uxtheme::PartCanvas on, off;
        ASSERT_TRUE(uxtheme::render_part(uxtheme::ThemeClass::Button, part, 5, 13, 13, 1.0, light, false, {}, on));
        ASSERT_TRUE(uxtheme::render_part(uxtheme::ThemeClass::Button, part, 1, 13, 13, 1.0, light, false, {}, off));
        EXPECT_EQ(pixel(on, 6, 1) & 0xFFFFFFu, accent) << part;  // accent fill away from the mark
        EXPECT_EQ(pixel(on, 6, 6) >> 24, 0xFFu) << part;
        const u32 off_px = pixel(off, 6, 6);
        EXPECT_GT((off_px >> 16) & 0xFF, 0xE0u) << part;  // a white face, no mark
    }
}

// The appearance follows the theme: DarkMode_* classes draw on dark faces.
TEST(UxThemeRender, DarkAppearanceDrawsDarkFaces) {
    const auto light = uxtheme::default_palette(windowing::ThemeAppearance::Light);
    const auto dark = uxtheme::default_palette(windowing::ThemeAppearance::Dark);
    uxtheme::PartCanvas l, d;
    ASSERT_TRUE(uxtheme::render_part(uxtheme::ThemeClass::Button, uxtheme::button::BP_PUSHBUTTON, 1, 75, 23, 1.0,
                                     light, false, {}, l));
    ASSERT_TRUE(uxtheme::render_part(uxtheme::ThemeClass::Button, uxtheme::button::BP_PUSHBUTTON, 1, 75, 23, 1.0,
                                     dark, true, {}, d));
    EXPECT_GT((pixel(l, 37, 11) >> 8) & 0xFF, 0xE0u);
    EXPECT_LT((pixel(d, 37, 11) >> 8) & 0xFF, 0x80u);
    // A pressed button is darker than a normal one.
    uxtheme::PartCanvas p;
    ASSERT_TRUE(uxtheme::render_part(uxtheme::ThemeClass::Button, uxtheme::button::BP_PUSHBUTTON, 3, 75, 23, 1.0,
                                     light, false, {}, p));
    EXPECT_LT((pixel(p, 37, 11) >> 8) & 0xFF, (pixel(l, 37, 11) >> 8) & 0xFF);
}

// The renderer only paints inside the guest's rectangle and clip.
TEST_F(UxThemeOracleTest, DrawThemeBackgroundPaintsInsideRectAndClip) {
    is64_ = true;
    (void)gdi_.with_device_surface(dc_, [](GdiManager::DeviceSurface& s) {
        std::fill(s.rgba, s.rgba + static_cast<usize>(s.width) * static_cast<usize>(s.height) * 4u, u8{0x7F});
    });
    const u64 h = open("Progress");
    rect(kRect, 10, 10, 110, 30);
    rect(kRect2, 10, 10, 60, 30);
    ASSERT_EQ(hr("DrawThemeBackground", {h, dc_, uxtheme::progress::PP_FILL, 1, kRect, kRect2}), 0u);
    bool inside = false, outside = false;
    (void)gdi_.read_device_surface(dc_, [&](const GdiManager::ReadOnlyDeviceSurface& s) {
        for (i32 y = 0; y < s.height; ++y)
            for (i32 x = 0; x < s.width; ++x) {
                const u8* p = s.rgba + (static_cast<usize>(y) * s.width + x) * 4;
                if (p[0] == 0x7F && p[1] == 0x7F && p[2] == 0x7F) continue;
                if (x >= 10 && x < 60 && y >= 10 && y < 30) inside = true;
                else outside = true;
            }
    });
    EXPECT_TRUE(inside);
    EXPECT_FALSE(outside);
}

}  // namespace

namespace {

// Aero's part images: a part IsThemeBackgroundPartiallyTransparent calls
// opaque fills every pixel of its rectangle, and drawing any part twice
// leaves what drawing it once leaves (apps repaint hot/pushed items without
// first repainting the background: VCL's menu bar).
TEST(UxThemeRender, OpaquePartsAreOpaqueAndRepaintsDoNotAccumulate) {
    std::map<std::string, uxtheme::ThemeClass> classes;
    for (usize i = 0; i < uxtheme::kThemeClassCount; ++i) {
        std::string n;
        for (char16_t ch : uxtheme::kThemeClasses[i].name) n.push_back(static_cast<char>(ch));
        classes[n] = static_cast<uxtheme::ThemeClass>(i);
    }
    std::vector<std::string> bad;
    for (const auto& line : expected_lines("uxtheme_aero_probe64.expected")) {
        const auto w = words(line);
        if (w.size() < 6 || w[0] != "ps" || w[1].find("::") != std::string::npos) continue;
        const auto cls = classes.at(w[1]);
        const i32 part = std::stoi(w[2]), state = std::stoi(w[3]);
        const bool opaque = fields(line)["pt"] == "0";
        for (bool dark : {false, true}) {
            const auto palette = uxtheme::default_palette(dark ? windowing::ThemeAppearance::Dark
                                                               : windowing::ThemeAppearance::Light);
            uxtheme::PartDrawOptions o;
            o.opaque = opaque;
            uxtheme::PartCanvas c;
            if (!uxtheme::render_part(cls, part, state, 48, 24, 1.0, palette, dark, o, c)) continue;
            auto alpha = [&](i32 x, i32 y) {
                if (x < 0 || y < 0 || x >= c.width || y >= c.height) return 0u;
                return static_cast<u32>(c.bgra[(static_cast<usize>(y) * c.width + x) * 4 + 3]);
            };
            bool any = false, holes = false, accumulates = false;
            usize drawn = 0, translucent = 0;
            for (i32 y = 0; y < c.height; ++y)
                for (i32 x = 0; x < c.width; ++x) {
                    const u32 a = alpha(x, y);
                    if (a != 0) any = true;
                    if (a != 255) holes = true;
                    // Interior pixels (every neighbour drawn) of a translucent
                    // fill darken on each repaint; anti-aliased edges do not count.
                    bool interior = a != 0;
                    for (i32 dy = -1; dy <= 1 && interior; ++dy)
                        for (i32 dx = -1; dx <= 1 && interior; ++dx) interior = alpha(x + dx, y + dy) != 0;
                    if (a != 0) ++drawn;
                    if (interior && a < 250) ++translucent;
                }
            // A translucent fill covers its whole interior; the anti-aliased
            // rims of rounded corners and thin glyph strokes are a few pixels.
            accumulates = translucent > std::max<usize>(16, drawn / 10);
            const bool overlay = cls == uxtheme::ThemeClass::Progress && part >= uxtheme::progress::PP_PULSEOVERLAY &&
                                 part <= uxtheme::progress::PP_MOVEOVERLAYVERT;
            if (overlay) accumulates = false;  // translucent highlight over the fill by design
            const std::string where = line.substr(0, line.find(" def=")) + (dark ? " dark" : " light");
            if (opaque && any && holes) bad.push_back(where + ": Aero part is opaque, artwork has transparent pixels");
            if (accumulates) bad.push_back(where + ": repainting darkens/lightens (translucent fill)");
        }
    }
    for (usize i = 0; i < bad.size() && i < 60; ++i) ADD_FAILURE() << bad[i];
    EXPECT_TRUE(bad.empty()) << bad.size() << " parts";
}

}  // namespace
