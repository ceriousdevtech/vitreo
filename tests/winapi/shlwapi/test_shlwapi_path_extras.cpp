/// @file test_shlwapi_path_extras.cpp
/// @brief StrPBrk, PathFindFileName, PathStripPath, PathGetDriveNumber,
///        PathCompactPathEx and PathRelativePathTo (A and W) replayed against
///        tools/oracle_probe/shlwapi_path_extras_probe.c's Windows output.
///
/// Every probe line carries its own inputs, so the test parses each line of
/// tests/oracle_data/shlwapi_path_extras_probe{,.x86}.expected, calls the
/// export with the same arguments in the matching bitness, renders the result
/// exactly as the probe prints it and compares the whole line.

#include <gtest/gtest.h>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/system/shlwapi/shlwapi.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kArgA = 0x20000;
constexpr GuestAddr kArgB = 0x22000;
constexpr GuestAddr kOut = 0x24000;
constexpr GuestAddr kStack = 0x2F000;
constexpr u32 kCap = 300;

struct Line {
    std::string tag;
    bool wide = false;
    std::map<std::string, std::optional<std::vector<u16>>> strs;  // nullopt = NULL
    std::map<std::string, std::string> nums;
};

/// Inverse of the probe's escaping for inputs: \0, \xNN, \uNNNN; any other
/// backslash is literal (no probe input has a literal '\' before "0", "x" + 2
/// hex digits or "u" + 4 hex digits).
std::vector<u16> unescape(const std::string& s) {
    auto hex = [&](usize at, usize n) -> std::optional<u16> {
        if (at + n > s.size()) return std::nullopt;
        u16 v = 0;
        for (usize i = 0; i < n; ++i) {
            const char c = s[at + i];
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return std::nullopt;
            v = static_cast<u16>(v * 16 + d);
        }
        return v;
    };
    std::vector<u16> out;
    for (usize i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            if (s[i + 1] == '0') { out.push_back(0); ++i; continue; }
            if (s[i + 1] == 'x') if (auto v = hex(i + 2, 2)) { out.push_back(*v); i += 3; continue; }
            if (s[i + 1] == 'u') if (auto v = hex(i + 2, 4)) { out.push_back(*v); i += 5; continue; }
        }
        out.push_back(static_cast<u8>(s[i]));
    }
    return out;
}

Line parse(const std::string& text) {
    Line l;
    usize p = text.find(' ');
    l.tag = text.substr(0, p);
    l.wide = text[p + 1] == 'W';
    p += 3;
    while (p < text.size()) {
        const usize eq = text.find('=', p);
        const std::string key = text.substr(p, eq - p);
        usize v = eq + 1;
        if (text[v] == '|') {
            const usize end = text.find('|', v + 1);
            l.strs[key] = unescape(text.substr(v + 1, end - v - 1));
            p = end + 2;
        } else {
            usize end = text.find(' ', v);
            if (end == std::string::npos) end = text.size();
            const std::string val = text.substr(v, end - v);
            if (val == "NULL") l.strs[key] = std::nullopt;
            l.nums[key] = val;
            p = end + 1;
        }
    }
    return l;
}

class ShlwapiPathExtrasTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        exports = create_shlwapi_thunks();
        ASSERT_TRUE(memory.map_region(0x10000, 0x20000, MemoryProtection::ReadWrite).is_ok());
    }
    bool x64() const { return GetParam(); }

    u64 call(const std::string& name, const std::vector<u64>& args) {
        const ExportEntry* entry = nullptr;
        for (const auto& e : exports.entries)
            if (e.name == name) entry = &e;
        EXPECT_NE(entry, nullptr) << name;
        if (entry == nullptr) return 0xBADBADu;
        EXPECT_EQ(entry->stdcall_bytes, args.size() * 4) << name;
        cpu = cpu::CpuState{};
        if (x64()) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = kStack;
        for (usize i = 0; i < args.size(); ++i) {
            if (x64()) {
                if (i == 0) cpu.gpr.rcx = args[i];
                else if (i == 1) cpu.gpr.rdx = args[i];
                else if (i == 2) cpu.gpr.r8 = args[i];
                else if (i == 3) cpu.gpr.r9 = args[i];
                else EXPECT_TRUE(memory.write_u64(kStack + 8 + i * 8, args[i]).is_ok());
            } else {
                EXPECT_TRUE(memory.write_u32(kStack + 4 + static_cast<u32>(i) * 4, static_cast<u32>(args[i])).is_ok());
            }
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        entry->handler(m);
        return x64() ? cpu.gpr.rax : static_cast<u32>(cpu.gpr.rax);
    }

    /// Places a probe string (or NULL) in guest memory.
    u64 put(GuestAddr at, const std::optional<std::vector<u16>>& s, bool wide) {
        if (!s) return 0;
        for (usize i = 0; i <= s->size(); ++i) {
            const u16 c = i < s->size() ? (*s)[i] : 0;
            EXPECT_TRUE((wide ? memory.write_u16(at + i * 2, c) : memory.write_u8(at + i, static_cast<u8>(c))).is_ok());
        }
        return at;
    }
    void fill_out(bool wide) {
        for (u32 i = 0; i < kCap; ++i)
            EXPECT_TRUE((wide ? memory.write_u16(kOut + i * 2, '#') : memory.write_u8(kOut + i, '#')).is_ok());
    }
    u16 unit(GuestAddr at, usize i, bool wide) {
        return wide ? memory.read_u16(at + i * 2).value_or(0xFFFF) : memory.read_u8(at + i).value_or(0xFF);
    }
    static std::string esc(const std::vector<u16>& s, bool wide) {
        std::string out;
        char buf[16];
        for (u16 c : s) {
            if (c == 0) out += "\\0";
            else if (c >= 0x20 && c < 0x7f && c != '|') out += static_cast<char>(c);
            else {
                std::snprintf(buf, sizeof buf, wide ? "\\u%04x" : "\\x%02x", c);
                out += buf;
            }
        }
        return out;
    }
    std::string in(const char* key, const std::optional<std::vector<u16>>& s, bool wide) {
        return std::string(" ") + key + "=" + (s ? "|" + esc(*s, wide) + "|" : std::string("NULL"));
    }
    /// The probe's out=: every unit up to the last one that is not '#'.
    std::string out_dump(GuestAddr at, bool wide) {
        usize n = kCap;
        while (n > 0 && unit(at, n - 1, wide) == '#') --n;
        std::vector<u16> v;
        for (usize i = 0; i < n; ++i) v.push_back(unit(at, i, wide));
        return " out=|" + esc(v, wide) + "|";
    }
    static std::string offset(u64 ret, u64 base, bool wide) {
        if (ret == 0) return " ret=NULL";
        return " ret=+" + std::to_string((ret - base) / (wide ? 2 : 1));
    }
    std::string err() { return " err=" + std::to_string(get_last_error()); }

    /// Replays one probe line and renders it the way the probe printed it;
    /// an empty result means the line is not replayed (see below).
    std::string replay(const std::string& text) {
        const Line l = parse(text);
        const bool w = l.wide;
        const std::string head = l.tag + (w ? " W" : " A");
        auto s = [&](const char* k) { return l.strs.at(k); };
        auto num = [&](const char* k) { return std::stoull(l.nums.at(k)); };
        set_last_error(0xDEAD);
        if (l.tag == "SPB") {
            const u64 a = put(kArgA, s("str"), w), b = put(kArgB, s("set"), w);
            const u64 r = call(w ? "StrPBrkW" : "StrPBrkA", {a, b});
            return head + in("str", s("str"), w) + in("set", s("set"), w) + offset(r, a, w) + err();
        }
        if (l.tag == "PGDN") {
            const u64 a = put(kArgA, s("src"), w);
            const i32 r = static_cast<i32>(call(w ? "PathGetDriveNumberW" : "PathGetDriveNumberA", {a}));
            return head + in("src", s("src"), w) + " ret=" + std::to_string(r) + err();
        }
        if (l.tag == "PFFN") {
            const u64 a = put(kArgA, s("src"), w);
            const u64 r = call(w ? "PathFindFileNameW" : "PathFindFileNameA", {a});
            return head + in("src", s("src"), w) + offset(r, a, w);
        }
        if (l.tag == "PSP") {
            fill_out(w);
            const u64 a = put(kOut, s("src"), w);
            (void)call(w ? "PathStripPathW" : "PathStripPathA", {a});
            return head + in("src", s("src"), w) + err() + (a ? out_dump(kOut, w) : "");
        }
        if (l.tag == "PCPE") {
            fill_out(w);
            const bool out_null = num("outnull") != 0;
            const u64 a = put(kArgA, s("src"), w);
            const u64 r = call(w ? "PathCompactPathExW" : "PathCompactPathExA",
                               {out_null ? 0 : kOut, a, num("max"), num("flags")});
            return head + in("src", s("src"), w) + " max=" + l.nums.at("max") + " flags=" + l.nums.at("flags") +
                   " outnull=" + l.nums.at("outnull") + " ret=" + std::to_string(r) + err() +
                   (out_null ? "" : out_dump(kOut, w));
        }
        if (l.tag == "PRPT") {
            // Windows reads past a terminator for these three (its prefix is
            // longer than from or to); the probe's A and W, x86 and x64
            // answers disagree. Vitreo's terminator-bounded answer is checked
            // in RelativePathPastTerminatorIsBounded instead.
            const auto from = s("from"), to = s("to");
            const std::vector<u16> c{'c', ':'}, a1{'\\', 'a'}, a2{'\\', 'a', '\\', 'b'};
            if ((from == c) || (from == a1 && to == a2) || (from == a2 && to == a1)) return {};
            fill_out(w);
            const bool out_null = num("outnull") != 0;
            const u64 f = put(kArgA, from, w), t = put(kArgB, to, w);
            const u64 r = call(w ? "PathRelativePathToW" : "PathRelativePathToA",
                               {out_null ? 0 : kOut, f, num("af"), t, num("at")});
            return head + in("from", from, w) + " af=" + l.nums.at("af") + in("to", to, w) + " at=" +
                   l.nums.at("at") + " outnull=" + l.nums.at("outnull") + " ret=" + std::to_string(r) + err() +
                   (out_null ? "" : out_dump(kOut, w));
        }
        // PRFS / PCP lines record PathRemoveFileSpec and PathCommonPrefix,
        // the steps PathRelativePathTo is built from; they are evidence for
        // its model, not exports under test here.
        return {};
    }

    std::vector<std::string> lines() const {
        const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                          "oracle_data" /
                          (x64() ? "shlwapi_path_extras_probe.expected" : "shlwapi_path_extras_probe.x86.expected");
        std::ifstream f(path);
        EXPECT_TRUE(f.good()) << path;
        std::vector<std::string> out;
        for (std::string line; std::getline(f, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            out.push_back(line);
        }
        return out;
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    DllExports exports;
};

TEST_P(ShlwapiPathExtrasTest, EveryProbeLineMatchesWindows) {
    const auto all = lines();
    ASSERT_FALSE(all.empty());
    EXPECT_EQ(all[0], x64() ? "bits=64" : "bits=32");
    usize replayed = 0;
    for (usize i = 1; i < all.size(); ++i) {
        const std::string got = replay(all[i]);
        if (got.empty()) continue;
        ++replayed;
        EXPECT_EQ(got, all[i]) << "line " << i + 1;
    }
    EXPECT_GT(replayed, 900u);
}

TEST_P(ShlwapiPathExtrasTest, RelativePathPastTerminatorIsBounded) {
    for (const bool w : {true, false}) {
        fill_out(w);
        const u64 f = put(kArgA, std::vector<u16>{'c', ':'}, w);
        const u64 t = put(kArgB, std::vector<u16>{'c', ':', '\\', 'x'}, w);
        EXPECT_EQ(call(w ? "PathRelativePathToW" : "PathRelativePathToA", {kOut, f, 0x10, t, 0x80}), 1u);
        EXPECT_EQ(out_dump(kOut, w), " out=|.\\x\\0|");
    }
}

INSTANTIATE_TEST_SUITE_P(Bitness, ShlwapiPathExtrasTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

} // namespace
