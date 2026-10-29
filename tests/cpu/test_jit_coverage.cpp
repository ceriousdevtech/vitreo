// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT native-lowering coverage accounting.
//
// Guards the plumbing behind VITREO_JIT_COVERAGE (see jit_coverage.hpp): the
// per-block count of natively-lowered instructions that supplies the
// denominator for delegate telemetry. Without that count the reported delegate
// share is meaningless, and nothing else in the suite would notice it breaking.
//
// Deliberately asserts on CompiledBlock::native_insns rather than on the
// stderr report. The report is gated on an environment variable read once and
// cached, so a test that sets the variable at runtime passes alone and fails
// inside the full suite depending on which test compiled a block first —
// exactly the ordering fragility JitSseHelperHistogramTest already exhibits.

#include <gtest/gtest.h>

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/jit/jit_compiler.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

class JitCoverageTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(0x1000, 0x10000,
                                         MemoryProtection::ReadWrite));
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_cpu = std::make_unique<CpuState>();
    }

    BasicBlock build_block(const u8* code, usize len, bool is_64bit) {
        EXPECT_TRUE(m_memory->write_bytes(0x1000, code, len));

        BasicBlock block;
        block.start_addr = 0x1000;
        block.is_64bit = is_64bit;

        MemoryByteReader reader(code, len);
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            if (decode_instruction(reader,
                                   is_64bit ? DecodeMode::Mode64
                                            : DecodeMode::Mode32,
                                   inst) != DecodeError::Success) {
                break;
            }
            block.instructions.push_back(inst);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        return block;
    }

    void prepare_cpu(bool is_64bit) {
        if (is_64bit) {
            m_cpu->init_long_mode();
            m_cpu->gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
        } else {
            m_cpu->init_protected_mode();
            m_cpu->gpr.set32(static_cast<u8>(RegIndex::RSP), 0x10000);
        }
        m_cpu->rip = 0x1000;
    }

    JitExecResult execute_block(const u8* code, usize len, bool is_64bit) {
        BasicBlock block = build_block(code, len, is_64bit);
        EXPECT_EQ(block.instruction_count, 1u);
        CompiledBlock* cb = m_jit->compile(block);
        EXPECT_NE(cb, nullptr);
        if (!cb) {
            return JitExecResult::Unsupported;
        }
        EXPECT_EQ(cb->native_insns, 1u);
        return m_jit->execute(*cb, *m_cpu);
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
    std::unique_ptr<CpuState> m_cpu;
};

// An all-native block must attribute every instruction to the native count.
// If a lowering silently regresses into the interpreter delegate, this drops.
TEST_F(JitCoverageTest, AllNativeBlockCountsEveryInstructionAsNative) {
    // mov eax, 1 ; add eax, ecx ; ret  — all three have native lowerings.
    const u8 code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00,  // mov eax, 1
        0x01, 0xC8,                     // add eax, ecx
        0xC3,                           // ret
    };

    BasicBlock block = build_block(code, sizeof(code), /*is_64bit=*/false);
    ASSERT_EQ(block.instruction_count, 3u);

    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    EXPECT_EQ(cb->native_insns, 3u);
}

// Far JMP/CALL m16:64 must lower natively in long mode (Intel SDM Vol. 2).
TEST_F(JitCoverageTest, FarM16_64IsNative) {
    const u8 jmp_far[] = {0xFF, 0x28};  // JMP far [RAX]
    BasicBlock bjmp = build_block(jmp_far, sizeof(jmp_far), /*is_64bit=*/true);
    ASSERT_EQ(bjmp.instruction_count, 1u);
    CompiledBlock* cb_jmp = m_jit->compile(bjmp);
    ASSERT_NE(cb_jmp, nullptr);
    EXPECT_EQ(cb_jmp->native_insns, 1u);

    const u8 call_far[] = {0xFF, 0x18};  // CALL far [RAX]
    BasicBlock bcall = build_block(call_far, sizeof(call_far), /*is_64bit=*/true);
    ASSERT_EQ(bcall.instruction_count, 1u);
    CompiledBlock* cb_call = m_jit->compile(bcall);
    ASSERT_NE(cb_call, nullptr);
    EXPECT_EQ(cb_call->native_insns, 1u);
}

// PAUSE (F3 90) must lower natively as a no-op, matching NOP. A silent
// fallback to the interpreter delegate would still execute correctly but
// would under-count native_insns.
TEST_F(JitCoverageTest, PauseIsNativeNoOp) {
    const u8 code32[] = {
        0xF3, 0x90,  // pause
        0xC3,        // ret
    };
    BasicBlock b32 = build_block(code32, sizeof(code32), /*is_64bit=*/false);
    ASSERT_EQ(b32.instruction_count, 2u);
    CompiledBlock* cb32 = m_jit->compile(b32);
    ASSERT_NE(cb32, nullptr);
    EXPECT_EQ(cb32->native_insns, 2u);

    const u8 code64[] = {
        0xF3, 0x90,  // pause
        0xC3,        // ret
    };
    BasicBlock b64 = build_block(code64, sizeof(code64), /*is_64bit=*/true);
    ASSERT_EQ(b64.instruction_count, 2u);
    CompiledBlock* cb64 = m_jit->compile(b64);
    ASSERT_NE(cb64, nullptr);
    EXPECT_EQ(cb64->native_insns, 2u);
}

// RDTSC (0F 31): EDX:EAX := virtual TSC. A silent interpreter fallback
// would under-count native_insns.
TEST_F(JitCoverageTest, RdtscIsNative) {
    const u8 code[] = {
        0x0F, 0x31,  // rdtsc
        0xC3,        // ret
    };
    BasicBlock b32 = build_block(code, sizeof(code), /*is_64bit=*/false);
    ASSERT_EQ(b32.instruction_count, 2u);
    CompiledBlock* cb32 = m_jit->compile(b32);
    ASSERT_NE(cb32, nullptr);
    EXPECT_EQ(cb32->native_insns, b32.instruction_count);

    BasicBlock b64 = build_block(code, sizeof(code), /*is_64bit=*/true);
    ASSERT_EQ(b64.instruction_count, 2u);
    CompiledBlock* cb64 = m_jit->compile(b64);
    ASSERT_NE(cb64, nullptr);
    EXPECT_EQ(cb64->native_insns, b64.instruction_count);
}

// RDTSCP (0F 01 F9): EDX:EAX := TSC, ECX := 0 (IA32_TSC_AUX not installed).
TEST_F(JitCoverageTest, RdtscpIsNative) {
    const u8 code[] = {
        0x0F, 0x01, 0xF9,  // rdtscp
        0xC3,              // ret
    };
    BasicBlock b32 = build_block(code, sizeof(code), /*is_64bit=*/false);
    ASSERT_EQ(b32.instruction_count, 2u);
    CompiledBlock* cb32 = m_jit->compile(b32);
    ASSERT_NE(cb32, nullptr);
    EXPECT_EQ(cb32->native_insns, b32.instruction_count);

    BasicBlock b64 = build_block(code, sizeof(code), /*is_64bit=*/true);
    ASSERT_EQ(b64.instruction_count, 2u);
    CompiledBlock* cb64 = m_jit->compile(b64);
    ASSERT_NE(cb64, nullptr);
    EXPECT_EQ(cb64->native_insns, b64.instruction_count);
}

// A delegated instruction must NOT count toward the native total. Otherwise
// the denominator absorbs the delegates it is meant to be measured against and
// the reported delegate share is understated.
TEST_F(JitCoverageTest, DelegatedInstructionIsExcludedFromNativeCount) {
    // mov eax, 1 ; cpuid ; ret — CPUID is intentionally never native (leaf
    // dispatch stays on the interpreter), so it takes the delegate path.
    // If a native CPUID lowering is added later this test will fail loudly
    // rather than silently stop testing anything; swap in another delegated
    // opcode from a VITREO_JIT_COVERAGE report at that point.
    const u8 code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00,  // mov eax, 1
        0x0F, 0xA2,                     // cpuid
        0xC3,                           // ret
    };

    BasicBlock block = build_block(code, sizeof(code), /*is_64bit=*/false);
    ASSERT_EQ(block.instruction_count, 3u);

    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    // Exactly one instruction delegated, so the native count is one short of
    // the block's instruction count.
    EXPECT_EQ(cb->native_insns, block.instruction_count - 1u);
}

// The counter must reset between blocks. It is a compiler member, so a missing
// reset would make every block report the running total of all prior blocks.
TEST_F(JitCoverageTest, NativeCountResetsBetweenBlocks) {
    const u8 first[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00,  // mov eax, 1
        0x01, 0xC8,                     // add eax, ecx
        0xC3,                           // ret
    };
    BasicBlock b1 = build_block(first, sizeof(first), /*is_64bit=*/false);
    CompiledBlock* cb1 = m_jit->compile(b1);
    ASSERT_NE(cb1, nullptr);
    ASSERT_EQ(cb1->native_insns, 3u);

    const u8 second[] = {
        0x01, 0xC8,  // add eax, ecx
        0xC3,        // ret
    };
    BasicBlock b2 = build_block(second, sizeof(second), /*is_64bit=*/false);
    CompiledBlock* cb2 = m_jit->compile(b2);
    ASSERT_NE(cb2, nullptr);
    EXPECT_EQ(cb2->native_insns, 2u);
}

// Intel SDM Vol. 2 RDTSC: EDX:EAX := TSC; ECX is unchanged. RIP advances by 2.
TEST_F(JitCoverageTest, RdtscExecuteWritesEdxEaxPreservesEcx) {
    const u8 code[] = {0x0F, 0x31};
    constexpr u8 kRax = static_cast<u8>(RegIndex::RAX);
    constexpr u8 kRcx = static_cast<u8>(RegIndex::RCX);
    constexpr u8 kRdx = static_cast<u8>(RegIndex::RDX);
    constexpr u64 kEcxSentinel = 0x1111222233334444ull;

    for (bool is_64bit : {false, true}) {
        prepare_cpu(is_64bit);
        m_cpu->gpr.set64(kRax, ~0ull);
        m_cpu->gpr.set64(kRdx, ~0ull);
        m_cpu->gpr.set64(kRcx, kEcxSentinel);

        ASSERT_EQ(execute_block(code, sizeof(code), is_64bit),
                  JitExecResult::Continue)
            << (is_64bit ? "64-bit" : "32-bit");
        EXPECT_FALSE(m_cpu->pending_raise.active);
        const u64 tsc = (static_cast<u64>(m_cpu->gpr.get32(kRdx)) << 32) |
                        m_cpu->gpr.get32(kRax);
        EXPECT_GT(tsc, 0u) << (is_64bit ? "64-bit" : "32-bit");
        EXPECT_EQ(m_cpu->gpr.get64(kRcx), kEcxSentinel);
        EXPECT_EQ(m_cpu->rip, 0x1000ull + sizeof(code));
    }
}

// Intel SDM Vol. 2 RDTSCP: EDX:EAX := TSC; ECX := IA32_TSC_AUX (0 here).
// The 32-bit ECX write zero-extends into RCX. RIP advances by 3.
TEST_F(JitCoverageTest, RdtscpExecuteWritesEdxEaxZerosEcx) {
    const u8 code[] = {0x0F, 0x01, 0xF9};
    constexpr u8 kRax = static_cast<u8>(RegIndex::RAX);
    constexpr u8 kRcx = static_cast<u8>(RegIndex::RCX);
    constexpr u8 kRdx = static_cast<u8>(RegIndex::RDX);

    for (bool is_64bit : {false, true}) {
        prepare_cpu(is_64bit);
        m_cpu->gpr.set64(kRax, ~0ull);
        m_cpu->gpr.set64(kRdx, ~0ull);
        m_cpu->gpr.set64(kRcx, ~0ull);

        ASSERT_EQ(execute_block(code, sizeof(code), is_64bit),
                  JitExecResult::Continue)
            << (is_64bit ? "64-bit" : "32-bit");
        EXPECT_FALSE(m_cpu->pending_raise.active);
        const u64 tsc = (static_cast<u64>(m_cpu->gpr.get32(kRdx)) << 32) |
                        m_cpu->gpr.get32(kRax);
        EXPECT_GT(tsc, 0u) << (is_64bit ? "64-bit" : "32-bit");
        EXPECT_EQ(m_cpu->gpr.get32(kRcx), 0u)
            << (is_64bit ? "64-bit" : "32-bit");
        if (is_64bit) {
            // Zero-extension into bits 63:32 is only architectural in 64-bit
            // mode. A 32-bit guest has no RCX, and the JIT pins ECX to an ARM
            // register whose write-back stores just the 32-bit view
            // (JitCompiler::emit_store_pins deliberately does not zero-extend,
            // so it cannot smash state that was never in the pin). Asserting
            // on the upper half in 32-bit mode would be testing an
            // implementation detail, not the contract.
            EXPECT_EQ(m_cpu->gpr.get64(kRcx), 0u);
        }
        EXPECT_EQ(m_cpu->rip, 0x1000ull + sizeof(code));
    }
}

}  // namespace
