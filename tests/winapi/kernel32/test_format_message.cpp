/// @file test_format_message.cpp
/// @brief kernel32 FormatMessageW / FormatMessageA against Windows.
///
/// Every expected line comes from tools/oracle_probe/format_message_probe.c
/// run on Windows 11 (tests/oracle_data/format_message_probe.expected for
/// x64, .x86.expected for x86). The cases below replay the probe's calls
/// with the same inputs and print the result in the probe's own format, so a
/// test compares one recorded line byte for byte. The message tables are the
/// ones tools/oracle_probe/format_message_probe_mt.py compiled into the probe.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/text/format_message.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <cstdio>
#include <deque>
#include <tuple>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
namespace fm = vitreo::winapi::format_message;

namespace {

constexpr u32 STR = fm::kFromString;
constexpr u32 IGN = fm::kIgnoreInserts;
constexpr u32 MOD = fm::kFromHmodule;
constexpr u32 SYS = fm::kFromSystem;
constexpr u32 ARR = fm::kArgumentArray;

// ---------------------------------------------------------------------------
// The probe's message tables (format_message_probe_mt.py).
// ---------------------------------------------------------------------------

struct Msg { u16 flags; std::u16string text; };
struct Block { u32 low; std::vector<Msg> msgs; };

std::vector<u8> encode(const Msg& msg) {
    std::vector<u8> body;
    if (msg.flags == 1) {
        for (char16_t c : msg.text) { body.push_back(static_cast<u8>(c)); body.push_back(static_cast<u8>(c >> 8)); }
        body.push_back(0); body.push_back(0);
    } else if (msg.flags == 2) {
        for (char16_t c : msg.text) {
            if (c < 0x80) body.push_back(static_cast<u8>(c));
            else if (c < 0x800) { body.push_back(static_cast<u8>(0xC0 | (c >> 6))); body.push_back(static_cast<u8>(0x80 | (c & 0x3F))); }
            else { body.push_back(static_cast<u8>(0xE0 | (c >> 12))); body.push_back(static_cast<u8>(0x80 | ((c >> 6) & 0x3F))); body.push_back(static_cast<u8>(0x80 | (c & 0x3F))); }
        }
        body.push_back(0);
    } else {
        for (char16_t c : msg.text) body.push_back(static_cast<u8>(c));  // latin-1
        body.push_back(0);
    }
    while ((4 + body.size()) % 4) body.push_back(0);
    const u16 len = static_cast<u16>(4 + body.size());
    std::vector<u8> out{static_cast<u8>(len), static_cast<u8>(len >> 8),
                        static_cast<u8>(msg.flags), static_cast<u8>(msg.flags >> 8)};
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

void put32(std::vector<u8>& v, u32 x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>(x >> (8 * i)));
}

std::vector<u8> table(const std::vector<Block>& blocks) {
    const u32 head = 4 + 12 * static_cast<u32>(blocks.size());
    std::vector<u8> out, entries;
    put32(out, static_cast<u32>(blocks.size()));
    for (const auto& b : blocks) {
        put32(out, b.low);
        put32(out, b.low + static_cast<u32>(b.msgs.size()) - 1);
        put32(out, head + static_cast<u32>(entries.size()));
        for (const auto& msg : b.msgs) {
            const auto e = encode(msg);
            entries.insert(entries.end(), e.begin(), e.end());
        }
    }
    out.insert(out.end(), entries.begin(), entries.end());
    return out;
}

const std::map<u16, std::vector<u8>>& probe_tables() {
    static const std::map<u16, std::vector<u8>> tables = {
        {0x0409, table({
            {1, {{1, u"One %1!s!\r\n"}, {1, u"Two\r\n"}, {1, u"Ж wide-only é\r\n"}}},
            {6, {{1, u"NoNewline"}, {1, u"Line1\r\nLine2 is longer\r\n"}}},
            {9, {{2, u"Utf8 é Ж\r\n"}}},
            {0x10, {{0, u"Ansi é\r\n"}, {0, u"ansi %1 %2!u!\r\n"}}},
            {0x20000001, {{1, u"High id\r\n"}}},
        })},
        {0x0407, table({{1, {{1, u"Eins %1!s!\r\n"}}}, {4, {{1, u"Vier\r\n"}}}})},
        {0x040C, table({{5, {{1, u"Cinq\r\n"}}}})},
    };
    return tables;
}

constexpr GuestAddr kNoMessageTableModule = 0x20000000;  // the probe's fmnomsg.dll

// ---------------------------------------------------------------------------
// Probe-format reporting
// ---------------------------------------------------------------------------

std::string esc_w(const std::vector<u16>& s) {
    std::string o;
    char buf[16];
    for (u16 c : s) {
        if (c == '\r') o += "\\r";
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c == '\\') o += "\\\\";
        else if (c == '\'') o += "\\'";
        else if (c >= 0x20 && c < 0x7F) o.push_back(static_cast<char>(c));
        else { std::snprintf(buf, sizeof buf, "\\u%04X", c); o += buf; }
    }
    return o;
}

std::string esc_a(const std::vector<u8>& s) {
    std::string o;
    char buf[16];
    for (u8 c : s) {
        if (c == '\r') o += "\\r";
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c == '\\') o += "\\\\";
        else if (c == '\'') o += "\\'";
        else if (c >= 0x20 && c < 0x7F) o.push_back(static_cast<char>(c));
        else { std::snprintf(buf, sizeof buf, "\\x%02X", c); o += buf; }
    }
    return o;
}

// ---------------------------------------------------------------------------
// Arguments as the probe passed them
// ---------------------------------------------------------------------------

struct Arg {
    enum Kind { Int, Int64, Wide, Narrow, Addr } kind;
    u64 value = 0;
    std::u16string wide;
    std::string narrow;
};
Arg I(i64 v) { return Arg{Arg::Int, static_cast<u64>(v), {}, {}}; }      // int / DWORD / DWORD_PTR
Arg L(u64 v) { return Arg{Arg::Int64, v, {}, {}}; }                     // unsigned long long
Arg W(std::u16string s) { return Arg{Arg::Wide, 0, std::move(s), {}}; } // LPCWSTR
Arg N(std::string s) { return Arg{Arg::Narrow, 0, {}, std::move(s)}; }  // LPCSTR
Arg P(GuestAddr a) { return Arg{Arg::Addr, a, {}, {}}; }

enum class Mode { None, VaList, Array };

constexpr usize BUFN = 4096;

class FormatMessageOracle : public ::testing::TestWithParam<bool> {
protected:
    static constexpr GuestAddr kStack = 0x80000;
    static constexpr GuestAddr kData = 0x01000000;
    static constexpr u64 kDataSize = 0x01000000;
    static constexpr GuestAddr kHeap = 0x03000000;
    static constexpr u64 kHeapSize = 0x01000000;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kStack, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kData, kDataSize, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kHeap, kHeapSize, MemoryProtection::ReadWrite).is_ok());
        is64 = GetParam();
        if (is64) cpu.init_long_mode(); else cpu.init_protected_mode();
        std::ifstream in(std::string(VITREO_ORACLE_PROBE_DIR) + "/format_message_probe" +
                         (is64 ? ".expected" : ".x86.expected"));
        ASSERT_TRUE(in.good());
        for (std::string line; std::getline(in, line);) {
            const auto at = line.find(" ret=");
            if (at == std::string::npos) continue;
            expected.emplace(line.substr(0, at), line);
        }
        env.message_table_languages = [](GuestAddr h, std::vector<u16>& out) {
            if (h != 0 && h != kMainImage) return false;
            for (const auto& [lang, data] : probe_tables()) out.push_back(lang);
            return true;
        };
        env.message_table_data = [](GuestAddr, u16 lang, std::vector<u8>& out) {
            auto it = probe_tables().find(lang);
            if (it == probe_tables().end()) return false;
            out = it->second;
            return true;
        };
        env.system_message = [](u32 id) { return lookup_error_message(id); };
        env.local_alloc = [this](u64 bytes) -> GuestAddr {
            heap_top = (heap_top + 15) & ~static_cast<GuestAddr>(15);
            if (heap_top + bytes > kHeap + kHeapSize) heap_top = kHeap;
            const GuestAddr p = heap_top;
            heap_top += bytes;
            sizes[p] = bytes;
            return p;
        };
    }

    // -- guest data ---------------------------------------------------------
    GuestAddr place(const void* p, usize n) {
        data_top = (data_top + 15) & ~static_cast<GuestAddr>(15);
        if (data_top + n + 16 > kData + kDataSize) data_top = kData;
        const GuestAddr a = data_top;
        if (n) EXPECT_TRUE(memory.write_bytes(a, static_cast<const Byte*>(p), n).is_ok());
        data_top += n;
        return a;
    }
    GuestAddr wstr(const std::u16string& s) {
        std::vector<u8> b;
        for (char16_t c : s) { b.push_back(static_cast<u8>(c)); b.push_back(static_cast<u8>(c >> 8)); }
        b.push_back(0); b.push_back(0);
        return place(b.data(), b.size());
    }
    GuestAddr astr(const std::string& s) { return place(s.c_str(), s.size() + 1); }
    u32 slot() const { return is64 ? 8 : 4; }
    u64 ptr_value(const Arg& a) {
        switch (a.kind) {
            case Arg::Wide: return wstr(a.wide);
            case Arg::Narrow: return astr(a.narrow);
            case Arg::Int: return is64 ? a.value : (a.value & 0xFFFFFFFFull);
            default: return a.value;
        }
    }
    /// The slots a compiler lays out for these variadic arguments.
    GuestAddr slots(const std::vector<Arg>& args, bool va) {
        std::vector<u8> bytes;
        auto push = [&](u64 v) {
            for (u32 i = 0; i < slot(); ++i) bytes.push_back(static_cast<u8>(v >> (8 * i)));
        };
        for (const auto& a : args) {
            if (a.kind == Arg::Int64 && va && !is64) {
                push(a.value & 0xFFFFFFFFull);
                push(a.value >> 32);
            } else {
                push(ptr_value(a));
            }
        }
        for (int i = 0; i < 4; ++i) push(0);
        return place(bytes.data(), bytes.size());
    }
    GuestAddr arguments(Mode mode, const std::vector<Arg>& args) {
        if (mode == Mode::None) return 0;
        const GuestAddr s = slots(args, mode == Mode::VaList);
        if (mode == Mode::Array) return s;
        const u64 v = s;
        return place(&v, slot());  // va_list* -> va_list -> slots
    }

    // -- the call -----------------------------------------------------------
    u32 invoke(bool wide, u32 flags, u64 src, u32 id, u32 lang, GuestAddr buf, u32 n,
               GuestAddr args) {
        const u64 a[] = {flags, src, id, lang, buf, n, args};
        cpu.set_sp(kStack + 0xE000);
        const GuestAddr sp = static_cast<GuestAddr>(cpu.sp());
        if (is64) {
            cpu.gpr.rcx = a[0]; cpu.gpr.rdx = a[1]; cpu.gpr.r8 = a[2]; cpu.gpr.r9 = a[3];
            for (int i = 4; i < 7; ++i) (void)memory.write_u64(sp + 0x28 + (i - 4) * 8, a[i]);
        } else {
            for (int i = 0; i < 7; ++i) (void)memory.write_u32(sp + 4 + i * 4, static_cast<u32>(a[i]));
        }
        cpu.pending_raise.active = false;
        Marshaler m(memory);
        m.attach_cpu(cpu);
        cpu.gpr.rax = 0xDEADDEADDEADDEADull;
        set_last_error(0xDEAD);
        fm::format_message(m, env, wide);
        return static_cast<u32>(cpu.gpr.rax);
    }

    u64 source(bool wide, u32 flags, const std::u16string* s) {
        if (!(flags & STR) || !s) return 0;
        if (wide) return wstr(*s);
        std::string n;
        for (char16_t c : *s) n.push_back(static_cast<char>(c));
        return astr(n);
    }

    /// The probe's call_w / call_a line.
    std::string fixed(bool wide, const std::string& label, u32 flags, const std::u16string* src,
                      u64 module, u32 id, u32 lang, u32 n, Mode mode = Mode::None,
                      const std::vector<Arg>& args = {}) {
        const u64 s = (flags & STR) ? source(wide, flags, src) : module;
        const GuestAddr ar = arguments(mode, args);
        const usize bytes = wide ? BUFN * 2 : BUFN;
        std::vector<u8> fill(bytes, 0x5A);
        const GuestAddr buf = place(fill.data(), fill.size());
        const u32 r = invoke(wide, flags, s, id, lang, buf, n, ar);
        const u32 e = get_last_error();
        char head[256];
        std::snprintf(head, sizeof head, "%s %s ret=%u err=%u", wide ? "W" : "A", label.c_str(), r, e);
        std::string line = head;
        if (wide) {
            std::vector<u16> out(BUFN);
            for (usize i = 0; i < BUFN; ++i) out[i] = memory.read_u16(buf + i * 2).value_or(0);
            usize touched = 0, last = 0;
            for (usize i = 0; i < BUFN; ++i) if (out[i] != 0x5A5A) { ++touched; last = i + 1; }
            if (r) {
                line += " out='" + esc_w(std::vector<u16>(out.begin(), out.begin() + std::min<usize>(r, BUFN))) + "'";
                line += std::string(" nul=") + (r < BUFN && out[r] == 0 ? "1" : "0");
            }
            line += " touched=" + std::to_string(touched) + " span=" + std::to_string(last);
            if (!r && touched) {
                line += " buf='" + esc_w(std::vector<u16>(out.begin(), out.begin() + std::min<usize>(last, 64))) + "'";
            }
        } else {
            std::vector<u8> out(BUFN);
            for (usize i = 0; i < BUFN; ++i) out[i] = memory.read_u8(buf + i).value_or(0);
            usize touched = 0, last = 0;
            for (usize i = 0; i < BUFN; ++i) if (out[i] != 0x5A) { ++touched; last = i + 1; }
            if (r) {
                line += " out='" + esc_a(std::vector<u8>(out.begin(), out.begin() + std::min<usize>(r, BUFN))) + "'";
                line += std::string(" nul=") + (r < BUFN && out[r] == 0 ? "1" : "0");
            }
            line += " touched=" + std::to_string(touched) + " span=" + std::to_string(last);
            if (!r && touched) {
                line += " buf='" + esc_a(std::vector<u8>(out.begin(), out.begin() + std::min<usize>(last, 64))) + "'";
            }
        }
        return line;
    }

    /// The probe's alloc_w / alloc_a line. LocalSize is left out: Windows
    /// reports heap-state-dependent sizes (ls probe: "a%1b" -> 17 WCHARs for
    /// 9 + NUL), so only the documented minimum is checked, separately.
    std::string allocated(bool wide, const std::string& label, u32 flags, const std::u16string* src,
                          u32 id, u32 n, Mode mode, const std::vector<Arg>& args, bool show,
                          bool with_localsize = false) {
        const u64 s = source(wide, flags, src);
        const GuestAddr ar = arguments(mode, args);
        const u64 sentinel = 0x1234;
        const GuestAddr slot_addr = place(&sentinel, 8);
        const u32 r = invoke(wide, flags | fm::kAllocateBuffer, s, id, 0, slot_addr, n, ar);
        const u32 e = get_last_error();
        const u64 p = is64 ? memory.read_u64(slot_addr).value_or(0) : memory.read_u32(slot_addr).value_or(0);
        char head[256];
        std::snprintf(head, sizeof head, "%s alloc %s ret=%u err=%u ptr=%s", wide ? "W" : "A",
                      label.c_str(), r, e, p == sentinel ? "unchanged" : p ? "set" : "null");
        std::string line = head;
        if (p && p != sentinel) {
            if (with_localsize) line += " localsize=" + std::to_string(sizes[static_cast<GuestAddr>(p)]);
            std::vector<u16> w;
            std::vector<u8> a;
            for (u32 i = 0; i <= r; ++i) {
                if (wide) w.push_back(memory.read_u16(p + i * 2).value_or(0xFFFF));
                else a.push_back(memory.read_u8(p + i).value_or(0xFF));
            }
            const bool nul = wide ? w[r] == 0 : a[r] == 0;
            if (wide) w.pop_back(); else a.pop_back();
            if (show) {
                line += " out='" + (wide ? esc_w(w) : esc_a(a)) + "' nul=" + (nul ? "1" : "0");
            } else {
                usize spaces = 0;
                if (wide) { for (u16 c : w) spaces += c == ' '; } else { for (u8 c : a) spaces += c == ' '; }
                line += " spaces=" + std::to_string(spaces) + " nul=" + (nul ? "1" : "0");
            }
        }
        return line;
    }

    std::string want(const std::string& key, bool strip_localsize = true) {
        auto it = expected.find(key);
        if (it == expected.end()) {
            ADD_FAILURE() << "no recording for '" << key << "'";
            return {};
        }
        std::string line = it->second;
        if (strip_localsize) {
            const auto at = line.find(" localsize=");
            if (at != std::string::npos) line.erase(at, line.find(' ', at + 1) - at);
        }
        return line;
    }

    // Shorthands mirroring the probe's helpers.
    void aw(const std::string& label, u32 width, const std::u16string& fmt, const std::vector<Arg>& a) {
        EXPECT_EQ(fixed(true, label, STR | ARR | width, &fmt, 0, 0, 0, 512, Mode::Array, a), want("W " + label));
    }
    void aa(const std::string& label, u32 width, const std::u16string& fmt, const std::vector<Arg>& a) {
        EXPECT_EQ(fixed(false, label, STR | ARR | width, &fmt, 0, 0, 0, 512, Mode::Array, a), want("A " + label));
    }
    void vw(const std::string& label, const std::u16string& fmt, const std::vector<Arg>& a) {
        EXPECT_EQ(fixed(true, label, STR, &fmt, 0, 0, 0, 512, Mode::VaList, a), want("W " + label));
    }
    void va(const std::string& label, const std::u16string& fmt, const std::vector<Arg>& a) {
        EXPECT_EQ(fixed(false, label, STR, &fmt, 0, 0, 0, 512, Mode::VaList, a), want("A " + label));
    }
    void cw(const std::string& label, u32 flags, const std::u16string& src, u32 n = 512) {
        EXPECT_EQ(fixed(true, label, flags, &src, 0, 0, 0, n), want("W " + label));
    }
    void ca(const std::string& label, u32 flags, const std::u16string& src, u32 n = 512) {
        EXPECT_EQ(fixed(false, label, flags, &src, 0, 0, 0, n), want("A " + label));
    }
    void mw(const std::string& label, u32 flags, u64 module, u32 id, u32 lang,
            Mode mode = Mode::None, const std::vector<Arg>& a = {}, u32 n = 512) {
        EXPECT_EQ(fixed(true, label, flags, nullptr, module, id, lang, n, mode, a), want("W " + label));
    }
    void ma(const std::string& label, u32 flags, u64 module, u32 id, u32 lang,
            Mode mode = Mode::None, const std::vector<Arg>& a = {}, u32 n = 512) {
        EXPECT_EQ(fixed(false, label, flags, nullptr, module, id, lang, n, mode, a), want("A " + label));
    }

    static constexpr GuestAddr kMainImage = 0x00400000;

    /// A source string that outlives the call.
    const std::u16string* S(std::u16string s) {
        kept.push_back(std::move(s));
        return &kept.back();
    }
    std::deque<std::u16string> kept;

    GuestMemory memory;
    cpu::CpuState cpu;
    fm::Environment env;
    std::map<std::string, std::string> expected;
    std::map<GuestAddr, u64> sizes;
    GuestAddr data_top = kData;
    GuestAddr heap_top = kHeap;
    bool is64 = false;
};

const std::vector<Arg> kNone = {I(0)};

// ---------------------------------------------------------------------------

TEST_P(FormatMessageOracle, VaListNumbering) {
    vw("va_seq", u"%1!u! %2!u! %3!u!", {I(101), I(102), I(103)});
    vw("va_rev", u"%2!u! %1!u!", {I(101), I(102), I(103)});
    vw("va_skip", u"%3!u!", {I(101), I(102), I(103)});
    vw("va_skip_then_back", u"%3!u! %1!u! %2!u!", {I(101), I(102), I(103)});
    vw("va_repeat", u"%1!u! %1!u! %2!u!", {I(101), I(102), I(103)});
    vw("va_star1_n2", u"%1!*u!|%2!u!|%3!u!", {I(5), I(101), I(102), I(103), I(104)});
    vw("va_star1_n3", u"%1!*u!|%3!u!", {I(5), I(101), I(102), I(103), I(104)});
    vw("va_star2_n2", u"%1!*.*u!|%2!u!|%3!u!|%4!u!", {I(6), I(4), I(101), I(102), I(103), I(104), I(105)});
    vw("va_star2_n3", u"%1!*.*u!|%3!u!|%4!u!", {I(6), I(4), I(101), I(102), I(103), I(104), I(105)});
    vw("va_star_second", u"%1!u!|%2!*u!|%3!u!|%4!u!", {I(101), I(5), I(102), I(103), I(104), I(105)});
    vw("va_star_rev", u"%2!*u!|%1!u!", {I(101), I(5), I(102), I(103), I(104)});
    vw("va_learn", u"%1!*.*s! %3 %4!*s!", {I(4), I(2), W(u"Bill"), W(u"Bob"), I(6), W(u"Bill")});
    vw("va_i64_n2", u"%1!I64u!|%2!u!|%3!u!", {L(0x0000000500000007ull), I(103), I(104), I(105)});
    vw("va_i64_second", u"%1!u!|%2!I64u!|%3!u!", {I(101), L(0x0000000500000007ull), I(104), I(105)});
    vw("va_i64_rev", u"%2!u!|%1!I64u!", {L(0x0000000500000007ull), I(103), I(104)});
    vw("va_ll", u"%1!llu!|%2!u!", {L(0x0000000500000007ull), I(103), I(104)});
    vw("va_I64x_hex", u"%1!I64X!|%2!u!", {L(0x1122334455667788ull), I(103)});
    vw("va_rep_star", u"%1!*u!|%1!*u!|%2!u!", {I(5), I(101), I(102), I(103), I(104)});
    vw("va_two_stars", u"%1!*u!|%2!*u!|%3!u!", {I(5), I(101), I(6), I(102), I(103), I(104), I(105)});
    vw("va_i64_rep", u"%1!I64u!|%1!I64u!|%2!u!", {L(0x0000000500000007ull), I(103), I(104)});
    vw("va_star_i64", u"%1!*I64u!|%2!u!|%3!u!", {I(25), L(0x0000000500000007ull), I(104), I(105), I(106)});
    vw("va_i64_after_walk", u"%2!I64u!|%1!u!|%3!u!", {I(101), L(0x0000000500000007ull), I(104), I(105)});
    vw("va_star2_rep", u"%1!*.*u!|%1!u!", {I(6), I(4), I(101), I(102)});
    vw("va_star_after_walk", u"%3!u!|%1!*u!|%2!u!|%4!u!", {I(5), I(101), I(102), I(103), I(104), I(105)});
    vw("va_star_then_skip", u"%1!*u!|%4!u!|%2!u!|%3!u!", {I(5), I(101), I(102), I(103), I(104), I(105)});
    vw("va_neg_width", u"[%1!*s!]", {I(-5), W(u"ab")});
    vw("va_I64d_neg", u"%1!I64d!|%2!u!", {L(static_cast<u64>(-2ll)), I(6)});
    va("va_seq", u"%1!u! %2!u! %3!u!", {I(101), I(102), I(103)});
    va("va_star2_n3", u"%1!*.*u!|%3!u!|%4!u!", {I(6), I(4), I(101), I(102), I(103), I(104), I(105)});
    va("va_i64_n2", u"%1!I64u!|%2!u!|%3!u!", {L(0x0000000500000007ull), I(103), I(104), I(105)});
}

TEST_P(FormatMessageOracle, ArgumentArrayNumbering) {
    const std::vector<Arg> a10 = {I(101), I(102), I(103), I(104), I(105), I(106), I(107), I(108), I(109), I(110)};
    aw("arr_seq", 0, u"%1!u! %2!u! %3!u!", a10);
    aw("arr_rev", 0, u"%2!u! %1!u!", a10);
    aw("arr_skip", 0, u"%3!u!", a10);
    const std::vector<Arg> s1 = {I(5), I(101), I(102), I(103), I(104), I(105)};
    aw("arr_star1_n2", 0, u"%1!*u!|%2!u!|%3!u!", s1);
    aw("arr_star1_n3", 0, u"%1!*u!|%3!u!", s1);
    const std::vector<Arg> s2 = {I(6), I(4), I(101), I(102), I(103), I(104), I(105)};
    aw("arr_star2_n2", 0, u"%1!*.*u!|%2!u!|%3!u!|%4!u!", s2);
    aw("arr_star2_n4", 0, u"%1!*.*u!|%4!u!|%5!u!", s2);
    aw("arr_star_second", 0, u"%1!u!|%2!*u!|%3!u!|%4!u!", {I(101), I(5), I(102), I(103), I(104), I(105)});
    aw("arr_learn", 0, u"%1!*.*s! %4 %5!*s!", {I(4), I(2), W(u"Bill"), W(u"Bob"), I(6), W(u"Bill")});
    const std::vector<Arg> i64 = {I(7), I(5), I(103), I(104), I(105)};
    aw("arr_i64", 0, u"%1!I64u!|%2!u!|%3!u!", i64);
    aw("arr_i64_second", 0, u"%2!I64u!|%3!u!", i64);
    aw("arr_ll", 0, u"%1!llu!|%2!u!", i64);
    aw("arr_rep_star", 0, u"%1!*u!|%1!*u!|%2!u!", {I(5), I(101), I(102), I(103), I(104)});
    aw("arr_neg_width", 0, u"[%1!*s!]", {I(-5), W(u"ab")});
    aw("arr_hi_bits_u", 0, u"%1!u!|%1!Iu!|%1!x!|%1!p!", {I(-3), I(6)});
    aa("arr_star1_n3", 0, u"%1!*u!|%3!u!", s1);
    aa("arr_i64", 0, u"%1!I64u!|%2!u!|%3!u!", i64);
}

// Hand-laid va_list and array: slot i holds i + 1 (format_message_probe va_model).
TEST_P(FormatMessageOracle, SlotModel) {
    static const std::pair<const char*, const char16_t*> cases[] = {
        {"plain", u"%1!x!|%2!x!|%3!x!"}, {"I", u"%1!I64x!|%2!x!|%3!x!"},
        {"I2", u"%1!x!|%2!I64x!|%3!x!"}, {"Irev", u"%2!x!|%1!I64x!|%3!x!"},
        {"Iwalk", u"%2!I64x!|%1!x!|%3!x!"}, {"Iwalk3", u"%3!I64x!|%1!x!|%2!x!|%4!x!"},
        {"Irep", u"%1!I64x!|%1!I64x!|%2!x!"}, {"IrepWalk", u"%2!I64x!|%2!I64x!|%3!x!"},
        {"I_after_plain", u"%1!x!|%1!I64x!|%2!x!"}, {"I_I", u"%1!I64x!|%2!I64x!|%3!x!"},
        {"s1", u"%1!*x!|%2!x!|%3!x!"}, {"s2", u"%1!*.*x!|%2!x!|%3!x!|%4!x!"},
        {"s1rep", u"%1!*x!|%1!*x!|%2!x!"}, {"s2rep", u"%1!*.*x!|%1!*.*x!|%2!x!"},
        {"s1walk", u"%2!*x!|%1!x!|%3!x!"}, {"s2walk", u"%2!*.*x!|%1!x!|%3!x!"},
        {"s1walk3", u"%3!*x!|%1!x!|%2!x!|%4!x!"}, {"s1cached", u"%3!x!|%1!*x!|%2!x!|%4!x!"},
        {"s2cached", u"%3!x!|%1!*.*x!|%2!x!|%4!x!"}, {"s1I", u"%1!*I64x!|%2!x!|%3!x!"},
        {"s2I", u"%1!*.*I64x!|%2!x!|%3!x!"}, {"s1Iwalk", u"%2!*I64x!|%1!x!|%3!x!"},
        {"s1Icached", u"%2!x!|%1!*I64x!|%3!x!"}, {"plain_after_s1", u"%1!*x!|%1!x!|%2!x!"},
        {"plain_after_s2", u"%1!*.*x!|%1!x!|%2!x!"}, {"s1_s1", u"%1!*x!|%2!*x!|%3!x!"},
        {"s2_s2", u"%1!*.*x!|%2!*.*x!|%3!x!"}, {"s2_gap", u"%1!*.*x!|%4!x!|%2!x!|%3!x!"},
        {"s1_gap", u"%1!*x!|%3!x!|%2!x!"}, {"star_after_plain", u"%1!x!|%1!*x!|%2!x!"},
        {"star2_after_plain", u"%1!x!|%1!*.*x!|%2!x!"}, {"ll", u"%1!llx!|%2!x!"},
        {"I32", u"%1!I32x!|%2!x!"}, {"Ix", u"%1!Ix!|%2!x!"}, {"skip_far", u"%5!x!|%1!x!|%6!x!"},
        {"prec_only", u"%1!.*x!|%2!x!|%3!x!"}, {"prec_only_cached", u"%2!x!|%1!.*x!|%3!x!"},
    };
    std::vector<Arg> slots16;
    for (int i = 1; i <= 16; ++i) slots16.push_back(I(i));
    for (const auto& [name, fmt] : cases) {
        const std::u16string f = fmt;
        EXPECT_EQ(fixed(true, std::string("vs_") + name, STR, &f, 0, 0, 0, 512, Mode::VaList, slots16),
                  want(std::string("W vs_") + name));
        EXPECT_EQ(fixed(true, std::string("as_") + name, STR | ARR, &f, 0, 0, 0, 512, Mode::Array, slots16),
                  want(std::string("W as_") + name));
    }
}

TEST_P(FormatMessageOracle, PrintfSpecs) {
    const std::string nar = "nar\xe9";
    const std::u16string wid = u"widЖ";
    // UNICODE_STRING / ANSI_STRING {Length, MaximumLength, Buffer}.
    auto counted = [&](u16 len, u16 max, GuestAddr buffer) {
        std::vector<u8> b{static_cast<u8>(len), static_cast<u8>(len >> 8), static_cast<u8>(max), static_cast<u8>(max >> 8)};
        if (is64) b.insert(b.end(), 4, 0);
        for (u32 i = 0; i < slot(); ++i) b.push_back(static_cast<u8>(static_cast<u64>(buffer) >> (8 * i)));
        return place(b.data(), b.size());
    };
    const GuestAddr us = counted(8, 10, wstr(u"cntd"));
    const GuestAddr as = counted(4, 5, astr("cnta"));
    auto w1 = [&](const char* name, const char16_t* fmt, Arg v) { aw(name, 0, fmt, {v, I(0), I(0)}); };
    auto a1 = [&](const char* name, const char16_t* fmt, Arg v) { aa(name, 0, fmt, {v, I(0), I(0)}); };
    w1("s_default", u"[%1]", W(u"abc"));
    w1("s_s", u"[%1!s!]", W(u"abc"));
    w1("s_5s", u"[%1!5s!]", W(u"abc"));
    w1("s_m5s", u"[%1!-5s!]", W(u"abc"));
    w1("s_p2s", u"[%1!.2s!]", W(u"abc"));
    w1("s_05s", u"[%1!05s!]", W(u"abc"));
    w1("s_S", u"[%1!S!]", N(nar));
    w1("s_hs", u"[%1!hs!]", N(nar));
    w1("s_ls", u"[%1!ls!]", W(wid));
    w1("s_ws", u"[%1!ws!]", W(wid));
    w1("s_wide", u"[%1]", W(wid));
    w1("s_Z_unicode", u"[%1!wZ!]", P(us));
    w1("s_Z", u"[%1!Z!]", P(as));
    w1("c_c", u"[%1!c!]", I(0x416));
    w1("c_C", u"[%1!C!]", I('q'));
    w1("c_hc", u"[%1!hc!]", I('r'));
    w1("c_lc", u"[%1!lc!]", I(0x416));
    w1("c_3c", u"[%1!3c!]", I('x'));
    w1("d_neg", u"[%1!d!]", I(-5));
    w1("i_neg", u"[%1!i!]", I(-5));
    w1("u_max", u"[%1!u!]", I(0xFFFFFFFF));
    w1("d_max", u"[%1!d!]", I(0xFFFFFFFF));
    w1("x", u"[%1!x!]", I(0xBEEF));
    w1("X", u"[%1!X!]", I(0xBEEF));
    w1("o", u"[%1!o!]", I(8));
    w1("hash_x", u"[%1!#x!]", I(0xBEEF));
    w1("hash_o", u"[%1!#o!]", I(8));
    w1("08x", u"[%1!08x!]", I(0xBEEF));
    w1("m8d", u"[%1!-8d!]", I(42));
    w1("plus_d", u"[%1!+d!]", I(42));
    w1("space_d", u"[%1! d!]", I(42));
    w1("p5d", u"[%1!.5d!]", I(42));
    w1("lu", u"[%1!lu!]", I(0xFFFFFFFF));
    w1("lx", u"[%1!lx!]", I(0xBEEF));
    w1("hu", u"[%1!hu!]", I(0x12345));
    w1("hd", u"[%1!hd!]", I(0x18000));
    w1("hx", u"[%1!hx!]", I(0x12345));
    w1("Ix", u"[%1!Ix!]", I(0x12345678));
    w1("I32x", u"[%1!I32x!]", I(0xBEEF));
    w1("zu", u"[%1!zu!]", I(77));
    w1("p", u"[%1!p!]", I(0x1234));
    for (const char* t : {"e", "f", "g", "E", "a"}) {
        const std::string fs = std::string("[%1!") + t + "!]";
        w1(t, std::u16string(fs.begin(), fs.end()).c_str(), I(0x40000000));
    }
    w1("unknown_q", u"[%1!q!]", I(7));
    w1("unknown_y", u"[%1!5y!]", I(7));
    w1("no_type", u"[%1!5!]", W(u"abc"));
    w1("empty_spec", u"[%1!!]", W(u"abc"));
    w1("pct_spec", u"[%1!%!]", W(u"abc"));
    w1("unterminated", u"[%1!d]", I(42));
    w1("unterminated_end", u"[%1!d", I(42));
    w1("bang_after", u"[%1%!]", W(u"abc"));
    aw("two_digit", 0, u"[%10]", std::vector<Arg>(12, I(0)));
    a1("s_default", u"[%1]", N("abc"));
    a1("s_S", u"[%1!S!]", W(wid));
    a1("s_ls", u"[%1!ls!]", W(wid));
    a1("s_hs", u"[%1!hs!]", N(nar));
    a1("s_ansi", u"[%1]", N(nar));
    a1("c_c", u"[%1!c!]", I('x'));
    a1("c_C", u"[%1!C!]", I(0x416));
    a1("d_neg", u"[%1!d!]", I(-5));
    a1("08x", u"[%1!08x!]", I(0xBEEF));
    a1("e", u"[%1!e!]", I(0x40000000));
    a1("unknown_q", u"[%1!q!]", I(7));
    a1("no_type", u"[%1!5!]", N("abc"));

    static const std::pair<const char*, const char16_t*> pf[] = {
        {"jd", u"[%1!jd!]"}, {"td", u"[%1!td!]"}, {"Ld", u"[%1!Ld!]"}, {"hhd", u"[%1!hhd!]"},
        {"hhx", u"[%1!hhx!]"}, {"hash_X", u"[%1!#X!]"}, {"hash_p", u"[%1!#p!]"},
        {"m08d", u"[%1!-08d!]"}, {"plus_u", u"[%1!+u!]"}, {"space_x", u"[%1! x!]"},
        {"G", u"[%1!G!]"}, {"A", u"[%1!A!]"}, {"F", u"[%1!F!]"}, {"w5e", u"[%1!5e!]"},
        {"I16d", u"[%1!I16d!]"}, {"Ilu", u"[%1!Ilu!]"}, {"h_only", u"[%1!h!]"},
        {"l_only", u"[%1!l!]"}, {"I_only", u"[%1!I!]"}, {"I64_only", u"[%1!I64!]"},
        {"flags_only", u"[%1!-+ #0!]"}, {"width_only_star", u"[%1!*!]"}, {"dot_only", u"[%1!.!]"},
        {"pct_in_spec", u"[%1!5%!]"}, {"two_types", u"[%1!dx!]"}, {"u_then_text", u"[%1!u text!]"},
        {"upper_D", u"[%1!D!]"}, {"upper_U", u"[%1!U!]"}, {"upper_O", u"[%1!O!]"},
    };
    const std::map<std::string, i64> value = {
        {"hhd", 0x1FF}, {"hhx", 0x1FF}, {"hash_X", 0xBEEF}, {"hash_p", 0x1234}, {"m08d", 42},
        {"plus_u", 42}, {"space_x", 42}, {"width_only_star", 3}};
    for (const auto& [name, fmt] : pf) {
        auto it = value.find(name);
        aw(std::string("pf_") + name, 0, fmt, {I(it == value.end() ? 5 : it->second), I(2), I(3), I(4)});
    }
    auto pfv = [&](const char* name, const char16_t* fmt, Arg v) { aw(std::string("pf_") + name, 0, fmt, {v, I(2), I(3), I(4)}); };
    pfv("wc", u"[%1!wc!]", I(0x416));
    pfv("p0d_0", u"[%1!.0d!]", I(0));
    pfv("plus_p0d_0", u"[%1!+.0d!]", I(0));
    pfv("hash_p0o_0", u"[%1!#.0o!]", I(0));
    pfv("hash_x_0", u"[%1!#x!]", I(0));
    pfv("m3c", u"[%1!-3c!]", I('x'));
    pfv("05c", u"[%1!05c!]", I('x'));
    pfv("p1c", u"[%1!.1c!]", I('x'));
    pfv("05d_neg", u"[%1!05d!]", I(-5));
    pfv("plus05d", u"[%1!+05d!]", I(5));
    pfv("p3s_null", u"[%1!.3s!]", I(0));
    pfv("Z_null", u"[%1!Z!]", I(0));
    pfv("wZ_null", u"[%1!wZ!]", I(0));
    pfv("c_nul", u"[%1!c!]x", I(0));
    pfv("lld_neg", u"[%1!lld!]", I(-1));
    pfv("I64d_neg", u"[%1!I64d!]", I(-2));
    pfv("Id_neg", u"[%1!Id!]", I(-2));
    pfv("Iu_neg", u"[%1!Iu!]", I(-2));
    pfv("u_neg", u"[%1!u!]", I(-2));
    pfv("p600d", u"[%1!.40d!]", I(7));
    pfv("w40s", u"[%1!40s!]", W(u"s"));
    pfv("lS", u"[%1!lS!]", W(u"wide"));
    pfv("hS", u"[%1!hS!]", N("nar"));
    pfv("hC", u"[%1!hC!]", I('n'));
    pfv("lC", u"[%1!lC!]", I(0x416));
    pfv("ws_lower", u"[%1!ws!]", W(u"w"));
    pfv("I32d_neg", u"[%1!I32d!]", I(-3));
    pfv("dot_d", u"[%1!.d!]", I(0));
    pfv("s_then_space", u"[%1!s !]", W(u"q"));
    pfv("i_hex", u"[%1!i!]", I(0x10));
    pfv("big_w", u"[%1!300d!]", I(1));
    aa("pf_a_s_hs_ls", 0, u"[%1|%1!hs!|%2!ls!|%2!ws!|%2!S!|%3!c!|%4!C!|%4!lc!]",
       {N(nar), W(u"wЖ"), I('q'), I(0x416)});
    aa("pf_a_I64d_neg", 0, u"[%1!I64d!|%1!Iu!]", {I(-2), I(0)});
}

TEST_P(FormatMessageOracle, InsertNumbersAndInsertText) {
    std::vector<Arg> ten;
    for (int i = 1; i <= 12; ++i) ten.push_back(I(i));
    aw("n10", 0, u"[%10!u!]", ten);
    aw("n09", 0, u"[%09!u!]", ten);
    aw("n_digit_then_alpha", 0, u"[%1!u!x%2!u!y]", ten);
    std::vector<Arg> many;
    for (int i = 1; i <= 1000; ++i) many.push_back(I(i));
    aw("n99", 0, u"[%99!u!]", many);
    for (const auto& [name, fmt] : std::vector<std::pair<std::string, std::u16string>>{
             {"n123", u"[%123!u!]"}, {"n100u", u"[%100!u!]"}, {"n999", u"[%999!u!]"},
             {"n1000", u"[%1000!u!]"}, {"n_10_u_0", u"[%10!u!0]"}, {"n_1_u", u"[%1!u!]"},
             {"n_99_then_digit", u"[%99!u!5]"}, {"n200", u"[%200!u!]"}, {"n201", u"[%201!u!]"}}) {
        aw(name, 0, fmt, many);
    }
    cw("n100_ign", STR | IGN, u"[%100 %123!u!]");
    cw("n200_ign", STR | IGN, u"[%200]");
    cw("n201_ign", STR | IGN, u"[%201]");
    cw("ign_n100", STR | IGN, u"[%1000]");
    aw("width300", 0, u"[%1!*s!]", {I(300), W(u"z")});
    const std::vector<Arg> br = {W(u"x\r\ny"), W(u"p\nq")};
    aw("insert_crlf", 0, u"[%1|%2]", br);
    aw("insert_crlf_ff", 0xFF, u"[%1|%2]", br);
    aw("insert_crlf_w4", 4, u"[%1|%2]", br);
    aw("insert_not_rescanned", 0, u"[%1]", {W(u"%2 %%"), W(u"no")});
}

TEST_P(FormatMessageOracle, Escapes) {
    static const std::pair<const char*, const char16_t*> src[] = {
        {"pct", u"a%%b"}, {"space", u"a% b"}, {"dot", u"a%.b"}, {"bang", u"a%!b"}, {"n", u"a%nb"},
        {"r", u"a%rb"}, {"t", u"a%tb"}, {"zero_mid", u"a%0b"}, {"other_x", u"a%xb"},
        {"trailing_pct", u"a%"}, {"pctpct_end", u"a%%"}, {"zero_only", u"%0"}, {"zero_end", u"abc%0"},
        {"n_zero", u"a%nb%0"}, {"R", u"a%Rb"}, {"N", u"a%Nb"}, {"T", u"a%Tb"}, {"backslash", u"a%\\b"},
        {"pct_crlf", u"a%\r\nb"}, {"pct_lf", u"a%\nb"}, {"letters", u"%b%c%d%e%f"},
        {"nonascii", u"aé%Ж"}, {"empty", u""}, {"dot_only", u"%."}, {"dot_lines", u"x%.\r\n%.y"},
    };
    for (const auto& [name, s] : src) {
        const std::string b = std::string("esc_") + name;
        aw(b, 0, s, kNone);
        cw(b + "_ign", STR | IGN, s);
        aw(b + "_ff", 0xFF, s, kNone);
        cw(b + "_ign_ff", STR | IGN | 0xFF, s);
    }
    for (int c = 0x21; c < 0x7F; ++c) {
        if (c >= '0' && c <= '9') continue;
        const std::u16string s{u'a', u'%', static_cast<char16_t>(c), u'b'};
        char label[32];
        std::snprintf(label, sizeof label, "chr_%02x", c);
        aw(label, 0, s, kNone);
        cw(std::string(label) + "_ign", STR | IGN, s);
    }
    cw("ign_inserts", STR | IGN, u"a%1b%2!d!c%3!*s!d%99");
    cw("ign_inserts_w5", STR | IGN | 5, u"a%1b c%2!d!dd ee");
    ca("ign_inserts", STR | IGN, u"a%1b%2!d!c%n%%d%0e");
    EXPECT_EQ(fixed(false, "esc_pct", STR | ARR, S(u"a%%b%nc%rd%te% f%.g%!h%0i"), 0, 0, 0, 512,
                    Mode::Array, kNone),
              want("A esc_pct"));
    cw("ign_unterminated", STR | IGN, u"[%1!d]");
    cw("ign_unterminated_end", STR | IGN, u"[%1!d");
    cw("ign_b", STR | IGN | 4, u"aa%bbb cc");
    cw("ign_spec_s", STR | IGN, u"a%1!s!b");
    cw("ign_spec_S", STR | IGN, u"a%1!S!b");
    cw("ign_spec_5s", STR | IGN, u"a%1!5s!b");
    cw("ign_spec_empty", STR | IGN, u"a%1!!b");
    cw("ign_spec_s_bang", STR | IGN, u"a%1!s!!b");
    ca("ign_spec_s", STR | IGN, u"a%1!s!b");
    ca("empty", STR | IGN, u"");
    ca("zero_only", STR | IGN, u"%0");
    cw("empty_n0", STR | IGN, u"", 0);
    ca("trailing_pct", STR | IGN, u"ab%");
}

TEST_P(FormatMessageOracle, LineBreaksAndWrapping) {
    static const std::pair<const char*, const char16_t*> lb[] = {
        {"crlf", u"a\r\nb"}, {"lf", u"a\nb"}, {"cr", u"a\rb"}, {"lfcr", u"a\n\rb"},
        {"crlfcrlf", u"a\r\n\r\nb"}, {"trail_crlf", u"abc\r\n"}, {"trail_lf", u"abc\n"},
        {"crcrlf", u"a\r\r\nb"}, {"spaces_around", u"a  \r\n  b"}, {"lead_crlf", u"\r\nabc"},
        {"multi_space", u"a b  c   d"},
    };
    for (const auto& [name, s] : lb) {
        for (u32 w : {0u, 0xFFu, 1u, 3u, 10u}) {
            aw(std::string("lb_") + name + "_w" + std::to_string(w), w, s, kNone);
        }
    }
    static const std::pair<const char*, const char16_t*> wrap[] = {
        {"fox", u"The quick brown fox jumps over the lazy dog"}, {"longword", u"abcdefghijklmnop qr"},
        {"lead_trail", u"  lead and trail  "}, {"hard_n", u"a%nb c d e%nf"}, {"tabs", u"tab\tsep\tword here"},
        {"insert", u"x %1 y"}, {"zero", u"one two%0"}, {"r", u"aa bb%rcc dd"}, {"trail_n", u"aaaa bbbb%n"},
        {"trail_space", u"aaaa bbbb "}, {"fit_exact", u"aaa bbb"}, {"fit_over", u"aaaa bbb"},
        {"word_eq_width", u"aaaaa"}, {"pcts", u"a%%%%%%%% b"}, {"tabsesc", u"a%tb%tc%td"},
        {"pctspaces", u"a% % % % b"},
    };
    for (const auto& [name, s] : wrap) {
        for (u32 w : {1u, 4u, 5u, 10u, 0xFEu}) {
            aw(std::string("wrap_") + name + "_w" + std::to_string(w), w, s, {W(u"in sert")});
        }
    }
    aa("wrap_fox_w10", 10, u"The quick brown fox jumps over the lazy dog", {N("in sert")});
    aa("lb_lf_w0", 0, u"a\nb", {N("in sert")});
    aa("lb_cr_wff", 0xFF, u"a\rb\r\n", {N("in sert")});
    for (const auto& [name, w, s] : std::vector<std::tuple<std::string, u32, std::u16string>>{
             {"wrap_zero_mid_w4", 4, u"aaa bbb%0ccc"}, {"wrap_long_trail_w4", 4, u"aaaaaaaaa"},
             {"wrap_long_space_w4", 4, u"aaaaaaaaa b"}, {"wrap_two_spaces_w4", 4, u"aa  bb  cc"},
             {"wrap_space_run_w4", 4, u"a      b"}, {"wrap_lf_inside_w4", 4, u"aa\r\nbb cc\r\ndd"},
             {"wrap_exact_then_space_w3", 3, u"abc def"}, {"wrap_exact_then_2space_w3", 3, u"abc  def"},
             {"wrap_r_mid_w4", 4, u"aaaa%rbbbbb"}, {"wrap_t_w3", 3, u"ab%tcd%tef"},
             {"wrap_n_at_w_w3", 3, u"abc%ndef"}, {"wrap_spc_esc_w3", 3, u"ab% % cd"},
             {"b_wrap_w4", 4, u"aa%bbb cc"}, {"b_wrap_w3", 3, u"ab%bcd"}, {"hard_space_w3", 3, u"ab% cd"},
             {"trim_before_n_w10", 10, u"a  %nb"}, {"trim_before_n_w0", 0, u"a  %nb"},
             {"trim_before_r_w10", 10, u"a  %rb"}, {"trim_hard_then_space_w3", 3, u"a% %  b"},
             {"trim_space_then_hard_w3", 3, u"a % b"}, {"tab_then_wrap_w20", 20, u"abc%tdef ghi jkl mno"},
             {"insert_after_tab_w20", 20, u"abc%t%1 xyz"}, {"pct_cr_wrap_w3", 3, u"ab%\rcd"},
             {"insert_space_only_w3", 3, u"ab%1cd"}}) {
        aw(name, w, s, kNone);
    }
    aw("wrap_insert_long_w3", 3, u"%1 %2", {W(u"abcdefgh"), W(u"x y")});
    aw("wrap_insert_glued_w5", 5, u"ab%1cd", {W(u"abcdefgh"), W(u"x y")});
    aw("insert_sp_w3", 3, u"ab%1", {W(u"x y")});
    for (u32 w : {10u, 15u, 16u, 17u, 24u, 25u, 40u}) {
        aw("tabs_w" + std::to_string(w), w, u"a%tb%tc%td", kNone);
        aw("tab_col11_w" + std::to_string(w), w, u"abcdefghijk%tz", kNone);
        aw("tab_wrap_w" + std::to_string(w), w, u"abc%tdef ghi jkl mno pqr", kNone);
    }
}

TEST_P(FormatMessageOracle, BufferSizes) {
    for (u32 n : {0u, 1u, 2u, 5u, 6u, 7u}) {
        cw("buf_hello_n" + std::to_string(n), STR | IGN, u"hello", n);
        ca("buf_hello_n" + std::to_string(n), STR | IGN, u"hello", n);
    }
    const std::u16string ins = u"abc%1xyz";
    for (u32 n : {3u, 6u, 8u, 9u, 10u}) {
        const std::string l = "buf_insert_n" + std::to_string(n);
        EXPECT_EQ(fixed(true, l, STR | ARR, &ins, 0, 0, 0, n, Mode::Array, {W(u"WORLD")}), want("W " + l));
        EXPECT_EQ(fixed(false, l, STR | ARR, &ins, 0, 0, 0, n, Mode::Array, {N("WORLD")}), want("A " + l));
    }
    ca("buf_highbytes_n3", STR | IGN, u"éèçæ", 3);
    cw("buf_wrap_n4", STR | IGN | 3, u"ab cd ef", 4);
    cw("buf_crlf_n3", STR | IGN, u"a\r\nb", 3);
    cw("buf_crlf_n4", STR | IGN, u"a\r\nb", 4);
    cw("buf_lf_n3", STR | IGN, u"a\nb", 3);
    ca("buf_n5_hello", STR | IGN, u"hello", 5);
    for (u32 n : {0xFFFFu, 0x10000u, 0x10001u, 0x10003u, 0x10006u, 0x20006u, 0x8000u, 0x7FFFu}) {
        char label[32];
        std::snprintf(label, sizeof label, "nsize_%x", n);
        ca(label, STR | IGN, u"hello", n);
        cw(label, STR | IGN, u"hello", n);
    }
    const std::u16string lit20000(20000, u'a');
    ca("lit20000_n30000", STR | IGN, lit20000, 30000);
    ca("lit20000_n70000", STR | IGN, lit20000, 70000);
}

TEST_P(FormatMessageOracle, AllocateBuffer) {
    const std::u16string hello = u"hello", empty = u"", ins = u"abc%1xyz";
    EXPECT_EQ(allocated(true, "hello_n0", STR | IGN, &hello, 0, 0, Mode::None, {}, true), want("W alloc hello_n0"));
    EXPECT_EQ(allocated(true, "hello_n1", STR | IGN, &hello, 0, 1, Mode::None, {}, true), want("W alloc hello_n1"));
    EXPECT_EQ(allocated(true, "hello_n100", STR | IGN, &hello, 0, 100, Mode::None, {}, true), want("W alloc hello_n100"));
    // Learn: nSize is the minimum number of TCHARs allocated.
    EXPECT_GE(sizes.rbegin()->second, 101u * 2);
    EXPECT_EQ(allocated(true, "hello_n40000", STR | IGN, &hello, 0, 40000, Mode::None, {}, true), want("W alloc hello_n40000"));
    EXPECT_EQ(allocated(true, "empty_n0", STR | IGN, &empty, 0, 0, Mode::None, {}, true), want("W alloc empty_n0"));
    EXPECT_EQ(allocated(true, "missing_sys", SYS | IGN, nullptr, 0x12345, 0, Mode::None, {}, true), want("W alloc missing_sys"));
    EXPECT_EQ(allocated(false, "hello_n0", STR | IGN, &hello, 0, 0, Mode::None, {}, true), want("A alloc hello_n0"));
    EXPECT_EQ(allocated(false, "hello_n100", STR | IGN, &hello, 0, 100, Mode::None, {}, true), want("A alloc hello_n100"));
    EXPECT_EQ(allocated(false, "empty_n0", STR | IGN, &empty, 0, 0, Mode::None, {}, true), want("A alloc empty_n0"));
    EXPECT_EQ(allocated(false, "missing_sys", SYS | IGN, nullptr, 0x12345, 0, Mode::None, {}, true), want("A alloc missing_sys"));
    EXPECT_EQ(allocated(true, "insert", STR | ARR, &ins, 0, 0, Mode::Array, {W(u"WORLD")}, true), want("W alloc insert"));
    const std::u16string sys_src;
    EXPECT_EQ(allocated(true, "sys_ff_n0", SYS | IGN | 0xFF, nullptr, 2, 0, Mode::None, {}, true), want("W alloc sys_ff_n0"));
    const std::u16string wrap = u"aa bb cc";
    EXPECT_EQ(allocated(true, "wrap_n0", STR | IGN | 4, &wrap, 0, 0, Mode::None, {}, true), want("W alloc wrap_n0"));

    // The 64K-WCHAR working units.
    struct Cap { u32 lit, ins, nsize; };
    for (const Cap c : {Cap{16377, 49158, 0}, Cap{16377, 49159, 0}, Cap{16378, 114693, 0}, Cap{16378, 114694, 0},
                        Cap{0, 65535, 0}, Cap{0, 65536, 0}, Cap{1, 65535, 0}, Cap{0, 65536, 65535},
                        Cap{0, 65536, 65536}, Cap{0, 131071, 65536}, Cap{0, 131072, 65536},
                        Cap{0, 131072, 131072}}) {
        const std::u16string src = std::u16string(c.lit, u'a') + u"%1!*s!";
        const std::string label = "cap_lit" + std::to_string(c.lit) + "_ins" + std::to_string(c.ins) +
                                  "_n" + std::to_string(c.nsize);
        // LocalSize of a block within one heap granule of its 64K unit is a
        // heap artefact, not part of the contract: compare without it.
        EXPECT_EQ(allocated(true, label, STR | ARR, &src, 0, c.nsize, Mode::Array, {I(c.ins), W(u"")}, false, false),
                  want("W alloc " + label, true));
    }
    for (u32 n : {32766u, 32767u, 65536u}) {
        const std::string label = "big_insert_" + std::to_string(n);
        const std::u16string f = u"%1!*s!";
        EXPECT_EQ(allocated(true, label, STR | ARR, &f, 0, 0, Mode::Array, {I(n), W(u"")}, false, false),
                  want("W alloc " + label, true));
        EXPECT_EQ(allocated(false, label, STR | ARR, &f, 0, 0, Mode::Array, {I(n), N("")}, false, false),
                  want("A alloc " + label, true));
        const std::u16string lit(n, u'a');
        EXPECT_EQ(allocated(true, "big_literal_" + std::to_string(n), STR | IGN, &lit, 0, 0, Mode::None, {}, false),
                  want("W alloc big_literal_" + std::to_string(n)));
        EXPECT_EQ(allocated(false, "big_literal_" + std::to_string(n), STR | IGN, &lit, 0, 0, Mode::None, {}, false),
                  want("A alloc big_literal_" + std::to_string(n)));
    }
    const std::u16string two = u"%1!*s!%3!*s!";
    EXPECT_EQ(allocated(true, "two_inserts_80000", STR | ARR, &two, 0, 0, Mode::Array,
                        {I(40000), W(u""), I(40000), W(u"")}, false),
              want("W alloc two_inserts_80000"));
    const std::u16string x32766 = u"%1!*s!x";
    EXPECT_EQ(allocated(false, "insert32766_then_x", STR | ARR, &x32766, 0, 0, Mode::Array, {I(32766), N("")}, false),
              want("A alloc insert32766_then_x"));
    const std::u16string lit20000(20000, u'a');
    EXPECT_EQ(allocated(false, "lit20000_n0", STR | IGN, &lit20000, 0, 0, Mode::None, {}, false),
              want("A alloc lit20000_n0"));
}

TEST_P(FormatMessageOracle, FlagsAndSystemTable) {
    cw("flags_none", 0, u"abc");
    cw("flags_ign_only", IGN, u"abc");
    EXPECT_EQ(fixed(true, "flags_str_sys", STR | SYS | IGN, S(u"abc"), 0, 2, 0, 512), want("W flags_str_sys"));
    EXPECT_EQ(fixed(true, "flags_str_mod", STR | MOD | IGN, S(u"abc"), 0, 1, 0, 512), want("W flags_str_mod"));
    cw("flags_str_highbit", STR | IGN | 0x10000, u"abc");
    cw("flags_str_highbit2", STR | IGN | 0x80000000, u"abc");
    EXPECT_EQ(fixed(true, "flags_str_lang", STR | IGN, S(u"abc"), 0, 0x12345, 0x12345, 512),
              want("W flags_str_lang"));
    cw("flags_arr_ign", STR | IGN | ARR, u"%1");
    ca("flags_none", 0, u"abc");
    EXPECT_EQ(fixed(false, "flags_str_sys", STR | SYS | IGN, S(u"abc"), 0, 2, 0, 512), want("A flags_str_sys"));

    mw("sys_2", SYS | IGN, 0, 2, 0);
    mw("sys_2_ins", SYS, 0, 2, 0);
    mw("sys_2_ff", SYS | IGN | 0xFF, 0, 2, 0);
    mw("sys_2_w20", SYS | IGN | 20, 0, 2, 0);
    mw("sys_2_409", SYS | IGN, 0, 2, 0x409);
    mw("sys_2_400", SYS | IGN, 0, 2, 0x400);
    mw("sys_2_800", SYS | IGN, 0, 2, 0x800);
    mw("sys_2_009", SYS | IGN, 0, 2, 0x009);
    mw("sys_2_407", SYS | IGN, 0, 2, 0x407);
    mw("sys_2_411", SYS | IGN, 0, 2, 0x411);
    mw("sys_2_badlang", SYS | IGN, 0, 2, 0x12345);
    mw("sys_0", SYS | IGN, 0, 0, 0);
    mw("sys_missing", SYS | IGN, 0, 0x12345, 0);
    mw("sys_hresult_2", SYS | IGN, 0, 0x80070002, 0);
    mw("sys_2_src_ignored", SYS | IGN, 0x1234, 2, 0);
    ma("sys_2", SYS | IGN, 0, 2, 0);
    ma("sys_2_ff", SYS | IGN | 0xFF, 0, 2, 0);
    ma("sys_missing", SYS | IGN, 0, 0x12345, 0);
}

TEST_P(FormatMessageOracle, ModuleMessageTables) {
    const std::vector<Arg> a = {W(u"X"), I(9)};
    const std::vector<Arg> an = {N("X"), I(9)};
    for (u32 id : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 0x10u, 0x11u, 0x20000001u, 0x20000002u}) {
        for (u32 lang : {0u, 0x409u, 0x407u, 0x40Cu, 0x411u, 0x009u, 0x809u, 0x007u, 0x400u, 0x800u, 0xC07u}) {
            char label[48];
            std::snprintf(label, sizeof label, "mod_%x_l%03x", id, lang);
            mw(label, MOD | ARR, 0, id, lang, Mode::Array, a);
        }
    }
    mw("mod_1_self", MOD | ARR, kMainImage, 1, 0, Mode::Array, a);
    mw("mod_1_ign", MOD | IGN, 0, 1, 0);
    mw("mod_7_ff", MOD | IGN | 0xFF, 0, 7, 0);
    mw("mod_7_w5", MOD | IGN | 5, 0, 7, 0);
    mw("mod_6_ff", MOD | IGN | 0xFF, 0, 6, 0);
    mw("modsys_1", MOD | SYS | ARR, 0, 1, 0, Mode::Array, a);
    mw("modsys_2", MOD | SYS | ARR, 0, 2, 0, Mode::Array, a);
    mw("modsys_5", MOD | SYS | ARR, 0, 5, 0, Mode::Array, a);
    mw("modsys_missing", MOD | SYS | IGN, 0, 0x12345, 0);
    mw("modsys_4_409", MOD | SYS | IGN, 0, 4, 0x409);
    mw("modsys_2_411", MOD | SYS | IGN, 0, 2, 0x411);
    mw("modsys_2_009", MOD | SYS | IGN, 0, 2, 0x009);
    mw("modsys_2_400", MOD | SYS | IGN, 0, 4, 0x400);
    mw("mod_badlang", MOD | IGN, 0, 1, 0x12345);
    ma("mod_1", MOD | ARR, 0, 1, 0, Mode::Array, an);
    ma("mod_3", MOD | IGN, 0, 3, 0);
    ma("mod_10", MOD | IGN, 0, 0x10, 0);
    ma("mod_11", MOD | ARR, 0, 0x11, 0, Mode::Array, an);
    ma("mod_4_l407", MOD | IGN, 0, 4, 0x407);
    ma("mod_8", MOD | IGN, 0, 8, 0);
    ma("mod_1_l411", MOD | IGN, 0, 1, 0x411);
    ma("mod_3_n3", MOD | IGN, 0, 3, 0, Mode::None, {}, 3);
    ma("mod_9", MOD | IGN, 0, 9, 0);
    mw("mod_10", MOD | IGN, 0, 0x10, 0);
    mw("mod_10_n3", MOD | IGN, 0, 0x10, 0, Mode::None, {}, 3);
    mw("mod_9", MOD | IGN, 0, 9, 0);
    EXPECT_EQ(allocated(true, "mod_2", MOD | IGN, nullptr, 2, 0, Mode::None, {}, true), want("W alloc mod_2"));
    EXPECT_EQ(allocated(false, "mod_3", MOD | IGN, nullptr, 3, 0, Mode::None, {}, true), want("A alloc mod_3"));

    // A module without RT_MESSAGETABLE (the probe's empty DLL).
    mw("nomsg_1", MOD | IGN, kNoMessageTableModule, 1, 0);
    mw("nomsg_1_l409", MOD | IGN, kNoMessageTableModule, 1, 0x409);
    mw("nomsg_sys_2", MOD | SYS | IGN, kNoMessageTableModule, 2, 0);
    mw("nomsg_sys_missing", MOD | SYS | IGN, kNoMessageTableModule, 0x12345, 0);
    ma("nomsg_1", MOD | IGN, kNoMessageTableModule, 1, 0);

    // dwLanguageId 0 follows the thread UI language (SetThreadUILanguage),
    // not the thread locale.
    env.thread_ui_language = 0x407;
    mw("lang_ui407_1", MOD | ARR, 0, 1, 0, Mode::Array, a);
    mw("lang_ui407_4", MOD | ARR, 0, 4, 0, Mode::Array, a);
    env.thread_ui_language = 0x409;
    mw("lang_lc407_1", MOD | ARR, 0, 1, 0, Mode::Array, a);
    mw("lang_lc407_4", MOD | ARR, 0, 4, 0, Mode::Array, a);
    env.thread_ui_language = 0x40C;
    mw("lang_ui40c_1", MOD | ARR, 0, 1, 0, Mode::Array, a);
    mw("lang_ui40c_5", MOD | ARR, 0, 5, 0, Mode::Array, a);
}

// The probe ran these in a child process because they may fault.
TEST_P(FormatMessageOracle, NullArgumentsAndBuffers) {
    EXPECT_EQ(fixed(true, "null_args_w", STR, S(u"a%1b"), 0, 0, 0, 512), want("W null_args_w"));
    EXPECT_EQ(fixed(true, "null_args_arr_w", STR | ARR, S(u"a%1b"), 0, 0, 0, 512), want("W null_args_arr_w"));
    EXPECT_EQ(fixed(true, "null_args_noins_w", STR, S(u"a%%b%n"), 0, 0, 0, 512), want("W null_args_noins_w"));
    EXPECT_EQ(fixed(false, "null_args_a", STR, S(u"a%1b"), 0, 0, 0, 512), want("A null_args_a"));
    aw("null_str_arg_w", 0, u"[%1]", {I(0), I(0)});
    aw("null_str_arg_S_w", 0, u"[%1!S!]", {I(0), I(0)});
    aa("null_str_arg_a", 0, u"[%1]", {I(0), I(0)});

    for (const auto& [label, flags, n] : std::vector<std::tuple<std::string, u32, u32>>{
             {"null_buf_w", STR | IGN, 10}, {"null_buf_n0_w", STR | IGN, 0},
             {"null_buf_alloc_w", STR | IGN | fm::kAllocateBuffer, 0}}) {
        const u32 r = invoke(true, flags, wstr(u"abc"), 0, 0, 0, n, 0);
        char line[128];
        std::snprintf(line, sizeof line, "W %s ret=%u err=%u", label.c_str(), r, get_last_error());
        EXPECT_EQ(line, want("W " + label));
    }

    // printf's %n is not a conversion here: it prints 'n' and writes nothing.
    const u32 v = 0x55;
    const GuestAddr pv = place(&v, 4);
    aw("printf_n_w", 0, u"[ab%1!n!]", {P(pv), I(0)});
    EXPECT_EQ(memory.read_u32(pv).value_or(0), 0x55u);

    // Windows faults on a NULL FROM_STRING source and on a bad string insert
    // (child exit 0xC0000005): the access violation is raised in the guest.
    (void)invoke(true, STR | IGN, 0, 0, 0, place(std::vector<u8>(64).data(), 64), 32, 0);
    EXPECT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    const u64 bad = 0x10;
    const GuestAddr arr = place(&bad, slot());
    (void)invoke(true, STR | ARR, wstr(u"[%1]"), 0, 0, place(std::vector<u8>(64).data(), 64), 32, arr);
    EXPECT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.args[1], 0x10u);
}

INSTANTIATE_TEST_SUITE_P(Bitness, FormatMessageOracle, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

// Microsoft Learn COM Error Codes (Security and Setup): CRYPT_E_NO_MATCH comes
// from the system table through ALLOCATE_BUFFER on both bitnesses.
TEST(FormatMessageSystem, CryptoMessageAllocatedBothBitnesses) {
    for (bool is64 : {false, true}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x80000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(0x100000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if (is64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(0x88000);
        const GuestAddr slot = 0x100000;
        const u64 args[] = {fm::kFromSystem | fm::kAllocateBuffer | fm::kIgnoreInserts, 0, 0x80092009, 0, slot, 0, 0};
        const GuestAddr sp = static_cast<GuestAddr>(cpu.sp());
        if (is64) {
            cpu.gpr.rcx = args[0]; cpu.gpr.rdx = args[1]; cpu.gpr.r8 = args[2]; cpu.gpr.r9 = args[3];
            for (int i = 4; i < 7; ++i) ASSERT_TRUE(memory.write_u64(sp + 0x28 + (i - 4) * 8, args[i]).is_ok());
        } else {
            for (int i = 0; i < 7; ++i) ASSERT_TRUE(memory.write_u32(sp + 4 + i * 4, static_cast<u32>(args[i])).is_ok());
        }
        fm::Environment env;
        env.system_message = [](u32 id) { return lookup_error_message(id); };
        env.local_alloc = [](u64) -> GuestAddr { return 0x100100; };
        Marshaler m(memory);
        m.attach_cpu(cpu);
        fm::format_message(m, env, true);
        const std::string expected = "Cannot find the requested object.\r\n";
        ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), expected.size());
        const u64 p = is64 ? memory.read_u64(slot).value_or(0) : memory.read_u32(slot).value_or(0);
        ASSERT_EQ(p, 0x100100u);
        EXPECT_EQ(m.read_wide_string(p).value_or(""), expected);
    }
}

} // namespace
