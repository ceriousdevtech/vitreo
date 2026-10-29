// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_x64_fused_branch.cpp
/// @brief Long-mode IR emission: fused terminal Jcc.

// These tests execute emitted ARM64, so Intel hosts must not reference the
// native-only IR backend through this translation unit.
#if defined(__aarch64__)
#include "ir_x64_memory_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_x64_memory;

namespace {
// =============================================================================
// Fused terminal Jcc (NZCV residency in the long-mode snapshot emitter)
// =============================================================================

// A compare whose flags only the block's terminal Jcc reads is emitted as an
// S-form and the branch reads host PSTATE instead of re-deriving the
// condition from the lazy record. Every x86 condition, every kind the
// emitter claims (Sub / Add / And at 32 and 64 bits, Sub8 on zero-extended
// bytes) and the operand boundaries where ARM and x86 disagree about the
// carry sense, compared against the interpreter: the branch taken (RIP), all
// sixteen GPRs and the materialized RFLAGS the successor would see.
class IrEmit64FusedBranch : public IrEmit64Memory {
protected:
    static constexpr u8 kRel = 0x10;
    void expect_branch_matches(std::vector<u8> prefix, unsigned prefix_insns,
                               const std::vector<std::pair<u64, u64>>& rcx_rdx,
                               bool memory_byte, u16 fused_conditions) {
        for (u8 cc = 0; cc < 16; ++cc) {
            std::vector<u8> bytes = prefix;
            bytes.push_back(static_cast<u8>(0x70 + cc));
            bytes.push_back(kRel);
            SCOPED_TRACE(testing::Message() << "jcc 0x" << std::hex
                                            << unsigned(0x70 + cc));
            jit = std::make_unique<JitCompiler>(memory);
            auto* native = compile(bytes);
            ASSERT_NE(native, nullptr);
            if ((fused_conditions >> cc) & 1u) {
                // The lazy-condition dispatch alone is ~130 words; a fused
                // compare-and-branch block is well under it.
                EXPECT_LT(native->code_size / 4u, 110u)
                    << "terminal Jcc did not fuse";
            }
            for (const auto& [rcx, rdx] : rcx_rdx) {
                auto oracle = seed();
                oracle.gpr.rax = kData;
                oracle.gpr.rcx = rcx;
                oracle.gpr.rdx = rdx;
                if (memory_byte)
                    ASSERT_TRUE(memory.write_u64(kData, rdx).is_ok());
                auto state = oracle;
                {
                    Interpreter interpreter(oracle, memory);
                    for (unsigned step = 0; step < prefix_insns; ++step)
                        ASSERT_EQ(interpreter.step(), ExecResult::Continue);
                    // The Jcc: Branch when taken, Continue when not.
                    const ExecResult last = interpreter.step();
                    ASSERT_TRUE(last == ExecResult::Continue ||
                                last == ExecResult::Branch);
                }
                ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
                EXPECT_EQ(state.rip, oracle.rip)
                    << "rcx=" << std::hex << rcx << " rdx=" << rdx;
                for (u8 index = 0; index < 16; ++index)
                    EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                        << "GPR " << unsigned(index) << " rcx=" << std::hex << rcx;
                state.materialize_lazy_flags();
                oracle.materialize_lazy_flags();
                EXPECT_EQ(state.flags.rflags, oracle.flags.rflags)
                    << "rcx=" << std::hex << rcx << " rdx=" << rdx;
            }
        }
    }
    static std::vector<std::pair<u64, u64>> wide_pairs() {
        const u64 values[] = {0, 1, 5, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull,
                              0xFFFFFFFFFFFFFFFFull, 0x00000000FFFFFFFFull,
                              0x0000000080000000ull, 0x123456789ABCDEF0ull};
        std::vector<std::pair<u64, u64>> pairs;
        for (u64 a : values)
            for (u64 b : values) pairs.emplace_back(a, b);
        return pairs;
    }
    static std::vector<std::pair<u64, u64>> byte_pairs() {
        // High bits set on purpose: a compare that read the whole register
        // instead of the byte would disagree on exactly these.
        const u64 values[] = {0x00, 0x01, 0x04, 0x7F, 0x80, 0xFE, 0xFF,
                              0xAAAAAAAAAAAAAA04ull, 0x5555555555555580ull};
        std::vector<std::pair<u64, u64>> pairs;
        for (u64 a : values)
            for (u64 b : values) pairs.emplace_back(a, b);
        return pairs;
    }
};

// Which conditions each claim serves from PSTATE (fused_arm_cond): a
// subtract, all of them; an add, all but BE/A (its carry has x86's sense,
// and CF|ZF has no single ARM condition); AND/TEST leave C clear, so not
// the unsigned ones; a byte compare (Sub8), only equality and unsigned.
constexpr u16 kFuseAll = 0xFFFF;
constexpr u16 kFuseAdd = static_cast<u16>(~((1u << 6) | (1u << 7)));
constexpr u16 kFuseAnd = static_cast<u16>(~((1u << 2) | (1u << 3) | (1u << 6) | (1u << 7)));
constexpr u16 kFuseSub8 = (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) |
                          (1u << 6) | (1u << 7);

TEST_F(IrEmit64FusedBranch, SixtyFourBitCompareSubtractAddTestAnd) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    expect_branch_matches({0x48, 0x39, 0xD1}, 1, wide_pairs(), false, kFuseAll);  // cmp rcx,rdx
    expect_branch_matches({0x48, 0x29, 0xD1}, 1, wide_pairs(), false, kFuseAll);  // sub rcx,rdx
    expect_branch_matches({0x48, 0x01, 0xD1}, 1, wide_pairs(), false, kFuseAdd);  // add rcx,rdx
    expect_branch_matches({0x48, 0x85, 0xD1}, 1, wide_pairs(), false, kFuseAnd);  // test rcx,rdx
    expect_branch_matches({0x48, 0x21, 0xD1}, 1, wide_pairs(), false, kFuseAnd);  // and rcx,rdx
}

// A narrow TEST/AND claims the full And kind through a width-shifted ANDS:
// SF is bit 7 or 15 of the result, which a plain 32-bit ANDS of the
// zero-extended operands would leave clear -- so JS/JNS/JL/JGE/JLE/JG are the
// conditions this has to get right, and the byte pairs carry high garbage
// above the operand width on purpose.
TEST_F(IrEmit64FusedBranch, NarrowTestAndAndClaimSignAndZeroAtTheirWidth) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    expect_branch_matches({0x84, 0xD1}, 1, byte_pairs(), false, kFuseAnd);        // test cl,dl
    expect_branch_matches({0x20, 0xD1}, 1, byte_pairs(), false, kFuseAnd);        // and cl,dl
    expect_branch_matches({0x84, 0xC9}, 1, byte_pairs(), false, kFuseAnd);        // test cl,cl
    expect_branch_matches({0xF6, 0xC1, 0x80}, 1, byte_pairs(), false, kFuseAnd);  // test cl,0x80
    expect_branch_matches({0x84, 0x08}, 1, byte_pairs(), true, kFuseAnd);         // test [rax],cl
    expect_branch_matches({0x66, 0x85, 0xD1}, 1, wide_pairs(), false, kFuseAnd);  // test cx,dx
    expect_branch_matches({0x66, 0x21, 0xD1}, 1, wide_pairs(), false, kFuseAnd);  // and cx,dx
    expect_branch_matches({0x66, 0xF7, 0xC1, 0x00, 0x80}, 1, wide_pairs(), false,
                          kFuseAnd);                                             // test cx,0x8000
}

TEST_F(IrEmit64FusedBranch, ThirtyTwoBitCompareSubtractAddTestAnd) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    expect_branch_matches({0x39, 0xD1}, 1, wide_pairs(), false, kFuseAll);  // cmp ecx,edx
    expect_branch_matches({0x29, 0xD1}, 1, wide_pairs(), false, kFuseAll);  // sub ecx,edx
    expect_branch_matches({0x01, 0xD1}, 1, wide_pairs(), false, kFuseAdd);  // add ecx,edx
    expect_branch_matches({0x85, 0xD1}, 1, wide_pairs(), false, kFuseAnd);  // test ecx,edx
    expect_branch_matches({0x21, 0xD1}, 1, wide_pairs(), false, kFuseAnd);  // and ecx,edx
}

// The claim survives what sits between a compare and its branch in compiled
// code: a store (whose primitive preserves NZCV when asked -- around a
// region-cache bounds check, or in a linear cold stub) and address
// arithmetic. Run under both memory models (VITREO_TEST_IR_LINEAR64).
TEST_F(IrEmit64FusedBranch, CompareSurvivesStoreAndAddressArithmetic) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // cmp rcx,rdx ; mov [rax],rcx ; lea rbx,[rcx+8] ; jcc
    expect_branch_matches({0x48, 0x39, 0xD1, 0x48, 0x89, 0x08,
                           0x48, 0x8D, 0x59, 0x08}, 3, wide_pairs(), false, 0);
    // cmp ecx,edx ; mov dword [rax],edx ; mov byte [rax+4],cl ; jcc
    expect_branch_matches({0x39, 0xD1, 0x89, 0x10, 0x88, 0x48, 0x04},
                          3, wide_pairs(), false, 0);
}

// The same claim when the store takes its COLD path: a write-watched page
// sends a linear store through its out-of-line stub and a recording helper,
// which is exactly where host NZCV would be destroyed unless the stub saves
// it (stub_spill_nzcv, set by should_hold_nzcv while the claim is live).
TEST_F(IrEmit64FusedBranch, CompareSurvivesAWatchedStoresColdPath) {
    if (!memory.is_linear()) GTEST_SKIP() << "needs VITREO_TEST_IR_LINEAR64";
    ASSERT_TRUE(memory.map_region(kData, 0x4000, MemoryProtection::ReadWrite));
    std::array<u8, 8> dirty{};
    std::array<u8, 8> armed{};
    write_watch::register_region(kData, 0x4000, dirty.data(), armed.data(),
                                 /*executable=*/false);
    // cmp rcx,rdx ; mov [rax],rcx ; jcc
    expect_branch_matches({0x48, 0x39, 0xD1, 0x48, 0x89, 0x08}, 2,
                          wide_pairs(), false, 0);
    write_watch::unregister_region(kData);
}

TEST_F(IrEmit64FusedBranch, ByteComparesFromMemoryRegistersAndImmediates) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // cmp byte [rax], 4 and cmp byte [rax], 0xFF; the memory byte is rdx's
    // low byte. Fusion is not asserted: the access sits in the block.
    expect_branch_matches({0x80, 0x38, 0x04}, 1, byte_pairs(), true, 0);
    expect_branch_matches({0x80, 0x38, 0xFF}, 1, byte_pairs(), true, 0);
    // cmp cl, dl ; cmp ch, dl (a legacy high byte) ; cmp cl, 0x80
    expect_branch_matches({0x38, 0xD1}, 1, byte_pairs(), false, kFuseSub8);
    expect_branch_matches({0x38, 0xD5}, 1, byte_pairs(), false, kFuseSub8);
    expect_branch_matches({0x80, 0xF9, 0x80}, 1, byte_pairs(), false, kFuseSub8);
}
} // namespace
#endif
