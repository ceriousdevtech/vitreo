// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_emit_regalloc_fusion.cpp
/// @brief Tier 1 IR emission parity: register allocation and Jcc fusion; contract in test_ir_emit.cpp.

#include "ir_emit_test_support.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_emit;

// =============================================================================
// Register allocation (M6)
// =============================================================================

// The shape that exposed the old allocator. `sub [ecx], imm` defines FOUR
// values — the address, the loaded value, the constant, and the result — but
// never has more than three simultaneously live. A running count of
// DEFINITIONS rejected it; live intervals accept it.
//
// This asserts the block is lowered by Tier 1, not merely that it is correct:
// before the interval scan it fell back to Tier 0 with
// `backend-reg-pressure`, which no result comparison would have noticed.
TEST(IrEmit, FourDefinitionsThreeLiveFitsThePool) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter_with_memory({0x83, 0x29, 0x01});
    expect_tier1_matches_interpreter_with_memory({0x83, 0x41, 0x08, 0x7F});
}

// A longer instruction mix in one block, to exercise interval retirement
// across several guest instructions in sequence.
TEST(IrEmit, MixedBlockAllocatesWithoutCollision) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter_with_memory({
        0x8B, 0x41, 0x10,        // mov eax,[ecx+16]
        0x03, 0x41, 0x14,        // add eax,[ecx+20]
        0x83, 0x29, 0x01,        // sub [ecx],1
        0x89, 0x41, 0x04,        // mov [ecx+4],eax
        0x8D, 0x54, 0x81, 0x08,  // lea edx,[ecx+eax*4+8]
    });
}

// =============================================================================
// Jcc fusion
// =============================================================================

// Asserted on the EMITTED WORDS, because a fusion that silently fails back to
// lazy evaluation is still correct — every condition test would keep passing
// while the optimization did nothing.
TEST(IrEmit, CompareAndBranchFuseIntoAFlagSettingSubAndBcond) {
    REQUIRE_IR_GATE();
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    const std::vector<u8> code = {0x39, 0xC8, 0x74, 0x05};  // cmp eax,ecx; jz +5
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    ASSERT_TRUE(jit.ir_build_block(block).ok);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_TRUE(jit.ir_emit_active());

    const u32* words = static_cast<const u32*>(compiled->code);
    const u32 count = compiled->code_size / 4u;
    bool has_subs = false, has_bcond = false, has_cbz = false;
    for (u32 i = 0; i < count; ++i) {
        // EAX is X23, ECX is X24 under the fixed 32-bit pin layout.
        if (words[i] == vitreo::cpu::jit::arm64::subs_w(0, 23, 24) ||
            words[i] == vitreo::cpu::jit::arm64::subs_w(1, 23, 24) ||
            words[i] == vitreo::cpu::jit::arm64::subs_w(2, 23, 24)) {
            has_subs = true;
        }
        if ((words[i] & 0xFF00'0010u) == 0x5400'0000u) has_bcond = true;
        if ((words[i] & 0x7F00'0000u) == 0x3400'0000u) has_cbz = true;
    }
    EXPECT_TRUE(has_subs) << "the compare must use the flag-setting form";
    EXPECT_TRUE(has_bcond) << "the branch must read PSTATE directly";
    EXPECT_FALSE(has_cbz)
        << "a CBZ means the condition was reconstructed from the record";
}

// ...and the case that must NOT fuse. `test` leaves ARM C and V cleared, so
// the UNSIGNED conditions invert: `fused_arm_cond` rejects them and the branch
// falls back to reading the architectural record. Correctness is covered by
// EveryConditionCodeMatchesTheInterpreter; this pins the mechanism.
TEST(IrEmit, UnsignedConditionAfterTestDoesNotFuse) {
    REQUIRE_IR_GATE();
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    const std::vector<u8> code = {0x85, 0xC8, 0x76, 0x05};  // test eax,ecx; jbe +5
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    ASSERT_TRUE(jit.ir_build_block(block).ok);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_TRUE(jit.ir_emit_active());

    const u32* words = static_cast<const u32*>(compiled->code);
    const u32 count = compiled->code_size / 4u;
    bool has_cbz = false;
    for (u32 i = 0; i < count; ++i) {
        if ((words[i] & 0x7F00'0000u) == 0x3400'0000u) has_cbz = true;
    }
    EXPECT_TRUE(has_cbz)
        << "BE after TEST tests x86 CF, which ANDS forced to 0 — it must not "
           "read PSTATE";
}
