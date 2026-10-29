// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// ADCX / ADOX against a *pending lazy FlagOp*.
//
// Rosetta #UDs both instructions, so the Rosetta oracle cannot check them at
// all. The expectations here were measured on real x86 silicon (the Windows
// Windows host), seeding EFLAGS to 0x8D5 and 0x002 and dumping the result:
//
//   ADCX  seed=8d5 a=ffffffff c=1 -> res=00000001 flags=8d5   (CF stays 1)
//   ADCX  seed=8d5 a=00000001 c=1 -> res=00000003 flags=8d4   (only CF cleared)
//   ADOX  seed=8d5 a=ffffffff c=1 -> res=00000001 flags=8d5   (OF stays 1)
//   ADOX  seed=8d5 a=00000001 c=1 -> res=00000003 flags=0d5   (only OF cleared)
//   ADCX  seed=002 a=ffffffff c=1 -> res=00000000 flags=001   (ZF NOT set!)
//   ADOX  seed=002 a=ffffffff c=1 -> res=00000000 flags=800
//
// So ADCX writes only CF and ADOX only OF; SF, ZF, AF, PF and the other of
// CF/OF are preserved -- including ZF when the result is zero.
//
// The oracle tests plant explicit RFLAGS with no lazy op pending, so they
// cannot see the bug this file exists for: reading raw RFLAGS instead of the
// lazy carry, which `ADD; ADCX` (the bignum shape ADX is *for*) hits directly.

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace vitreo;
using namespace vitreo::cpu;

namespace {

constexpr u64 kCode = 0x1000;

class AdxLazyFlagsTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(0x1000, 0x20000,
                                         MemoryProtection::ReadWrite));
    }

    // Run `code` to completion through the interpreter.
    void run(const std::vector<u8>& code, CpuState& cpu) {
        ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        cpu.rip = kCode;
        Interpreter interp(cpu, *m_memory);
        const u64 end = kCode + code.size();
        for (int i = 0; i < 16 && cpu.rip < end; ++i) {
            ExecResult r = interp.step();
            if (r != ExecResult::Continue && r != ExecResult::Branch) break;
        }
    }

    static CpuState fresh() {
        CpuState cpu;
        cpu.init_long_mode();
        cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
        return cpu;
    }

    std::unique_ptr<GuestMemory> m_memory;
};

// ADD EAX, ECX leaves a pending lazy Add32 FlagOp; ADCX must consume the
// carry that op implies, not whatever happens to be sitting in RFLAGS.
TEST_F(AdxLazyFlagsTest, AdcxConsumesLazyCarryFromPrecedingAdd) {
    // ADD eax, ecx ; ADCX edx, ebx   (66 0F 38 F6 D3)
    const std::vector<u8> code{0x01, 0xC8, 0x66, 0x0F, 0x38, 0xF6, 0xD3};

    auto cpu = fresh();
    cpu.gpr.set32(0, 0xFFFFFFFFu);  // EAX — ADD carries out
    cpu.gpr.set32(1, 0x00000001u);  // ECX
    cpu.gpr.set32(2, 0x00000010u);  // EDX
    cpu.gpr.set32(3, 0x00000020u);  // EBX
    run(code, cpu);

    // 0x10 + 0x20 + carry(1) = 0x31
    EXPECT_EQ(cpu.gpr.get32(2), 0x31u);
    cpu.materialize_lazy_flags();
    EXPECT_FALSE(cpu.flags.cf());  // 0x31 does not carry out
}

TEST_F(AdxLazyFlagsTest, AdcxSeesNoCarryFromNonCarryingAdd) {
    const std::vector<u8> code{0x01, 0xC8, 0x66, 0x0F, 0x38, 0xF6, 0xD3};

    auto cpu = fresh();
    cpu.gpr.set32(0, 0x00000001u);  // ADD does not carry
    cpu.gpr.set32(1, 0x00000001u);
    cpu.gpr.set32(2, 0x00000010u);
    cpu.gpr.set32(3, 0x00000020u);
    run(code, cpu);

    EXPECT_EQ(cpu.gpr.get32(2), 0x30u);  // no carry added
}

// The flags ADCX preserves must survive it. ADD sets ZF=0/SF=0 here; ADCX
// must not disturb them even though its own result is zero.
TEST_F(AdxLazyFlagsTest, AdcxPreservesZfAndSfWhenResultIsZero) {
    // ADD eax, ecx ; ADCX edx, ebx
    const std::vector<u8> code{0x01, 0xC8, 0x66, 0x0F, 0x38, 0xF6, 0xD3};

    auto cpu = fresh();
    cpu.gpr.set32(0, 0x00000001u);  // ADD -> 2, ZF=0, SF=0, CF=0
    cpu.gpr.set32(1, 0x00000001u);
    cpu.gpr.set32(2, 0xFFFFFFFFu);  // ADCX: 0xFFFFFFFF + 1 + 0 = 0, CF out = 1
    cpu.gpr.set32(3, 0x00000001u);
    run(code, cpu);

    EXPECT_EQ(cpu.gpr.get32(2), 0u);
    cpu.materialize_lazy_flags();
    EXPECT_TRUE(cpu.flags.cf());   // carry out
    EXPECT_FALSE(cpu.flags.zf());  // silicon: ADCX does NOT write ZF
}

// ADOX is the mirror image: it consumes and writes OF, and leaves CF alone.
TEST_F(AdxLazyFlagsTest, AdoxWritesOnlyOfAndPreservesCf) {
    // STC ; ADOX edx, ebx   (F3 0F 38 F6 D3)
    const std::vector<u8> code{0xF9, 0xF3, 0x0F, 0x38, 0xF6, 0xD3};

    auto cpu = fresh();
    cpu.gpr.set32(2, 0xFFFFFFFFu);
    cpu.gpr.set32(3, 0x00000001u);
    cpu.flags.set_of(false);
    run(code, cpu);

    EXPECT_EQ(cpu.gpr.get32(2), 0u);
    cpu.materialize_lazy_flags();
    EXPECT_TRUE(cpu.flags.of());  // carry out of the add lands in OF
    EXPECT_TRUE(cpu.flags.cf());  // STC's CF is untouched by ADOX
}

}  // namespace
