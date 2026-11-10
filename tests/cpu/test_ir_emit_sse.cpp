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

namespace {

void expect_xmm_read_reuse(const std::vector<u8>& code, bool long_mode,
                          unsigned expected_loads, bool fault = false,
                          bool expect_native = true) {
    SCOPED_TRACE(long_mode ? "x64" : "x86");
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                  MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));
    ASSERT_TRUE(memory.write_u32(0x2000, 0x12345678).is_ok());
    BasicBlock block;
    block.start_addr = kCode;
    block.is_64bit = long_mode;
    MemoryByteReader reader(code.data(), code.size());
    while (reader.remaining()) {
        DecodedInstruction instruction;
        ASSERT_EQ(decode_instruction(reader, long_mode ? DecodeMode::Mode64
                                                       : DecodeMode::Mode32,
                                     instruction), DecodeError::Success);
        block.instructions.push_back(instruction);
    }
    block.byte_length = static_cast<u32>(code.size());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    CpuState oracle;
    if (long_mode) oracle.init_long_mode();
    else oracle.init_protected_mode();
    oracle.rip = kCode;
    oracle.gpr.set64(3, fault ? 0xDEAD0000 : 0x2000);
    oracle.flags.rflags = 0xAD7;
    for (u8 r = 0; r < 16; ++r) {
        oracle.sse.xmm[r].u64_[0] = 0x0123456789ABCDEFull ^ (0x1111111111111111ull * r);
        oracle.sse.xmm[r].u64_[1] = 0xFEDCBA9876543210ull ^ (0x2222222222222222ull * r);
    }
    auto state = oracle;
    auto expected = oracle;
    Interpreter interpreter(oracle, memory);
    for (const auto& instruction : block.instructions) {
        // Intel SDM Vol. 2D XORPS: full-width XOR, with no flag changes.
        if (instruction.opcode == Opcode::XORPS_XMM_XMMM128) {
            const auto dst = instruction.operands[0].reg;
            const auto src = instruction.operands[1].reg;
            for (unsigned lane = 0; lane < 2; ++lane)
                expected.sse.xmm[dst].u64_[lane] ^=
                    expected.sse.xmm[src].u64_[lane];
        }
        const auto result = interpreter.step();
        if (fault && result != ExecResult::Continue) break;
        ASSERT_EQ(result, ExecResult::Continue);
    }
    JitCompiler jit(memory);
    ASSERT_TRUE(jit.ir_build_block(block).ok);
    auto* native = jit.compile(block);
    ASSERT_NE(native, nullptr);
    ASSERT_EQ(jit.ir_emit_active(), expect_native)
        << ir::decline_reason_name(jit.ir_emit_decline_reason());
    const auto result = jit.execute(*native, state);
    EXPECT_EQ(result, JitExecResult::Continue);
    EXPECT_EQ(state.pending_raise.active, fault);
    unsigned loads = 0;
    const auto* words = static_cast<const u32*>(native->code);
    for (usize at = 0; at < native->code_size / sizeof(u32); ++at) {
        for (u8 reg = 0; reg < 16; ++reg) {
            const usize offset = offsetof(CpuState, sse.xmm) +
                                 reg * sizeof(state.sse.xmm[0]);
            for (u8 v = 0; v < 32; ++v) {
                // X19 is the fixed CpuState pin in both JIT ABIs.
                if (words[at] == arm64::ldr_q_uoff(
                        v, 19, static_cast<u32>(offset >> 4))) ++loads;
            }
        }
    }
    if (expect_native) EXPECT_EQ(loads, expected_loads);
    for (u8 reg = 0; reg < 16; ++reg) {
        for (unsigned lane = 0; lane < 2; ++lane) {
            EXPECT_EQ(state.sse.xmm[reg].u64_[lane],
                      expected.sse.xmm[reg].u64_[lane])
                << "xmm" << unsigned(reg) << " lane " << lane;
            EXPECT_EQ(state.sse.xmm[reg].u64_[lane],
                      oracle.sse.xmm[reg].u64_[lane]);
        }
        EXPECT_EQ(state.gpr.get64(reg), oracle.gpr.get64(reg));
    }
    EXPECT_EQ(state.rip, oracle.rip);
    state.materialize_lazy_flags();
    EXPECT_EQ(state.flags.rflags, 0xAD7u);
    EXPECT_EQ(state.sse.mxcsr, expected.sse.mxcsr);
    if (fault) {
        EXPECT_EQ(state.pending_raise.num_args, 2u);
        EXPECT_EQ(state.pending_raise.args[0], 0u);
        EXPECT_EQ(state.pending_raise.args[1], 0xDEAD0000u);
    }
}

}  // namespace

TEST(IrEmitSse, XmmReadReuseHandlesAliasedSources) {
    REQUIRE_IR_GATE();
    for (const bool long_mode : {false, true})
        expect_xmm_read_reuse({0x0F, 0x57, 0xC0}, long_mode, 1);
    expect_xmm_read_reuse({0x45, 0x0F, 0x57, 0xC0}, true, 1);
}

TEST(IrEmitSse, XmmReadReuseTracksOverwrittenSources) {
    REQUIRE_IR_GATE();
    for (const bool long_mode : {false, true})
        expect_xmm_read_reuse({
            0x0F, 0x57, 0xC1,  // xorps xmm0,xmm1
            0x0F, 0x57, 0xCA,  // xorps xmm1,xmm2
            0x0F, 0x57, 0xD9,  // xorps xmm3,xmm1
        }, long_mode, long_mode ? 4 : 6);
    expect_xmm_read_reuse({
        0x45, 0x0F, 0x57, 0xC1,  // xorps xmm8,xmm9
        0x45, 0x0F, 0x57, 0xCA,  // xorps xmm9,xmm10
        0x45, 0x0F, 0x57, 0xD9,  // xorps xmm11,xmm9
    }, true, 4);
}

TEST(IrEmitSse, XmmReadReuseStopsAtMemoryBoundaries) {
    REQUIRE_IR_GATE();
    for (const bool long_mode : {false, true})
        expect_xmm_read_reuse({
            0x0F, 0x57, 0xC1,  // xorps xmm0,xmm1
            0x8B, 0x03,        // mov eax,[ebx/rbx]
            0x0F, 0x57, 0xD1,  // xorps xmm2,xmm1
        }, long_mode, 4);
}

TEST(IrEmitSse, XmmReadReusePreservesStateAtAFault) {
    REQUIRE_IR_GATE();
    for (const bool long_mode : {false, true})
        expect_xmm_read_reuse({
            0x0F, 0x57, 0xC1,
            0x8B, 0x03,
            0x0F, 0x57, 0xD1,
        }, long_mode, 4, true);
}

TEST(IrEmitSse, XmmReadReuseSurvivesNativeScalarMoves) {
    REQUIRE_IR_GATE();
    for (const bool long_mode : {false, true})
        expect_xmm_read_reuse({
            0x0F, 0x57, 0xC1,
            0x89, 0xD8,        // mov eax,ebx
            0x0F, 0x57, 0xD1,
        }, long_mode, long_mode ? 3 : 4);
}

TEST(IrEmitSse, XmmReadReuseStopsAtConditionHelpers) {
    REQUIRE_IR_GATE();
    expect_xmm_read_reuse({
        0x0F, 0x57, 0xC1,
        0x0F, 0x94, 0xC0,  // setz al: incoming flags can require a helper
        0x0F, 0x57, 0xD1,
    }, true, 4);
}

TEST(IrEmitSse, XmmReadReuseAdmitsLongModeVectorChains) {
    REQUIRE_IR_GATE();
    std::vector<u8> code;
    for (unsigned repeat = 0; repeat < 12; ++repeat)
        code.insert(code.end(), {0x0F, 0x57, 0xC1});
    expect_xmm_read_reuse(code, true, 2);
}

TEST(IrEmitSse, XmmReadReuseRespectsThePhysicalVectorRegisterLimit) {
    REQUIRE_IR_GATE();
    std::vector<u8> code;
    for (unsigned repeat = 0; repeat < 30; ++repeat)
        code.insert(code.end(), {0x0F, 0x57, 0xC1});
    expect_xmm_read_reuse(code, true, 2);
    code.insert(code.end(), {0x0F, 0x57, 0xC1});
    expect_xmm_read_reuse(code, true, 0, false, false);
}

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
