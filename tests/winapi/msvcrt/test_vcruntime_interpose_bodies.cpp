// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_vcruntime_interpose_bodies.cpp
/// @brief Host-body contracts of the vcruntime140 interposer primitives.
///
/// Expected values come from Microsoft Learn (memcpy/memmove/memset/memcmp/
/// memchr/strchr/strrchr/strstr/wcs*) and the winoracle behaviour records in
/// tests/oracle_data/ (vcruntime_memstr_probe*, crt_memcmp_sign_probe*,
/// crt_memcpy_overlap_probe*), never from running Vitreo.

#include "winapi/msvcrt/memory/vcruntime_interpose.hpp"

#include "core/memory/guard_page_traps.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/core/host_in_chain.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/text/crt_string.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
namespace vcrt = vitreo::winapi::vcrt;

namespace {

constexpr GuestAddr kP0 = 0x200000;
constexpr GuestAddr kP1 = kP0 + 0x1000;
constexpr GuestAddr kP2 = kP0 + 0x2000;
constexpr GuestAddr kBuf = 0x300000;   // 64 KiB scratch
constexpr usize kBufSize = 0x10000;

enum class Prot { NoAccess, ReadOnly, Guard };

class VcrtBodies : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(mem.map_region(kP0, 0x3000, MemoryProtection::ReadWrite));
        ASSERT_TRUE(mem.map_region(kBuf, kBufSize, MemoryProtection::ReadWrite));
    }
    void TearDown() override { guard_page::reset_all(); }

    void protect_p1(Prot p) {
        switch (p) {
            case Prot::NoAccess:
                ASSERT_TRUE(mem.protect_region(kP1, 0x1000, MemoryProtection::None));
                break;
            case Prot::ReadOnly:
                ASSERT_TRUE(mem.protect_region(kP1, 0x1000, MemoryProtection::Read));
                break;
            case Prot::Guard:
                guard_page::arm_range(kP0, 0x3000, kP1, kP1 + 0x1000, guard_bits, 3u);
                break;
        }
    }
    static u32 code_for(Prot p) {
        return p == Prot::Guard ? vcrt::kStatusGuardPageViolation
                                : vcrt::kStatusAccessViolation;
    }

    void fill_pattern(GuestAddr a, usize n, u8 seed) {
        for (usize i = 0; i < n; ++i) {
            ASSERT_TRUE(mem.write_u8(a + i, static_cast<u8>(seed + i * 7)));
        }
    }
    std::vector<u8> bytes(GuestAddr a, usize n) {
        std::vector<u8> v(n);
        EXPECT_TRUE(mem.read_bytes(a, v.data(), n));
        return v;
    }
    void put(GuestAddr a, const void* p, usize n) {
        ASSERT_TRUE(mem.write_bytes(a, static_cast<const Byte*>(p), n));
    }

    GuestMemory mem;
    u8 guard_bits[8]{};
};

TEST_F(VcrtBodies, MemcpyIsMemmoveSafe) {
    const int deltas[] = {1, 2, 3, 4, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 1000};
    const u64 sizes[] = {1,  2,  3,   4,   7,   8,   15,  16,  17,   31,   32,  33,  63,
                         64, 65, 100, 127, 128, 129, 255, 256, 257, 1000, 4096, 5000};
    constexpr usize kLen = 12000;
    constexpr GuestAddr kBase = kBuf + 0x100;
    for (int sign : {1, -1}) {
        for (int d : deltas) {
            for (u64 n : sizes) {
                std::vector<u8> ref(kLen);
                for (usize i = 0; i < kLen; ++i) ref[i] = static_cast<u8>(i * 13 + 5);
                put(kBase, ref.data(), kLen);
                const usize src_off = 6000;
                const usize dst_off = static_cast<usize>(static_cast<long>(src_off) + sign * d);
                // In-test reference: memmove through a temporary.
                std::vector<u8> tmp(ref.begin() + static_cast<long>(src_off),
                                    ref.begin() + static_cast<long>(src_off + n));
                std::copy(tmp.begin(), tmp.end(), ref.begin() + static_cast<long>(dst_off));
                auto f = vcrt::move_bytes(mem, kBase + dst_off, kBase + src_off, n);
                ASSERT_FALSE(f.has_value());
                ASSERT_EQ(bytes(kBase, kLen), ref) << "delta=" << sign * d << " n=" << n;
            }
        }
    }
}

TEST_F(VcrtBodies, MemcmpReturnsMinusOneZeroOne) {
    struct Pair { u8 a, b; int r; };
    const Pair pairs[] = {{0x01, 0x80, -1}, {0x80, 0x01, 1}, {0x00, 0xFF, -1},
                          {0x41, 0x42, -1}, {0x42, 0x41, 1}, {0x10, 0x30, -1}};
    const u64 positions[] = {0, 1, 5, 15, 17, 31, 33, 100, 4100};
    const u64 sizes[] = {1, 2, 7, 8, 15, 16, 31, 32, 33, 63, 64, 100, 4096, 8192};
    const GuestAddr a = kBuf, b = kBuf + 0x8000;
    for (const Pair& p : pairs) {
        for (u64 n : sizes) {
            for (u64 pos : positions) {
                if (pos >= n) continue;
                std::vector<u8> buf(n, 0x55);
                put(a, buf.data(), n);
                put(b, buf.data(), n);
                ASSERT_TRUE(mem.write_u8(a + pos, p.a));
                ASSERT_TRUE(mem.write_u8(b + pos, p.b));
                auto r = vcrt::compare_bytes(mem, a, b, n);
                ASSERT_FALSE(r.fault.has_value());
                ASSERT_EQ(r.value, p.r) << "n=" << n << " pos=" << pos;
            }
            std::vector<u8> eq(n, 0x33);
            put(a, eq.data(), n);
            put(b, eq.data(), n);
            ASSERT_EQ(vcrt::compare_bytes(mem, a, b, n).value, 0);
        }
    }
    auto z = vcrt::compare_bytes(mem, 0, 0, 0);
    EXPECT_EQ(z.value, 0);
    EXPECT_FALSE(z.fault.has_value());
}

TEST_F(VcrtBodies, MemsetUsesLowByte) {
    // run_body truncates c to unsigned char; the u8 parameter encodes it.
    ASSERT_FALSE(vcrt::fill_bytes(mem, kBuf, static_cast<u8>(0x1A5), 8));
    EXPECT_EQ(bytes(kBuf, 8), std::vector<u8>(8, 0xA5));
    ASSERT_FALSE(vcrt::fill_bytes(mem, kBuf, static_cast<u8>(-1), 8));
    EXPECT_EQ(bytes(kBuf, 8), std::vector<u8>(8, 0xFF));
}

TEST_F(VcrtBodies, FindByteCharWcharMatchOracle) {
    const char s[] = "abcabc\x80xyz";  // 10 chars + NUL at 10
    put(kBuf, s, sizeof(s));
    const int cs[] = {97, 99, 0, 353, -159, 128, -128, 384, 113};
    const long chr[] = {0, 2, 10, 0, 0, 6, 6, 6, -1};
    const long rchr[] = {3, 5, 10, 3, 3, 6, 6, 6, -1};
    auto off = [](GuestAddr at) { return at == 0 ? -1L : static_cast<long>(at - kBuf); };
    for (usize i = 0; i < std::size(cs); ++i) {
        const u8 c = static_cast<u8>(cs[i]);
        auto a = vcrt::find_char(mem, kBuf, c, false);
        auto b = vcrt::find_char(mem, kBuf, c, true);
        auto d = vcrt::find_byte(mem, kBuf, c, 11);
        EXPECT_FALSE(a.fault || b.fault || d.fault);
        EXPECT_EQ(off(a.at), chr[i]) << "strchr c=" << cs[i];
        EXPECT_EQ(off(b.at), rchr[i]) << "strrchr c=" << cs[i];
        EXPECT_EQ(off(d.at), chr[i]) << "memchr c=" << cs[i];
    }
    const u16 w[] = {'a', 'b', 'c', 'a', 'b', 0x8000, 'x', 'y', 0};
    put(kBuf + 0x100, w, sizeof(w));
    const u32 wc[] = {0x61, 0x62, 0, 0x10061, 0x8000, 0x18000, 0x71};
    const long wchr[] = {0, 1, 8, 0, 5, 5, -1};
    const long wrchr[] = {3, 4, 8, 3, 5, 5, -1};
    auto woff = [](GuestAddr at) {
        return at == 0 ? -1L : static_cast<long>((at - kBuf - 0x100) / 2);
    };
    for (usize i = 0; i < std::size(wc); ++i) {
        const u16 c = static_cast<u16>(wc[i]);
        auto a = vcrt::find_wchar(mem, kBuf + 0x100, c, false);
        auto b = vcrt::find_wchar(mem, kBuf + 0x100, c, true);
        EXPECT_FALSE(a.fault || b.fault);
        EXPECT_EQ(woff(a.at), wchr[i]) << "wcschr c=" << wc[i];
        EXPECT_EQ(woff(b.at), wrchr[i]) << "wcsrchr c=" << wc[i];
    }
}

TEST_F(VcrtBodies, FindStrMatchesOracle) {
    put(kBuf, "abcab", 6);
    put(kBuf + 0x40, "", 1);
    put(kBuf + 0x50, "cab", 4);
    put(kBuf + 0x60, "abd", 4);
    put(kBuf + 0x70, "ab", 3);
    put(kBuf + 0x80, "abc", 4);
    EXPECT_EQ(vcrt::find_str(mem, kBuf, kBuf + 0x40).at, kBuf);
    EXPECT_EQ(vcrt::find_str(mem, kBuf, kBuf + 0x50).at, kBuf + 2);
    EXPECT_EQ(vcrt::find_str(mem, kBuf, kBuf + 0x60).at, 0u);
    EXPECT_EQ(vcrt::find_str(mem, kBuf, kBuf).at, kBuf);
    EXPECT_EQ(vcrt::find_str(mem, kBuf + 0x70, kBuf + 0x80).at, 0u);

    auto putw = [&](GuestAddr a, const char* s) {
        for (usize i = 0;; ++i) {
            ASSERT_TRUE(mem.write_u16(a + i * 2, static_cast<u8>(s[i])));
            if (s[i] == 0) break;
        }
    };
    const GuestAddr w = kBuf + 0x1000;
    putw(w, "abcab");
    putw(w + 0x40, "");
    putw(w + 0x50, "cab");
    putw(w + 0x60, "abd");
    putw(w + 0x70, "ab");
    putw(w + 0x80, "abc");
    EXPECT_EQ(vcrt::find_wstr(mem, w, w + 0x40).at, w);
    EXPECT_EQ(vcrt::find_wstr(mem, w, w + 0x50).at, w + 4);
    EXPECT_EQ(vcrt::find_wstr(mem, w, w + 0x60).at, 0u);
    EXPECT_EQ(vcrt::find_wstr(mem, w, w).at, w);
    EXPECT_EQ(vcrt::find_wstr(mem, w + 0x70, w + 0x80).at, 0u);
}

class VcrtBodiesFault : public VcrtBodies,
                        public ::testing::WithParamInterface<Prot> {};

TEST_P(VcrtBodiesFault, MoveFaultsLikeWindows) {
    const Prot p = GetParam();
    for (u64 n : {200u, 300u, 1000u, 5000u}) {
        fill_pattern(kBuf, 6000, static_cast<u8>(n));
        if (p != Prot::NoAccess) fill_pattern(kP1, 16, 0xEE);  // before protect
        protect_p1(p);
        const std::vector<u8> p1_before =
            p == Prot::ReadOnly ? bytes(kP1, 0x1000) : std::vector<u8>{};
        auto f = vcrt::move_bytes(mem, kP1 - 100, kBuf, n);
        ASSERT_TRUE(f.has_value()) << n;
        EXPECT_TRUE(f->write);
        EXPECT_EQ(f->address, kP1);
        EXPECT_EQ(f->code, code_for(p));
        EXPECT_EQ(bytes(kP1 - 100, 100), bytes(kBuf, 100));
        if (p == Prot::ReadOnly) EXPECT_EQ(bytes(kP1, 0x1000), p1_before);
        // Restore for the next size.
        guard_page::reset_all();
        std::memset(guard_bits, 0, sizeof(guard_bits));
        ASSERT_TRUE(mem.protect_region(kP1, 0x1000, MemoryProtection::ReadWrite));
    }
}

TEST_P(VcrtBodiesFault, MoveReadFaultsLikeWindows) {
    const Prot p = GetParam();
    if (p == Prot::ReadOnly) GTEST_SKIP() << "a readable source cannot fault";
    fill_pattern(kP1 - 100, 100, 3);
    protect_p1(p);
    auto f = vcrt::move_bytes(mem, kBuf, kP1 - 100, 1000);
    ASSERT_TRUE(f.has_value());
    EXPECT_FALSE(f->write);
    EXPECT_GE(f->address, kP1);
    EXPECT_LT(f->address, kP1 + 0x1000);
    EXPECT_EQ(f->code, code_for(p));
    EXPECT_EQ(bytes(kBuf, 100), bytes(kP1 - 100, 100));
}

TEST_P(VcrtBodiesFault, BackwardMoveFaultsOnHighestSourcePage) {
    const Prot p = GetParam();
    if (p == Prot::ReadOnly) GTEST_SKIP() << "dst overlaps P1 only through src here";
    // src = P1-150 .. P1-150+5000 (into P2 and past); dst = src + 50.
    // Map a P3 so the tail lies in mapped memory; the highest unreadable page
    // the backward copy touches first is P1 (P2 and P3 are readable).
    ASSERT_TRUE(mem.map_region(kP2 + 0x1000, 0x2000, MemoryProtection::ReadWrite));
    protect_p1(p);
    auto f = vcrt::move_bytes(mem, kP1 - 100, kP1 - 150, 5000);
    ASSERT_TRUE(f.has_value());
    EXPECT_FALSE(f->write);
    EXPECT_GE(f->address, kP1);
    EXPECT_LT(f->address, kP1 + 0x1000);
    EXPECT_EQ(f->code, code_for(p));
}

TEST_P(VcrtBodiesFault, FillFaultsLikeWindows) {
    const Prot p = GetParam();
    protect_p1(p);
    auto f = vcrt::fill_bytes(mem, kP1 - 100, 0x5A, 300);
    ASSERT_TRUE(f.has_value());
    EXPECT_TRUE(f->write);
    EXPECT_EQ(f->address, kP1);
    EXPECT_EQ(f->code, code_for(p));
    EXPECT_EQ(bytes(kP1 - 100, 100), std::vector<u8>(100, 0x5A));
}

INSTANTIATE_TEST_SUITE_P(Prot, VcrtBodiesFault,
                         ::testing::Values(Prot::NoAccess, Prot::ReadOnly, Prot::Guard));

TEST_F(VcrtBodies, StopsBeforeInaccessiblePage) {
    // memcmp: a difference before P1 ends the compare without a fault.
    const GuestAddr a = kP1 - 100;
    for (u64 diff : {2u, 90u}) {
        fill_pattern(a, 100, 1);
        fill_pattern(kBuf, 100, 1);
        ASSERT_TRUE(mem.write_u8(kBuf + diff, 0xFF));
        ASSERT_TRUE(mem.write_u8(a + diff, 0x00));
        protect_p1(Prot::NoAccess);
        auto r = vcrt::compare_bytes(mem, kBuf, a, 5000);
        EXPECT_EQ(r.value, 1);
        EXPECT_FALSE(r.fault.has_value());
        ASSERT_TRUE(mem.protect_region(kP1, 0x1000, MemoryProtection::ReadWrite));
    }
    fill_pattern(a, 100, 1);
    fill_pattern(kBuf, 100, 1);
    protect_p1(Prot::NoAccess);
    {
        auto r = vcrt::compare_bytes(mem, kBuf, a, 5000);
        ASSERT_TRUE(r.fault.has_value());
        EXPECT_FALSE(r.fault->write);
        EXPECT_EQ(r.fault->address, kP1);
    }
    // memchr: hit before P1 -> no fault; miss -> fault at P1.
    ASSERT_TRUE(mem.protect_region(kP1, 0x1000, MemoryProtection::ReadWrite));
    std::vector<u8> z(100, 0x11);
    z[40] = 0x77;
    z[97] = 0x66;
    put(a, z.data(), z.size());
    protect_p1(Prot::NoAccess);
    EXPECT_EQ(vcrt::find_byte(mem, a, 0x77, 5000).at, a + 40);
    EXPECT_EQ(vcrt::find_byte(mem, a, 0x66, 5000).at, a + 97);
    EXPECT_FALSE(vcrt::find_byte(mem, a, 0x66, 5000).fault.has_value());
    {
        auto r = vcrt::find_byte(mem, a, 0x99, 5000);
        ASSERT_TRUE(r.fault.has_value());
        EXPECT_EQ(r.fault->address, kP1);
        EXPECT_FALSE(r.fault->write);
    }
    ASSERT_TRUE(mem.protect_region(kP1, 0x1000, MemoryProtection::ReadWrite));

    // Narrow string of 63 'x' + NUL ending at P1-1.
    const GuestAddr s = kP1 - 64;
    put(s, std::string(63, 'x').c_str(), 64);
    put(kBuf, "xy", 3);
    protect_p1(Prot::NoAccess);
    {
        auto r1 = vcrt::find_char(mem, s, 'q', false);
        auto r2 = vcrt::find_char(mem, s, 'x', true);
        auto r3 = vcrt::find_str(mem, s, kBuf);
        EXPECT_EQ(r1.at, 0u);
        EXPECT_EQ(r2.at, s + 62);
        EXPECT_EQ(r3.at, 0u);
        EXPECT_FALSE(r1.fault || r2.fault || r3.fault);
    }
    ASSERT_TRUE(mem.protect_region(kP1, 0x1000, MemoryProtection::ReadWrite));

    // Wide: 31 units + NUL ending at P1-1, and odd-aligned at P1-63:
    // 29 units + NUL, whose last byte is P1-4 (the oracle's layout).
    for (GuestAddr ws : {kP1 - 64, kP1 - 63}) {
        const int units = ws & 1 ? 29 : 31;
        ASSERT_TRUE(mem.protect_region(kP1, 0x1000, MemoryProtection::ReadWrite));
        for (int i = 0; i < units; ++i) ASSERT_TRUE(mem.write_u16(ws + i * 2, 'x'));
        ASSERT_TRUE(mem.write_u16(ws + units * 2, 0));
        for (int i = 0; i < 3; ++i) ASSERT_TRUE(mem.write_u16(kBuf + 0x200 + i * 2, "xy"[i]));
        protect_p1(Prot::NoAccess);
        auto r1 = vcrt::find_wchar(mem, ws, 'q', false);
        auto r2 = vcrt::find_wchar(mem, ws, 'x', true);
        auto r3 = vcrt::find_wstr(mem, ws, kBuf + 0x200);
        EXPECT_EQ(r1.at, 0u);
        EXPECT_EQ(r2.at, ws + static_cast<GuestAddr>(units - 1) * 2);
        EXPECT_EQ(r3.at, 0u);
        EXPECT_FALSE(r1.fault || r2.fault || r3.fault) << std::hex << ws;
    }
    ASSERT_TRUE(mem.protect_region(kP1, 0x1000, MemoryProtection::ReadWrite));

    // 64 'x' running into P1.
    put(s, std::string(64, 'x').data(), 64);
    protect_p1(Prot::NoAccess);
    {
        auto r1 = vcrt::find_char(mem, s, 'q', false);
        ASSERT_TRUE(r1.fault.has_value());
        EXPECT_EQ(r1.fault->address, kP1);
        EXPECT_FALSE(r1.fault->write);
        auto r2 = vcrt::find_char(mem, s, 'x', false);
        EXPECT_EQ(r2.at, s);
        EXPECT_FALSE(r2.fault.has_value());
    }
    // Wide 32 'x' into P1.
    ASSERT_TRUE(mem.protect_region(kP1, 0x1000, MemoryProtection::ReadWrite));
    for (int i = 0; i < 32; ++i) ASSERT_TRUE(mem.write_u16(s + i * 2, 'x'));
    protect_p1(Prot::NoAccess);
    {
        auto r = vcrt::find_wchar(mem, s, 'x', true);
        ASSERT_TRUE(r.fault.has_value());
        EXPECT_EQ(r.fault->address, kP1);
    }
}

TEST_F(VcrtBodies, GuardFiresOnce) {
    protect_p1(Prot::Guard);
    auto f = vcrt::fill_bytes(mem, kP1 - 4, 1, 16);
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->code, vcrt::kStatusGuardPageViolation);
    // deliver() disarms; emulate that step here (no Marshaler in this suite).
    ASSERT_TRUE(guard_page::handle_fault(f->address));
    EXPECT_FALSE(vcrt::fill_bytes(mem, kP1 - 4, 1, 16).has_value());
}

TEST_F(VcrtBodies, ZeroLengthTouchesNothing) {
    EXPECT_FALSE(vcrt::move_bytes(mem, 0, 0, 0).has_value());
    EXPECT_FALSE(vcrt::fill_bytes(mem, 0, 0, 0).has_value());
    EXPECT_FALSE(vcrt::compare_bytes(mem, 0, 0, 0).fault.has_value());
    auto r = vcrt::find_byte(mem, 0, 0, 0);
    EXPECT_EQ(r.at, 0u);
    EXPECT_FALSE(r.fault.has_value());
}

TEST_F(VcrtBodies, WrapFaultsNotCaps) {
    // Two equal 128 KiB ranges followed by an unmapped page; n = 256 KiB.
    constexpr GuestAddr kA = 0x400000, kB = 0x500000;
    ASSERT_TRUE(mem.map_region(kA, 0x20000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(mem.map_region(kB, 0x20000, MemoryProtection::ReadWrite));
    auto r = vcrt::compare_bytes(mem, kA, kB, 0x40000);
    ASSERT_TRUE(r.fault.has_value());
    EXPECT_FALSE(r.fault->write);
    EXPECT_EQ(r.fault->address, kA + 0x20000);
}

// ---------------------------------------------------------------------------
// Task 2: internal class-A host entries and the raise path
// ---------------------------------------------------------------------------

TEST(VcrtBodiesEntries, InternalVcrtExportsAreHiddenAndClassA) {
    host_in_chain::force_for_test(host_in_chain::Force::On);
    GuestMemory memory;
    capsule::Vfs vfs;
    CrtMemoryManager allocation(memory);
    CrtStringManager strings;
    ThunkRegistry registry(0x7E000000);
    ASSERT_TRUE(registry.register_dll("ucrtbase.dll",
                                      create_crt_string_thunks(strings, allocation, vfs)));
    for (usize i = 0; i < vcrt::kFnCount; ++i) {
        const auto fn = static_cast<vcrt::Fn>(i);
        const char* name = vcrt::internal_export_name(fn);
        EXPECT_TRUE(registry.get_thunk_address("ucrtbase.dll", name).is_err()) << name;
        auto slot = registry.get_thunk_address("ucrtbase.dll", name, true);
        ASSERT_TRUE(slot.is_ok()) << name;
        bool is_stub = false;
        const DirectThunk* d = registry.in_chain_candidate(slot.value(), is_stub);
        ASSERT_NE(d, nullptr) << name;
        EXPECT_TRUE(is_stub);
        EXPECT_EQ(d->in_chain, InChainClass::A) << name;
        EXPECT_TRUE(d->in_chain_only) << name;
    }
    host_in_chain::force_for_test(host_in_chain::Force::None);
}

class VcrtBodiesRun : public VcrtBodies {
protected:
    // Call @p fn with @p args through run_body on a fresh CPU of the given
    // bitness; x86 args are cdecl on the stack, x64 in RCX/RDX/R8.
    cpu::CpuState call(bool x64, vcrt::Fn fn, std::initializer_list<u64> args) {
        cpu::CpuState cpu;
        if (x64) {
            cpu.init_long_mode();
            const u64* a = args.begin();
            if (args.size() > 0) cpu.gpr.rcx = a[0];
            if (args.size() > 1) cpu.gpr.rdx = a[1];
            if (args.size() > 2) cpu.gpr.r8 = a[2];
            cpu.set_sp(kBuf + 0xF000);
        } else {
            cpu.init_protected_mode();
            const GuestAddr sp = kBuf + 0xF000;
            cpu.set_sp(sp);
            EXPECT_TRUE(mem.write_u32(sp, 0x401000));
            usize k = 0;
            for (u64 v : args) EXPECT_TRUE(mem.write_u32(sp + 4 + 4 * k++, static_cast<u32>(v)));
        }
        Marshaler m(mem);
        m.attach_cpu(cpu);
        vcrt::run_body(fn, m);
        return cpu;
    }
};

TEST_F(VcrtBodiesRun, RunBodyRaisesThroughPendingRaise) {
    protect_p1(Prot::NoAccess);
    for (bool x64 : {false, true}) {
        auto cpu = call(x64, vcrt::Fn::Memcpy, {kP1 - 100, kBuf, 300});
        ASSERT_TRUE(cpu.pending_raise.active) << x64;
        EXPECT_EQ(cpu.pending_raise.code, vcrt::kStatusAccessViolation);
        EXPECT_EQ(cpu.pending_raise.num_args, 2u);
        EXPECT_EQ(cpu.pending_raise.args[0], 1u);
        EXPECT_EQ(cpu.pending_raise.args[1], kP1);
    }
}

TEST_F(VcrtBodiesRun, RunBodyRaisesGuardOnceAndDisarms) {
    protect_p1(Prot::Guard);
    auto cpu = call(true, vcrt::Fn::Memset, {kP1 - 4, 0x41, 16});
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, vcrt::kStatusGuardPageViolation);
    EXPECT_EQ(cpu.pending_raise.args[0], 1u);
    EXPECT_EQ(cpu.pending_raise.args[1], kP1);
    EXPECT_FALSE(guard_page::is_armed(kP1));
    auto again = call(true, vcrt::Fn::Memset, {kP1 - 4, 0x41, 16});
    EXPECT_FALSE(again.pending_raise.active);
}

TEST_F(VcrtBodiesRun, RunBodyReturnsDest) {
    for (bool x64 : {false, true}) {
        auto ret = [&](const cpu::CpuState& c) { return x64 ? c.gpr.rax : (c.gpr.rax & 0xFFFFFFFFu); };
        for (vcrt::Fn fn : {vcrt::Fn::Memcpy, vcrt::Fn::Memmove}) {
            auto c = call(x64, fn, {kBuf + 0x100, kBuf, 16});
            EXPECT_FALSE(c.pending_raise.active);
            EXPECT_EQ(ret(c), kBuf + 0x100);
        }
        auto c = call(x64, vcrt::Fn::Memset, {kBuf + 0x200, 0x1A5, 8});
        EXPECT_EQ(ret(c), kBuf + 0x200);
        EXPECT_EQ(bytes(kBuf + 0x200, 8), std::vector<u8>(8, 0xA5));

        ASSERT_TRUE(mem.write_u8(kBuf + 0x300, 0x10));
        ASSERT_TRUE(mem.write_u8(kBuf + 0x400, 0x30));
        auto r = call(x64, vcrt::Fn::Memcmp, {kBuf + 0x300, kBuf + 0x400, 1});
        if (x64) {
            EXPECT_EQ(static_cast<i64>(r.gpr.rax), -1);
        } else {
            EXPECT_EQ(r.gpr.rax & 0xFFFFFFFFu, 0xFFFFFFFFu);
        }
        put(kBuf + 0x500, "abcabc", 7);
        auto f = call(x64, vcrt::Fn::Strchr, {kBuf + 0x500, 0x163});  // c -> 'c'
        EXPECT_EQ(ret(f), kBuf + 0x502);
        auto g = call(x64, vcrt::Fn::Memchr, {kBuf + 0x500, 'z', 7});
        EXPECT_EQ(ret(g), 0u);
    }
}

// A faulted move/fill reports its progress in copy order, and a call
// resumed with that progress completes exactly what one uninterrupted
// memmove/memset would have written -- including an overlapping move whose
// source the first part already overwrote (where a restart would corrupt).
TEST_F(VcrtBodies, MoveResumesFromItsProgress) {
    struct Shape { const char* name; GuestAddr src, dst; u64 n; };
    const Shape shapes[] = {
        {"forward-overlap", kP1 - 983, kP1 - 1000, 1800},
        {"backward-overlap", kP1 + 0x1000 - 50, kP1 + 0x1000 - 33, 1500},
        {"disjoint", kP0 + 0x100, kP1 - 300, 900},
    };
    for (const Shape& sh : shapes) {
        SCOPED_TRACE(sh.name);
        fill_pattern(kBuf, 0x1000, 9);
        fill_pattern(kP1 - 0x1000, 0x3000, 21);
        const GuestAddr lo = std::min(sh.src, sh.dst), hi = std::max(sh.src, sh.dst) + sh.n;
        std::vector<u8> want = bytes(lo, hi - lo);
        std::memmove(want.data() + (sh.dst - lo), want.data() + (sh.src - lo), sh.n);
        protect_p1(Prot::NoAccess);
        u64 done = ~u64{0};
        auto f = vcrt::move_bytes(mem, sh.dst, sh.src, sh.n, 0, &done);
        ASSERT_TRUE(f.has_value());
        EXPECT_GT(done, 0u) << "faulted mid-copy";
        EXPECT_LT(done, sh.n);
        ASSERT_TRUE(mem.protect_region(kP1, 0x1000, MemoryProtection::ReadWrite));
        u64 done2 = 0;
        EXPECT_FALSE(vcrt::move_bytes(mem, sh.dst, sh.src, sh.n, done, &done2).has_value());
        EXPECT_EQ(done2, sh.n);
        EXPECT_EQ(bytes(lo, hi - lo), want);
    }
}

TEST_F(VcrtBodies, RestartingAnOverlappingMoveWouldCorrupt) {
    // Guards the test above: the shape really is one where re-running the
    // whole call after a partial copy gives a different result.
    fill_pattern(kP1 - 0x1000, 0x3000, 21);
    const GuestAddr src = kP1 - 983, dst = kP1 - 1000;
    const u64 n = 1800;
    std::vector<u8> want = bytes(dst, n + 17);
    std::memmove(want.data(), want.data() + 17, n);
    protect_p1(Prot::NoAccess);
    ASSERT_TRUE(vcrt::move_bytes(mem, dst, src, n).has_value());
    ASSERT_TRUE(mem.protect_region(kP1, 0x1000, MemoryProtection::ReadWrite));
    EXPECT_FALSE(vcrt::move_bytes(mem, dst, src, n).has_value());  // restart
    EXPECT_NE(bytes(dst, n + 17), want);
}

TEST_F(VcrtBodies, FillResumesFromItsProgress) {
    fill_pattern(kP1 - 0x1000, 0x3000, 4);
    protect_p1(Prot::NoAccess);
    u64 done = 0;
    ASSERT_TRUE(vcrt::fill_bytes(mem, kP1 - 100, 0x77, 0x1000, 0, &done).has_value());
    EXPECT_EQ(done, 100u);
    ASSERT_TRUE(mem.protect_region(kP1, 0x1000, MemoryProtection::ReadWrite));
    EXPECT_FALSE(vcrt::fill_bytes(mem, kP1 - 100, 0x77, 0x1000, done).has_value());
    EXPECT_EQ(bytes(kP1 - 100, 0x1000), std::vector<u8>(0x1000, 0x77));
    // A progress beyond n (a handler rewrote it) restarts from 0.
    EXPECT_FALSE(vcrt::fill_bytes(mem, kBuf, 0x11, 8, 999).has_value());
    EXPECT_EQ(bytes(kBuf, 8), std::vector<u8>(8, 0x11));
}

}  // namespace
