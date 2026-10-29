// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_x64_side_exit.cpp
/// @brief Long-mode IR emission: mid-trace conditional exits (Op::CondExit).

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
// Mid-trace conditional exits (Op::CondExit)
// =============================================================================

// A long-mode trace continues past its conditional branches along the
// fall-through, so each Jcc but the last is a side exit. The interpreter steps
// exactly what the native block executes -- up to the first TAKEN branch, or
// through the whole block -- and the two must agree on RIP, every GPR, guest
// memory, and the materialized RFLAGS a successor would see. Operand values
// put every branch on both sides.
class IrEmit64SideExit : public IrEmit64Memory {
protected:
    void expect_trace_matches(const std::vector<u8>& bytes,
                              const std::vector<std::array<u64, 3>>& rcx_rdx_mem) {
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(bytes);
        ASSERT_NE(native, nullptr);
        MemoryByteReader reader(bytes.data(), bytes.size());
        u32 instructions = 0;
        while (reader.remaining()) {
            DecodedInstruction instruction;
            ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, instruction),
                      DecodeError::Success);
            ++instructions;
        }
        for (const auto& [rcx, rdx, mem] : rcx_rdx_mem) {
            auto oracle = seed();
            oracle.gpr.rax = 0x1111;
            oracle.gpr.rbx = kData;
            oracle.gpr.rcx = rcx;
            oracle.gpr.rdx = rdx;
            ASSERT_TRUE(memory.write_u64(kData, mem).is_ok());
            auto state = oracle;
            {
                Interpreter interpreter(oracle, memory);
                for (u32 step = 0; step < instructions; ++step) {
                    const ExecResult r = interpreter.step();
                    ASSERT_TRUE(r == ExecResult::Continue || r == ExecResult::Branch);
                    if (r == ExecResult::Branch) break;   // the block exits here
                }
            }
            const auto oracle_memory = memory.read_u64(kData);
            ASSERT_TRUE(memory.write_u64(kData, mem).is_ok());
            ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
            SCOPED_TRACE(testing::Message() << std::hex << "rcx=" << rcx
                                            << " rdx=" << rdx << " mem=" << mem);
            EXPECT_EQ(state.rip, oracle.rip);
            EXPECT_EQ(memory.read_u64(kData).value_or(1), oracle_memory.value_or(2));
            for (u8 index = 0; index < 16; ++index)
                EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                    << "GPR " << unsigned(index);
            state.materialize_lazy_flags();
            oracle.materialize_lazy_flags();
            EXPECT_EQ(state.flags.rflags, oracle.flags.rflags);
        }
    }
    static std::vector<std::array<u64, 3>> cases() {
        std::vector<std::array<u64, 3>> out;
        for (u64 rcx : {0ull, 1ull, 5ull, 0x8000000000000000ull, ~0ull})
            for (u64 rdx : {0ull, 1ull, 5ull, ~0ull})
                for (u64 mem : {0ull, 5ull, 0x7FFFFFFFFFFFFFFFull})
                    out.push_back({rcx, rdx, mem});
        return out;
    }
};

TEST_F(IrEmit64SideExit, CompareExitThenArithmeticAndTerminalBranch) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // cmp rcx,rdx ; je +0x20 ; add rax,rcx ; cmp rax,rdx ; jb +0x10
    expect_trace_matches({0x48,0x39,0xD1, 0x74,0x20, 0x48,0x01,0xC8,
                          0x48,0x39,0xD0, 0x72,0x10}, cases());
}

TEST_F(IrEmit64SideExit, MemoryAccessesOnBothSidesOfTheExit) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // mov rax,[rbx] ; cmp rax,rcx ; jb +0x20 ; add rax,rdx ; mov [rbx],rax ;
    // test rax,rax ; jnz +0x10
    expect_trace_matches({0x48,0x8B,0x03, 0x48,0x39,0xC8, 0x72,0x20,
                          0x48,0x01,0xD0, 0x48,0x89,0x03,
                          0x48,0x85,0xC0, 0x75,0x10}, cases());
}

TEST_F(IrEmit64SideExit, ValueReadBeforeTheExitIsUsedAfterIt) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // mov rax,rcx ; cmp rdx,5 ; je +0x20 ; add rax,rax ; sub rax,rdx ;
    // cmp eax,ecx ; jg +0x10
    expect_trace_matches({0x48,0x89,0xC8, 0x48,0x83,0xFA,0x05, 0x74,0x20,
                          0x48,0x01,0xC0, 0x48,0x29,0xD0,
                          0x39,0xC8, 0x7F,0x10}, cases());
}

TEST_F(IrEmit64SideExit, CarryPreservingIncAfterTheExitAndTwoExits) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // sub rcx,rdx ; jz +0x30 ; inc rcx ; js +0x20 ; cmp ecx,edx ; ja +0x10
    expect_trace_matches({0x48,0x29,0xD1, 0x74,0x30, 0x48,0xFF,0xC1,
                          0x78,0x20, 0x39,0xD1, 0x77,0x10}, cases());
}

TEST_F(IrEmit64SideExit, ByteCompareExitFromMemory) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // cmp byte [rbx],5 ; jne +0x20 ; mov eax,[rbx] ; cmp eax,ecx ; jae +0x10
    expect_trace_matches({0x80,0x3B,0x05, 0x75,0x20, 0x8B,0x03,
                          0x39,0xC8, 0x73,0x10}, cases());
}
// A record whose only reader is the exit right after it is published on the
// exit's taken edge, in its cold stub (FlagDef::exit_deferred); the state a
// successor sees there must be the interpreter's, and the fall-through must
// not need the record.
TEST_F(IrEmit64SideExit, ExitDeferredLogicRecordReachesTheTakenEdge) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // and rdx,0xff ; je +0x20 ; shr ecx,6 ; jnz +0x10
    expect_trace_matches({0x48,0x81,0xE2,0xFF,0x00,0x00,0x00, 0x74,0x20,
                          0xC1,0xE9,0x06, 0x75,0x10}, cases());
}

TEST_F(IrEmit64SideExit, ExitDeferredRecordWithALoadBeforeItsKiller) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // test rcx,rcx ; je +0x20 ; mov eax,[rbx] ; and edx,eax ; jz +0x10
    expect_trace_matches({0x48,0x85,0xC9, 0x74,0x20, 0x8B,0x03,
                          0x21,0xC2, 0x74,0x10}, cases());
}

TEST_F(IrEmit64SideExit, ExitDeferredRecordNamingAnUnpinnedRegister) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // mov r9,rcx ; cmp r9,rdx ; jb +0x20 ; add r9,rdx ; mov [rbx],r9 ;
    // test r9,r9 ; jz +0x10
    expect_trace_matches({0x49,0x89,0xC9, 0x49,0x39,0xD1, 0x72,0x20,
                          0x49,0x01,0xD1, 0x4C,0x89,0x0B,
                          0x4D,0x85,0xC9, 0x74,0x10}, cases());
}

TEST_F(IrEmit64SideExit, ExitDeferredByteRecordAndSignedConditions) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // cmp cl,dl ; je +0x30 ; sub ecx,edx ; jl +0x20 ; add eax,ecx ; js +0x10
    expect_trace_matches({0x38,0xD1, 0x74,0x30, 0x29,0xD1, 0x7C,0x20,
                          0x01,0xC8, 0x78,0x10}, cases());
}

TEST_F(IrEmit64SideExit, ExitDeferredRecordWhoseOperandRegisterIsRewritten) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // sub rcx,rdx ; jz +0x20 ; mov rdx,rax ; cmp rcx,rdx ; jae +0x10
    // (the record's operands are RCX and RDX; the exit's fall-through
    // rewrites RDX before the killer, the taken edge must not see that)
    expect_trace_matches({0x48,0x29,0xD1, 0x74,0x20, 0x48,0x89,0xC2,
                          0x48,0x39,0xD1, 0x73,0x10}, cases());
}
} // namespace
#endif
