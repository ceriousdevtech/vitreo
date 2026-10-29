// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_emit_inc_dec.cpp
/// @brief Tier 1 IR emission parity: INC/DEC CF preservation; contract in test_ir_emit.cpp.

#include "ir_emit_test_support.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_emit;

// =============================================================================
// INC / DEC — CF preservation
// =============================================================================

// The defining property of INC/DEC is that they PRESERVE CF, so a test whose
// incoming carry is always the same value cannot see a lowering that loses it.
// Each case here establishes a KNOWN incoming CF with a preceding instruction
// and then checks the full materialized RFLAGS afterwards.
TEST(IrEmit, IncDecPreserveTheIncomingCarry) {
    REQUIRE_IR_GATE();
    struct Case { const char* name; std::vector<u8> code; u32 eax; u32 ecx; };
    const std::vector<Case> cases = {
        // cmp eax,ecx sets CF=1 when eax<ecx, CF=0 otherwise; then inc/dec.
        {"cmp(CF=1); inc eax", {0x39, 0xC8, 0x40}, 3u, 9u},
        {"cmp(CF=0); inc eax", {0x39, 0xC8, 0x40}, 9u, 3u},
        {"cmp(CF=1); dec eax", {0x39, 0xC8, 0x48}, 3u, 9u},
        {"cmp(CF=0); dec eax", {0x39, 0xC8, 0x48}, 9u, 3u},
        // Boundary values where INC/DEC set OF and ZF.
        {"inc at INT_MAX",     {0x39, 0xC8, 0x40}, 0x7FFF'FFFFu, 1u},
        {"dec at INT_MIN",     {0x39, 0xC8, 0x48}, 0x8000'0000u, 1u},
        {"inc to zero",        {0x39, 0xC8, 0x40}, 0xFFFF'FFFFu, 1u},
        {"dec to zero",        {0x39, 0xC8, 0x48}, 1u, 5u},
    };

    for (const Case& c : cases) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                      MemoryProtection::ReadWrite));
        ASSERT_TRUE(memory.write_bytes(kCode, c.code.data(), c.code.size()));

        const auto seed = [&](CpuState& cpu) {
            cpu.init_protected_mode();
            cpu.gpr.set32(0, c.eax);
            cpu.gpr.set32(1, c.ecx);
            cpu.gpr.set32(4, 0x20800);
            cpu.rip = kCode;
        };

        CpuState oracle;
        seed(oracle);
        {
            Interpreter interp(oracle, memory);
            for (int i = 0; i < 2; ++i) {
                ASSERT_EQ(interp.step(), ExecResult::Continue) << c.name;
            }
        }
        oracle.materialize_lazy_flags();

        CpuState jit_cpu;
        seed(jit_cpu);
        JitCompiler jit(memory);
        BasicBlock block = decode_bytes(c.code);
        ASSERT_TRUE(jit.ir_build_block(block).ok)
            << c.name << ": "
            << ir::decline_reason_name(jit.ir_last_result().reason);
        CompiledBlock* compiled = jit.compile(block);
        ASSERT_NE(compiled, nullptr) << c.name;
        ASSERT_TRUE(jit.ir_emit_active())
            << c.name << ": fell back to Tier 0, so this asserts nothing";
        jit.execute(*compiled, jit_cpu);
        jit_cpu.materialize_lazy_flags();

        EXPECT_EQ(jit_cpu.gpr.get32(0), oracle.gpr.get32(0)) << c.name;
        EXPECT_EQ(jit_cpu.flags.rflags, oracle.flags.rflags)
            << c.name << ": RFLAGS diverged (CF is the one INC/DEC preserves)";
    }
}

// INC with the carry forced by CLC/STC rather than computed. The captured CF
// must come from the OVERLAY, which `emit_lazy_cf_to_call_tmp` consults before
// any lazy-record fast path — `ADD; CLC; INC` must inherit the cleared carry,
// not the ADD's.
TEST(IrEmit, IncInheritsAForcedCarryFromClcStc) {
    REQUIRE_IR_GATE();
    for (const auto& c : std::vector<std::pair<const char*, std::vector<u8>>>{
             {"add; clc; inc", {0x01, 0xC8, 0xF8, 0x40}},
             {"add; stc; inc", {0x01, 0xC8, 0xF9, 0x40}},
         }) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                      MemoryProtection::ReadWrite));
        ASSERT_TRUE(memory.write_bytes(kCode, c.second.data(), c.second.size()));

        const auto seed = [&](CpuState& cpu) {
            cpu.init_protected_mode();
            cpu.gpr.set32(0, 0xFFFF'FFFFu);   // makes the ADD carry
            cpu.gpr.set32(1, 2u);
            cpu.gpr.set32(4, 0x20800);
            cpu.rip = kCode;
        };

        CpuState oracle;
        seed(oracle);
        {
            Interpreter interp(oracle, memory);
            for (int i = 0; i < 3; ++i) {
                ASSERT_EQ(interp.step(), ExecResult::Continue) << c.first;
            }
        }
        oracle.materialize_lazy_flags();

        CpuState jit_cpu;
        seed(jit_cpu);
        JitCompiler jit(memory);
        BasicBlock block = decode_bytes(c.second);
        // CLC/STC are not in the frontend's op set, so this block goes to
        // Tier 0 — which is the point: the comparison still has to hold, and
        // when CLC/STC do land the same assertion covers Tier 1.
        CompiledBlock* compiled = jit.compile(block);
        ASSERT_NE(compiled, nullptr) << c.first;
        jit.execute(*compiled, jit_cpu);
        jit_cpu.materialize_lazy_flags();
        EXPECT_EQ(jit_cpu.flags.rflags, oracle.flags.rflags) << c.first;
    }
}
