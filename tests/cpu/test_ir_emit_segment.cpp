// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_emit_segment.cpp
/// @brief Tier 1 IR emission parity: segment-override addressing; contract in test_ir_emit.cpp.

#include "ir_emit_test_support.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_emit;

// Segment-override addressing.
//
// `mov eax, fs:[0x18]` is how a Win32 guest reaches its TEB, and on
// RollerCoaster Tycoon 3 the FS override alone was 3014 declines — 35% of
// everything left. The base comes from CpuState, so every segment uses the
// same mechanism; FS is just the one with a non-zero base in practice, which
// is exactly why a test that only used a zero-base segment would prove
// nothing.
namespace {

void expect_segment_matches_interpreter(const std::vector<u8>& code,
                                        const char* what) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                  MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(0x30000, 0x1000,
                                  MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));
    ASSERT_TRUE(memory.write_u32(0x30018, 0xCAFE'B0BAu));
    ASSERT_TRUE(memory.write_u32(0x2000, 0x1111'2222u));

    const auto seed = [](CpuState& cpu) {
        cpu.init_protected_mode();
        // A NON-ZERO FS base: with a zero base the segment add is invisible
        // and the test would pass without the feature.
        cpu.seg.fs_desc.base = 0x30000;
        cpu.gpr.set32(0, 0x1234'5678u);
        cpu.gpr.set32(1, 0x2000);
        cpu.gpr.set32(3, 0x18);
        cpu.gpr.set32(4, 0x3000);
        cpu.rip = kCode;
    };

    CpuState oracle;
    seed(oracle);
    {
        Interpreter interp(oracle, memory);
        ASSERT_EQ(interp.step(), ExecResult::Continue) << what;
    }
    oracle.materialize_lazy_flags();

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
    jit_cpu.materialize_lazy_flags();

    for (u8 r = 0; r < 8; ++r) {
        EXPECT_EQ(jit_cpu.gpr.get32(r), oracle.gpr.get32(r))
            << what << ": GPR " << static_cast<int>(r);
    }
    EXPECT_EQ(jit_cpu.flags.rflags, oracle.flags.rflags) << what << ": EFLAGS";
}

}  // namespace

TEST(IrEmitSegment, FsAbsoluteLoadReadsThroughTheSegmentBase) {
    REQUIRE_IR_GATE();
    // mov eax, fs:[0x18]  — the TEB self-pointer read.
    expect_segment_matches_interpreter(
        {0x64, 0xA1, 0x18, 0x00, 0x00, 0x00}, "mov eax, fs:[0x18]");
}

TEST(IrEmitSegment, FsBaseRelativeAndIndexedForms) {
    REQUIRE_IR_GATE();
    // mov eax, fs:[ebx]     — base register plus the segment base
    expect_segment_matches_interpreter({0x64, 0x8B, 0x03}, "mov eax, fs:[ebx]");
    // mov eax, fs:[ebx+4]
    expect_segment_matches_interpreter({0x64, 0x8B, 0x43, 0x04},
                                       "mov eax, fs:[ebx+4]");
}

TEST(IrEmitSegment, FsStoreWritesThroughTheSegmentBase) {
    REQUIRE_IR_GATE();
    expect_segment_matches_interpreter({0x64, 0x89, 0x03}, "mov fs:[ebx], eax");
}

TEST(IrEmitSegment, FsCompareReadsThroughTheSegmentBase) {
    REQUIRE_IR_GATE();
    expect_segment_matches_interpreter({0x64, 0x3B, 0x03}, "cmp eax, fs:[ebx]");
}

// A stack instruction reaches memory through an IMPLICIT operand that never
// carries `mem.segment`, so an override on it would be silently dropped.
// It must keep declining rather than quietly ignore the prefix.
TEST(IrEmitSegment, SegmentOverrideOnAnImplicitStackOperandDeclines) {
    REQUIRE_IR_GATE();
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                  MemoryProtection::ReadWrite));
    const std::vector<u8> code = {0x64, 0x50};   // fs: push eax
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));
    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    EXPECT_FALSE(jit.ir_build_block(block).ok)
        << "an override the lowering cannot apply must decline, not be ignored";
}
