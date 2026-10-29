/// @file test_crt_s6_contracts.cpp
/// @brief CRT exports added or rebuilt by the S6 Steam audit (2026-09-30):
///        strtod family, _aligned_offset_*, _strlwr_s, _mbscmp, _makepath
///        family, _callnewh, x86 _ftol2/_ftol2_sse, lgamma special cases,
///        __unDName, msvcr* invalid-parameter handler separation.
///
/// Expected values are copied from Windows runs:
/// tools/oracle_probe/crt_strtod_probe{64,32}.expected,
/// crt_s6_missing_probe{64,32}.expected and crt_fpctrl_probe{64,32}.expected.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include "capsule/vfs/vfs.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/math/host_fenv.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/msvcrt/math/crt_math.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/runtime/crt_init.hpp"
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

struct Rig {
    explicit Rig(bool is64) : x64(is64) {
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(kStack);
        m.attach_cpu(cpu);
        add_crt_strtod_exports(exports, mm);
        add_crt_secure_string_exports(exports, mm);
        add_crt_aligned_exports(exports, mm);
        add_crt_fpenv_exports(exports);
        for (auto& e : create_crt_memory_thunks(mm).entries) exports.entries.push_back(std::move(e));
        for (auto& e : create_crt_math_thunks(math, mm).entries) exports.entries.push_back(std::move(e));
        for (auto& e : create_crt_string_thunks(strings, mm, vfs).entries) exports.entries.push_back(std::move(e));
    }
    GuestAddr str(const std::string& s) {
        const GuestAddr a = next;
        for (usize i = 0; i < s.size(); ++i) (void)memory.write_u8(a + i, static_cast<u8>(s[i]));
        (void)memory.write_u8(a + s.size(), 0);
        next += (s.size() + 16) & ~usize{7};
        return a;
    }
    GuestAddr wstr(const std::u16string& s) {
        const GuestAddr a = next;
        for (usize i = 0; i < s.size(); ++i) (void)memory.write_u16(a + i * 2, s[i]);
        (void)memory.write_u16(a + s.size() * 2, 0);
        next += (s.size() * 2 + 16) & ~usize{7};
        return a;
    }
    u64 call(const std::string& name, const std::vector<u64>& args) {
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
        // The last registration of a name wins, as in ThunkRegistry.
        const ExportEntry* found = nullptr;
        for (const auto& e : exports.entries) {
            if (e.name == name) found = &e;
        }
        if (found == nullptr) {
            ADD_FAILURE() << "no export " << name;
            return 0;
        }
        found->handler(m);
        return cpu.gpr.rax;
    }
    /// A double result: XMM0 on x64, ST(0) (popped here) on x86.
    u64 double_result() {
        if (x64) return cpu.sse.xmm[0].u64_[0];
        const double d = cpu.fpu.get_st(0).to_double();
        (void)cpu.fpu.pop();
        u64 b;
        std::memcpy(&b, &d, 8);
        return b;
    }
    u32 float_result() {
        if (x64) return cpu.sse.xmm[0].u32_[0];
        const float f = static_cast<float>(cpu.fpu.get_st(0).to_double());
        (void)cpu.fpu.pop();
        u32 b;
        std::memcpy(&b, &f, 4);
        return b;
    }

    bool x64;
    GuestMemory memory;
    CrtMemoryManager mm{memory, kArena, 0x100000};
    CrtMathManager math;
    CrtStringManager strings;
    capsule::Vfs vfs;
    cpu::CpuState cpu;
    Marshaler m{memory};
    DllExports exports;
    GuestAddr next{kData};
};

u64 dbits(double d) {
    u64 b;
    std::memcpy(&b, &d, 8);
    return b;
}

} // namespace

// crt_strtod_probe rows: ucrt 5/22/23/25/26/31/32/34/52 and wide 0, msvcrt 5
// and 22 (no hex floats, no NAN spelling).
TEST(CrtS6ContractTest, StrtodFamilyMatchesWindows) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        Rig r(x64);
        const GuestAddr end = r.str("12345678");
        auto strtod = [&](const std::string& s, u64& consumed) {
            const GuestAddr p = r.str(s);
            r.mm.set_errno(0);
            (void)r.call("strtod", {p, end});
            consumed = r.m.read_ptr(end).value() - p;
            return r.double_result();
        };
        u64 n = 0;
        EXPECT_EQ(strtod("0x1p3", n), 0x4020000000000000ULL); EXPECT_EQ(n, 5u);
        EXPECT_EQ(strtod("nan", n), 0x7fffffffffffffffULL); EXPECT_EQ(n, 3u);
        EXPECT_EQ(strtod("-nan", n), 0xffffffffffffffffULL); EXPECT_EQ(n, 4u);
        EXPECT_EQ(strtod("nan(ind)", n), 0xfff8000000000000ULL); EXPECT_EQ(n, 8u);
        EXPECT_EQ(strtod("1e309", n), 0x7ff0000000000000ULL); EXPECT_EQ(n, 5u);
        EXPECT_EQ(r.mm.get_errno(), 34u);
        EXPECT_EQ(strtod("2.2250738585072011e-308", n), 0x000fffffffffffffULL);
        EXPECT_EQ(r.mm.get_errno(), 0u);  // a denormal is not a range error
        EXPECT_EQ(strtod("-", n), 0u); EXPECT_EQ(n, 0u);  // no digits: +0
        EXPECT_EQ(strtod("1e", n), 0x3ff0000000000000ULL); EXPECT_EQ(n, 1u);

        // strtof: float range and the float NaN pattern.
        r.mm.set_errno(0);
        (void)r.call("strtof", {r.str("1e40"), 0});
        EXPECT_EQ(r.float_result(), 0x7f800000u);
        EXPECT_EQ(r.mm.get_errno(), 34u);
        // wcstof accepts the UCRT's Unicode digit blocks and U+00A0 as space.
        (void)r.call("wcstof", {r.wstr(u" 5"), end});
        EXPECT_EQ(r.float_result(), 0x40a00000u);
        (void)r.call("wcstod", {r.wstr(u"١٢.٣"), end});
        EXPECT_EQ(r.double_result(), 0x402899999999999aULL);
        // atof never reports ERANGE.
        r.mm.set_errno(0);
        (void)r.call("atof", {r.str("1e309")});
        EXPECT_EQ(r.double_result(), 0x7ff0000000000000ULL);
        EXPECT_EQ(r.mm.get_errno(), 0u);

        CrtDllFlavorScope msvcrt(CrtDllFlavor::Msvcrt);
        EXPECT_EQ(strtod("0x1p3", n), 0u); EXPECT_EQ(n, 1u);
        EXPECT_EQ(strtod("nan", n), 0u); EXPECT_EQ(n, 0u);
    }
}

// _aligned_offset_malloc: UCRT align.cpp layout; crt_s6_missing_probe "aom".
TEST(CrtS6ContractTest, AlignedOffsetFamily) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        Rig r(x64);
        const u64 p = r.call("_aligned_offset_malloc", {100, 16, 4});
        ASSERT_NE(p, 0u);
        EXPECT_EQ((p + 4) % 16, 0u);
        EXPECT_EQ(r.call("_aligned_msize", {p, 16, 4}), 100u);
        for (u32 i = 0; i < 100; ++i) (void)r.memory.write_u8(p + i, static_cast<u8>(i));
        const u64 q = r.call("_aligned_offset_realloc", {p, 200, 16, 4});
        ASSERT_NE(q, 0u);
        EXPECT_EQ((q + 4) % 16, 0u);
        EXPECT_EQ(r.memory.read_u8(q + 99).value(), 99u);
        (void)r.call("_aligned_free", {q});

        r.mm.set_errno(0);
        EXPECT_EQ(r.call("_aligned_offset_malloc", {100, 24, 0}), 0u);
        EXPECT_EQ(r.mm.get_errno(), 22u);
        r.mm.set_errno(0);
        EXPECT_EQ(r.call("_aligned_offset_malloc", {100, 16, 100}), 0u);
        EXPECT_EQ(r.mm.get_errno(), 22u);
        const u64 z = r.call("_aligned_malloc", {0, 16});
        EXPECT_NE(z, 0u);
        (void)r.call("_aligned_free", {z});
        r.mm.set_errno(0);
        const u64 huge = x64 ? ~0ULL - 7 : 0xFFFFFFF8ULL;
        EXPECT_EQ(r.call("_aligned_offset_malloc", {huge, 16, 0}), 0u);
        EXPECT_EQ(r.mm.get_errno(), 12u);
    }
}

// _strlwr_s, _mbscmp, _makepath/_wmakepath_s (crt_s6_missing_probe).
TEST(CrtS6ContractTest, StringHelpers) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        Rig r(x64);
        const GuestAddr b = r.str(std::string("AbC\xC9Z\0QQ", 8));
        EXPECT_EQ(r.call("_strlwr_s", {b, 8}), 0u);
        std::string out;
        for (u32 i = 0; i < 8; ++i) out.push_back(static_cast<char>(r.memory.read_u8(b + i).value()));
        EXPECT_EQ(out, std::string("abc\xC9z\0QQ", 8));
        const GuestAddr c = r.str("ABCD");
        EXPECT_EQ(r.call("_strlwr_s", {c, 4}), 22u);
        EXPECT_EQ(r.memory.read_u8(c).value(), 0u);

        EXPECT_EQ(static_cast<i32>(r.call("_mbscmp", {r.str("abc"), r.str("abd")})), -1);
        EXPECT_EQ(static_cast<i32>(r.call("_mbscmp", {r.str("\xe9"), r.str("a")})), 1);
        r.mm.set_errno(0);
        EXPECT_EQ(static_cast<u32>(r.call("_mbscmp", {0, r.str("a")})), 0x7fffffffu);
        EXPECT_EQ(r.mm.get_errno(), 22u);

        (void)r.call("_makepath", {kBuf, r.str("c"), r.str("dir"), r.str("name"), r.str("txt")});
        std::string made;
        for (u32 i = 0;; ++i) {
            const u8 ch = r.memory.read_u8(kBuf + i).value();
            if (ch == 0) break;
            made.push_back(static_cast<char>(ch));
        }
        EXPECT_EQ(made, "c:dir\\name.txt");

        // msvcrt.dll leaves room for the terminator per character.
        CrtDllFlavorScope msvcrt(CrtDllFlavor::Msvcrt);
        for (u32 i = 0; i < 6; ++i) (void)r.memory.write_u16(kBuf + i * 2, 0x5a5a);
        EXPECT_EQ(r.call("_wmakepath_s", {kBuf, 5, r.wstr(u"c"), r.wstr(u"dir"), r.wstr(u"f"), r.wstr(u"e")}), 34u);
        const u16 expect[6] = {0, ':', 'd', 'i', 0x5a5a, 0x5a5a};
        for (u32 i = 0; i < 6; ++i) EXPECT_EQ(r.memory.read_u16(kBuf + i * 2).value(), expect[i]) << i;
    }
}

// _callnewh calls the installed new handler with the size; 1 only when it
// returned nonzero (crt_s6_missing_probe "callnewh").
TEST(CrtS6ContractTest, CallnewhRunsTheNewHandler) {
    Rig r(true);
    u64 seen = ~0ULL;
    u64 handler_result = 7;
    r.mm.set_guest_caller([&](GuestAddr fn, const u64* args, usize count) -> u64 {
        EXPECT_EQ(fn, 0x1234u);
        EXPECT_EQ(count, 1u);
        seen = args[0];
        return handler_result;
    });
    EXPECT_EQ(r.call("_callnewh", {5}), 0u);
    EXPECT_EQ(seen, ~0ULL);
    EXPECT_EQ(r.call("_set_new_handler", {0x1234}), 0u);
    EXPECT_EQ(r.call("_callnewh", {42}), 1u);
    EXPECT_EQ(seen, 42u);
    handler_result = 0;
    EXPECT_EQ(r.call("_callnewh", {9}), 0u);
    EXPECT_EQ(r.call("_query_new_handler", {}), 0x1234u);
}

// x86 _ftol2 (EDX:EAX, truncation, indefinite) and _ftol2_sse (EAX only).
TEST(CrtS6ContractTest, FtolHelpersTruncateAsWindows) {
    Rig r(false);
    auto run = [&](const char* name, u64 bits) -> u64 {
        double d;
        std::memcpy(&d, &bits, 8);
        r.cpu.fpu.push(cpu::Float80(d));
        r.cpu.gpr.rdx = 0x11ff638;
        (void)r.call(name, {});
        return (static_cast<u64>(static_cast<u32>(r.cpu.gpr.rdx)) << 32) |
               static_cast<u32>(r.cpu.gpr.rax);
    };
    EXPECT_EQ(run("_ftol2", 0xbff8000000000000ULL), 0xffffffffffffffffULL);
    EXPECT_EQ(run("_ftol2", 0x41e65a0bc0000000ULL), 0xb2d05e00ULL);
    EXPECT_EQ(run("_ftol2", 0xc1e65a0bc0000000ULL), 0xffffffff4d2fa200ULL);
    EXPECT_EQ(run("_ftol2", 0x43e158e460913d00ULL), 0x8000000000000000ULL);
    EXPECT_EQ(run("_ftol2", 0x7ff8000000000000ULL), 0x8000000000000000ULL);
    EXPECT_EQ(run("_ftol", 0x41f00000000b3333ULL), 0x100000000ULL);
    EXPECT_EQ(run("_ftol2_sse", 0xbff8000000000000ULL) & 0xffffffffu, 0xffffffffu);
    EXPECT_EQ(run("_ftol2_sse", 0x41e65a0bc0000000ULL), 0x011ff63880000000ULL);
}

// lgamma poles, infinities and NaN payloads (crt_s6_missing_probe).
TEST(CrtS6ContractTest, LgammaSpecialCases) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        Rig r(x64);
        auto lgamma = [&](u64 bits) {
            if (x64) r.cpu.sse.xmm[0].u64_[0] = bits;
            r.mm.set_errno(0);
            (void)r.call("lgamma", x64 ? std::vector<u64>{} : std::vector<u64>{bits & 0xffffffffu, bits >> 32});
            return r.double_result();
        };
        EXPECT_EQ(lgamma(0), 0x7ff0000000000000ULL);
        EXPECT_EQ(r.mm.get_errno(), 34u);
        EXPECT_EQ(lgamma(dbits(-3.0)), 0x7ff0000000000000ULL);
        EXPECT_EQ(r.mm.get_errno(), 34u);
        EXPECT_EQ(lgamma(0xfff0000000000000ULL), 0x7ff0000000000000ULL);
        EXPECT_EQ(r.mm.get_errno(), 0u);
        EXPECT_EQ(lgamma(dbits(1.0)), 0u);
        if (x64) EXPECT_EQ(lgamma(0x7ff8000000000001ULL), 0x7ff8000000000001ULL);
    }
}

// __unDName: no allocator -> NULL; an undecorated name is copied through
// (crt_s6_missing_probe "undname notmangled").
TEST(CrtS6ContractTest, UndnameContract) {
    Rig r(true);
    const GuestAddr name = r.str("notmangled");
    EXPECT_EQ(r.call("__unDName", {kBuf, name, 256, 0, 0, 0}), 0u);
    EXPECT_EQ(r.call("__unDName", {kBuf, name, 256, 0x5000, 0x5008, 0}), kBuf);
    std::string out;
    for (u32 i = 0;; ++i) {
        const u8 ch = r.memory.read_u8(kBuf + i).value();
        if (ch == 0) break;
        out.push_back(static_cast<char>(ch));
    }
    EXPECT_EQ(out, "notmangled");
}

// A handler installed through ucrtbase is not run by msvcrt.dll code
// (crt_strtod_probe "msvcrt strtod null ... ip=0").
TEST(CrtS6ContractTest, InvalidParameterHandlersArePerDll) {
    Rig r(true);
    unsigned calls = 0;
    r.mm.set_guest_caller([&](GuestAddr, const u64*, usize) -> u64 { ++calls; return 0; });
    (void)r.mm.exchange_invalid_parameter_handler(0x4444);  // via ucrtbase
    {
        CrtDllFlavorScope msvcrt(CrtDllFlavor::Msvcrt);
        EXPECT_EQ(r.mm.invalid_parameter_handler(), 0u);
        EXPECT_TRUE(r.mm.invalid_parameter());
        EXPECT_EQ(calls, 0u);
    }
    EXPECT_TRUE(r.mm.invalid_parameter());
    EXPECT_EQ(calls, 1u);
}

// crt_fpctrl_probe64.expected: on x64 the control word is MXCSR alone --
// masks, rounding and FTZ/DAZ -- read from the hardware each call; precision
// control is ignored, and _controlfp keeps the DENORMAL mask.
TEST(CrtS6ContractTest, FpEnvironmentX64IsMxcsr) {
    Rig r(true);
    auto normal = [&r] {
        r.cpu.sse.mxcsr = 0x1F80;
        r.cpu.fpu.control_word = 0x27F;
        r.cpu.fpu.status_word = 0;
        r.cpu.thunk_sets_fp_control = false;
        math::clear_host_fp_exceptions();
        math::discard_sse_exceptions();
    };
    normal();
    EXPECT_EQ(r.call("_controlfp", {0, 0}), 0x8001Fu);
    EXPECT_EQ(r.call("_controlfp", {0x200, 0x300}), 0x8021Fu);           // cfp rc=0x200
    EXPECT_EQ(r.cpu.sse.mxcsr, 0x5F80u);
    EXPECT_EQ(r.cpu.fpu.control_word, 0x27Fu);
    EXPECT_TRUE(r.cpu.thunk_sets_fp_control);  // exempt from the ABI restore
    normal();
    EXPECT_EQ(r.call("_controlfp", {0x20000, 0x30000}), 0x8001Fu);       // cfp pc=0x20000
    EXPECT_EQ(r.cpu.sse.mxcsr, 0x1F80u);
    EXPECT_EQ(r.call("_controlfp", {0x01000000, 0x03000000}), 0x108001Fu);  // cfp dn=flush
    EXPECT_EQ(r.cpu.sse.mxcsr, 0x9FC0u);
    normal();
    EXPECT_EQ(r.call("_control87", {0x10, 0x8001F}), 0x10u);             // c87 em=invalid
    EXPECT_EQ(r.cpu.sse.mxcsr, 0x80u);
    normal();
    EXPECT_EQ(r.call("_controlfp", {0x10, 0x8001F}), 0x80010u);          // cfp em=invalid
    EXPECT_EQ(r.cpu.sse.mxcsr, 0x180u);
    normal();
    r.cpu.sse.mxcsr = 0x3F80;                                            // hw mxcsr rc=down
    EXPECT_EQ(r.call("_controlfp", {0, 0}), 0x8011Fu);
    r.cpu.sse.mxcsr = 0x9F80;                                            // hw mxcsr ftz
    EXPECT_EQ(r.call("_controlfp", {0, 0}), 0x308001Fu);
    normal();
    EXPECT_EQ(r.call("_controlfp_s", {0, 0x100, 0x300}), 0u);            // cfps null
    EXPECT_EQ(r.cpu.sse.mxcsr, 0x3F80u);
    normal();
    r.cpu.sse.mxcsr = 0x1FA8;                                            // sse overflow
    EXPECT_EQ(r.call("_statusfp", {}), 0x5u);
    EXPECT_EQ(r.call("_clearfp", {}), 0x5u);
    EXPECT_EQ(r.cpu.sse.mxcsr, 0x1F80u);
    normal();
    r.cpu.fpu.control_word = 0x373;                                      // fpreset
    r.cpu.sse.mxcsr = 0x1F80 | 0x6000 | 0x8040;
    (void)r.call("_fpreset", {});
    EXPECT_EQ(r.cpu.fpu.control_word, 0x373u);
    EXPECT_EQ(r.cpu.sse.mxcsr, 0x1F80u);
    EXPECT_EQ(r.call("__fpe_flt_rounds", {}), 1u);
}

// crt_fpctrl_probe32.expected: on x86 both units; the word is x87 | SSE plus
// _EM_AMBIGUOUS when they disagree, and msvcrt.dll's _clearfp reports only
// the x87 flags.
TEST(CrtS6ContractTest, FpEnvironmentX86CombinesBothUnits) {
    Rig r(false);
    auto normal = [&r] {
        r.cpu.sse.mxcsr = 0x1F80;
        r.cpu.fpu.control_word = 0x27F;
        r.cpu.fpu.status_word = 0;
        math::clear_host_fp_exceptions();
        math::discard_sse_exceptions();
    };
    normal();
    EXPECT_EQ(r.call("_controlfp", {0, 0}), 0x9001Fu);
    EXPECT_EQ(r.call("_controlfp", {0x200, 0x300}), 0x9021Fu);           // cfp rc=0x200
    EXPECT_EQ(r.cpu.fpu.control_word, 0xA7Fu);
    EXPECT_EQ(r.cpu.sse.mxcsr, 0x5F80u);
    normal();
    EXPECT_EQ(r.call("_controlfp", {0x20000, 0x30000}), 0xA001Fu);       // cfp pc=0x20000
    EXPECT_EQ(r.cpu.fpu.control_word, 0x7Fu);
    normal();
    EXPECT_EQ(r.call("_controlfp", {0x40000, 0x40000}), 0xD001Fu);       // cfp ic=affine
    EXPECT_EQ(r.cpu.fpu.control_word, 0x127Fu);
    normal();
    EXPECT_EQ(r.call("_controlfp", {0x10, 0x8001F}), 0x90010u);          // cfp em=invalid
    EXPECT_EQ(r.cpu.fpu.control_word, 0x243u);
    EXPECT_EQ(r.cpu.sse.mxcsr, 0x180u);
    normal();
    r.cpu.sse.mxcsr = 0x3F80;                                            // hw mxcsr rc=down
    EXPECT_EQ(r.call("_controlfp", {0, 0}), 0x8009011Fu);
    normal();
    r.cpu.fpu.control_word = 0x37F;                                      // hw fcw pc=64
    EXPECT_EQ(r.call("_control87", {0, 0}), 0x8001Fu);
    normal();
    EXPECT_EQ(r.call("_controlfp_s", {0, 0, 0xFFFFFFFFu}), 0u);          // cfps mask=all
    EXPECT_EQ(r.cpu.fpu.control_word, 0x342u);
    EXPECT_EQ(r.cpu.sse.mxcsr, 0x100u);
    normal();
    r.cpu.fpu.status_word = 0x20;                                        // both
    r.cpu.sse.mxcsr = 0x1F84;
    EXPECT_EQ(r.call("_statusfp", {}), 0x9u);
    EXPECT_EQ(r.call("_clearfp", {}), 0x9u);
    EXPECT_EQ(r.cpu.fpu.status_word & 0x3Fu, 0u);
    EXPECT_EQ(r.cpu.sse.mxcsr, 0x1F80u);
    normal();
    r.cpu.fpu.status_word = 0x20;
    r.cpu.sse.mxcsr = 0x1F84;
    {
        CrtDllFlavorScope scope(CrtDllFlavor::Msvcrt);
        EXPECT_EQ(r.call("_clearfp", {}), 0x1u);
    }
    EXPECT_EQ(r.cpu.sse.mxcsr, 0x1F80u);
    normal();
    r.cpu.fpu.control_word = 0x373;
    r.cpu.sse.mxcsr = 0x1F80 | 0x6000 | 0x8040;
    (void)r.call("_fpreset", {});                                        // fpreset
    EXPECT_EQ(r.cpu.fpu.control_word, 0x27Fu);
    EXPECT_EQ(r.cpu.sse.mxcsr, 0x1F80u);
    normal();
    (void)r.memory.write_u32(kBuf, 0xEEEEEEEEu);                         // c87_2 sse rc=up
    EXPECT_EQ(r.call("__control87_2", {0x200, 0x300, 0, kBuf}), 1u);
    EXPECT_EQ(r.memory.read_u32(kBuf).value(), 0x8021Fu);
    EXPECT_EQ(r.cpu.fpu.control_word, 0x27Fu);
    EXPECT_EQ(r.call("_controlfp", {0, 0}), 0x8009021Fu);
}

// nextafter family (tests/oracle_data/nextafter_probe{32,64}.expected, x86 ==
// x64): result bits and errno, ERANGE whenever the result is subnormal, zero
// or infinite, a NaN argument returned unchanged, equal arguments giving y.
TEST(CrtS6ContractTest, NextafterFamilyMatchesWindows) {
    struct Row { const char* fn; u64 x, y, want; u32 err; };
    const Row dbl[] = {
        {"nextafter", 0x3ff0000000000000, 0x4000000000000000, 0x3ff0000000000001, 0},
        {"nextafter", 0x3ff0000000000000, 0x0000000000000000, 0x3fefffffffffffff, 0},
        {"nextafter", 0x3ff0000000000000, 0x3ff0000000000000, 0x3ff0000000000000, 0},
        {"nextafter", 0x0000000000000000, 0x8000000000000000, 0x8000000000000000, 0},
        {"nextafter", 0x8000000000000000, 0x0000000000000000, 0x0000000000000000, 0},
        {"nextafter", 0x0000000000000000, 0x3ff0000000000000, 0x0000000000000001, 34},
        {"nextafter", 0x0000000000000001, 0x0000000000000000, 0x0000000000000000, 34},
        {"nextafter", 0x8000000000000001, 0x0000000000000000, 0x8000000000000000, 34},
        {"nextafter", 0x0010000000000000, 0x0000000000000000, 0x000fffffffffffff, 34},
        {"nextafter", 0x7fefffffffffffff, 0x7ff0000000000000, 0x7ff0000000000000, 34},
        {"nextafter", 0x7ff0000000000000, 0x0000000000000000, 0x7fefffffffffffff, 0},
        {"nextafter", 0x7ff8000000000001, 0x3ff0000000000000, 0x7ff8000000000001, 0},
        {"nextafter", 0x3ff0000000000000, 0x7ff8000000000001, 0x7ff8000000000001, 0},
        {"_nextafter", 0x7fefffffffffffff, 0x7ff0000000000000, 0x7ff0000000000000, 34},
        {"nextafterl", 0x3ff0000000000000, 0x4000000000000000, 0x3ff0000000000001, 0},
        {"nexttoward", 0x0000000000000001, 0x0000000000000000, 0x0000000000000000, 34},
        {"nexttowardl", 0x3ff0000000000000, 0x0000000000000000, 0x3fefffffffffffff, 0},
    };
    // nextafterf(float, float) and nexttowardf(float, long double == double).
    const Row flt[] = {
        {"nextafterf", 0x3f800000, 0x40000000, 0x3f800001, 0},
        {"nextafterf", 0x3f800000, 0x3f800000, 0x3f800000, 0},
        {"nextafterf", 0x00000000, 0xbf800000, 0x80000001, 34},
        {"nextafterf", 0x00000001, 0x00000000, 0x00000000, 34},
        {"nextafterf", 0x00800000, 0x00000000, 0x007fffff, 34},
        {"nextafterf", 0x7f7fffff, 0x7f800000, 0x7f800000, 34},
        {"nextafterf", 0x7fc00001, 0x3f800000, 0x7fc00001, 0},
        {"nexttowardf", 0x3f800000, 0x4000000000000000, 0x3f800001, 0},
        {"nexttowardf", 0x3f800000, 0x3ff0000000000000, 0x3f800000, 0},
        {"nexttowardf", 0x3f800000, 0x3ff000000006df38, 0x3f800001, 0},
        {"nexttowardf", 0x7f7fffff, 0x7ff0000000000000, 0x7f800000, 34},
        {"nexttowardf", 0x00000001, 0x0000000000000000, 0x00000000, 34},
    };
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        for (const Row& row : dbl) {
            SCOPED_TRACE(row.fn);
            Rig r(x64);
            r.mm.set_errno(0);
            if (x64) {
                r.cpu.sse.xmm[0].u64_[0] = row.x;
                r.cpu.sse.xmm[1].u64_[0] = row.y;
                (void)r.call(row.fn, {});
            } else {
                (void)r.call(row.fn, {row.x & 0xffffffff, row.x >> 32, row.y & 0xffffffff, row.y >> 32});
            }
            EXPECT_EQ(r.double_result(), row.want);
            EXPECT_EQ(r.mm.get_errno(), row.err);
        }
        for (const Row& row : flt) {
            SCOPED_TRACE(row.fn);
            const bool toward = std::string(row.fn) == "nexttowardf";
            Rig r(x64);
            r.mm.set_errno(0);
            if (x64) {
                r.cpu.sse.xmm[0].u32_[0] = static_cast<u32>(row.x);
                if (toward) r.cpu.sse.xmm[1].u64_[0] = row.y;
                else r.cpu.sse.xmm[1].u32_[0] = static_cast<u32>(row.y);
                (void)r.call(row.fn, {});
            } else if (toward) {
                (void)r.call(row.fn, {row.x, row.y & 0xffffffff, row.y >> 32});
            } else {
                (void)r.call(row.fn, {row.x, row.y});
            }
            EXPECT_EQ(r.float_result(), row.want);
            EXPECT_EQ(r.mm.get_errno(), row.err);
        }
    }
}
