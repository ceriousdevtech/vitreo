/// @file test_block_decoder.cpp
/// @brief x64 JIT block boundaries must stop at indirect call/jmp and UD2.

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>

#include "core/memory/guest_memory.hpp"
#include "runtime/execution/block_decoder.hpp"
#include "core/cpu/jit/jit_compiler.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cli;

namespace {

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const char* value) : m_name(name) {
        if (const char* previous = std::getenv(name)) m_previous = previous;
        if (value) setenv(name, value, 1);
        else unsetenv(name);
        // decode_basic_block caches its environment-derived flags (getenv on
        // every decode measured 3.0% of running CPU in a live CEF guest), so
        // an in-process A/B must publish the change explicitly.
        refresh_decode_env_flags();
    }
    ~ScopedEnvVar() {
        if (m_previous) setenv(m_name.c_str(), m_previous->c_str(), 1);
        else unsetenv(m_name.c_str());
        refresh_decode_env_flags();
    }

private:
    std::string m_name;
    std::optional<std::string> m_previous;
};

jit::BasicBlock decode_bytes(const std::initializer_list<u8>& bytes, bool is_64bit) {
    GuestMemory memory;
    EXPECT_TRUE(memory.map_region(0x140001000, 0x1000, MemoryProtection::ReadWriteExecute));
    std::vector<u8> data(bytes);
    EXPECT_TRUE(memory.write_bytes(0x140001000, data.data(), data.size()));
    return decode_basic_block(memory, 0x140001000, is_64bit);
}

}  // namespace

TEST(BlockDecoder64, CallRm64StopsBeforeUd2Padding) {
    // call rax ; ud2  — MSVC/Clang pad after an indirect call site.
    auto block = decode_bytes({0xFF, 0xD0, 0x0F, 0x0B}, true);
    ASSERT_EQ(block.instruction_count, 1u);
    ASSERT_EQ(block.byte_length, 2u);
    EXPECT_TRUE(block.ends_with_branch);
    EXPECT_EQ(block.instructions[0].opcode, Opcode::CALL_RM64);
}

TEST(BlockDecoder64, ShortBlockDoesNotReserveMaximumInstructionStorage) {
    auto block = decode_bytes({0xC3}, true);
    ASSERT_EQ(block.instruction_count, 1u);
    ASSERT_EQ(block.instructions.size(), 1u);
    EXPECT_LT(block.instructions.capacity(), 64u);
}

TEST(BlockDecoder64, JmpRm64TerminatesBlock) {
    auto block = decode_bytes({0xFF, 0xE0, 0x90, 0x90}, true);
    ASSERT_EQ(block.instruction_count, 1u);
    ASSERT_EQ(block.byte_length, 2u);
    EXPECT_TRUE(block.ends_with_branch);
    EXPECT_EQ(block.instructions[0].opcode, Opcode::JMP_RM64);
}

TEST(BlockDecoder64, Ud2TerminatesBlock) {
    auto block = decode_bytes({0x0F, 0x0B, 0x48, 0x31, 0xC0}, true);
    ASSERT_EQ(block.instruction_count, 1u);
    ASSERT_EQ(block.byte_length, 2u);
    EXPECT_TRUE(block.ends_with_branch);
    EXPECT_EQ(block.instructions[0].opcode, Opcode::UD2);
}

TEST(BlockDecoder64, GuardedTerminalTraceKeepsSkippedUd2AndTarget) {
    ScopedEnvVar guarded_traces("VITREO_JIT_GUARDED_TERMINAL_TRACES", "1");
    // je target ; ud2 ; xor rax,rax ; ret
    // The taken trace reaches target in the same translation. The cold
    // fall-through UD2 remains in the IR so its #UD behavior is preserved.
    auto block = decode_bytes(
        {0x74, 0x02, 0x0F, 0x0B, 0x48, 0x31, 0xC0, 0xC3}, true);
    ASSERT_EQ(block.instruction_count, 4u);
    ASSERT_EQ(block.byte_length, 8u);
    EXPECT_EQ(block.instructions[0].opcode, Opcode::JZ_REL8);
    EXPECT_EQ(block.instructions[1].opcode, Opcode::UD2);
    EXPECT_EQ(block.instructions[2].opcode, Opcode::XOR_RM64_R64);
    EXPECT_EQ(block.instructions[3].opcode, Opcode::RET_NEAR);
}

TEST(BlockDecoder64, GuardedTerminalTraceDefaultsOnAndCanBeDisabled) {
    ScopedEnvVar default_traces("VITREO_JIT_GUARDED_TERMINAL_TRACES", nullptr);
    // Keep both blocks alive so the second decode exercises the same-address
    // memo key transition rather than merely replacing an expired entry.
    auto default_block = decode_bytes(
        {0x74, 0x02, 0x0F, 0x0B, 0x48, 0x31, 0xC0, 0xC3}, true);
    ASSERT_EQ(default_block.instruction_count, 4u);
    ASSERT_EQ(default_block.byte_length, 8u);

    ScopedEnvVar disabled_traces("VITREO_JIT_GUARDED_TERMINAL_TRACES", "0");
    auto disabled_block = decode_bytes(
        {0x74, 0x02, 0x0F, 0x0B, 0x48, 0x31, 0xC0, 0xC3}, true);
    ASSERT_EQ(disabled_block.instruction_count, 2u);
    ASSERT_EQ(disabled_block.byte_length, 4u);
    EXPECT_EQ(disabled_block.instructions[1].opcode, Opcode::UD2);
}

// 32-bit traces are default ON since 2026-09-06: a 32-bit block that ends at
// its first conditional branch is two guest instructions carrying a whole
// prologue, a lazy-flag publication and an epilogue, which measured 4.5x on
// cmp_jcc. See trace_blocks_32's note and jit-refactor/TRACES32.md.
//
// The bytes are `je +2 ; ud2 ; xor eax,eax ; ret`, with the guarded-terminal
// island explicitly off so this measures trace formation alone. The Jcc target
// is forward and inside the decode window, so a trace keeps the fall-through
// in the same translation and decodes through UD2; without traces the block
// stops at the branch.
//
// The predecessor of this test asserted only the DEFAULT, because
// trace_blocks_32 cached its getenv in a function-local static and no test
// could reach the other arm. It now uses the same refreshable slot as the
// guarded-island flag, so both arms are covered and the default is pinned by
// the third case rather than being the only thing checked.
TEST(BlockDecoder32, TracesFollowAForwardJccAndAreOnByDefault) {
    ScopedEnvVar default_guarded("VITREO_JIT_GUARDED_TERMINAL_TRACES", nullptr);
    const std::initializer_list<u8> code{0x74, 0x02, 0x0F, 0x0B,
                                         0x31, 0xC0, 0xC3};
    {
        ScopedEnvVar traces_off("VITREO_JIT_TRACES", "0");
        auto block = decode_bytes(code, false);
        ASSERT_EQ(block.instruction_count, 1u);
        ASSERT_EQ(block.byte_length, 2u);
        EXPECT_EQ(block.instructions[0].opcode, Opcode::JZ_REL8);
    }
    {
        ScopedEnvVar traces_on("VITREO_JIT_TRACES", "1");
        auto block = decode_bytes(code, false);
        ASSERT_EQ(block.instruction_count, 2u);
        ASSERT_EQ(block.byte_length, 4u);
        EXPECT_EQ(block.instructions[0].opcode, Opcode::JZ_REL8);
        EXPECT_EQ(block.instructions[1].opcode, Opcode::UD2);
    }
    // Unset: the shipped configuration, which must match the traces-on arm.
    ScopedEnvVar traces_default("VITREO_JIT_TRACES", nullptr);
    auto block = decode_bytes(code, false);
    ASSERT_EQ(block.instruction_count, 2u)
        << "VITREO_JIT_TRACES defaults ON; a 32-bit block that still ends at "
           "its first conditional branch costs 4.5x on cmp_jcc";
    EXPECT_EQ(block.instructions[1].opcode, Opcode::UD2);
}

TEST(BlockDecoder64, GuardedTerminalTraceRequiresExactUd2Target) {
    ScopedEnvVar guarded_traces("VITREO_JIT_GUARDED_TERMINAL_TRACES", "1");
    // The target is one byte beyond UD2, so the trap is not an exact island.
    auto block = decode_bytes({0x74, 0x03, 0x0F, 0x0B, 0x90, 0xC3}, true);
    ASSERT_EQ(block.instruction_count, 2u);
    ASSERT_EQ(block.byte_length, 4u);
    EXPECT_EQ(block.instructions[1].opcode, Opcode::UD2);
}

TEST(BlockDecoder64, GuardedTerminalTraceStopsWhenTargetPageIsUnmapped) {
    ScopedEnvVar guarded_traces("VITREO_JIT_GUARDED_TERMINAL_TRACES", "1");
    constexpr GuestAddr page = 0x140001000;
    constexpr GuestAddr start = page + 0xFFCu;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        page, 0x1000, MemoryProtection::ReadWriteExecute));

    // je target ; ud2, with target exactly at the first byte of the unmapped
    // next page. The guarded pair remains valid IR, but decoding must stop
    // without inventing an instruction at the unavailable target.
    const u8 code[] = {0x74, 0x02, 0x0F, 0x0B};
    ASSERT_TRUE(memory.write_bytes(start, code, sizeof(code)));
    auto block = decode_basic_block(memory, start, true);

    ASSERT_EQ(block.instruction_count, 2u);
    ASSERT_EQ(block.byte_length, sizeof(code));
    EXPECT_TRUE(block.ends_with_branch);
    EXPECT_EQ(block.instructions[0].opcode, Opcode::JZ_REL8);
    EXPECT_EQ(block.instructions[1].opcode, Opcode::UD2);
}

TEST(BlockDecoder64, GuardedTerminalTraceAtInstructionCapHasNoTargetInsn) {
    ScopedEnvVar guarded_traces("VITREO_JIT_GUARDED_TERMINAL_TRACES", "1");
    constexpr GuestAddr start = 0x140001000;
    constexpr u32 max_block_insns = 192;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        start, 0x1000, MemoryProtection::ReadWriteExecute));

    // Fill 190 slots, then place `je target ; ud2` in the final two. A RET is
    // present at target, but the hard instruction cap must leave it outside
    // the block rather than partially extending the guarded trace.
    std::vector<u8> code(max_block_insns - 2, 0x90);
    code.insert(code.end(), {0x74, 0x02, 0x0F, 0x0B, 0xC3});
    ASSERT_TRUE(memory.write_bytes(start, code.data(), code.size()));
    auto block = decode_basic_block(memory, start, true);

    ASSERT_EQ(block.instruction_count, max_block_insns);
    ASSERT_EQ(block.byte_length, code.size() - 1);
    EXPECT_TRUE(block.ends_with_branch);
    EXPECT_EQ(block.instructions[max_block_insns - 2].opcode,
              Opcode::JZ_REL8);
    EXPECT_EQ(block.instructions[max_block_insns - 1].opcode, Opcode::UD2);
}

TEST(BlockDecoder64, Int3TerminatesBlock) {
    auto block = decode_bytes({0xCC, 0x48, 0x31, 0xC0}, true);
    ASSERT_EQ(block.instruction_count, 1u);
    ASSERT_EQ(block.byte_length, 1u);
    EXPECT_TRUE(block.ends_with_branch);
    EXPECT_EQ(block.instructions[0].opcode, Opcode::INT_3);
}

// ---------------------------------------------------------------------------
// Optimizing-IR gate.
//
// ir_policy() caches its parse in a static, because a gate that changed
// mid-process would let an IR-compiled block link to a stencil block emitted
// under a different internal ABI. That makes the parsing unreachable from an
// environment-manipulating test, so the parse is a pure function and these
// tests call it directly.
// ---------------------------------------------------------------------------
namespace {
using Jit = vitreo::cpu::jit::JitCompiler;

u32 policy_of(const char* mode, const char* ops = nullptr) {
    return Jit::parse_ir_policy(mode, ops);
}
}  // namespace

TEST(JitIrGate, DefaultsToOff) {
    const u32 p = policy_of(nullptr);
    EXPECT_EQ(Jit::ir_policy_bitness(p), Jit::IrBitness::Off);
    EXPECT_EQ(Jit::ir_policy_ops(p), 0u);
}

TEST(JitIrGate, SelectsBitness) {
    EXPECT_EQ(Jit::ir_policy_bitness(policy_of("32")), Jit::IrBitness::Bits32);
    EXPECT_EQ(Jit::ir_policy_bitness(policy_of("64")), Jit::IrBitness::Bits64);
    EXPECT_EQ(Jit::ir_policy_bitness(policy_of("all")), Jit::IrBitness::All);
    EXPECT_EQ(Jit::ir_policy_bitness(policy_of("1")), Jit::IrBitness::All);
}

// An unrecognised value must fail CLOSED. A typo that silently enabled the IR
// would be indistinguishable from the control arm in an A/B, and would surface
// as an unexplained correctness difference rather than a config mistake.
TEST(JitIrGate, UnrecognisedValuesFailClosed) {
    for (const char* bad : {"", "0", "off", "yes", "true", "3 2", "32x", " 32"}) {
        EXPECT_EQ(Jit::ir_policy_bitness(policy_of(bad)), Jit::IrBitness::Off)
            << "value: '" << bad << "'";
    }
}

TEST(JitIrGate, OpsDefaultToEveryImplementedClassWhenEnabled) {
    const u32 p = policy_of("32");
    EXPECT_EQ(Jit::ir_policy_ops(p), static_cast<u32>(Jit::IrOps::Alu));
}

TEST(JitIrGate, OpsMaskNarrowsForBisection) {
    EXPECT_EQ(Jit::ir_policy_ops(policy_of("32", "0")), 0u);
    EXPECT_EQ(Jit::ir_policy_ops(policy_of("32", "1")),
              static_cast<u32>(Jit::IrOps::Alu));
    EXPECT_EQ(Jit::ir_policy_ops(policy_of("32", "0x1")),
              static_cast<u32>(Jit::IrOps::Alu));
}

// A malformed ops mask must not silently disable everything -- it keeps the
// default -- but a malformed BITNESS still wins and turns the IR off.
TEST(JitIrGate, MalformedOpsKeepsDefaultButOffStaysOff) {
    EXPECT_EQ(Jit::ir_policy_ops(policy_of("32", "garbage")),
              static_cast<u32>(Jit::IrOps::Alu));
    EXPECT_EQ(Jit::ir_policy_ops(policy_of("nonsense", "1")), 0u);
}

// Ops are meaningless with the IR off, and must not leak into the packed
// policy -- otherwise two "off" configurations would hash to different
// CompiledKeys and needlessly split the compile memo.
TEST(JitIrGate, OffPolicyIsCanonicalRegardlessOfOps) {
    EXPECT_EQ(policy_of(nullptr, "7"), policy_of("off", "1"));
    EXPECT_EQ(policy_of(nullptr, nullptr), 0u);
}
