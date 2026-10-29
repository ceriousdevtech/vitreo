// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_emit_branch.cpp
/// @brief Tier 1 IR emission parity: control flow (M7); contract in test_ir_emit.cpp.

#include "ir_emit_test_support.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_emit;

// =============================================================================
// Control flow (M7)
// =============================================================================

namespace {

/// Run a terminal-branch block under Tier 1 and report the guest RIP it exits
/// to, with the interpreter running the same bytes as the oracle.
struct BranchOutcome {
    u64 jit_rip{0};
    u64 interp_rip{0};
    u32 jit_eax{0};
    u32 interp_eax{0};
};

BranchOutcome run_branch_block(const std::vector<u8>& code, u32 eax, u32 ecx) {
    BranchOutcome out;
    GuestMemory memory;
    EXPECT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    EXPECT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    const auto seed = [&](CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(0, eax);
        cpu.gpr.set32(1, ecx);
        cpu.gpr.set32(4, 0x20800);
        cpu.rip = kCode;
    };

    const u32 insn_count =
        static_cast<u32>(decode_bytes(code).instruction_count);
    CpuState interp_cpu;
    seed(interp_cpu);
    {
        Interpreter interp(interp_cpu, memory);
        for (u32 i = 0; i < insn_count; ++i) {
            if (interp.step() != ExecResult::Continue) break;
        }
    }
    out.interp_rip = interp_cpu.rip;
    out.interp_eax = interp_cpu.gpr.get32(0);

    CpuState jit_cpu;
    seed(jit_cpu);
    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    EXPECT_TRUE(jit.ir_build_block(block).ok)
        << ir::decline_reason_name(jit.ir_last_result().reason);
    CompiledBlock* compiled = jit.compile(block);
    EXPECT_NE(compiled, nullptr);
    EXPECT_TRUE(jit.ir_emit_active())
        << "block fell back to Tier 0; this test would be asserting Tier 0";
    if (compiled != nullptr) jit.execute(*compiled, jit_cpu);
    out.jit_rip = jit_cpu.rip;
    out.jit_eax = jit_cpu.gpr.get32(0);
    return out;
}

}  // namespace

// BOTH edges, because a branch that always falls through passes a
// taken-edge-only test and vice versa.
TEST(IrEmit, ConditionalBranchTakesBothEdgesCorrectly) {
    REQUIRE_IR_GATE();
    const std::vector<u8> code = {0x39, 0xC8, 0x74, 0x05};  // cmp eax,ecx; jz +5
    {   // equal -> taken: 0x1004 + 5
        const BranchOutcome o = run_branch_block(code, 7, 7);
        EXPECT_EQ(o.jit_rip, o.interp_rip);
        EXPECT_EQ(o.jit_rip, kCode + 9);
    }
    {   // not equal -> fall through to 0x1004
        const BranchOutcome o = run_branch_block(code, 7, 9);
        EXPECT_EQ(o.jit_rip, o.interp_rip);
        EXPECT_EQ(o.jit_rip, kCode + 4);
    }
}

// Every condition code. The condition is derived by ARITHMETIC on the opcode
// enum, so an off-by-one would affect exactly one code out of sixteen and a
// JZ-only test would never see it.
TEST(IrEmit, EveryConditionCodeMatchesTheInterpreter) {
    REQUIRE_IR_GATE();
    const std::vector<std::pair<u32, u32>> operands = {
        {7u, 7u}, {7u, 9u}, {0x8000'0000u, 1u}, {0xFFFF'FFFFu, 1u}, {1u, 3u},
    };
    for (u8 cc = 0; cc <= 0xF; ++cc) {
        const std::vector<u8> code = {
            0x39, 0xC8,                        // cmp eax, ecx
            static_cast<u8>(0x70 + cc), 0x05,  // jcc +5
        };
        for (const auto& op : operands) {
            const BranchOutcome o = run_branch_block(code, op.first, op.second);
            EXPECT_EQ(o.jit_rip, o.interp_rip)
                << "condition 0x" << std::hex << int(cc)
                << " eax=" << op.first << " ecx=" << op.second;
        }
    }
}

TEST(IrEmit, UnconditionalBranch) {
    REQUIRE_IR_GATE();
    const BranchOutcome o = run_branch_block({0xEB, 0x05}, 1, 2);
    EXPECT_EQ(o.jit_rip, o.interp_rip);
    EXPECT_EQ(o.jit_rip, kCode + 7);
}

// Register state must commit on BOTH edges — a lowering that stored pins on
// only one of them would pass the RIP check and silently corrupt the guest.
TEST(IrEmit, RegisterStateIsCommittedOnBothBranchEdges) {
    REQUIRE_IR_GATE();
    const std::vector<u8> code = {
        0x01, 0xC8,        // add eax, ecx
        0x39, 0xC8,        // cmp eax, ecx
        0x75, 0x05,        // jnz +5
    };
    const std::vector<std::pair<u32, u32>> cases = {{5u, 3u}, {0u, 0u}};
    for (const auto& c : cases) {
        const BranchOutcome o = run_branch_block(code, c.first, c.second);
        EXPECT_EQ(o.jit_rip, o.interp_rip);
        EXPECT_EQ(o.jit_eax, o.interp_eax) << "EAX must commit on either edge";
    }
}
