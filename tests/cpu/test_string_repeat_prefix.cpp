// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// F3 (REP) and F2 (REPNZ) are the same prefix byte class. The SDM gives them
// separate mnemonics only because CMPS and SCAS additionally terminate on ZF;
// MOVS, STOS and LODS have no comparison to terminate on, so for those both
// forms mean the same thing -- repeat RCX times.
//
// Vitreo honoured only F3 for the three non-comparing forms, so `REPNE MOVSB`
// copied nothing. Found by tests/cpu/test_hardware_differential.cpp, which
// executes the same bytes on a real x86 core.

#include <gtest/gtest.h>

#include <initializer_list>
#include <memory>
#include <vector>

#include "core/cpu/interpreter/interpreter.hpp"

using namespace vitreo;
using namespace vitreo::cpu;

namespace {

constexpr u8 kRcx = static_cast<u8>(RegIndex::RCX);
constexpr u8 kRsi = static_cast<u8>(RegIndex::RSI);
constexpr u8 kRdi = static_cast<u8>(RegIndex::RDI);
constexpr u8 kRax = static_cast<u8>(RegIndex::RAX);

constexpr u64 kCode = 0x10000;
constexpr u64 kSrc = 0x12000;
constexpr u64 kDst = 0x13000;

// One instruction on freshly mapped memory. Each case gets its own machine so
// that a loop over prefixes cannot re-map an already-mapped region.
struct Machine {
    GuestMemory mem;
    CpuState cpu;

    Machine() {
        EXPECT_TRUE(mem.map_region(kCode, 0x10000, MemoryProtection::ReadWrite));
        u8 pattern[16];
        for (u8 i = 0; i < 16; ++i) pattern[i] = static_cast<u8>(0xA0 + i);
        EXPECT_TRUE(mem.write_bytes(kSrc, reinterpret_cast<const Byte*>(pattern),
                                    sizeof(pattern)));
        cpu.init_long_mode();
        cpu.rip = kCode;
        cpu.flags.rflags = 0x202;   // DF clear, ZF clear
    }

    void step(std::initializer_list<u8> code) {
        const std::vector<u8> bytes(code);
        EXPECT_TRUE(mem.write_bytes(kCode, reinterpret_cast<const Byte*>(bytes.data()),
                                    bytes.size()));
        Interpreter interp(cpu, mem);
        interp.step();
    }
};

}  // namespace

TEST(StringRepeatPrefix, RepAndRepneBothRepeatMovs) {
    for (const u8 prefix : {u8(0xF3), u8(0xF2)}) {
        Machine m;
        m.cpu.gpr.set64(kRsi, kSrc);
        m.cpu.gpr.set64(kRdi, kDst);
        m.cpu.gpr.set64(kRcx, 4);
        m.step({prefix, 0xA4});   // rep/repne movsb
        EXPECT_EQ(m.cpu.gpr.get64(kRcx), 0u) << "prefix " << std::hex << int(prefix);
        EXPECT_EQ(m.cpu.gpr.get64(kRsi), kSrc + 4);
        EXPECT_EQ(m.cpu.gpr.get64(kRdi), kDst + 4);
        u8 copied[4]{};
        ASSERT_TRUE(m.mem.read_bytes(kDst, reinterpret_cast<Byte*>(copied), 4));
        EXPECT_EQ(copied[0], 0xA0);
        EXPECT_EQ(copied[3], 0xA3);
    }
}

TEST(StringRepeatPrefix, RepAndRepneBothRepeatStos) {
    for (const u8 prefix : {u8(0xF3), u8(0xF2)}) {
        Machine m;
        m.cpu.gpr.set64(kRdi, kDst);
        m.cpu.gpr.set64(kRcx, 5);
        m.cpu.gpr.set64(kRax, 0x5A);
        m.step({prefix, 0xAA});   // rep/repne stosb
        EXPECT_EQ(m.cpu.gpr.get64(kRcx), 0u) << "prefix " << std::hex << int(prefix);
        EXPECT_EQ(m.cpu.gpr.get64(kRdi), kDst + 5);
    }
}

TEST(StringRepeatPrefix, RepAndRepneBothRepeatLods) {
    for (const u8 prefix : {u8(0xF3), u8(0xF2)}) {
        Machine m;
        m.cpu.gpr.set64(kRsi, kSrc);
        m.cpu.gpr.set64(kRcx, 3);
        m.step({prefix, 0xAC});   // rep/repne lodsb
        EXPECT_EQ(m.cpu.gpr.get64(kRcx), 0u) << "prefix " << std::hex << int(prefix);
        EXPECT_EQ(m.cpu.gpr.get64(kRax) & 0xFF, 0xA2u);   // last element loaded
    }
}

// The ZF condition still applies where the SDM says it does, so the fix must
// not have collapsed the two prefixes everywhere.
TEST(StringRepeatPrefix, ScasStillDistinguishesRepFromRepne) {
    // The buffer holds 0xA0..; the search value is not in it.
    // REPNE SCASB scans while not equal, so it runs the count out.
    {
        Machine m;
        m.cpu.gpr.set64(kRdi, kSrc);
        m.cpu.gpr.set64(kRcx, 4);
        m.cpu.gpr.set64(kRax, 0xFF);
        m.step({0xF2, 0xAE});
        EXPECT_EQ(m.cpu.gpr.get64(kRcx), 0u) << "repne scasb should exhaust the count";
    }
    // REPE SCASB scans while equal; the first byte already differs, so it
    // stops after one element.
    {
        Machine m;
        m.cpu.gpr.set64(kRdi, kSrc);
        m.cpu.gpr.set64(kRcx, 4);
        m.cpu.gpr.set64(kRax, 0xFF);
        m.step({0xF3, 0xAE});
        EXPECT_EQ(m.cpu.gpr.get64(kRcx), 3u) << "repe scasb should stop on the first mismatch";
    }
}
