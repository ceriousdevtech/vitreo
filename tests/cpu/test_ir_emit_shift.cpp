// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_emit_shift.cpp
/// @brief Tier 1 IR emission parity: immediate shifts; contract in test_ir_emit.cpp.

#include "ir_emit_test_support.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_emit;

// Immediate shifts.
//
// The count-dependent flag rules are the content here, and they are why the
// seeds below cross a sign boundary and set the bits that get shifted out:
//   - count 0 modifies NOTHING, not even ZF
//   - CF is the last bit shifted out
//   - OF is defined only for count 1
//   - the count is masked to 5 bits, so 32 and 33 are 0 and 1
namespace {

/// Flags a shift ARCHITECTURALLY defines: CF, PF, ZF, SF always; OF only for a
/// count of 1. AF is undefined for every shift.
///
/// The comparison is masked to these because Vitreo's lazy `Shl32`/`Shr32`/
/// `Sar32` record and the interpreter disagree on the undefined ones — the
/// record CLEARS AF and clears OF for counts above 1, while the interpreter
/// leaves both unchanged. That difference is Tier 0's too: it publishes the
/// same record through the same `emit_set_shift_lazy_flags`. Asserting on
/// undefined bits would be asserting that two tiers agree about something the
/// SDM says nothing about, and would fail for a lowering that is correct.
constexpr u64 kShiftDefinedFlags = 0x1 | 0x4 | 0x40 | 0x80;   // CF PF ZF SF
constexpr u64 kOverflowFlag = 0x800;

void expect_shift_matches_interpreter(const std::vector<u8>& code,
                                      const char* what,
                                      bool count_is_one = false) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                  MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    const auto seed = [](CpuState& cpu) {
        cpu.init_protected_mode();
        // Bit 31 and bit 0 both set, so a left shift drops a 1 into CF and a
        // right shift does too — a lowering that got CF from the wrong end
        // still differs from the oracle.
        cpu.gpr.set32(0, 0x8000'0001u);
        cpu.gpr.set32(1, 0x7FFF'FFFFu);
        cpu.gpr.set32(3, 0x0000'00FFu);
        cpu.gpr.set32(4, 0x3000);
        cpu.rip = kCode;
        // A DISTINCTIVE prior flag state: a zero-count shift must leave it
        // exactly as it was, which a test starting from cleared flags could
        // not tell from "the shift cleared everything".
        cpu.flags.rflags = 0x8D5;
    };

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

    // One interpreter step PER INSTRUCTION. Stepping once while the JIT runs
    // the whole block makes the oracle execute less of the program than the
    // thing under test, and it reports the JIT's correct answer as a
    // divergence. Second time this harness shape has been written; it is worth
    // copying the loop, not the single step.
    CpuState oracle;
    seed(oracle);
    {
        Interpreter interp(oracle, memory);
        for (u32 i = 0; i < block.instruction_count; ++i) {
            ASSERT_EQ(interp.step(), ExecResult::Continue) << what;
        }
    }
    oracle.materialize_lazy_flags();

    jit.execute(*compiled, jit_cpu);
    jit_cpu.materialize_lazy_flags();

    EXPECT_EQ(jit_cpu.gpr.get32(0), oracle.gpr.get32(0)) << what << ": EAX";
    const u64 mask =
        kShiftDefinedFlags | (count_is_one ? kOverflowFlag : 0ull);
    EXPECT_EQ(jit_cpu.flags.rflags & mask, oracle.flags.rflags & mask)
        << what << ": defined EFLAGS (got 0x" << std::hex
        << (jit_cpu.flags.rflags & mask) << ", oracle 0x"
        << (oracle.flags.rflags & mask) << std::dec << ")";
}

}  // namespace

TEST(IrEmitShift, ShlByOneAndByImmediate) {
    REQUIRE_IR_GATE();
    expect_shift_matches_interpreter({0xD1, 0xE0}, "shl eax, 1", true);
    expect_shift_matches_interpreter({0xC1, 0xE0, 0x04}, "shl eax, 4");
    expect_shift_matches_interpreter({0xC1, 0xE0, 0x1F}, "shl eax, 31");
}

TEST(IrEmitShift, ShrByOneAndByImmediate) {
    REQUIRE_IR_GATE();
    expect_shift_matches_interpreter({0xD1, 0xE8}, "shr eax, 1", true);
    expect_shift_matches_interpreter({0xC1, 0xE8, 0x04}, "shr eax, 4");
    expect_shift_matches_interpreter({0xC1, 0xE8, 0x1F}, "shr eax, 31");
}

// SAR differs from SHR in the bits it feeds in, and the seed has bit 31 set
// so the two produce different results and different SF.
TEST(IrEmitShift, SarIsArithmetic) {
    REQUIRE_IR_GATE();
    expect_shift_matches_interpreter({0xD1, 0xF8}, "sar eax, 1", true);
    expect_shift_matches_interpreter({0xC1, 0xF8, 0x04}, "sar eax, 4");
    expect_shift_matches_interpreter({0xC1, 0xF8, 0x1F}, "sar eax, 31");
}

// THE case. `shl eax, 0` modifies the destination and the flags NOT AT ALL.
// The seed sets a distinctive prior RFLAGS precisely so "unchanged" is
// distinguishable from "cleared".
TEST(IrEmitShift, ZeroCountModifiesNothingIncludingFlags) {
    REQUIRE_IR_GATE();
    expect_shift_matches_interpreter({0xC1, 0xE0, 0x00}, "shl eax, 0");
    expect_shift_matches_interpreter({0xC1, 0xE8, 0x00}, "shr eax, 0");
    expect_shift_matches_interpreter({0xC1, 0xF8, 0x00}, "sar eax, 0");
}

// The count is masked to 5 bits, so 32 is a ZERO count (no flags at all) and
// 33 is a shift by 1. A lowering that forgot the mask would shift by 32 —
// undefined on ARM and wrong either way.
TEST(IrEmitShift, CountIsMaskedToFiveBits) {
    REQUIRE_IR_GATE();
    expect_shift_matches_interpreter({0xC1, 0xE0, 0x20}, "shl eax, 32 (= 0)");
    expect_shift_matches_interpreter({0xC1, 0xE0, 0x21}, "shl eax, 33 (= 1)", true);
    expect_shift_matches_interpreter({0xC1, 0xE8, 0x20}, "shr eax, 32 (= 0)");
}

// A shift followed by a conditional branch: the record has to materialize
// correctly through the fused path, not just through `materialize_lazy_flags`.
TEST(IrEmitShift, ShiftThenConditionalBranchMatchesInterpreter) {
    REQUIRE_IR_GATE();
    expect_shift_matches_interpreter({0xC1, 0xE8, 0x04,   // shr eax, 4
                                      0x39, 0xC8},        // cmp eax, ecx
                                     "shr then cmp");
}

// A CL-count shift must still DECLINE: the count is a runtime value that may
// be zero, and a zero count modifies no flags — so it cannot be lowered as a
// full-flag writer without a branch this form does not emit.
TEST(IrEmitShift, ClCountShiftDeclines) {
    REQUIRE_IR_GATE();
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                  MemoryProtection::ReadWrite));
    const std::vector<u8> code = {0xD3, 0xE0};   // shl eax, cl
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));
    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    EXPECT_FALSE(jit.ir_build_block(block).ok)
        << "a runtime count can be zero, which modifies no flags";
}
