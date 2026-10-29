// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — branch-reach decline and in-chain API-exit reach

#include "jit_exec_test_fixture.hpp"

// This binary runs with VITREO_JIT_TEST_BRANCH_REACH_FATAL=1 unless the caller
// set it: a block whose deferred branch cannot reach is declined in a guest run,
// and that decline is silent to every parity assertion (the interpreter runs
// the block and agrees with itself), so under test it stops the process
// instead. Tests that provoke a decline on purpose scope it back to 0.
namespace {
const bool kBranchReachFatalUnderTest = [] {
    ::setenv("VITREO_JIT_TEST_BRANCH_REACH_FATAL", "1", /*overwrite=*/0);
    return true;
}();

// setenv/unsetenv scope for one test (the knobs are read per compile).
class ScopedJitTestEnv {
public:
    ScopedJitTestEnv(const char* name, const char* value) : m_name(name) {
        if (const char* prev = std::getenv(name)) {
            m_had = true;
            m_prev = prev;
        }
        ::setenv(name, value, 1);
        JitCompiler::reload_branch_reach_test_knobs_for_test();
    }
    ~ScopedJitTestEnv() {
        if (m_had) ::setenv(m_name, m_prev.c_str(), 1);
        else ::unsetenv(m_name);
        JitCompiler::reload_branch_reach_test_knobs_for_test();
    }
    ScopedJitTestEnv(const ScopedJitTestEnv&) = delete;
    ScopedJitTestEnv& operator=(const ScopedJitTestEnv&) = delete;

private:
    const char* m_name;
    bool m_had{false};
    std::string m_prev;
};

// mov eax, [ebx]: a checked load, whose raise exit is a deferred branch.
BasicBlock reach_test_block(GuestMemory& memory) {
    static const u8 code[] = {0x8B, 0x03, 0x90};
    EXPECT_TRUE(memory.write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block;
    block.start_addr = 0x1000;
    MemoryByteReader reader(code, sizeof(code));
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        if (decode_instruction(reader, DecodeMode::Mode32, inst) !=
            DecodeError::Success) {
            break;
        }
        block.instructions.push_back(inst);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    return block;
}
}  // namespace

// finalize_code declines a block one of whose deferred branches cannot reach
// (JitCompiler::cold_branch_disp): compile returns nullptr, nothing is
// published, and the decline is counted. VITREO_JIT_TEST_BRANCH_REACH_WORDS
// shrinks every deferred branch's reach so an ordinary block takes the path.
TEST(JitBranchReachDecline, BlockWithAnUnreachableBranchIsDeclinedAndCounted) {
    (void)kBranchReachFatalUnderTest;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    const BasicBlock block = reach_test_block(memory);
    ASSERT_EQ(block.instruction_count, 2u);
    {
        JitCompiler jit(memory);
        ASSERT_NE(jit.compile(block), nullptr) << "compiles at full reach";
    }
    ScopedJitTestEnv fatal("VITREO_JIT_TEST_BRANCH_REACH_FATAL", "0");
    ScopedJitTestEnv cap("VITREO_JIT_TEST_BRANCH_REACH_WORDS", "1");
    const u64 before = vitreo::cpu::jit::stats::branch_reach_declines();
    JitCompiler jit(memory);
    EXPECT_EQ(jit.compile(block), nullptr)
        << "a block whose branch cannot reach is left to the interpreter";
    EXPECT_GT(vitreo::cpu::jit::stats::branch_reach_declines(), before);
}

// With the test switch on (this binary's default), the same decline aborts.
TEST(JitBranchReachDeclineDeathTest, TestSwitchTurnsADeclineIntoAnAbort) {
    // Threadsafe style, scoped to this test (see Arm64Imm12Encoding).
    const std::string saved_style = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    struct RestoreStyle {
        std::string style;
        ~RestoreStyle() { GTEST_FLAG_SET(death_test_style, style); }
    } restore{saved_style};
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    const BasicBlock block = reach_test_block(memory);
    ScopedJitTestEnv fatal("VITREO_JIT_TEST_BRANCH_REACH_FATAL", "1");
    ScopedJitTestEnv cap("VITREO_JIT_TEST_BRANCH_REACH_WORDS", "1");
    EXPECT_DEATH(
        {
            JitCompiler jit(memory);
            (void)jit.compile(block);
        },
        "VITREO_JIT_TEST_BRANCH_REACH_FATAL");
}

// L3's in-chain API-call exit is joined to its block, and its hot part to
// its cold part and back, by B (imm26) words patched once both ends are
// placed (JitCompiler::patch_api_call_b). One that cannot reach is a block
// size the JIT does not lay out: the block is declined to the interpreter
// like any other deferred branch (cold_branch_disp), never a process stop.
// The block is one constant JMP to a thunk, whose only other deferred branch
// is the one-word B into the continue-exit stub placed right after it, so at
// a reach of two words only the in-chain exit cannot be laid out.
namespace {
constexpr GuestAddr kReachThunkLo = 0x70000000u;

struct InChainCodegenScope {
    InChainCodegenScope() {
        JitCompiler::enable_jit_link();
        JitCompiler::set_inchain_thunks_codegen_for_test(1);
    }
    ~InChainCodegenScope() {
        JitCompiler::set_inchain_thunks_codegen_for_test(-1);
        JitCompiler::disable_jit_link();
    }
    InChainCodegenScope(const InChainCodegenScope&) = delete;
    InChainCodegenScope& operator=(const InChainCodegenScope&) = delete;
};

// jmp 0x70000000 from 0x1000: a constant tail into the thunk region.
BasicBlock api_exit_reach_block(GuestMemory& memory) {
    static const u8 code[] = {0xE9, 0xFB, 0xEF, 0xFF, 0x6F};
    EXPECT_TRUE(memory.write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block;
    block.start_addr = 0x1000;
    MemoryByteReader reader(code, sizeof(code));
    DecodedInstruction inst;
    EXPECT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
              DecodeError::Success);
    block.instructions.push_back(inst);
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = 1;
    return block;
}

CompiledBlock* compile_with_thunk_region(JitCompiler& jit,
                                         const BasicBlock& block) {
    jit.set_thunk_region(kReachThunkLo, kReachThunkLo + 0x1000);
    return jit.compile(block);
}
}  // namespace

TEST(JitBranchReachDecline, InChainApiCallExitPastItsReachDeclinesTheBlock) {
    (void)kBranchReachFatalUnderTest;
    InChainCodegenScope codegen;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    const BasicBlock block = api_exit_reach_block(memory);
    {
        JitCompiler jit(memory);
        CompiledBlock* cb = compile_with_thunk_region(jit, block);
        ASSERT_NE(cb, nullptr) << "compiles at full reach";
        // The tail is the in-chain exit: its cold part loads the trampoline.
        const u32 load_trampoline = arm64::ldr_x_uoff(
            16, REG_JIT_CTX, offsetof(JitContext, inchain_trampoline) >> 3);
        usize loads = 0;
        for (const WordRange& range : block_word_ranges(*cb)) {
            loads += static_cast<usize>(
                std::count(range.begin, range.end, load_trampoline));
        }
        ASSERT_EQ(loads, 1u) << "the tail no longer takes the in-chain exit";
    }
    ScopedJitTestEnv fatal("VITREO_JIT_TEST_BRANCH_REACH_FATAL", "0");
    ScopedJitTestEnv cap("VITREO_JIT_TEST_BRANCH_REACH_WORDS", "2");
    {
        // Without the in-chain exit the same tail lays out at this reach, so
        // a decline below is the in-chain exit's.
        JitCompiler::set_inchain_thunks_codegen_for_test(0);
        JitCompiler jit(memory);
        EXPECT_NE(compile_with_thunk_region(jit, block), nullptr)
            << "the reach declines this tail without the in-chain exit";
        JitCompiler::set_inchain_thunks_codegen_for_test(1);
    }
    const u64 before = vitreo::cpu::jit::stats::branch_reach_declines();
    JitCompiler jit(memory);
    EXPECT_EQ(compile_with_thunk_region(jit, block), nullptr)
        << "a block whose in-chain exit cannot reach was laid out";
    EXPECT_GT(vitreo::cpu::jit::stats::branch_reach_declines(), before);
}

// Under the test switch the same decline aborts, naming the in-chain exit.
TEST(JitBranchReachDeclineDeathTest, InChainApiCallExitPastItsReachIsNamed) {
    SharedExitStubsOff legacy_exits;
    const std::string saved_style = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    struct RestoreStyle {
        std::string style;
        ~RestoreStyle() { GTEST_FLAG_SET(death_test_style, style); }
    } restore{saved_style};
    InChainCodegenScope codegen;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    const BasicBlock block = api_exit_reach_block(memory);
    ScopedJitTestEnv fatal("VITREO_JIT_TEST_BRANCH_REACH_FATAL", "1");
    ScopedJitTestEnv cap("VITREO_JIT_TEST_BRANCH_REACH_WORDS", "2");
    EXPECT_DEATH(
        {
            JitCompiler jit(memory);
            (void)compile_with_thunk_region(jit, block);
        },
        "api-call exit displacement .* exceeds the imm26 branch field "
        ".*VITREO_JIT_TEST_BRANCH_REACH_FATAL");
}
