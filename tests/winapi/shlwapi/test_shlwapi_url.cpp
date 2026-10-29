/// @file test_shlwapi_url.cpp
/// @brief shlwapi's URL family (A and W, x86 and x64) replayed against
///        tools/oracle_probe/shlwapi_url_probe.c's Windows output.
///
/// Every probe line carries its own inputs, so the test parses each line of
/// tests/oracle_data/shlwapi_url_probe{,.x86}.expected, calls the export with
/// the same arguments in the matching bitness (same buffer sizes, NULL
/// arguments and '#'-filled buffers), renders the result exactly as the probe
/// prints it -- return value, count, last error and buffer -- and compares
/// the whole line. The registry gets the URL\\Prefixes and DefaultPrefix
/// values winoracle has, which UrlApplyScheme reads.
///
/// Not replayed (each named where it is skipped): UrlFixupW lines whose
/// scheme is not a known one (Windows corrects misspellings; Vitreo reports
/// UNIMPLEMENTED), UrlHash/HashData calls that hash (the hash function is
/// not modelled), and one x86 line where Windows reads stale memory.

#include <gtest/gtest.h>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/advapi32/registry/registry.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/memory/global_local.hpp>
#include <winapi/kernel32/memory/heap.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <winapi/system/shlwapi/shlwapi.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kArgA = 0x7000000;
constexpr GuestAddr kArgB = 0x7004000;
constexpr GuestAddr kOut = 0x7008000;
constexpr GuestAddr kCount = 0x7010000;
constexpr GuestAddr kStruct = 0x7010100;
constexpr GuestAddr kStack = 0x701F000;
constexpr u32 kCap = 4200;

struct Line {
    std::string tag;
    bool wide = false;
    std::map<std::string, std::optional<std::vector<u16>>> strs;
    std::map<std::string, std::string> nums;
};

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
        if (eq == std::string::npos) break;
        const std::string key = text.substr(p, eq - p);
        const usize v = eq + 1;
        if (v < text.size() && text[v] == '|') {
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

std::string esc(const std::vector<u16>& s, bool wide) {
    std::string out;
    char buf[16];
    for (const u16 c : s) {
        if (c == 0) out += "\\0";
        else if (c >= 0x20 && c < 0x7f && c != '|' && c != '\\') out += static_cast<char>(c);
        else {
            std::snprintf(buf, sizeof buf, wide ? "\\u%04x" : "\\x%02x", c);
            out += buf;
        }
    }
    return out;
}

std::string hex32(u64 v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08x", static_cast<u32>(v));
    return b;
}

/// The schemes shlwapi.h's URL_SCHEME_* names (UrlFixupW keeps these).
bool known_scheme(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* kNames[] = {"ftp", "http", "gopher", "mailto", "news", "nntp", "telnet", "wais",
                                   "file", "mk", "https", "shell", "snews", "local", "javascript",
                                   "vbscript", "about", "res", "ms-shell-rooted", "ms-shell-idlist", "hcp",
                                   "search-ms", "search", "knownfolder"};
    for (const char* n : kNames)
        if (s == n) return true;
    return false;
}

class ShlwapiUrlTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        reg_thunks = std::make_unique<RegistryThunks>(registry, handles);
        seed_registry();
        exports = create_shlwapi_thunks(nullptr, nullptr, reg_thunks.get(), &local_mgr);
        ASSERT_TRUE(memory.map_region(kArgA, 0x20000, MemoryProtection::ReadWrite).is_ok());
    }
    bool x64() const { return GetParam(); }

    /// winoracle's HKLM\\...\\URL values (UrlApplyScheme's data).
    void seed_registry() {
        auto set_sz = [&](const std::wstring& path, const std::wstring& name, const std::wstring& value) {
            u64 key = 0;
            u32 disposition = 0;
            ASSERT_EQ(reg_thunks->reg_create_key_ex_w(capsule::HKEY_LOCAL_MACHINE, path, &key, &disposition), 0u);
            std::vector<u8> data;
            for (const wchar_t c : value) { data.push_back(static_cast<u8>(c)); data.push_back(static_cast<u8>(c >> 8)); }
            data.push_back(0); data.push_back(0);
            ASSERT_EQ(reg_thunks->reg_set_value_ex_w(key, name, 1 /*REG_SZ*/, data.data(), static_cast<u32>(data.size())), 0u);
            (void)reg_thunks->reg_close_key(key);
        };
        const std::wstring url = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\URL";
        set_sz(url + L"\\DefaultPrefix", L"", L"http://");
        set_sz(url + L"\\Prefixes", L"ftp", L"ftp://");
        set_sz(url + L"\\Prefixes", L"home", L"http://");
        set_sz(url + L"\\Prefixes", L"mosaic", L"http://");
        set_sz(url + L"\\Prefixes", L"www", L"http://");
    }

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

    u64 put(GuestAddr at, const std::optional<std::vector<u16>>& s, bool wide) {
        if (!s) return 0;
        for (usize i = 0; i <= s->size(); ++i) {
            const u16 c = i < s->size() ? (*s)[i] : 0;
            EXPECT_TRUE((wide ? memory.write_u16(at + i * 2, c) : memory.write_u8(at + i, static_cast<u8>(c))).is_ok());
        }
        return at;
    }
    void fill(GuestAddr at, bool wide) {
        for (u32 i = 0; i < kCap; ++i)
            EXPECT_TRUE((wide ? memory.write_u16(at + i * 2, '#') : memory.write_u8(at + i, '#')).is_ok());
    }
    u16 unit(GuestAddr at, usize i, bool wide) {
        return wide ? memory.read_u16(at + i * 2).value_or(0xFFFF) : memory.read_u8(at + i).value_or(0xFF);
    }
    std::string dump(GuestAddr at, bool wide) {
        usize n = kCap;
        while (n > 0 && unit(at, n - 1, wide) == '#') --n;
        std::vector<u16> v;
        for (usize i = 0; i < n; ++i) v.push_back(unit(at, i, wide));
        return " out=|" + esc(v, wide) + "|";
    }
    std::string in(const char* key, const std::optional<std::vector<u16>>& s, bool wide) {
        return std::string(" ") + key + "=" + (s ? "|" + esc(*s, wide) + "|" : std::string("NULL"));
    }
    std::string err() { return " err=" + std::to_string(get_last_error()); }
    std::string offset(u64 r, u64 base, bool wide) {
        if (r == 0) return " ret=NULL";
        return " ret=+" + std::to_string((r - base) / (wide ? 2 : 1));
    }

    /// The count / buffer arguments of a probe mode (0: cch = 4200,
    /// 1: NULL buffer, 2: NULL count, 3: in place, n >= 10: cch = n - 10).
    struct Mode { u64 out; u64 pcch; };
    Mode mode(int mode_value, bool wide) {
        fill(kOut, wide);
        const u32 cch = mode_value >= 10 ? static_cast<u32>(mode_value - 10) : kCap;
        EXPECT_TRUE(memory.write_u32(kCount, cch).is_ok());
        return {mode_value == 1 ? 0 : kOut, mode_value == 2 ? 0 : kCount};
    }
    std::string tail(u64 r, bool wide) {
        return " ret=" + hex32(r) + " n=" + std::to_string(memory.read_u32(kCount).value_or(0)) + err() +
               dump(kOut, wide);
    }

    std::string replay(const std::string& text) {
        const Line l = parse(text);
        const bool w = l.wide;
        const std::string head = l.tag + (w ? " W" : " A");
        auto s = [&](const char* k) { return l.strs.at(k); };
        auto num = [&](const char* k) { return std::stoull(l.nums.at(k), nullptr, 0); };
        const std::string sfx = w ? "W" : "A";
        set_last_error(0xDEAD);

        if (l.tag == "CANON" || l.tag == "ESC" || l.tag == "U2P" || l.tag == "P2U" || l.tag == "APPLY") {
            if (!x64() && l.tag == "CANON" && !w && l.nums.at("flags") == "0x40000000" &&
                s("in") == unescape("/abs/path/../x"))
                return {};  // x86 UrlCanonicalizeA reads a stale byte here ("//abs..."); x64 does not.
            if (l.tag == "CANON" && w && l.nums.at("flags") == "0x0" && l.nums.at("mode") == "0" &&
                s("in") == unescape("http://user:pass@host:8080/path?q=1#frag"))
                return {};  // Windows' first port parse in the process clears the last error (one-time
                            // initialisation; the same URL under every other flag keeps it).
            const int md = static_cast<int>(num("mode"));
            const Mode mo = mode(md, w);
            const u64 a = put(kArgA, s("in"), w);
            const std::string fn = l.tag == "CANON" ? "UrlCanonicalize" : l.tag == "ESC" ? "UrlEscape"
                                 : l.tag == "U2P" ? "UrlCreateFromPath" : l.tag == "P2U" ? "PathCreateFromUrl"
                                                  : "UrlApplyScheme";
            set_last_error(0xDEAD);
            const u64 r = call(fn + sfx, {a, mo.out, mo.pcch, num("flags")});
            return head + in("in", s("in"), w) + " flags=" + l.nums.at("flags") + " mode=" + l.nums.at("mode") +
                   tail(r, w);
        }
        if (l.tag == "UNESC") {
            const int md = static_cast<int>(num("mode"));
            const Mode mo = mode(md, w);
            fill(kArgA, w);
            const u64 a = put(kArgA, s("in"), w);
            const u32 flags = static_cast<u32>(num("flags"));
            set_last_error(0xDEAD);
            const u64 r = md == 3 ? call("UrlUnescape" + sfx, {a, 0, 0, flags | 0x00100000})
                                  : call("UrlUnescape" + sfx, {a, mo.out, mo.pcch, flags});
            std::string out = head + in("in", s("in"), w) + " flags=" + l.nums.at("flags") + " mode=" +
                              l.nums.at("mode") + " ret=" + hex32(r) + " n=" +
                              std::to_string(memory.read_u32(kCount).value_or(0)) + err();
            return out + dump(md == 3 ? kArgA : kOut, w);
        }
        if (l.tag == "COMB") {
            const int md = static_cast<int>(num("mode"));
            const Mode mo = mode(md, w);
            const u64 a = put(kArgA, s("base"), w), b = put(kArgB, s("rel"), w);
            set_last_error(0xDEAD);
            const u64 r = call("UrlCombine" + sfx, {a, b, mo.out, mo.pcch, num("flags")});
            return head + in("base", s("base"), w) + in("rel", s("rel"), w) + " flags=" + l.nums.at("flags") +
                   " mode=" + l.nums.at("mode") + tail(r, w);
        }
        if (l.tag == "PART") {
            const int md = static_cast<int>(num("mode"));
            const Mode mo = mode(md, w);
            const u64 a = put(kArgA, s("in"), w);
            set_last_error(0xDEAD);
            const u64 r = call("UrlGetPart" + sfx, {a, mo.out, mo.pcch, num("part"), num("flags")});
            return head + in("in", s("in"), w) + " part=" + l.nums.at("part") + " flags=" + l.nums.at("flags") +
                   " mode=" + l.nums.at("mode") + tail(r, w);
        }
        if (l.tag == "IS") {
            const u64 a = put(kArgA, s("in"), w);
            const i32 r = static_cast<i32>(call("UrlIs" + sfx, {a, num("k")}));
            if (!s("in")) return head + " in=NULL k=" + l.nums.at("k") + " ret=" + std::to_string(r);
            return head + in("in", s("in"), w) + " k=" + l.nums.at("k") + " ret=" + std::to_string(r) + err();
        }
        if (l.tag == "OPQ" || l.tag == "NOH" || l.tag == "PISU") {
            const u64 a = put(kArgA, s("in"), w);
            const std::string fn = l.tag == "OPQ" ? "UrlIsOpaque" : l.tag == "NOH" ? "UrlIsNoHistory" : "PathIsURL";
            const i32 r = static_cast<i32>(call(fn + sfx, {a}));
            return head + in("in", s("in"), w) + " ret=" + std::to_string(r);
        }
        if (l.tag == "LOC") {
            const u64 a = put(kArgA, s("in"), w);
            return head + in("in", s("in"), w) + offset(call("UrlGetLocation" + sfx, {a}), a, w);
        }
        if (l.tag == "PARSE") {
            const u64 a = put(kArgA, s("in"), w);
            const u32 cb = static_cast<u32>(num("cb"));
            for (u32 i = 0; i < 48; ++i) EXPECT_TRUE(memory.write_u8(kStruct + i, 0xAB).is_ok());
            EXPECT_TRUE(memory.write_u32(kStruct, cb).is_ok());
            const u64 r = call("ParseURL" + sfx, {a, kStruct});
            std::string out = head + in("in", s("in"), w) + " cb=" + l.nums.at("cb") + " ret=" + hex32(r);
            if (text.find(" scheme=") == std::string::npos) return out;
            const u32 o_p = x64() ? 8 : 4, o_cp = x64() ? 16 : 8, o_s = x64() ? 24 : 12, o_cs = x64() ? 32 : 16,
                      o_sc = x64() ? 36 : 20;
            auto rp = [&](u32 off) { return x64() ? memory.read_u64(kStruct + off).value_or(0) : memory.read_u32(kStruct + off).value_or(0); };
            const u32 scheme = memory.read_u32(kStruct + o_sc).value_or(0);
            if (r == 0) {
                out += " proto=+" + std::to_string((rp(o_p) - a) / (w ? 2 : 1)) + " cchp=" +
                       std::to_string(memory.read_u32(kStruct + o_cp).value_or(0)) + " suf=+" +
                       std::to_string((rp(o_s) - a) / (w ? 2 : 1)) + " cchs=" +
                       std::to_string(memory.read_u32(kStruct + o_cs).value_or(0));
            }
            return out + " scheme=" + std::to_string(scheme);
        }
        if (l.tag == "CMP") {
            const u64 a = put(kArgA, s("a"), w), b = put(kArgB, s("b"), w);
            const i32 r = static_cast<i32>(call("UrlCompare" + sfx, {a, b, num("ign")}));
            return head + in("a", s("a"), w) + in("b", s("b"), w) + " ign=" + l.nums.at("ign") +
                   " ret=" + std::to_string(r < 0 ? -1 : r > 0 ? 1 : 0);
        }
        if (l.tag == "FIX") {
            const auto i = s("in");
            std::string scheme;
            for (const u16 c : *i) {
                if (c == ':' || c == ';') break;
                scheme.push_back(static_cast<char>(c));
            }
            if (!known_scheme(scheme)) return {};  // misspelled-scheme correction: UNIMPLEMENTED
            fill(kOut, true);
            const u64 a = put(kArgA, i, true);
            set_last_error(0xDEAD);
            const u64 r = call("UrlFixupW", {a, kOut, num("cch")});
            return head + in("in", i, true) + " cch=" + l.nums.at("cch") + " ret=" + hex32(r) + err() + dump(kOut, true);
        }
        if (l.tag == "PCFUA") {
            if (l.nums.count("outnull")) return {};
            const u64 a = put(kArgA, s("in"), true);
            EXPECT_TRUE(memory.write_u64(kStruct, 0x1234).is_ok());
            set_last_error(0xDEAD);
            const u64 r = call("PathCreateFromUrlAlloc", {a, kStruct, num("flags")});
            std::string out = head + in("in", s("in"), true) + " flags=" + l.nums.at("flags") + " ret=" + hex32(r) + err();
            const u64 p = x64() ? memory.read_u64(kStruct).value_or(0) : memory.read_u32(kStruct).value_or(0);
            if (r == 0) {
                std::vector<u16> v;
                for (usize k = 0; k < 4096; ++k) {
                    const u16 c = memory.read_u16(p + k * 2).value_or(0);
                    if (c == 0) break;
                    v.push_back(c);
                }
                return out + " out=|" + esc(v, true) + "|";
            }
            return out + " p=" + (p == 0 ? "NULL" : p == 0x1234 ? "UNCHANGED" : "OTHER");
        }
        if (l.tag == "HASH") {
            if (num("cb") != 0) return {};  // the hash function: UNIMPLEMENTED
            const u64 a = put(kArgA, s("in"), w);
            for (u32 i = 0; i < 300; ++i) EXPECT_TRUE(memory.write_u8(kOut + i, 0xEE).is_ok());
            const u64 r = call("UrlHash" + sfx, {a, kOut, 0});
            const bool touched = memory.read_u8(kOut).value_or(0) != 0xEE || memory.read_u8(kOut + 1).value_or(0) != 0xEE;
            return head + in("in", s("in"), w) + " cb=0 ret=" + hex32(r) + " touched=" + std::to_string(touched);
        }
        return {};
    }

    /// Lines without the "TAG W|A" layout.
    std::string replay_special(const std::string& text) {
        if (text.rfind("HD ", 0) == 0) {
            EXPECT_TRUE(memory.write_u32(kArgA, 0x04030201).is_ok());
            if (text.rfind("HD nulld", 0) == 0) return "HD nulld ret=" + hex32(call("HashData", {0, 4, kOut, 8}));
            if (text.rfind("HD nullh", 0) == 0) return "HD nullh ret=" + hex32(call("HashData", {kArgA, 4, 0, 8}));
            if (text.rfind("HD hash0", 0) == 0) return "HD hash0 ret=" + hex32(call("HashData", {kArgA, 4, kOut, 0}));
            return {};  // hashing: UNIMPLEMENTED
        }
        if (text.rfind("GAL ", 0) == 0) {
            const bool w = text[4] == 'W';
            const std::string sfx = w ? "W" : "A";
            if (text.find("outnull") != std::string::npos) {
                EXPECT_TRUE(memory.write_u32(kCount, 100).is_ok());
                const u64 r = call("GetAcceptLanguages" + sfx, {0, kCount});
                return "GAL " + sfx + " outnull ret=" + hex32(r) + " n=" + std::to_string(memory.read_u32(kCount).value_or(0));
            }
            if (text.find("pcchnull") != std::string::npos)
                return "GAL " + sfx + " pcchnull ret=" + hex32(call("GetAcceptLanguages" + sfx, {kOut, 0}));
            const Line l = parse(text);
            const u32 cch = static_cast<u32>(std::stoul(l.nums.at("cch")));
            fill(kOut, w);
            EXPECT_TRUE(memory.write_u32(kCount, cch).is_ok());
            set_last_error(0xDEAD);
            const u64 r = call("GetAcceptLanguages" + sfx, {kOut, kCount});
            return "GAL " + sfx + " cch=" + std::to_string(cch) + " ret=" + hex32(r) + " n=" +
                   std::to_string(memory.read_u32(kCount).value_or(0)) + err() + dump(kOut, w);
        }
        return {};
    }

    std::vector<std::string> lines() const {
        const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "oracle_data" /
                          (x64() ? "shlwapi_url_probe.expected" : "shlwapi_url_probe.x86.expected");
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
    HandleTable handles;
    VirtualMemoryManager vmm{memory};
    HeapManager heap_mgr{vmm, memory, handles};
    GlobalLocalManager local_mgr{heap_mgr};
    capsule::Registry registry;
    std::unique_ptr<RegistryThunks> reg_thunks;
    DllExports exports;
};

TEST_P(ShlwapiUrlTest, EveryProbeLineMatchesWindows) {
    const auto all = lines();
    ASSERT_FALSE(all.empty());
    EXPECT_EQ(all[0], x64() ? "bits=64 acp=1252" : "bits=32 acp=1252");
    std::map<std::string, std::pair<usize, usize>> per_tag;
    usize replayed = 0, failed = 0;
    for (usize i = 1; i < all.size(); ++i) {
        const std::string& line = all[i];
        if (line.rfind("locale=", 0) == 0) continue;
        std::string got;
        if (line.rfind("HD ", 0) == 0 || line.rfind("GAL ", 0) == 0) got = replay_special(line);
        else got = replay(line);
        if (got.empty()) continue;
        ++replayed;
        auto& t = per_tag[line.substr(0, line.find(' '))];
        ++t.second;
        if (got == line) { ++t.first; continue; }
        if (++failed <= 40) ADD_FAILURE() << "line " << i + 1 << "\n want: " << line << "\n got:  " << got;
    }
    for (const auto& [tag, c] : per_tag) EXPECT_EQ(c.first, c.second) << tag;
    EXPECT_EQ(failed, 0u);
    EXPECT_GT(replayed, 30000u);
}

INSTANTIATE_TEST_SUITE_P(Bitness, ShlwapiUrlTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

} // namespace
