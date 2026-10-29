// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_emit_sse.cpp
/// @brief Tier 1 IR emission parity: SSE MOVSS/XORPS; contract in test_ir_emit.cpp.

#include "ir_emit_test_support.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_emit;

// =============================================================================
// M9 — SSE: establishing MOVSS/XORPS semantics from the oracle first
// =============================================================================
//
// MOVSS's merge behaviour differs by FORM, and getting it wrong is invisible
// whenever the destination's upper lanes happen to be zero — which, right
// after the `xorps xmm,xmm` idiom that dominates the same code, they usually
// are. Intel SDM Vol. 2 says register-to-register preserves bits 127:32 while
// a load zeroes them. Measured here rather than trusted, because the
// interpreter is the semantic oracle and a lowering has to match IT.
TEST(SseSemantics, MovssMergeBehaviourByForm) {
    struct Case { const char* what; std::vector<u8> code; };
    const Case cases[] = {
        {"movss xmm0, xmm1  (reg-reg)", {0xF3, 0x0F, 0x10, 0xC1}},
        {"movss xmm0, [edx]  (load)",   {0xF3, 0x0F, 0x10, 0x02}},
    };
    for (const Case& c : cases) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                      MemoryProtection::ReadWrite));
        ASSERT_TRUE(memory.write_bytes(kCode, c.code.data(), c.code.size()));
        ASSERT_TRUE(memory.write_u32(0x2000, 0x41424344u));

        CpuState cpu;
        cpu.init_protected_mode();
        cpu.gpr.set32(2, 0x2000);
        cpu.gpr.set32(4, 0x3000);
        cpu.rip = kCode;
        // Distinctive upper lanes on the DESTINATION: if a lowering zeroes
        // them when it should not, or keeps them when it should not, these
        // are what say so.
        cpu.sse.xmm[0].u64_[0] = 0x1111'1111'1111'1111ull;
        cpu.sse.xmm[0].u64_[1] = 0x2222'2222'2222'2222ull;
        cpu.sse.xmm[1].u64_[0] = 0x3333'3333'4444'4444ull;
        cpu.sse.xmm[1].u64_[1] = 0x5555'5555'5555'5555ull;

        Interpreter interp(cpu, memory);
        ASSERT_EQ(interp.step(), ExecResult::Continue) << c.what;
        std::printf("[measure] %-30s xmm0 = %016llx_%016llx\n", c.what,
                    static_cast<unsigned long long>(cpu.sse.xmm[0].u64_[1]),
                    static_cast<unsigned long long>(cpu.sse.xmm[0].u64_[0]));
    }
}

// XORPS is a full 128-bit bitwise XOR. The register-register form is by far
// the common one (`xorps xmm,xmm` zeroes a register) and cannot fault; the
// memory form additionally requires 16-byte alignment, which is a fault
// condition this slice does not take on.
TEST(SseSemantics, XorpsIsAFull128BitXor) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                  MemoryProtection::ReadWrite));
    const std::vector<u8> code = {0x0F, 0x57, 0xC1};   // xorps xmm0, xmm1
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(4, 0x3000);
    cpu.rip = kCode;
    cpu.sse.xmm[0].u64_[0] = 0x0F0F'0F0F'0F0F'0F0Full;
    cpu.sse.xmm[0].u64_[1] = 0xFFFF'0000'FFFF'0000ull;
    cpu.sse.xmm[1].u64_[0] = 0x00FF'00FF'00FF'00FFull;
    cpu.sse.xmm[1].u64_[1] = 0x1234'5678'9ABC'DEF0ull;

    Interpreter interp(cpu, memory);
    ASSERT_EQ(interp.step(), ExecResult::Continue);
    EXPECT_EQ(cpu.sse.xmm[0].u64_[0], 0x0F0F'0F0F'0F0F'0F0Full ^
                                     0x00FF'00FF'00FF'00FFull);
    EXPECT_EQ(cpu.sse.xmm[0].u64_[1], 0xFFFF'0000'FFFF'0000ull ^
                                     0x1234'5678'9ABC'DEF0ull);
}

// Tier 1 SSE parity: the same block on both tiers, compared on the full
// 128-bit XMM file. Upper lanes are seeded distinctively because the MOVSS
// merge rule is exactly what a lowering gets wrong, and a zeroed upper lane
// hides it.
namespace {

void expect_sse_matches_interpreter(const std::vector<u8>& code,
                                    const char* what) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                  MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));
    ASSERT_TRUE(memory.write_u32(0x2000, 0x41424344u));

    const auto seed = [](CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(2, 0x2000);
        cpu.gpr.set32(4, 0x3000);
        cpu.rip = kCode;
        for (u8 r = 0; r < 8; ++r) {
            cpu.sse.xmm[r].u64_[0] = 0x1111'1111'0000'0000ull + r;
            cpu.sse.xmm[r].u64_[1] = 0x2222'2222'0000'0000ull + r;
        }
    };

    CpuState oracle;
    seed(oracle);
    {
        Interpreter interp(oracle, memory);
        ASSERT_EQ(interp.step(), ExecResult::Continue) << what;
    }

    CpuState jit_cpu;
    seed(jit_cpu);
    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    ASSERT_TRUE(jit.ir_build_block(block).ok)
        << what << ": " << ir::decline_reason_name(jit.ir_last_result().reason);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr) << what;
    ASSERT_TRUE(jit.ir_emit_active())
        << what << ": fell back to Tier 0; this would assert Tier 0";
    jit.execute(*compiled, jit_cpu);

    for (u8 r = 0; r < 8; ++r) {
        EXPECT_EQ(jit_cpu.sse.xmm[r].u64_[0], oracle.sse.xmm[r].u64_[0])
            << what << ": xmm" << static_cast<int>(r) << " low";
        EXPECT_EQ(jit_cpu.sse.xmm[r].u64_[1], oracle.sse.xmm[r].u64_[1])
            << what << ": xmm" << static_cast<int>(r) << " HIGH (the MOVSS "
                                                         "merge rule)";
    }
    const auto mem = memory.read_u32(0x2000);
    ASSERT_TRUE(mem.is_ok());
    EXPECT_EQ(mem.value(), 0x41424344u) << what << ": memory unexpectedly hit";
}

}  // namespace

TEST(IrEmitSse, XorpsRegisterRegisterMatchesInterpreter) {
    REQUIRE_IR_GATE();
    expect_sse_matches_interpreter({0x0F, 0x57, 0xC1}, "xorps xmm0, xmm1");
}

// The zeroing idiom, which is what makes XORPS the single largest Tier 1
// blocker on The Sims 3.
TEST(IrEmitSse, XorpsSelfZeroesMatchesInterpreter) {
    REQUIRE_IR_GATE();
    expect_sse_matches_interpreter({0x0F, 0x57, 0xC0}, "xorps xmm0, xmm0");
}

TEST(IrEmitSse, MovssRegisterRegisterPreservesUpperLanes) {
    REQUIRE_IR_GATE();
    expect_sse_matches_interpreter({0xF3, 0x0F, 0x10, 0xC1},
                                   "movss xmm0, xmm1");
}

TEST(IrEmitSse, MovssLoadZeroesUpperLanes) {
    REQUIRE_IR_GATE();
    expect_sse_matches_interpreter({0xF3, 0x0F, 0x10, 0x02},
                                   "movss xmm0, [edx]");
}

// MOVAPS copies the WHOLE register, upper lanes included — the opposite of
// MOVSS reg-reg, which preserves the destination's. Seeding both registers
// distinctively is what separates the two.
TEST(IrEmitSse, MovapsRegisterRegisterCopiesAll128Bits) {
    REQUIRE_IR_GATE();
    expect_sse_matches_interpreter({0x0F, 0x28, 0xC1}, "movaps xmm0, xmm1");
}

// The 0F 29 encoding with mod=11 is the same register-register copy with the
// operands the other way round. A lowering that assumed operand 0 is always
// the destination register would move the wrong way here.
TEST(IrEmitSse, MovapsStoreFormWithRegisterOperandsMatchesInterpreter) {
    REQUIRE_IR_GATE();
    expect_sse_matches_interpreter({0x0F, 0x29, 0xC1}, "movaps xmm1, xmm0");
}

// A 128-bit MEMORY operand must still decline: the alignment #GP is not
// modelled. Asserting the decline keeps a later widening honest.
TEST(IrEmitSse, MovapsMemoryFormDeclines) {
    REQUIRE_IR_GATE();
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                  MemoryProtection::ReadWrite));
    const std::vector<u8> code = {0x0F, 0x28, 0x02};   // movaps xmm0, [edx]
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));
    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    EXPECT_FALSE(jit.ir_build_block(block).ok)
        << "a 128-bit memory operand needs an alignment fault this does not "
           "model";
}

TEST(IrEmitSse, MovssStoreWritesOnlyTheLowLane) {
    REQUIRE_IR_GATE();
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                  MemoryProtection::ReadWrite));
    const std::vector<u8> code = {0xF3, 0x0F, 0x11, 0x02};  // movss [edx],xmm0
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    const auto seed = [](CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(2, 0x2000);
        cpu.gpr.set32(4, 0x3000);
        cpu.rip = kCode;
        cpu.sse.xmm[0].u64_[0] = 0xAAAA'AAAA'1234'5678ull;
        cpu.sse.xmm[0].u64_[1] = 0xBBBB'BBBB'BBBB'BBBBull;
    };

    CpuState oracle;
    seed(oracle);
    ASSERT_TRUE(memory.write_u64(0x2000, 0xDEAD'BEEF'CAFE'BABEull));
    {
        Interpreter interp(oracle, memory);
        ASSERT_EQ(interp.step(), ExecResult::Continue);
    }
    const auto oracle_mem = memory.read_u64(0x2000);
    ASSERT_TRUE(oracle_mem.is_ok());

    ASSERT_TRUE(memory.write_u64(0x2000, 0xDEAD'BEEF'CAFE'BABEull));
    CpuState jit_cpu;
    seed(jit_cpu);
    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    ASSERT_TRUE(jit.ir_build_block(block).ok)
        << ir::decline_reason_name(jit.ir_last_result().reason);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_TRUE(jit.ir_emit_active());
    jit.execute(*compiled, jit_cpu);

    const auto jit_mem = memory.read_u64(0x2000);
    ASSERT_TRUE(jit_mem.is_ok());
    // 8 bytes on purpose: a lowering that stored the whole low 64 bits instead
    // of 32 would still match on the low word.
    EXPECT_EQ(jit_mem.value(), oracle_mem.value())
        << "movss [mem],xmm must write exactly 4 bytes";
    EXPECT_EQ(jit_cpu.sse.xmm[0].u64_[0], oracle.sse.xmm[0].u64_[0]);
    EXPECT_EQ(jit_cpu.sse.xmm[0].u64_[1], oracle.sse.xmm[0].u64_[1]);
}
