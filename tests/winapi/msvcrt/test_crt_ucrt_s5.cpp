/// @file test_crt_ucrt_s5.cpp
/// @brief mbstowcs_s, mbsrtowcs_s, wcsrtombs_s, bsearch_s and the
///        fmin/fminf/fminl/fmaxf/fmaxl family against real-Windows ucrtbase
///        outputs, both ABIs. Expected values: oracle/crt_ucrt_s5_probe.c run
///        on winoracle (crt_ucrt_s5_oracle.inc is generated from its output).

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include "capsule/vfs/vfs.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/msvcrt/math/crt_math.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/text/crt_string.hpp"

#include <algorithm>
#include <cstring>
#include <string>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {
#include "crt_ucrt_s5_oracle.inc"

constexpr GuestAddr kBase = 0x10000;
constexpr GuestAddr kStack = 0x18000;
constexpr GuestAddr kBuffer = 0x11000;
constexpr GuestAddr kRetval = 0x11100;
constexpr GuestAddr kSrcPtr = 0x11200;
constexpr GuestAddr kState = 0x11300;
constexpr GuestAddr kNarrow = 0x12000;   // + 0x100 per string
constexpr GuestAddr kWide = 0x13000;     // + 0x100 per string
constexpr GuestAddr kKeys = 0x14000;
constexpr GuestAddr kKey = 0x14100;
constexpr GuestAddr kContext = 0x14200;
constexpr GuestAddr kHandler = 0x1234;
constexpr GuestAddr kCompare = 0x5000;

const ExportEntry* find(const DllExports& exports, const char* name) {
    auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                           [name](const ExportEntry& e) { return e.name == name; });
    return it == exports.entries.end() ? nullptr : &*it;
}

void set_args(cpu::CpuState& cpu, GuestMemory& memory, bool x64,
              std::initializer_list<u64> args) {
    usize i = 0;
    for (u64 a : args) {
        if (x64) {
            switch (i) {
                case 0: cpu.gpr.rcx = a; break;
                case 1: cpu.gpr.rdx = a; break;
                case 2: cpu.gpr.r8 = a; break;
                case 3: cpu.gpr.r9 = a; break;
                default: ASSERT_TRUE(memory.write_u64(kStack + 8 * (i + 1), a).is_ok());
            }
        } else {
            ASSERT_TRUE(memory.write_u32(kStack + 4 + 4 * i, static_cast<u32>(a)).is_ok());
        }
        ++i;
    }
}

struct Harness {
    GuestMemory memory;
    capsule::Vfs vfs;
    CrtMemoryManager crt{memory, 0x20000000, 0x10000};
    CrtStringManager strings;
    CrtMathManager math;
    DllExports string_exports, memory_exports, math_exports;
    cpu::CpuState cpu;
    Marshaler m{memory};
    unsigned iph_calls = 0, iph_errno = 0;
    unsigned cmp_calls = 0, cmp_bad = 0, visited = 0;

    explicit Harness(bool x64) {
        EXPECT_TRUE(memory.map_region(kBase, 0x10000, MemoryProtection::ReadWrite).is_ok());
        string_exports = create_crt_string_thunks(strings, crt, vfs);
        memory_exports = create_crt_memory_thunks(crt);
        math_exports = create_crt_math_thunks(math, crt);
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(kStack);
        m.attach_cpu(cpu);
        crt.exchange_invalid_parameter_handler(kHandler);
        crt.set_guest_caller([this](GuestAddr fn, const u64* args, usize count) -> u64 {
            if (fn == kHandler) {
                EXPECT_EQ(count, 5u);
                ++iph_calls;
                iph_errno = crt.get_errno();
                return 0;
            }
            EXPECT_EQ(fn, kCompare);
            EXPECT_EQ(count, 3u);
            ++cmp_calls;
            if (args[0] != kContext) cmp_bad |= 1;
            const GuestAddr element = static_cast<GuestAddr>(args[2]);
            if (element < kKeys || element >= kKeys + 20) cmp_bad |= 2;
            else visited = (visited << 4) | static_cast<unsigned>((element - kKeys) / 4);
            if (args[1] == 0) { cmp_bad |= 4; return static_cast<u32>(-1); }
            const i32 key = static_cast<i32>(memory.read_u32(static_cast<GuestAddr>(args[1])).value());
            const i32 elem = static_cast<i32>(memory.read_u32(element).value());
            return static_cast<u32>(key - elem);
        });
    }
};

}  // namespace

TEST(CrtUcrtS5Test, SecureConversionsMatchWindowsBothAbis) {
    const char* narrow[] = {"abcd", "", "\x80\xe9\xff", "a\x81\x62"};
    const u16 wide[][5] = {{'a', 'b', 'c', 'd', 0}, {0}, {0x80, 0xe9, 0xff, 0}, {'a', 0x100, 'b', 0}};
    const char* names[] = {"mbstowcs_s", "mbsrtowcs_s", "wcsrtombs_s"};
    for (bool x64 : {false, true}) {
        Harness h(x64);
        for (u32 s = 0; s < 4; ++s) {
            for (u32 i = 0; i <= std::strlen(narrow[s]); ++i)
                ASSERT_TRUE(h.memory.write_u8(kNarrow + 0x100 * s + i, static_cast<u8>(narrow[s][i])).is_ok());
            for (u32 i = 0; i < 5; ++i)
                ASSERT_TRUE(h.memory.write_u16(kWide + 0x100 * s + i * 2, wide[s][i]).is_ok());
        }
        const auto wide_arg = [x64](u64 v) { return x64 || v != kTrunc ? v : 0xFFFFFFFFull; };
        for (const auto& row : kS5ConvOracle) {
            const auto& c = kS5ConvCases[row.cse];
            SCOPED_TRACE(std::string(x64 ? "x64 " : "x86 ") + names[row.fn] + " case " + std::to_string(row.cse));
            const bool to_narrow = row.fn == 2;
            const GuestAddr src = c.src >= 4 ? 0 : (to_narrow ? kWide : kNarrow) + 0x100 * c.src;
            for (u32 i = 0; i < 8; ++i) {
                if (to_narrow) ASSERT_TRUE(h.memory.write_u8(kBuffer + i, 0xcc).is_ok());
                else ASSERT_TRUE(h.memory.write_u16(kBuffer + i * 2, 0xcccc).is_ok());
            }
            ASSERT_TRUE(h.memory.write_u64(kRetval, kUntouched).is_ok());
            ASSERT_TRUE(h.memory.write_u64(kState, 0).is_ok());
            if (x64) ASSERT_TRUE(h.memory.write_u64(kSrcPtr, src).is_ok());
            else ASSERT_TRUE(h.memory.write_u32(kSrcPtr, static_cast<u32>(src)).is_ok());
            const GuestAddr ret = c.null_ret ? 0 : kRetval;
            const GuestAddr dst = c.dst ? kBuffer : 0;
            if (row.fn == 0) {
                set_args(h.cpu, h.memory, x64, {ret, dst, wide_arg(c.size), src, wide_arg(c.count)});
            } else {
                const GuestAddr pp = c.src == 5 ? 0 : kSrcPtr;
                set_args(h.cpu, h.memory, x64, {ret, dst, wide_arg(c.size), pp, wide_arg(c.count), kState});
            }
            h.crt.set_errno(0x1234);
            h.iph_calls = h.iph_errno = 0;
            set_last_error(0xdead);
            const ExportEntry* e = find(h.string_exports, names[row.fn]);
            ASSERT_NE(e, nullptr);
            EXPECT_EQ(e->stdcall_bytes, 0u);
            e->handler(h.m);
            EXPECT_EQ(static_cast<u32>(h.cpu.gpr.rax), row.ret);
            EXPECT_EQ(h.crt.get_errno(), row.err);
            EXPECT_EQ(h.iph_calls, row.iph_calls);
            EXPECT_EQ(h.iph_errno, row.iph_errno);
            EXPECT_EQ(get_last_error(), row.lasterr);
            const u64 retval = x64 ? h.memory.read_u64(kRetval).value() : h.memory.read_u32(kRetval).value();
            EXPECT_EQ(retval, x64 ? row.retval : (row.retval & 0xFFFFFFFFu));
            if (row.off != 0xeeee) {
                const u64 now = x64 ? h.memory.read_u64(kSrcPtr).value() : h.memory.read_u32(kSrcPtr).value();
                const u32 off = now == 0 ? 0xffffu : static_cast<u32>((now - src) / (to_narrow ? 2 : 1));
                EXPECT_EQ(off, row.off);
            }
            EXPECT_EQ(h.memory.read_u64(kState).value(), 0u);
            for (u32 i = 0; i < 8; ++i) {
                const u32 unit = to_narrow ? h.memory.read_u8(kBuffer + i).value()
                                           : h.memory.read_u16(kBuffer + i * 2).value();
                EXPECT_EQ(unit, row.buf[i]) << "unit " << i;
            }
        }
    }
}

TEST(CrtUcrtS5Test, BsearchSMatchesWindowsBothAbis) {
    for (bool x64 : {false, true}) {
        Harness h(x64);
        for (u32 i = 0; i < 5; ++i) ASSERT_TRUE(h.memory.write_u32(kKeys + 4 * i, 1 + 2 * i).is_ok());
        const ExportEntry* e = find(h.memory_exports, "bsearch_s");
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(e->stdcall_bytes, 0u);
        for (const auto& row : kS5BsearchOracle) {
            const auto& c = kS5BsearchCases[row.cse];
            SCOPED_TRACE(std::string(x64 ? "x64" : "x86") + " case " + std::to_string(row.cse));
            ASSERT_TRUE(h.memory.write_u32(kKey, static_cast<u32>(c.key)).is_ok());
            set_args(h.cpu, h.memory, x64, {c.null_key ? 0u : kKey, c.null_base ? 0u : kKeys,
                                            c.number, c.width, c.null_cmp ? 0u : kCompare, kContext});
            h.crt.set_errno(0x1234);
            h.iph_calls = h.iph_errno = h.cmp_calls = h.cmp_bad = h.visited = 0;
            e->handler(h.m);
            const u64 result = x64 ? h.cpu.gpr.rax : static_cast<u32>(h.cpu.gpr.rax);
            EXPECT_EQ(result == 0 ? 0xffu : static_cast<u32>((result - kKeys) / 4), row.index);
            EXPECT_EQ(h.crt.get_errno(), row.err);
            EXPECT_EQ(h.iph_calls, row.iph_calls);
            EXPECT_EQ(h.iph_errno, row.iph_errno);
            EXPECT_EQ(h.cmp_calls, row.cmp_calls);
            EXPECT_EQ(h.cmp_bad, row.cmp_bad);
            EXPECT_EQ(h.visited, row.visited);
        }
    }
}

TEST(CrtUcrtS5Test, MinMaxFamilyMatchesWindowsBothAbis) {
    for (bool x64 : {false, true}) {
        Harness h(x64);
        for (const auto& row : kS5MinMaxOracle) {
            SCOPED_TRACE(std::string(x64 ? "x64 " : "x86 ") + row.name + " " +
                         std::to_string(row.i) + "," + std::to_string(row.j));
            const bool is_float = row.name[std::strlen(row.name) - 1] == 'f';
            const ExportEntry* e = find(h.math_exports, row.name);
            ASSERT_NE(e, nullptr);
            h.cpu.fpu.status_word = 0;
            h.cpu.sse.mxcsr = 0x1f80;
            if (is_float) {
                const u32 a = kS5FloatInputs[row.i], b = kS5FloatInputs[row.j];
                if (x64) { h.cpu.sse.xmm[0].u64_[0] = a; h.cpu.sse.xmm[1].u64_[0] = b; }
                else set_args(h.cpu, h.memory, false, {a, b});
            } else {
                const u64 a = kS5DoubleInputs[row.i], b = kS5DoubleInputs[row.j];
                if (x64) { h.cpu.sse.xmm[0].u64_[0] = a; h.cpu.sse.xmm[1].u64_[0] = b; }
                else set_args(h.cpu, h.memory, false, {a & 0xFFFFFFFFu, a >> 32, b & 0xFFFFFFFFu, b >> 32});
            }
            e->handler(h.m);
            const u32 flags = x64 ? h.cpu.sse.mxcsr & 3u : h.cpu.fpu.status_word & 3u;
            const u32 expected = x64 ? row.flags64 : row.flags32;
            EXPECT_EQ(flags, ((expected & 0x10u) ? 1u : 0u) | ((expected & 0x80000u) ? 2u : 0u));
            u64 result;
            if (is_float) {
                u32 bits;
                if (x64) bits = h.cpu.sse.xmm[0].u32_[0];
                else { const float f = static_cast<float>(h.cpu.fpu.pop().to_double()); std::memcpy(&bits, &f, 4); }
                result = bits;
            } else if (x64) {
                result = h.cpu.sse.xmm[0].u64_[0];
            } else {
                const double d = h.cpu.fpu.pop().to_double();
                std::memcpy(&result, &d, 8);
            }
            EXPECT_EQ(result, x64 ? row.result64 : row.result32);
        }
    }
}
