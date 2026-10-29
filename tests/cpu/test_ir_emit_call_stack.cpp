// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_emit_call_stack.cpp
/// @brief Tier 1 IR emission parity: stack, CALL and RET; contract in test_ir_emit.cpp.

#include "ir_emit_test_support.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_emit;

// =============================================================================
// Stack, CALL and RET
// =============================================================================

TEST(IrEmit, PushAndPop) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter_on_stack({0x50}, 1);
    expect_tier1_matches_interpreter_on_stack({0x50, 0x59}, 2);
    expect_tier1_matches_interpreter_on_stack({0x50, 0x51, 0x5A}, 3);
}

TEST(IrEmit, CallPushesTheReturnAddressAndBranches) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter_on_stack(
        {0xE8, 0x10, 0x00, 0x00, 0x00}, 1);
}

TEST(IrEmit, PushThenRetRoundTrips) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter_on_stack({0x50, 0xC3}, 2);
}

// THE fault-atomicity test, and the reason the frontend orders the ESP commit
// AFTER the store. Intel SDM Vol. 2, PUSH: if the stack write faults, the
// instruction is aborted and ESP is left UNCHANGED. A lowering that decremented
// ESP first would hand the guest's own exception handler a corrupted stack
// pointer to run on.
TEST(IrEmit, PushThatFaultsLeavesEspUnchanged) {
    REQUIRE_IR_GATE();
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    const std::vector<u8> code = {0x50};  // push eax
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(0, 0xCAFE'BABEu);
    cpu.gpr.set32(4, 0xDEAD'0000u);
    cpu.rip = kCode;

    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    ASSERT_TRUE(jit.ir_build_block(block).ok);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_TRUE(jit.ir_emit_active())
        << "block fell back to Tier 0; this test would be asserting Tier 0";
    jit.execute(*compiled, cpu);

    ASSERT_TRUE(cpu.pending_raise.active) << "the push must fault";
    EXPECT_EQ(cpu.pending_raise.args[0], 1u) << "a push is a WRITE fault";
    EXPECT_EQ(cpu.pending_raise.args[1], 0xDEAD'0000u - 4)
        << "the faulting address is ESP-4";
    EXPECT_EQ(cpu.gpr.get32(4), 0xDEAD'0000u)
        << "ESP must be UNCHANGED when the push faults (SDM Vol. 2, PUSH)";
    EXPECT_EQ(cpu.rip, kCode) << "the fault belongs to the PUSH itself";
}

// Symmetrically, a POP whose load faults must not have advanced ESP.
TEST(IrEmit, PopThatFaultsLeavesEspUnchanged) {
    REQUIRE_IR_GATE();
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    const std::vector<u8> code = {0x59};  // pop ecx
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(1, 0x1234'5678u);
    cpu.gpr.set32(4, 0xDEAD'0000u);
    cpu.rip = kCode;

    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    ASSERT_TRUE(jit.ir_build_block(block).ok);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_TRUE(jit.ir_emit_active()) << "must be a Tier 1 block";
    jit.execute(*compiled, cpu);

    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.get32(4), 0xDEAD'0000u) << "ESP unchanged";
    EXPECT_EQ(cpu.gpr.get32(1), 0x1234'5678u) << "ECX unchanged";
}

// POP ESP is declined rather than special-cased: the loaded value IS the new
// stack pointer, so the increment would overwrite it.
TEST(IrEmit, PopEspDeclines) {
    GuestMemory memory;
    JitCompiler jit(memory);
    BasicBlock block = decode_bytes({0x5C});  // pop esp
    EXPECT_FALSE(jit.ir_build_block(block).ok);
}

// Runs on BOTH tiers. It was Tier 1-only while the stencil compiler committed
// ESP before its fault-capable store; that is fixed (ops/stack.cpp), so the
// assertion now guards both lowerings against regressing.
//
// Intel SDM Vol. 2, PUSH: if the stack write faults the instruction is aborted
// and ESP is restored. A faulting stack write is NORMAL on Windows — touching
// the guard page is how the stack grows — and with ESP already decremented the
// retry after the handler commits a page decrements it again, drifting the
// guest's stack pointer by 4 per hit.
TEST(IrEmit, FaultingPushMatchesTheInterpreterOnEsp) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    const std::vector<u8> code = {0x50};  // push eax
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    const auto seed = [](CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(0, 0xCAFE'BABEu);
        cpu.gpr.set32(4, 0xDEAD'0000u);
        cpu.rip = kCode;
    };

    CpuState interp_cpu;
    seed(interp_cpu);
    {
        Interpreter interp(interp_cpu, memory);
        (void)interp.step();
    }

    CpuState jit_cpu;
    seed(jit_cpu);
    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    jit.execute(*compiled, jit_cpu);

    std::printf("[measure] faulting PUSH, %s: interpreter ESP=0x%08x  "
                "JIT ESP=0x%08x\n",
                jit.ir_emit_active() ? "Tier 1" : "Tier 0",
                interp_cpu.gpr.get32(4), jit_cpu.gpr.get32(4));
    EXPECT_EQ(jit_cpu.gpr.get32(4), interp_cpu.gpr.get32(4))
        << "ESP after a faulting PUSH must match the interpreter";
}

// Every other stack lowering that writes through a fault-capable store shares
// the PUSH contract above, and each was committing ESP first. Intel SDM Vol. 2
// (PUSH, PUSHA, CALL, ENTER): a faulting stack write commits no register, so
// ESP keeps its starting value in both engines -- including PUSHA, whose fault
// lands MID-sequence after earlier slots were already written.
namespace {

constexpr GuestAddr kStackRegion = 0x20000;

// Runs `code` on the interpreter and the JIT from one seed, with the guest
// stack positioned to run off the bottom of its own region, and requires the
// two to agree on every architectural register.
void expect_faulting_stack_write_matches_interpreter(
        const std::vector<u8>& code, u32 esp, const char* what) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(kStackRegion, 0x1000,
                                  MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    const auto seed = [&](CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(0, 0xCAFE'BABEu);
        cpu.gpr.set32(1, kStackRegion + 0x100);  // safe pointer for [ecx] forms
        cpu.gpr.set32(2, 0x1111'2222u);
        cpu.gpr.set32(3, 0x3333'4444u);
        cpu.gpr.set32(4, esp);
        cpu.gpr.set32(5, 0x5555'6666u);
        cpu.gpr.set32(6, 0x7777'8888u);
        cpu.gpr.set32(7, 0x9999'AAAAu);
        cpu.rip = kCode;
    };

    CpuState interp_cpu;
    seed(interp_cpu);
    {
        Interpreter interp(interp_cpu, memory);
        (void)interp.step();
    }

    CpuState jit_cpu;
    seed(jit_cpu);
    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr) << what;
    jit.execute(*compiled, jit_cpu);

    std::printf("[measure] faulting %s, %s: interpreter ESP=0x%08x  "
                "JIT ESP=0x%08x\n",
                what, jit.ir_emit_active() ? "Tier 1" : "Tier 0",
                interp_cpu.gpr.get32(4), jit_cpu.gpr.get32(4));

    EXPECT_EQ(interp_cpu.gpr.get32(4), esp)
        << what << ": a faulting stack write commits no register";
    EXPECT_EQ(jit_cpu.gpr.get32(4), interp_cpu.gpr.get32(4))
        << what << ": ESP after a faulting stack write must match the "
                   "interpreter";
    for (u8 r = 0; r < 8; ++r) {
        if (r == 4) continue;
        EXPECT_EQ(jit_cpu.gpr.get32(r), interp_cpu.gpr.get32(r))
            << what << ": GPR " << static_cast<int>(r);
    }
}

}  // namespace

// PUSHA faults on its THIRD slot: two slots are already written, and ESP still
// reads its starting value -- the lowering commits ESP once, after the eighth
// store (a per-slot commit would leave it 8 lower).
TEST(IrEmit, FaultingPushaMatchesTheInterpreterOnEsp) {
    expect_faulting_stack_write_matches_interpreter(
        {0x60}, kStackRegion + 8, "PUSHA");
}

TEST(IrEmit, FaultingEnterMatchesTheInterpreterOnEsp) {
    // enter 0x10, 0 — the Push(EBP) faults before the frame is allocated.
    expect_faulting_stack_write_matches_interpreter(
        {0xC8, 0x10, 0x00, 0x00}, kStackRegion, "ENTER");
}

TEST(IrEmit, FaultingCallRel32MatchesTheInterpreterOnEsp) {
    expect_faulting_stack_write_matches_interpreter(
        {0xE8, 0x00, 0x00, 0x00, 0x00}, kStackRegion, "CALL rel32");
}

TEST(IrEmit, FaultingCallIndirectMatchesTheInterpreterOnEsp) {
    // call [ecx] — the target load succeeds, the return-address push faults.
    expect_faulting_stack_write_matches_interpreter(
        {0xFF, 0x11}, kStackRegion, "CALL r/m32");
}

TEST(IrEmit, FaultingPushImmediateMatchesTheInterpreterOnEsp) {
    expect_faulting_stack_write_matches_interpreter(
        {0x68, 0x78, 0x56, 0x34, 0x12}, kStackRegion, "PUSH imm32");
    expect_faulting_stack_write_matches_interpreter(
        {0x6A, 0x7F}, kStackRegion, "PUSH imm8");
}

TEST(IrEmit, FaultingPushMemoryMatchesTheInterpreterOnEsp) {
    expect_faulting_stack_write_matches_interpreter(
        {0xFF, 0x31}, kStackRegion, "PUSH r/m32");
}
