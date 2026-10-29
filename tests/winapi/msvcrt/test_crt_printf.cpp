/// @file test_crt_printf.cpp
/// @brief The CRT printf output processor through its exports, UCRT and
///        msvcrt.dll flavors, x86 and x64; secure string copies; the x64
///        legacy _iob layout.
///
/// Every expected value is copied from a Windows run:
/// tools/oracle_probe/crt_printf_probe{64,32}.expected (UCRT),
/// crt_msvcrt_printf_probe{64,32}.expected and
/// crt_msvcrt_stdio_probe{64,32}.expected (msvcrt.dll), or from the UCRT
/// source's documented behaviour where noted.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/msvcrt/io/crt_printf.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "winapi/msvcrt/text/crt_string.hpp"

#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kArena = 0x20000000;
constexpr GuestAddr kStack = kArena + 0x40000;
constexpr GuestAddr kData = kArena + 0x50000;
constexpr GuestAddr kBuf = kArena + 0x60000;
constexpr GuestAddr kVa = kArena + 0x70000;

/// One guest thread's view: registers, a stack, a scratch area, and the
/// printf + secure-string export tables.
struct Rig {
    explicit Rig(bool is64) : x64(is64) {
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(kStack);
        m.attach_cpu(cpu);
        exports = create_crt_printf_thunks(mm);
        add_crt_secure_string_exports(exports, mm);
    }

    /// A 64-bit varargs value: one slot on x64, two on x86.
    void push64(std::vector<u64>& slots, u64 v) const {
        if (x64) {
            slots.push_back(v);
        } else {
            slots.push_back(v & 0xFFFFFFFFu);
            slots.push_back(v >> 32);
        }
    }
    static u64 bits(double d) {
        u64 b;
        std::memcpy(&b, &d, 8);
        return b;
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
    /// A guest va_list holding @p slots.
    GuestAddr va(const std::vector<u64>& slots) {
        const usize w = x64 ? 8 : 4;
        for (usize i = 0; i < slots.size(); ++i) {
            if (x64) (void)memory.write_u64(kVa + i * w, slots[i]);
            else (void)memory.write_u32(kVa + i * w, static_cast<u32>(slots[i]));
        }
        return kVa;
    }

    /// Call an export with argument slots laid out per the calling convention.
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

    void fill(u8 v = 'Z', usize n = 64) {
        for (usize i = 0; i < n; ++i) (void)memory.write_u8(kBuf + i, v);
    }
    std::string bytes(usize n) const {
        std::string s;
        for (usize i = 0; i < n; ++i) s.push_back(static_cast<char>(memory.read_u8(kBuf + i).value()));
        return s;
    }
    std::string cstr() const {
        std::string s;
        for (usize i = 0;; ++i) {
            const u8 c = memory.read_u8(kBuf + i).value();
            if (c == 0) return s;
            s.push_back(static_cast<char>(c));
        }
    }
    std::u16string wbytes(usize n) const {
        std::u16string s;
        for (usize i = 0; i < n; ++i) s.push_back(memory.read_u16(kBuf + i * 2).value());
        return s;
    }

    bool x64;
    GuestMemory memory;
    CrtMemoryManager mm{memory, kArena, 0x100000};
    cpu::CpuState cpu;
    Marshaler m{memory};
    DllExports exports;
    GuestAddr next_data{kData};
};

u64 truncate_value(bool x64) { return x64 ? ~0ULL : 0xFFFFFFFFULL; }

} // namespace

// UCRT %a (13 hex digits, standard-rounded carry), C99 special values, and
// the legacy strings under LEGACY_MSVCRT_COMPATIBILITY|THREE_DIGIT_EXPONENTS.
// crt_printf_probe{64,32}.expected rows "a", "special o=0x00", "special o=0x18".
TEST(CrtPrintfTest, UcrtFloatLayoutsMatchWindows) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        Rig r(x64);
        std::vector<u64> v;
        r.push64(v, Rig::bits(1.0));
        r.push64(v, Rig::bits(1.0));
        r.push64(v, Rig::bits(-0.1));
        r.push64(v, Rig::bits(1.5));
        const GuestAddr fmt = r.str("%a %A %a %.0a");
        std::vector<u64> args = x64 ? std::vector<u64>{2} : std::vector<u64>{2, 0};
        for (u64 a : {u64{kBuf}, u64{4096}, u64{fmt}, u64{0}, u64{r.va(v)}}) args.push_back(a);
        const std::string expected =
            "0x1.0000000000000p+0 0X1.0000000000000P+0 -0x1.999999999999ap-4 0x2p+0";
        EXPECT_EQ(r.call("__stdio_common_vsprintf", args), expected.size());
        EXPECT_EQ(r.cstr(), expected);

        const double inf = std::numeric_limits<double>::infinity();
        double qnan, ind;
        const u64 qbits = 0x7ff8000000000000ULL, ibits = 0xfff8000000000000ULL;
        std::memcpy(&qnan, &qbits, 8);
        std::memcpy(&ind, &ibits, 8);
        std::vector<u64> s;
        for (double d : {inf, inf, -inf, -inf, qnan, qnan, ind, ind}) r.push64(s, Rig::bits(d));
        const GuestAddr sfmt = r.str("%f|%F|%e|%E|%g|%G|%a|%A");
        for (u64 options : {u64{2}, u64{0x1a}}) {
            std::vector<u64> a = x64 ? std::vector<u64>{options} : std::vector<u64>{options, 0};
            for (u64 x : {u64{kBuf}, u64{4096}, u64{sfmt}, u64{0}, u64{r.va(s)}}) a.push_back(x);
            (void)r.call("__stdio_common_vsprintf", a);
            EXPECT_EQ(r.cstr(), options == 2
                ? "inf|INF|-inf|-INF|nan|NAN|-nan(ind)|-NAN(IND)"
                : "1.#INF00||1.#INF00e+000|-1.#INF00E+000|-1.#INF|1.#QNAN|0x1.#QNAN00000000p+0|-0X1.#IND000000000P+0");
        }
    }
}

// Truncation modes (option bits 0/1/2), %ls outside the "C" locale's Latin-1
// range (EILSEQ), and positional parameters. crt_printf_probe rows
// "vsprintf o=0 n=04", "vsprintf o=1 n=05", "s nonlatin", "vsprintf_p pos".
TEST(CrtPrintfTest, UcrtTerminationEilseqAndPositional) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        Rig r(x64);
        const GuestAddr fmt = r.str("%s");
        const GuestAddr hello = r.str("hello");
        auto call = [&](const char* name, u64 options, u64 count, GuestAddr f,
                        const std::vector<u64>& va) {
            std::vector<u64> a = x64 ? std::vector<u64>{options} : std::vector<u64>{options, 0};
            for (u64 x : {u64{kBuf}, count, u64{f}, u64{0}, u64{r.va(va)}}) a.push_back(x);
            r.mm.set_errno(0);
            r.fill();
            return static_cast<i32>(r.call(name, a));
        };
        EXPECT_EQ(call("__stdio_common_vsprintf", 0, 4, fmt, {hello}), -2);
        EXPECT_EQ(r.bytes(8), std::string("hel\0ZZZZ", 8));
        EXPECT_EQ(call("__stdio_common_vsprintf", 1, 5, fmt, {hello}), 5);
        EXPECT_EQ(r.bytes(8), "helloZZZ");

        const GuestAddr lsfmt = r.str("[%ls]");
        const GuestAddr smile = r.wstr(u"a☺");
        EXPECT_EQ(call("__stdio_common_vsprintf", 2, 4096, lsfmt, {smile}), -1);
        EXPECT_EQ(r.mm.get_errno(), 42u);
        EXPECT_EQ(r.bytes(8), std::string("\0aZZZZZZ", 8));

        const GuestAddr pfmt = r.str("%2$s-%1$d-%2$s");
        EXPECT_EQ(call("__stdio_common_vsprintf_p", 2, 64, pfmt, {7, r.str("ab")}), 7);
        EXPECT_EQ(r.cstr(), "ab-7-ab");
    }
}

// msvcrt.dll's own formatter: 3-digit exponents, 17 significant digits
// (ties to even), %a with 6 hex digits rounding only above half.
// crt_msvcrt_printf_probe rows "f vals", "f exact", "a", "dig 228".
TEST(CrtPrintfTest, MsvcrtLegacyFloatFormatting) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        Rig r(x64);
        CrtDllFlavorScope scope(CrtDllFlavor::Msvcrt);
        auto snprintf = [&](const char* f, const std::vector<double>& ds) {
            std::vector<u64> a{kBuf, 4096, r.str(f)};
            for (double d : ds) r.push64(a, Rig::bits(d));
            return static_cast<i32>(r.call("_snprintf", a));
        };
        EXPECT_EQ(snprintf("%f %e %g %g %g %g %g",
                           {0.1, 123456789.0, 100000.0, 1e6, 0.0001, 0.00001, 123456789.0}), 63);
        EXPECT_EQ(r.cstr(), "0.100000 1.234568e+008 100000 1e+006 0.0001 1e-005 1.23457e+008");
        EXPECT_EQ(snprintf("%.20f %.30e %.17g", {0.1, 0.1, 0.1}), 80);
        EXPECT_EQ(r.cstr(), "0.10000000000000001000 1.000000000000000100000000000000e-001 0.10000000000000001");
        EXPECT_EQ(snprintf("%a %A %a %.3a %.0a", {1.0, 1.0, -0.1, 1.0, 1.5}), 60);
        EXPECT_EQ(r.cstr(), "0x1.000000p+0 0X1.000000P+0 -0x1.99999ap-4 0x1.000p+0 0x1p+0");
        (void)snprintf("%.16e", {348389360662118.125});
        EXPECT_EQ(r.cstr(), "3.4838936066211812e+014");
        const double inf = std::numeric_limits<double>::infinity();
        double qnan;
        const u64 qbits = 0x7ff8000000000000ULL;
        std::memcpy(&qnan, &qbits, 8);
        EXPECT_EQ(snprintf("%f|%F|%e|%E|%g|%G", {inf, inf, -inf, -inf, qnan, qnan}), 54);
        EXPECT_EQ(r.cstr(), "1.#INF00||1.#INF00e+000|-1.#INF00E+000|-1.#INF|1.#QNAN");

        // _set_output_format(_TWO_DIGIT_EXPONENT); an undefined bit is EINVAL
        // and leaves the setting alone (row "two digit").
        EXPECT_EQ(r.call("_set_output_format", {1}), 0u);
        (void)snprintf("%e", {1.5});
        EXPECT_EQ(r.cstr(), "1.500000e+00");
        r.mm.set_errno(0);
        EXPECT_EQ(r.call("_set_output_format", {3}), 1u);
        EXPECT_EQ(r.mm.get_errno(), 22u);
        EXPECT_EQ(r.call("_get_output_format", {}), 1u);
        EXPECT_EQ(r.call("_set_output_format", {0}), 1u);
    }
}

// Per-export termination and errno. crt_msvcrt_printf_probe rows
// "_snprintf n=05", "_snprintf_c n=04", "sprintf_s n=04",
// "_snprintf_s n=8 max=6", "_snwprintf_s n=4 max=T", "[%llp]".
TEST(CrtPrintfTest, MsvcrtReturnRules) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        Rig r(x64);
        CrtDllFlavorScope scope(CrtDllFlavor::Msvcrt);
        const GuestAddr fmt = r.str("%s");
        const GuestAddr hello = r.str("hello");
        auto run = [&](const char* name, std::vector<u64> a) {
            r.mm.set_errno(0);
            r.fill();
            return static_cast<i32>(r.call(name, a));
        };
        EXPECT_EQ(run("_snprintf", {kBuf, 5, fmt, hello}), 5);
        EXPECT_EQ(r.mm.get_errno(), 34u);
        EXPECT_EQ(r.bytes(8), "helloZZZ");
        EXPECT_EQ(run("_snprintf_c", {kBuf, 4, fmt, hello}), -1);
        EXPECT_EQ(r.bytes(8), std::string("\0ellZZZZ", 8));
        EXPECT_EQ(run("sprintf_s", {kBuf, 4, fmt, hello}), -1);
        EXPECT_EQ(r.mm.get_errno(), 34u);
        EXPECT_EQ(r.bytes(8), std::string("\0el\0ZZZZ", 8));
        EXPECT_EQ(run("_snprintf_s", {kBuf, 8, 6, fmt, hello}), 5);
        EXPECT_EQ(r.bytes(9), std::string("hello\0\0ZZ", 9));

        const GuestAddr wfmt = r.wstr(u"%s");
        const GuestAddr whello = r.wstr(u"hello");
        EXPECT_EQ(run("_snwprintf_s", {kBuf, 4, truncate_value(x64), wfmt, whello}), -1);
        EXPECT_EQ(r.mm.get_errno(), 0u);
        EXPECT_EQ(r.wbytes(5), std::u16string(u"hel\0婚", 5));

        std::vector<u64> a{kBuf, 4096, r.str("[%llp]")};
        r.push64(a, 0x0000004100000041ULL);
        (void)run("_snprintf", a);
        EXPECT_EQ(r.cstr(), x64 ? "[0000004100000041]" : "[4100000041]");
    }
}

// msvcrt.dll exports no handler setter and a failed check returns (every
// crt_msvcrt_stdio_probe case reaches "done"); the UCRT fast-fails.
TEST(CrtPrintfTest, InvalidParameterPolicyFollowsTheDll) {
    Rig r(true);
    u32 terminated = 0;
    r.mm.set_process_terminator([&](u32 status) { terminated = status; });
    {
        CrtDllFlavorScope scope(CrtDllFlavor::Msvcrt);
        EXPECT_TRUE(r.mm.invalid_parameter());
        EXPECT_EQ(terminated, 0u);
    }
    EXPECT_FALSE(r.mm.invalid_parameter());
    EXPECT_EQ(terminated, 0xC0000409u);
}

// strcpy_s/strcat_s/wcscpy_s copy until the buffer runs out, then reset [0]
// (UCRT common_tcscpy_s / common_tcscat_s; crt_msvcrt_stdio_probe rows
// strcpy_s_small, strcat_s_small, wcscpy_s_small).
TEST(CrtPrintfTest, SecureStringCopiesKeepThePrefixBeforeTheReset) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        Rig r(x64);
        CrtDllFlavorScope scope(CrtDllFlavor::Msvcrt);
        r.fill();
        EXPECT_EQ(r.call("strcpy_s", {kBuf, 2, r.str("hello")}), 34u);
        EXPECT_EQ(r.bytes(4), std::string("\0eZZ", 4));
        EXPECT_EQ(r.mm.get_errno(), 34u);
        r.fill();
        (void)r.memory.write_u8(kBuf, 'a');
        (void)r.memory.write_u8(kBuf + 1, 0);
        EXPECT_EQ(r.call("strcat_s", {kBuf, 3, r.str("bcd")}), 34u);
        EXPECT_EQ(r.bytes(4), std::string("\0bcZ", 4));
        r.fill();
        EXPECT_EQ(r.call("wcscpy_s", {kBuf, 2, r.wstr(u"hello")}), 34u);
        EXPECT_EQ(r.wbytes(2), std::u16string(u"\0e", 2));
        EXPECT_EQ(r.call("strcpy_s", {0, 5, r.str("x")}), 22u);
        r.fill();
        EXPECT_EQ(r.call("strncpy_s", {kBuf, 4, r.str("hello"), truncate_value(x64)}), 80u);
        EXPECT_EQ(r.bytes(5), std::string("hel\0Z", 5));
        EXPECT_EQ(r.call("strcpy_s", {kBuf, 8, r.str("ok")}), 0u);
        EXPECT_EQ(r.cstr(), "ok");
    }
}

// x64 msvcrt `FILE` is 48 bytes (_flag +24, _file +28); stdout is
// __iob_func()+48 (crt_msvcrt_stdio_probe64 "iob[1] ... 0x2 0x1"). The x86
// array keeps its 32-byte stride (probe32 "iob[1] 0x0 0x0 0x0 0x2 0x1").
TEST(CrtPrintfTest, LegacyIobLayoutFollowsBitness) {
    GuestMemory memory;
    CrtDataBlock data;
    ASSERT_TRUE(data.initialize(memory));
    for (bool x64 : {false, true}) {
        const u64 base = data.legacy_iob_addr(x64);
        const u64 size = x64 ? 48 : 32;
        const u64 flag = x64 ? 24 : 12;
        const u64 file = x64 ? 28 : 16;
        for (u32 i = 1; i < 3; ++i) {
            const u64 stream = data.legacy_stream_addr(i, x64);
            EXPECT_EQ(stream, base + i * size);
            EXPECT_EQ(memory.read_u32(stream + flag).value(), 2u);
            EXPECT_EQ(memory.read_u32(stream + file).value(), i);
            EXPECT_EQ(data.legacy_stream_index(stream), static_cast<i32>(i));
        }
    }
    data.set_guest_64bit(true);
    EXPECT_EQ(data.resolve_data_import("_iob"), data.legacy_iob_addr(true));
}
