/// @file test_crt_scanf.cpp
/// @brief The CRT scanf input processor through its exports: the UCRT
///        __stdio_common_vs[w]scanf / vfscanf and msvcrt.dll's sscanf,
///        swscanf, sscanf_s and fscanf, x86 and x64.
///
/// Every expected value is copied from a Windows run,
/// tools/oracle_probe/crt_scanf_probe{64,32}.expected; the row each check
/// comes from is named next to it.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/msvcrt/io/crt_stdio.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kArena = 0x20000000;
constexpr GuestAddr kStack = kArena + 0x40000;
constexpr GuestAddr kData = kArena + 0x50000;
constexpr GuestAddr kOut = kArena + 0x60000;  // six 32-byte result slots
constexpr GuestAddr kVa = kArena + 0x70000;
constexpr u64 kSlot = 32;
constexpr u32 kEof = 0xFFFFFFFFu;

GuestAddr slot(u64 i) { return kOut + i * kSlot; }

struct Rig {
    explicit Rig(bool is64) : x64(is64) {
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(kStack);
        m.attach_cpu(cpu);
        add_crt_scanf_exports(exports, io, mm, crt_data);
        fill();
    }

    void fill() {
        for (u64 i = 0; i < 6 * kSlot; ++i) (void)memory.write_u8(kOut + i, 0xCC);
    }
    GuestAddr str(const std::string& s) {
        const GuestAddr a = next_data;
        for (usize i = 0; i < s.size(); ++i) (void)memory.write_u8(a + i, static_cast<u8>(s[i]));
        (void)memory.write_u8(a + s.size(), 0);
        next_data += (s.size() + 16) & ~usize{7};
        return a;
    }
    GuestAddr wstr(const std::u16string& s) {
        const GuestAddr a = next_data;
        for (usize i = 0; i < s.size(); ++i) (void)memory.write_u16(a + i * 2, s[i]);
        (void)memory.write_u16(a + s.size() * 2, 0);
        next_data += (s.size() * 2 + 16) & ~usize{7};
        return a;
    }
    GuestAddr va(const std::vector<u64>& slots) {
        const usize w = x64 ? 8 : 4;
        for (usize i = 0; i < slots.size(); ++i) {
            if (x64) (void)memory.write_u64(kVa + i * w, slots[i]);
            else (void)memory.write_u32(kVa + i * w, static_cast<u32>(slots[i]));
        }
        return kVa;
    }
    u32 call(const std::string& name, const std::vector<u64>& args) {
        if (x64) {
            u64* regs[4] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
            for (usize i = 0; i < args.size(); ++i) {
                if (i < 4) *regs[i] = args[i];
                else (void)memory.write_u64(kStack + 0x28 + (i - 4) * 8, args[i]);
            }
        } else {
            for (usize i = 0; i < args.size(); ++i) {
                (void)memory.write_u32(kStack + 4 + i * 4, static_cast<u32>(args[i]));
            }
        }
        for (const auto& e : exports.entries) {
            if (e.name == name) {
                e.handler(m);
                return static_cast<u32>(cpu.gpr.rax);
            }
        }
        ADD_FAILURE() << "no export " << name;
        return 0;
    }
    /// __stdio_common_vs[w]scanf(options, buffer, SIZE_MAX, format, NULL, va).
    u32 ucrt(bool wide, u64 options, GuestAddr input, GuestAddr format,
             const std::vector<u64>& va_slots) {
        const u64 size_max = x64 ? ~0ULL : 0xFFFFFFFFULL;
        std::vector<u64> args;
        if (x64) {
            args.push_back(options);
        } else {
            args.push_back(options & 0xFFFFFFFFu);
            args.push_back(options >> 32);
        }
        for (u64 a : {u64{input}, size_max, u64{format}, u64{0}, u64{va(va_slots)}}) {
            args.push_back(a);
        }
        return call(wide ? "__stdio_common_vswscanf" : "__stdio_common_vsscanf", args);
    }
    /// A msvcrt export: fixed arguments, then the variadic destinations.
    u32 legacy(const std::string& name, std::vector<u64> fixed, const std::vector<u64>& rest) {
        CrtDllFlavorScope scope(CrtDllFlavor::Msvcrt);
        fixed.insert(fixed.end(), rest.begin(), rest.end());
        return call(name, fixed);
    }

    /// The bytes a case wrote into slot @p i (up to the first untouched 0xCC).
    std::string written(u64 i) const {
        std::string s;
        for (u64 k = 0; k < kSlot; ++k) s.push_back(static_cast<char>(memory.read_u8(slot(i) + k).value()));
        while (!s.empty() && static_cast<u8>(s.back()) == 0xCC) s.pop_back();
        return s;
    }
    u32 u32_at(u64 i) const { return memory.read_u32(slot(i)).value(); }
    u64 u64_at(u64 i) const { return memory.read_u64(slot(i)).value(); }

    bool x64;
    GuestMemory memory;
    CrtMemoryManager mm{memory, kArena, 0x100000};
    CrtStdioManager io;
    CrtDataBlock crt_data;
    cpu::CpuState cpu;
    Marshaler m{memory};
    DllExports exports;
    GuestAddr next_data{kData};
};

std::string hex(const std::string& bytes) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (char c : bytes) {
        out.push_back(d[static_cast<u8>(c) >> 4]);
        out.push_back(d[static_cast<u8>(c) & 15]);
    }
    return out;
}

} // namespace

// c004: the length modifiers size the stores; hh is a byte in the UCRT and an
// int in msvcrt.dll.
TEST(CrtScanfTest, LengthModifiersSizeTheStores) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        Rig r(x64);
        const GuestAddr in = r.str("70000 300 -5 12345678901 -9");
        const GuestAddr fmt = r.str("%hd %hhd %ld %lld %I64d");
        EXPECT_EQ(r.ucrt(false, 0, in, fmt, {slot(0), slot(1), slot(2), slot(3), slot(4)}), 5u);
        EXPECT_EQ(hex(r.written(0)), "7011");
        EXPECT_EQ(hex(r.written(1)), "2c");
        EXPECT_EQ(hex(r.written(2)), "fbffffff");
        EXPECT_EQ(hex(r.written(3)), "351cdcdf02000000");
        EXPECT_EQ(hex(r.written(4)), "f7ffffffffffffff");
        r.fill();
        EXPECT_EQ(r.legacy("sscanf", {in, fmt}, {slot(0), slot(1), slot(2), slot(3), slot(4)}), 5u);
        EXPECT_EQ(hex(r.written(1)), "2c010000");
        // c027: z and t are pointer-sized, j is 64-bit, in both engines.
        r.fill();
        EXPECT_EQ(r.legacy("sscanf", {r.str("1 2 3"), r.str("%zd %jd %td")},
                           {slot(0), slot(1), slot(2)}),
                  3u);
        EXPECT_EQ(r.written(0).size(), x64 ? 8u : 4u);
        EXPECT_EQ(r.written(1).size(), 8u);
        EXPECT_EQ(r.written(2).size(), x64 ? 8u : 4u);
        // c016: %p is pointer-sized hex.
        r.fill();
        EXPECT_EQ(r.ucrt(false, 0, r.str("0000ABCD"), r.str("%p"), {slot(0)}), 1u);
        EXPECT_EQ(hex(r.written(0)), x64 ? "cdab000000000000" : "cdab0000");
    }
}

// c100..c109: msvcrt.dll's old size counters (h makes a float a double, ll
// does not widen one, L is l) and its smaller conversion set.
TEST(CrtScanfTest, MsvcrtKeepsTheOldModifierCounters) {
    Rig r(true);
    EXPECT_EQ(r.legacy("sscanf", {r.str("1.5"), r.str("%hf")}, {slot(0)}), 1u);
    EXPECT_EQ(hex(r.written(0)), "000000000000f83f");
    r.fill();
    EXPECT_EQ(r.legacy("sscanf", {r.str("1.5"), r.str("%llf")}, {slot(0)}), 1u);
    EXPECT_EQ(hex(r.written(0)), "0000c03f");
    r.fill();
    EXPECT_EQ(r.legacy("sscanf", {r.str("7"), r.str("%Ld")}, {slot(0)}), 1u);
    EXPECT_EQ(hex(r.written(0)), "07000000");
    EXPECT_EQ(r.ucrt(false, 0, r.str("7"), r.str("%Ld"), {slot(1)}), 0u);
    r.fill();
    EXPECT_EQ(r.legacy("sscanf", {r.str("2.5"), r.str("%a")}, {slot(0)}), 0u);
    EXPECT_EQ(r.ucrt(false, 0, r.str("2.5"), r.str("%a"), {slot(0)}), 1u);
    EXPECT_EQ(hex(r.written(0)), "00002040");
    r.fill();
    EXPECT_EQ(r.legacy("sscanf", {r.str("ls"), r.str("%lls")}, {slot(0)}), 1u);
    EXPECT_EQ(hex(r.written(0)), "6c7300");
    r.fill();
    EXPECT_EQ(r.ucrt(false, 0, r.str("ls"), r.str("%lls"), {slot(0)}), 1u);
    EXPECT_EQ(hex(r.written(0)), "6c0073000000");
}

// c019, c046, c049, c052, c071, c073: the UCRT parses hex floats and INF/NAN
// but cannot back up a stream, so "1e" fails; msvcrt.dll collects a decimal
// field and converts whatever it got.
TEST(CrtScanfTest, FloatingFieldsFollowEachEngine) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        Rig r(x64);
        const GuestAddr lf_s = r.str("%lf%s");
        EXPECT_EQ(r.ucrt(false, 0, r.str("1ex"), lf_s, {slot(0), slot(1)}), 0u);
        EXPECT_EQ(r.written(0), "");
        EXPECT_EQ(r.legacy("sscanf", {r.str("1ex"), lf_s}, {slot(0), slot(1)}), 2u);
        EXPECT_EQ(hex(r.written(0)), "000000000000f03f");
        EXPECT_EQ(r.written(1), std::string("x\0", 2));
        r.fill();
        EXPECT_EQ(r.ucrt(false, 0, r.str("0x.8p1z"), lf_s, {slot(0), slot(1)}), 2u);
        EXPECT_EQ(hex(r.written(0)), "000000000000f03f");
        EXPECT_EQ(r.written(1), std::string("z\0", 2));
        r.fill();
        EXPECT_EQ(r.legacy("sscanf", {r.str("0x.8p1z"), lf_s}, {slot(0), slot(1)}), 2u);
        EXPECT_EQ(hex(r.written(0)), "0000000000000000");
        EXPECT_EQ(r.written(1), std::string("x.8p1z\0", 7));
        r.fill();
        EXPECT_EQ(r.ucrt(false, 0, r.str("nan(ab)c"), lf_s, {slot(0), slot(1)}), 2u);
        EXPECT_EQ(hex(r.written(0)), "ffffffffffffff7f");
        EXPECT_EQ(r.ucrt(false, 0, r.str("infx"), lf_s, {slot(0), slot(1)}), 2u);
        EXPECT_EQ(hex(r.written(0)), "000000000000f07f");
        EXPECT_EQ(r.ucrt(false, 0, r.str("infinx"), lf_s, {slot(0), slot(1)}), 0u);
        EXPECT_EQ(r.legacy("sscanf", {r.str("inf"), r.str("%lf")}, {slot(0)}), 0u);
        // c071 / c070: "1e" at the end of input.
        EXPECT_EQ(r.ucrt(false, 0, r.str("1e"), r.str("%lf"), {slot(0)}), kEof);
        EXPECT_EQ(r.legacy("sscanf", {r.str("1.5e+"), r.str("%e%s")}, {slot(2), slot(3)}), 1u);
        EXPECT_EQ(hex(r.written(2)), "0000c03f");
        // c039: the width bounds the field.
        EXPECT_EQ(r.ucrt(false, 0, r.str("123.456"), r.str("%5f"), {slot(4)}), 1u);
        EXPECT_EQ(hex(r.written(4)), "cdccf642");
    }
}

// c075, c076, "ucrt erange" / "msvcrt erange": the UCRT clamps and sets
// ERANGE; msvcrt.dll wraps and leaves errno alone.
TEST(CrtScanfTest, IntegerOverflowClampsOrWraps) {
    Rig r(true);
    r.mm.set_errno(0);
    EXPECT_EQ(r.ucrt(false, 0, r.str("99999999999999999999"), r.str("%d"), {slot(0)}), 1u);
    EXPECT_EQ(r.u32_at(0), 0xFFFFFFFFu);
    EXPECT_EQ(r.mm.get_errno(), 34u);
    r.mm.set_errno(0);
    EXPECT_EQ(r.legacy("sscanf", {r.str("99999999999999999999"), r.str("%d")}, {slot(1)}), 1u);
    EXPECT_EQ(r.u32_at(1), 0x630FFFFFu);
    EXPECT_EQ(r.legacy("sscanf", {r.str("-99999999999999999999"), r.str("%lld")}, {slot(2)}), 1u);
    EXPECT_EQ(r.u64_at(2), 0x9438A1D29CF00001ULL);
    EXPECT_EQ(r.mm.get_errno(), 0u);
    EXPECT_EQ(r.ucrt(false, 0, r.str("-99999999999999999999"), r.str("%lld"), {slot(3)}), 1u);
    EXPECT_EQ(r.u64_at(3), 0x8000000000000000ULL);
}

// c012, c037, c043, c057: running out of input before the first conversion is
// EOF -- except in msvcrt.dll's swscanf, which returns 0 -- and a short %c
// field fails in the UCRT but not in msvcrt.dll.
TEST(CrtScanfTest, EndOfInputRules) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        Rig r(x64);
        EXPECT_EQ(r.ucrt(false, 0, r.str(""), r.str("%d"), {slot(0)}), kEof);
        EXPECT_EQ(r.legacy("sscanf", {r.str("   "), r.str("%d")}, {slot(0)}), kEof);
        EXPECT_EQ(r.ucrt(true, 2, r.wstr(u"0x"), r.wstr(u"%x"), {slot(0)}), kEof);
        EXPECT_EQ(r.legacy("swscanf", {r.wstr(u"0x"), r.wstr(u"%x")}, {slot(0)}), 0u);
        EXPECT_EQ(r.legacy("swscanf", {r.wstr(u""), r.wstr(u"%d")}, {slot(0)}), 0u);
        EXPECT_EQ(r.legacy("sscanf", {r.str("ab"), r.str("abc%d")}, {slot(0)}), kEof);
        EXPECT_EQ(r.ucrt(false, 0, r.str("ab"), r.str("%4c"), {slot(0)}), kEof);
        EXPECT_EQ(r.written(0), "ab");
        EXPECT_EQ(r.legacy("sscanf", {r.str("ab"), r.str("%4c")}, {slot(1)}), 1u);
        EXPECT_EQ(r.written(1), "ab");
        // c060, c093: %n is not a receiving argument.
        EXPECT_EQ(r.ucrt(false, 0, r.str("abc"), r.str("%*s%n"), {slot(2)}), 0u);
        EXPECT_EQ(r.u32_at(2), 3u);
        // c044, c045: a bad conversion ends the walk.
        EXPECT_EQ(r.ucrt(false, 0, r.str("1"), r.str("%y"), {slot(3)}), 0u);
        EXPECT_EQ(r.ucrt(false, 0, r.str("3"), r.str("%d%"), {slot(3)}), 1u);
    }
}

// "sscanf_s small s", "sscanf_s c small", "sscanf_s zero", "ucrt_s c small",
// "ucrt_s zero", "ucrt_s bad fmt", "sscanf_s bad fmt": secure buffers keep
// what fit, then reset [0] with ENOMEM.
TEST(CrtScanfTest, SecureBuffersResetOnOverflow) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        Rig r(x64);
        u32 terminated = 0;
        r.mm.set_process_terminator([&](u32 status) { terminated = status; });
        r.mm.set_errno(0);
        EXPECT_EQ(r.legacy("sscanf_s", {r.str("hello"), r.str("%s")}, {slot(0), 4}), 0u);
        EXPECT_EQ(r.written(0), std::string("\0el", 3));
        EXPECT_EQ(r.mm.get_errno(), 12u);
        EXPECT_EQ(r.legacy("sscanf_s", {r.str("xy"), r.str("%2c")}, {slot(1), 1}), 0u);
        EXPECT_EQ(r.written(1), std::string("\0", 1));
        EXPECT_EQ(r.ucrt(false, 1, r.str("xy"), r.str("%2c"), {slot(2), 1}), kEof);
        EXPECT_EQ(r.legacy("sscanf_s", {r.str("ab"), r.str("%s")}, {slot(3), 0}), 0u);
        EXPECT_EQ(r.written(3), std::string("\0", 1));
        EXPECT_EQ(r.ucrt(false, 1, r.str("ab"), r.str("%s"), {slot(4), 0}), 0u);
        EXPECT_EQ(r.written(4), "");
        EXPECT_EQ(r.ucrt(false, 5, r.str("ab"), r.str("%s"), {slot(4), 0}), 0u);
        EXPECT_EQ(r.written(4), std::string("\0", 1));
        r.fill();
        EXPECT_EQ(r.ucrt(false, 1, r.str("12 ab"), r.str("%d %s"), {slot(0), slot(1), 8}), 2u);
        EXPECT_EQ(r.written(1), std::string("ab\0", 3));
        // A format error in a secure call reaches the UCRT handler (here: none
        // installed, so the process would fast-fail), not msvcrt.dll's.
        EXPECT_EQ(r.legacy("sscanf_s", {r.str("12"), r.str("%d%")}, {slot(2)}), 1u);
        EXPECT_EQ(terminated, 0u);
        EXPECT_EQ(r.ucrt(false, 1, r.str("12"), r.str("%d%"), {slot(2)}), 1u);
        EXPECT_EQ(terminated, 0xC0000409u);
    }
}

// c024, c098, c032, "uw hs 263a", "uw arabic d": which buffers are wide, and
// what a wide character does in a narrow one.
TEST(CrtScanfTest, WideAndNarrowBuffers) {
    Rig r(true);
    EXPECT_EQ(r.ucrt(false, 0, r.str("cap"), r.str("%S"), {slot(0)}), 1u);
    EXPECT_EQ(hex(r.written(0)), "6300610070000000");
    EXPECT_EQ(r.legacy("swscanf", {r.wstr(u"cap"), r.wstr(u"%S")}, {slot(1)}), 1u);
    EXPECT_EQ(hex(r.written(1)), "63617000");
    EXPECT_EQ(r.ucrt(false, 0, r.str("ab"), r.str("%2lc"), {slot(2)}), 1u);
    EXPECT_EQ(hex(r.written(2)), "61006200");
    r.fill();
    r.mm.set_errno(0);
    EXPECT_EQ(r.ucrt(true, 0, r.wstr(u"a☺b"), r.wstr(u"%hs"), {slot(0)}), 1u);
    EXPECT_EQ(hex(r.written(0)), "616200000000");
    EXPECT_EQ(r.mm.get_errno(), 42u);
    EXPECT_EQ(r.ucrt(true, 1, r.wstr(u"a☺b"), r.wstr(u"%hs"), {slot(1), 8}), 1u);
    EXPECT_EQ(hex(r.written(1)), "61620000000000");
    EXPECT_EQ(r.ucrt(true, 0, r.wstr(u"٣٤"), r.wstr(u"%d"), {slot(2)}), 1u);
    EXPECT_EQ(r.u32_at(2), 34u);
    EXPECT_EQ(r.legacy("swscanf", {r.wstr(u"٣٤"), r.wstr(u"%d")}, {slot(3)}), 0u);
    EXPECT_EQ(r.ucrt(true, 0, r.wstr(u"　" u"5"), r.wstr(u"%d"), {slot(4)}), 1u);
    EXPECT_EQ(r.u32_at(4), 5u);
}

// "snscanf 3", "ucrt count 2": the count bounds the input.
TEST(CrtScanfTest, CountBoundsTheInput) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        Rig r(x64);
        EXPECT_EQ(r.legacy("_snscanf", {r.str("12345"), 3, r.str("%d")}, {slot(0)}), 1u);
        EXPECT_EQ(r.u32_at(0), 123u);
        EXPECT_EQ(r.legacy("_sscanf_l", {r.str("42"), r.str("%d"), 0}, {slot(1)}), 1u);
        EXPECT_EQ(r.u32_at(1), 42u);
    }
}

// The stream forms read through the stream's own getc/ungetc and leave it
// positioned after the last character used (UCRT stream_input_adapter).
TEST(CrtScanfTest, StreamFormsConsumeOnlyWhatTheyUse) {
    Rig r(true);
    std::string data = "12x 7";
    usize pos = 0;
    r.io.configure_guest_character_io(
        [&](u32) -> i32 { return pos < data.size() ? static_cast<u8>(data[pos++]) : -1; },
        [&](i32 c, u32) -> i32 {
            if (c == -1 || pos == 0) return -1;
            --pos;
            return c;
        });
    constexpr u64 kFile = 0x1000;
    EXPECT_EQ(r.legacy("fscanf", {kFile, r.str("%d")}, {slot(0)}), 1u);
    EXPECT_EQ(r.u32_at(0), 12u);
    EXPECT_EQ(pos, 2u);  // 'x' was pushed back
    EXPECT_EQ(r.legacy("fscanf", {kFile, r.str("x%d%n")}, {slot(1), slot(2)}), 1u);
    EXPECT_EQ(r.u32_at(1), 7u);
    EXPECT_EQ(r.u32_at(2), 3u);
    EXPECT_EQ(r.legacy("fscanf", {kFile, r.str("%d")}, {slot(3)}), kEof);
    pos = 0;
    const u64 va = r.va({slot(4)});
    EXPECT_EQ(r.call("__stdio_common_vfscanf", {0, kFile, r.str("%2d"), 0, va}), 1u);
    EXPECT_EQ(r.u32_at(4), 12u);
    EXPECT_EQ(pos, 2u);
}
