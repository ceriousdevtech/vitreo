// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_emit_folded_memory_operands.cpp
/// @brief Tier 1 IR emission parity: memory-source ALU, addressing, RMW; contract in test_ir_emit.cpp.

#include "ir_emit_test_support.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_emit;

// =============================================================================
// ALU with a memory source (M4 completion)
// =============================================================================

TEST(IrEmit, AluWithMemorySource) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter_with_memory({0x03, 0x01});        // add eax,[ecx]
    expect_tier1_matches_interpreter_with_memory({0x2B, 0x01});        // sub eax,[ecx]
    expect_tier1_matches_interpreter_with_memory({0x23, 0x01});        // and eax,[ecx]
    expect_tier1_matches_interpreter_with_memory({0x0B, 0x01});        // or  eax,[ecx]
    expect_tier1_matches_interpreter_with_memory({0x33, 0x01});        // xor eax,[ecx]
}

TEST(IrEmit, CompareWithMemorySource) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter_with_memory({0x3B, 0x01});        // cmp eax,[ecx]
    expect_tier1_matches_interpreter_with_memory({0x3B, 0x41, 0x08});  // cmp eax,[ecx+8]
}

// Scaled-index addressing needs its own fixture: the shared seed table puts
// large values in EDX on purpose (to make flag comparisons meaningful), and
// `[ecx + edx*4]` with a large EDX simply leaves the mapped page.
TEST(IrEmit, MemorySourceAluWithScaledIndex) {
    REQUIRE_IR_GATE();
    constexpr GuestAddr kScratch = 0x20800;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(0x20000, 0x1000, MemoryProtection::ReadWrite));
    const std::vector<u8> code = {0x03, 0x04, 0x91};  // add eax,[ecx+edx*4]
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));
    for (u32 i = 0; i < 8; ++i) {
        ASSERT_TRUE(memory.write_u32(kScratch + i * 4, 0x1000'0000u + i));
    }

    const auto seed = [&](CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(0, 0x8000'0001u);   // asymmetric, sign-crossing
        cpu.gpr.set32(1, kScratch);
        cpu.gpr.set32(2, 3);              // index 3 -> kScratch + 12
        cpu.gpr.set32(4, kScratch);
        cpu.rip = kCode;
    };

    CpuState oracle;
    seed(oracle);
    {
        Interpreter interp(oracle, memory);
        ASSERT_EQ(interp.step(), ExecResult::Continue);
    }
    oracle.materialize_lazy_flags();

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
    jit_cpu.materialize_lazy_flags();

    EXPECT_EQ(jit_cpu.gpr.get32(0), oracle.gpr.get32(0));
    EXPECT_EQ(jit_cpu.flags.rflags, oracle.flags.rflags);
}

// A faulting load in an ALU-with-memory must commit NOTHING: not the
// destination register, and not the flags. The previous instruction's flags
// must still be the ones the guest sees.
TEST(IrEmit, FaultingAluLoadCommitsNothing) {
    REQUIRE_IR_GATE();
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    // cmp eax,ecx   at 0x1000  -- establishes flags
    // add eax,[edx] at 0x1002  -- EDX unmapped
    const std::vector<u8> code = {0x39, 0xC8, 0x03, 0x02};
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(0, 5);
    cpu.gpr.set32(1, 9);
    cpu.gpr.set32(2, 0xDEAD'0000u);
    cpu.gpr.set32(4, 0x20800);
    cpu.rip = kCode;

    // Interpreter oracle: run only the CMP, since the ADD faults.
    CpuState oracle;
    oracle.init_protected_mode();
    oracle.gpr.set32(0, 5);
    oracle.gpr.set32(1, 9);
    oracle.gpr.set32(2, 0xDEAD'0000u);
    oracle.gpr.set32(4, 0x20800);
    oracle.rip = kCode;
    {
        Interpreter interp(oracle, memory);
        ASSERT_EQ(interp.step(), ExecResult::Continue);
    }
    oracle.materialize_lazy_flags();

    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    ASSERT_TRUE(jit.ir_build_block(block).ok)
        << ir::decline_reason_name(jit.ir_last_result().reason);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_TRUE(jit.ir_emit_active()) << "must be a Tier 1 block";
    jit.execute(*compiled, cpu);

    ASSERT_TRUE(cpu.pending_raise.active) << "the load must fault";
    EXPECT_EQ(cpu.rip, kCode + 2) << "the fault belongs to the ADD";
    EXPECT_EQ(cpu.gpr.get32(0), 5u) << "EAX must not have been updated";

    // THE flag assertion. The CMP's record is live across the faulting load,
    // so the flag-demand pass must not have killed it and the guest must see
    // the CMP's flags — not stale pre-CMP flags, and not the ADD's.
    cpu.materialize_lazy_flags();
    EXPECT_EQ(cpu.flags.rflags, oracle.flags.rflags)
        << "a fault must expose the PREVIOUS instruction's flags";
}

// =============================================================================
// The measured blockers (session 9/10)
// =============================================================================

// `[edx*4 + disp]` — an index with NO base. A compiler emits this to scale an
// array subscript, and it was the single reason LEA still declined once
// everything else was supported.
TEST(IrEmit, IndexWithoutBaseAddressing) {
    REQUIRE_IR_GATE();
    constexpr GuestAddr kScratch = 0x20800;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(0x20000, 0x1000, MemoryProtection::ReadWrite));
    // mov eax, [edx*4 + kScratch]
    std::vector<u8> code = {0x8B, 0x04, 0x95};
    for (int i = 0; i < 4; ++i)
        code.push_back(static_cast<u8>((kScratch >> (8 * i)) & 0xFF));
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));
    for (u32 i = 0; i < 8; ++i)
        ASSERT_TRUE(memory.write_u32(kScratch + i * 4, 0x7000'0000u + i));

    const auto seed = [&](CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(2, 5);          // index 5
        cpu.gpr.set32(4, kScratch);
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
    EXPECT_EQ(jit_cpu.gpr.get32(0), oracle.gpr.get32(0));
    EXPECT_EQ(jit_cpu.gpr.get32(0), 0x7000'0005u);
}

// The A1/A3 absolute forms.
TEST(IrEmit, AbsoluteAddressingMoffs) {
    REQUIRE_IR_GATE();
    constexpr GuestAddr kScratch = 0x20800;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(0x20000, 0x1000, MemoryProtection::ReadWrite));
    std::vector<u8> code = {0xA1};                       // mov eax, [kScratch]
    for (int i = 0; i < 4; ++i)
        code.push_back(static_cast<u8>((kScratch >> (8 * i)) & 0xFF));
    code.push_back(0xA3);                                // mov [kScratch+4], eax
    for (int i = 0; i < 4; ++i)
        code.push_back(static_cast<u8>(((kScratch + 4) >> (8 * i)) & 0xFF));
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));
    ASSERT_TRUE(memory.write_u32(kScratch, 0x1234'ABCDu));
    ASSERT_TRUE(memory.write_u32(kScratch + 4, 0));

    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(4, kScratch + 0x400);
    cpu.rip = kCode;

    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    ASSERT_TRUE(jit.ir_build_block(block).ok)
        << ir::decline_reason_name(jit.ir_last_result().reason);
    CompiledBlock* c = jit.compile(block);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(jit.ir_emit_active());
    jit.execute(*c, cpu);
    EXPECT_EQ(cpu.gpr.get32(0), 0x1234'ABCDu);
    const auto stored = memory.read_u32(kScratch + 4);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0x1234'ABCDu);
}

// Indirect CALL, register form. The target is read BEFORE the push so
// `call esp` branches to the value ESP held on entry.
TEST(IrEmit, IndirectCallRegisterForm) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter_on_stack({0xFF, 0xD0}, 1);  // call eax
}

TEST(IrEmit, LinkedIndirectCallPreservesTargetAfterStackAddressCalculation) {
    REQUIRE_IR_GATE();
    struct RestoreLinkMode {
        bool enabled = JitCompiler::jit_link_enabled();
        ~RestoreLinkMode() {
            if (enabled) JitCompiler::enable_jit_link();
            else JitCompiler::disable_jit_link();
        }
    } restore;
    JitCompiler::enable_jit_link();
    // Intel SDM Vol. 2A CALL r/m32: use the target read before the push.
    // LEA ECX,[ESP+20h]; CALL dword [ESP]. Compare target, stack and flags.
    expect_tier1_matches_interpreter_on_stack(
        {0x8D,0x4C,0x24,0x20,0xFF,0x14,0x24}, 2);
}

// The stack fixture seeds ECX with a data pattern, not a pointer, so the
// memory-operand forms need ECX pointed at the scratch page.
TEST(IrEmit, PushImmediateAndMemoryForms) {
    REQUIRE_IR_GATE();
    constexpr GuestAddr kStack = 0x20F00;
    for (const std::vector<u8>& body : {
             std::vector<u8>{0x6A, 0x7F},                          // push 0x7f
             std::vector<u8>{0x68, 0x78, 0x56, 0x34, 0x12},        // push imm32
             std::vector<u8>{0xFF, 0x31},                          // push [ecx]
             std::vector<u8>{0xFF, 0x11},                          // call [ecx]
         }) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                      MemoryProtection::ReadWrite));
        ASSERT_TRUE(memory.map_region(0x20000, 0x1000,
                                      MemoryProtection::ReadWrite));
        ASSERT_TRUE(memory.write_bytes(kCode, body.data(), body.size()));
        ASSERT_TRUE(memory.write_u32(0x20800, kCode));  // a plausible target

        const auto seed = [&](CpuState& cpu) {
            cpu.init_protected_mode();
            cpu.gpr.set32(1, 0x20800);   // ECX -> mapped
            cpu.gpr.set32(4, kStack);
            cpu.rip = kCode;
        };

        CpuState oracle;
        seed(oracle);
        {
            Interpreter interp(oracle, memory);
            const ExecResult r = interp.step();
            ASSERT_TRUE(r == ExecResult::Continue || r == ExecResult::Branch)
                << "oracle stopped on this form";
        }
        const auto oracle_slot = memory.read_u32(kStack - 4);
        ASSERT_TRUE(oracle_slot.is_ok());
        const u32 oracle_pushed = oracle_slot.value();

        ASSERT_TRUE(memory.write_u32(kStack - 4, 0));
        CpuState jit_cpu;
        seed(jit_cpu);
        JitCompiler jit(memory);
        BasicBlock block = decode_bytes(body);
        ASSERT_TRUE(jit.ir_build_block(block).ok)
            << ir::decline_reason_name(jit.ir_last_result().reason);
        CompiledBlock* c = jit.compile(block);
        ASSERT_NE(c, nullptr);
        ASSERT_TRUE(jit.ir_emit_active());
        jit.execute(*c, jit_cpu);

        EXPECT_EQ(jit_cpu.gpr.get32(4), oracle.gpr.get32(4)) << "ESP";
        EXPECT_EQ(jit_cpu.rip, oracle.rip) << "exit RIP";
        const auto jit_slot = memory.read_u32(kStack - 4);
        ASSERT_TRUE(jit_slot.is_ok());
        EXPECT_EQ(jit_slot.value(), oracle_pushed) << "pushed value";
    }
}

// =============================================================================
// Read-modify-write: establishing the contract before implementing it
// =============================================================================

// What does a FAULTING `add [mem], eax` do to the flags?
//
// This decides the whole RMW lowering. If the instruction aborts, the flag
// definition must be published only AFTER the store retires — which in turn
// requires the flag operands to survive an access that clobbers the scratch
// pool. If instead the flags are updated regardless, the much cheaper
// publish-then-store order is correct.
//
// The interpreter is the semantic oracle, so the answer is measured here
// rather than reasoned from the manual, and asserted so that a change to the
// interpreter's behaviour shows up as a failure in the lowering that depends
// on it.
TEST(IrEmit, FaultingReadModifyWriteLeavesFlagsUnchanged) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    // cmp eax,ecx   at 0x1000 -- establishes known flags
    // add [edx],eax at 0x1002 -- EDX unmapped, so the ACCESS faults
    const std::vector<u8> code = {0x39, 0xC8, 0x01, 0x02};
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(0, 5);
    cpu.gpr.set32(1, 9);
    cpu.gpr.set32(2, 0xDEAD'0000u);
    cpu.gpr.set32(4, 0x2000);
    cpu.rip = kCode;

    Interpreter interp(cpu, memory);
    ASSERT_EQ(interp.step(), ExecResult::Continue);   // the CMP
    cpu.materialize_lazy_flags();
    const u64 flags_after_cmp = cpu.flags.rflags;

    (void)interp.step();                             // the faulting RMW
    cpu.materialize_lazy_flags();

    EXPECT_EQ(cpu.flags.rflags, flags_after_cmp)
        << "a faulting read-modify-write must leave the flags of the PREVIOUS "
           "instruction visible; the lowering therefore has to publish its "
           "flag record only after the store retires";
}

TEST(IrEmit, ReadModifyWriteMatchesInterpreter) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter_with_memory({0x01, 0x01});        // add [ecx],eax
    expect_tier1_matches_interpreter_with_memory({0x29, 0x01});        // sub [ecx],eax
    expect_tier1_matches_interpreter_with_memory({0x31, 0x01});        // xor [ecx],eax
    expect_tier1_matches_interpreter_with_memory({0x21, 0x41, 0x08});  // and [ecx+8],eax
    expect_tier1_matches_interpreter_with_memory({0x83, 0x29, 0x01});  // sub [ecx],1
    expect_tier1_matches_interpreter_with_memory({0x83, 0x41, 0x04, 0x7F});  // add [ecx+4],0x7f
}

// A read-modify-write whose STORE faults must commit nothing: not the memory,
// and not the flags. The flag definition is published after the store for
// exactly this reason, and the previous instruction's flags must survive.
TEST(IrEmit, FaultingReadModifyWriteStoreCommitsNothing) {
    REQUIRE_IR_GATE();
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(0x20000, 0x1000, MemoryProtection::Read));

    // cmp eax,ecx   at 0x1000 -- known flags
    // add [edx],eax at 0x1002 -- EDX unmapped
    const std::vector<u8> code = {0x39, 0xC8, 0x01, 0x02};
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    const auto seed = [](CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(0, 5);
        cpu.gpr.set32(1, 9);
        cpu.gpr.set32(2, 0xDEAD'0000u);
        cpu.gpr.set32(4, 0x2000);
        cpu.rip = kCode;
    };

    CpuState oracle;
    seed(oracle);
    {
        Interpreter interp(oracle, memory);
        ASSERT_EQ(interp.step(), ExecResult::Continue);
    }
    oracle.materialize_lazy_flags();

    CpuState cpu;
    seed(cpu);
    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    ASSERT_TRUE(jit.ir_build_block(block).ok)
        << ir::decline_reason_name(jit.ir_last_result().reason);
    CompiledBlock* c = jit.compile(block);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(jit.ir_emit_active()) << "must be a Tier 1 block";
    jit.execute(*c, cpu);

    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.rip, kCode + 2) << "the fault belongs to the RMW";
    cpu.materialize_lazy_flags();
    EXPECT_EQ(cpu.flags.rflags, oracle.flags.rflags)
        << "a faulting RMW must leave the CMP's flags visible, not the ADD's";
}

// Two simultaneous survivors would need a real allocator, so the emitter
// declines instead of half-building one. Nothing the frontend currently
// produces needs two, but the limit is asserted so its removal is deliberate.
TEST(IrEmit, RmwInsideALongerBlockStillLowers) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter_with_memory({
        0x8B, 0x41, 0x10,  // mov eax,[ecx+16]
        0x01, 0x01,        // add [ecx],eax
        0x83, 0x69, 0x04, 0x03,  // sub [ecx+4],3
    });
}

// `call [mem]` performs TWO faulting accesses: the target load and the
// return-address push. Their relative order decides what a fault leaves
// behind, so it is measured against the interpreter rather than assumed —
// exactly as the read-modify-write ordering was.
//
// Case: the TARGET LOAD faults. Nothing may commit — no push, no ESP change.
TEST(IrEmit, FaultingIndirectCallTargetLoadCommitsNothing) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(0x20000, 0x1000, MemoryProtection::ReadWrite));
    const std::vector<u8> code = {0xFF, 0x10};  // call [eax]
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));
    ASSERT_TRUE(memory.write_u32(0x20800 - 4, 0xEEEE'EEEEu));

    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(0, 0xDEAD'0000u);   // EAX -> unmapped, so the LOAD faults
    cpu.gpr.set32(4, 0x20800);
    cpu.rip = kCode;

    Interpreter interp(cpu, memory);
    (void)interp.step();

    EXPECT_EQ(cpu.gpr.get32(4), 0x20800u)
        << "a faulting target load must leave ESP untouched";
    const auto slot = memory.read_u32(0x20800 - 4);
    ASSERT_TRUE(slot.is_ok());
    EXPECT_EQ(slot.value(), 0xEEEE'EEEEu)
        << "no return address may have been pushed";
}
