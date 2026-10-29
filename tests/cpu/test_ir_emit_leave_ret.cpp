// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_emit_leave_ret.cpp
/// @brief Tier 1 IR emission parity: LEAVE fault ordering and RET imm16; contract in test_ir_emit.cpp.

#include "ir_emit_test_support.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_emit;

// =============================================================================
// LEAVE: establishing the fault ordering before implementing it
// =============================================================================

// `LEAVE` is `ESP <- EBP; EBP <- Pop()`. The SDM writes those as two
// sequential steps, which leaves a real question: if the POP faults, has ESP
// already been assigned EBP, or is the whole instruction aborted?
//
// The two answers need different lowerings, and this project has twice found
// the tree getting an ordering like this wrong. Hardware answers it: Windows on
// Intel reports the STARTING ESP for a LEAVE whose pop faults
// (tools/oracle_probe/stack_access_fault_probe.expected, `leave` ctxsp=+0) --
// the whole instruction is aborted. An earlier version of this test measured
// the interpreter instead and asserted ESP := EBP, which the interpreter then
// got wrong in exactly the way the test enshrined.
TEST(IrEmit, FaultingLeaveOrdering) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(0x20000, 0x1000, MemoryProtection::ReadWrite));
    const std::vector<u8> code = {0xC9};  // leave
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(4, 0x20800);        // ESP -> mapped
    cpu.gpr.set32(5, 0xDEAD'0000u);   // EBP -> unmapped, so the POP faults
    cpu.rip = kCode;

    Interpreter interp(cpu, memory);
    (void)interp.step();

    // Nothing commits: ESP and EBP keep their entry values, the rule PUSH
    // and the read-modify-write forms follow too.
    EXPECT_EQ(cpu.gpr.get32(4), 0x20800u)
        << "a faulting LEAVE leaves ESP as it was";
    EXPECT_EQ(cpu.gpr.get32(5), 0xDEAD'0000u)
        << "EBP still holds its entry value";
}

TEST(IrEmit, RetImm16AdjustsTheStackByFourPlusImmediate) {
    REQUIRE_IR_GATE();
    // push eax ; ret 8  -> pops the return address AND discards 8 more bytes.
    expect_tier1_matches_interpreter_on_stack({0x50, 0xC2, 0x08, 0x00}, 2);
    expect_tier1_matches_interpreter_on_stack({0x50, 0xC2, 0x00, 0x00}, 2);
    // The adjustment is the UNSIGNED imm16 plus the slot, up to 65539: past
    // the 12-bit ADD immediate, which must not truncate it (Intel SDM Vol. 2
    // RET: ESP := ESP + SRC).
    expect_tier1_matches_interpreter_on_stack({0x50, 0xC2, 0xF7, 0x0F}, 2);  // 4087
    expect_tier1_matches_interpreter_on_stack({0x50, 0xC2, 0xFC, 0x0F}, 2);  // 4092
    expect_tier1_matches_interpreter_on_stack({0x50, 0xC2, 0x00, 0x10}, 2);  // 4096
    expect_tier1_matches_interpreter_on_stack({0x50, 0xC2, 0xF8, 0x7F}, 2);  // 0x7FF8
    expect_tier1_matches_interpreter_on_stack({0x50, 0xC2, 0xFF, 0xFF}, 2);  // 0xFFFF
}

// LEAVE, non-faulting: EBP points into the stack, so the pop succeeds.
TEST(IrEmit, LeaveRestoresEspAndEbp) {
    REQUIRE_IR_GATE();
    constexpr GuestAddr kFrame = 0x20A00;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(0x20000, 0x1000, MemoryProtection::ReadWrite));
    const std::vector<u8> code = {0xC9};  // leave
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));
    ASSERT_TRUE(memory.write_u32(kFrame, 0x1234'5678u));   // the saved EBP

    const auto seed = [&](CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(4, kFrame - 0x40);   // ESP below the frame
        cpu.gpr.set32(5, kFrame);          // EBP
        cpu.rip = kCode;
    };

    CpuState oracle;
    seed(oracle);
    { Interpreter i(oracle, memory); ASSERT_EQ(i.step(), ExecResult::Continue); }

    CpuState jit_cpu;
    seed(jit_cpu);
    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    ASSERT_TRUE(jit.ir_build_block(block).ok)
        << ir::decline_reason_name(jit.ir_last_result().reason);
    CompiledBlock* c = jit.compile(block);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(jit.ir_emit_active());
    jit.execute(*c, jit_cpu);

    EXPECT_EQ(jit_cpu.gpr.get32(4), oracle.gpr.get32(4)) << "ESP";
    EXPECT_EQ(jit_cpu.gpr.get32(5), oracle.gpr.get32(5)) << "EBP";
    EXPECT_EQ(jit_cpu.gpr.get32(4), kFrame + 4) << "ESP = EBP + 4";
    EXPECT_EQ(jit_cpu.gpr.get32(5), 0x1234'5678u) << "EBP = popped value";
}

// LEAVE whose pop FAULTS. Hardware commits nothing (see
// FaultingLeaveOrdering); Tier 1 must match the interpreter, which does the
// same.
TEST(IrEmit, FaultingLeaveMatchesTheInterpreter) {
    REQUIRE_IR_GATE();
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(0x20000, 0x1000, MemoryProtection::ReadWrite));
    const std::vector<u8> code = {0xC9};  // leave
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    const auto seed = [&](CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(4, 0x20800);
        cpu.gpr.set32(5, 0xDEAD'0000u);   // EBP unmapped -> the pop faults
        cpu.rip = kCode;
    };

    CpuState oracle;
    seed(oracle);
    { Interpreter i(oracle, memory); (void)i.step(); }

    CpuState jit_cpu;
    seed(jit_cpu);
    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    ASSERT_TRUE(jit.ir_build_block(block).ok);
    CompiledBlock* c = jit.compile(block);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(jit.ir_emit_active());
    jit.execute(*c, jit_cpu);

    ASSERT_TRUE(jit_cpu.pending_raise.active) << "the pop must fault";
    EXPECT_EQ(jit_cpu.gpr.get32(4), oracle.gpr.get32(4))
        << "ESP after a faulting LEAVE must match the interpreter";
    EXPECT_EQ(jit_cpu.gpr.get32(5), oracle.gpr.get32(5)) << "EBP";
    EXPECT_EQ(jit_cpu.rip, kCode) << "the fault belongs to the LEAVE";
}
