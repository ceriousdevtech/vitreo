// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_x64_flag_recovery.cpp
/// @brief Long-mode Tier 1 flag recovery: a flag record elided across a
///        faulting access must be rebuilt exactly, in both memory models.
///
/// Each case is producer ; faulting access ; killer. The killer makes the
/// producer's record dead except for the fault, which is the shape the
/// recovery planner elides. The interpreter is the oracle for the exception
/// state: RIP, all sixteen GPRs and materialized RFLAGS at the fault.

#if defined(__aarch64__)
#include "ir_x64_memory_fixture.hpp"
#include "ir_tier1_first_compile.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_x64_memory;

namespace {

struct RecoveryCase {
    const char* what;
    std::vector<u8> producer;
    /// The planner must elide this producer's record: every operand is a
    /// pinned register or an immediate, and nothing clobbers it before the
    /// fault. False for the shapes it must refuse.
    bool expect_plan;
    /// Guest instructions in `producer`.
    unsigned steps{1};
};

class IrX64FlagRecovery : public ::testing::TestWithParam<bool> {
protected:
    vitreo::cpu::jit::test::IrTier1FirstCompile m_tier1_first_compile;
    void SetUp() override {
        if (!JitCompiler::ir_enabled_for(true))
            GTEST_SKIP() << "x64 IR explicitly disabled by policy";
        if (GetParam()) {
            constexpr usize reservation = usize{16} << 30;
            ASSERT_TRUE(install_linear_fault_handler());
            ASSERT_TRUE(memory.enable_linear_reservation(reservation));
            ASSERT_TRUE(memory.enable_linear_hardware_protection());
            ASSERT_TRUE(register_linear_reservation(memory.linear_base(),
                                                    reservation));
        }
        ASSERT_TRUE(memory.map_region(kCode, 0x1000,
                                      MemoryProtection::ReadWriteExecute));
        ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::Read));
    }
    void TearDown() override {
        jit.reset();
        if (memory.is_linear()) unregister_linear_reservation(memory.linear_base());
    }

    BasicBlock decode(const std::vector<u8>& bytes) {
        EXPECT_TRUE(memory.write_bytes(kCode, bytes.data(), bytes.size()));
        BasicBlock block;
        block.start_addr = kCode;
        block.is_64bit = true;
        MemoryByteReader reader(bytes.data(), bytes.size());
        while (reader.remaining()) {
            DecodedInstruction instruction;
            EXPECT_EQ(decode_instruction(reader, DecodeMode::Mode64, instruction),
                      DecodeError::Success);
            block.instructions.push_back(instruction);
        }
        block.byte_length = static_cast<u32>(bytes.size());
        block.instruction_count = static_cast<u32>(block.instructions.size());
        return block;
    }

    /// Sign-crossing, asymmetric operands: a recovery that swapped the
    /// record's operands, or rebuilt it at the wrong width, disagrees on
    /// CF/OF/SF/AF for at least one of these.
    static void seed(CpuState& state, u64 a, u64 c, u64 d, GuestAddr fault_at) {
        state.init_long_mode();
        state.rip = kCode;
        state.flags.rflags = 0xAD7;
        state.gpr.rax = a;
        state.gpr.rcx = c;
        state.gpr.rdx = d;
        state.gpr.rbx = 0x0123456789ABCDEFull;
        state.gpr.rsi = 0xFFFFFFFF00000001ull;
        state.gpr.r8 = fault_at;
        state.gpr.r9 = 0xABCD0123456789EFull;
        state.gpr.r10 = 0x7FFFFFFFFFFFFFFFull;
        state.gpr.r11 = 0x8000000000000000ull;
        // A push faults on the slot below this.
        state.gpr.rsp = kData + 0x8008;
    }

    /// `tier0`: compile the block at Tier 0 (the stencil emitter and its own
    /// liveness, flag_liveness.cpp) instead of the IR; only the exception
    /// state is compared there.
    void expect_fault_matches(const RecoveryCase& c, const std::vector<u8>& access,
                              GuestAddr fault_at, u32 access_kind,
                              bool tier0 = false) {
        // producer ; access ; add rbx, 1 (the killer, never reached)
        std::vector<u8> bytes = c.producer;
        bytes.insert(bytes.end(), access.begin(), access.end());
        bytes.insert(bytes.end(), {0x48, 0x83, 0xC3, 0x01});
        jit = std::make_unique<JitCompiler>(memory);
        BasicBlock block = decode(bytes);
        CompiledBlock* native = nullptr;
        if (tier0) {
            JitCompiler::set_ir_tier_threshold_for_test(-1);
            native = jit->compile(block);
            JitCompiler::set_ir_tier_threshold_for_test(0);
            ASSERT_NE(native, nullptr) << c.what;
            ASSERT_FALSE(jit->ir_emit_active()) << c.what << ": not Tier 0";
        } else {
            ASSERT_TRUE(jit->ir_build_block(block).ok) << c.what;
            native = jit->compile(block);
            ASSERT_NE(native, nullptr) << c.what;
            ASSERT_TRUE(jit->ir_emit_active()) << c.what << ": fell back to Tier 0";
            EXPECT_EQ(!jit->ir_block().flag_recovery.empty(), c.expect_plan)
                << c.what << ": planner verdict";
        }

        static constexpr u64 kSeeds[][3] = {
            {0x8000000000000001ull, 0x7FFFFFFFFFFFFFFFull, 0x00000000FFFFFFFFull},
            {0xFFFFFFFFFFFFFFFFull, 0x0000000000000001ull, 0x8000000080000000ull},
            {0x000000007FFFFFFFull, 0x0000000080000000ull, 0x0F0F0F0F0F0F0F0Full},
            {0x0000000000000000ull, 0x0000000000000000ull, 0xFFFFFFFF00000000ull},
            {0x123456789ABCDEF0ull, 0x123456789ABCDEF0ull, 0x00000000000000FFull},
        };
        for (const auto& s : kSeeds) {
            SCOPED_TRACE(testing::Message() << c.what << " rax=" << std::hex
                         << s[0] << " rcx=" << s[1] << " rdx=" << s[2]);
            CpuState oracle;
            seed(oracle, s[0], s[1], s[2], fault_at);
            CpuState state = oracle;
            {
                Interpreter interpreter(oracle, memory);
                for (unsigned i = 0; i < c.steps; ++i)
                    ASSERT_EQ(interpreter.step(), ExecResult::Continue);
                ASSERT_NE(interpreter.step(), ExecResult::Continue)
                    << "the access must fault; the case is wrong";
            }
            (void)jit->execute(*native, state);
            ASSERT_TRUE(state.pending_raise.active);
            EXPECT_EQ(state.pending_raise.code, 0xC0000005u);
            ASSERT_GE(state.pending_raise.num_args, 2u);
            EXPECT_EQ(state.pending_raise.args[0], access_kind);
            EXPECT_EQ(state.pending_raise.args[1], fault_at);
            EXPECT_EQ(state.rip, kCode + c.producer.size());
            for (u8 reg = 0; reg < 16; ++reg)
                EXPECT_EQ(state.gpr.get64(reg), oracle.gpr.get64(reg))
                    << "GPR " << unsigned(reg);
            state.materialize_lazy_flags();
            oracle.materialize_lazy_flags();
            EXPECT_EQ(state.flags.rflags, oracle.flags.rflags)
                << "RFLAGS at the fault";
        }
    }

    GuestMemory memory;
    std::unique_ptr<JitCompiler> jit;
};

// Every record the handler rebuilds, at both widths, in both modes: a
// destination written (DirectDst) and a result discarded (CMP/TEST, the
// handler recomputes it), register and immediate sources.
const std::vector<RecoveryCase>& recoverable_cases() {
    static const std::vector<RecoveryCase> cases{
        {"add eax, ecx", {0x01, 0xC8}, true},
        {"add rax, rcx", {0x48, 0x01, 0xC8}, true},
        {"add rax, -1", {0x48, 0x83, 0xC0, 0xFF}, true},
        {"sub eax, 5", {0x83, 0xE8, 0x05}, true},
        {"sub rax, rdx", {0x48, 0x29, 0xD0}, true},
        {"and rcx, rdx", {0x48, 0x21, 0xD1}, true},
        {"or ecx, edx", {0x09, 0xD1}, true},
        {"xor rdx, rcx", {0x48, 0x31, 0xCA}, true},
        {"cmp rcx, rdx", {0x48, 0x39, 0xD1}, true},
        {"cmp ecx, 7", {0x83, 0xF9, 0x07}, true},
        {"cmp rax, rcx", {0x48, 0x39, 0xC8}, true},
    };
    return cases;
}

TEST_P(IrX64FlagRecovery, StoreFaultAfterProducerRebuildsTheRecord) {
    // mov [r8], r9 into a read-only page: a write fault, nothing committed.
    for (const RecoveryCase& c : recoverable_cases())
        expect_fault_matches(c, {0x4D, 0x89, 0x08}, kData, 1);
}

TEST_P(IrX64FlagRecovery, UnmappedStoreFaultAfterProducerRebuildsTheRecord) {
    for (const RecoveryCase& c : recoverable_cases())
        expect_fault_matches(c, {0x4D, 0x89, 0x08}, kData + 0x8000, 1);
}

TEST_P(IrX64FlagRecovery, LoadFaultAfterProducerRebuildsTheRecord) {
    // mov r10, [r8] from an unmapped page.
    for (const RecoveryCase& c : recoverable_cases())
        expect_fault_matches(c, {0x4D, 0x8B, 0x10}, kData + 0x8000, 0);
}

// push r9 into an unmapped stack slot. Intel SDM Vol. 2 PUSH: the faulting
// write commits nothing, so RSP is the pre-push value; the long-mode tier may
// move RSP first and undo it on the fault path.
TEST_P(IrX64FlagRecovery, PushFaultAfterProducerRestoresRspAndTheRecord) {
    for (const RecoveryCase& c : recoverable_cases())
        expect_fault_matches(c, {0x41, 0x51}, kData + 0x8000, 1);
}

// Shapes the planner must refuse, or whose plan must still be exact.
TEST_P(IrX64FlagRecovery, ClobberedAndUnrecoverableOperandsKeepTheRecord) {
    const std::vector<RecoveryCase> cases{
        // ADD of a register to itself leaves only the result in the one
        // register; CF/OF/AF need the source, which is gone.
        {"add rax, rax", {0x48, 0x01, 0xC0}, false},
        // The compared register is rewritten before the fault.
        {"cmp ecx, edx ; mov ecx, 9",
         {0x39, 0xD1, 0xB9, 0x09, 0x00, 0x00, 0x00}, false, 2},
        // A NARROW write clobbers the whole register: AH is index 4 in the
        // encoding and RAX architecturally.
        {"cmp eax, ecx ; mov ah, 1", {0x39, 0xC8, 0xB4, 0x01}, false, 2},
        // An 8-bit record is not one the handler rebuilds.
        {"add al, cl", {0x00, 0xC8}, false},
    };
    for (const RecoveryCase& c : cases)
        expect_fault_matches(c, {0x4D, 0x89, 0x08}, kData + 0x8000, 1);
}

// R8-R15 have no pin: an unpinned register is written through to CpuState by
// the instruction that defines it, so a plan may name it there
// (JitFlagRecovery::kCpuStateGpr). Neither producer touches R8, the address.
TEST_P(IrX64FlagRecovery, UnpinnedOperandsRebuildTheRecordFromCpuState) {
    const std::vector<RecoveryCase> cases{
        {"add r9, r10", {0x4D, 0x01, 0xD1}, true},
        {"and r10d, 0xfff", {0x41, 0x81, 0xE2, 0xFF, 0x0F, 0x00, 0x00}, true},
        {"xor r11d, r11d", {0x45, 0x31, 0xDB}, true},
        {"cmp r11, rcx", {0x49, 0x39, 0xCB}, true},
        {"sub r9, r11", {0x4D, 0x29, 0xD9}, true},
        {"test r10, r10", {0x4D, 0x85, 0xD2}, true},
    };
    for (const RecoveryCase& c : cases) {
        expect_fault_matches(c, {0x4D, 0x89, 0x08}, kData + 0x8000, 1);
        expect_fault_matches(c, {0x4D, 0x8B, 0x10}, kData + 0x8000, 0);
        // Tier 0 elides the same records under its own planner
        // (try_plan_stencil_flag_recovery) and publishes them the same way.
        expect_fault_matches(c, {0x4D, 0x89, 0x08}, kData + 0x8000, 1, true);
        expect_fault_matches(c, {0x4D, 0x8B, 0x10}, kData + 0x8000, 0, true);
    }
}

// INC/DEC keep CF (Intel SDM Vol. 2A INC/DEC). Their record is rebuilt at a
// fault from the result and the carry CpuState still holds: the block's entry
// state, or a full writer before them whose record therefore stays
// published. The seed's RFLAGS (0xAD7) has CF set, so an entry-state carry
// that was lost shows; the in-block writers produce both carries across the
// seeds.
TEST_P(IrX64FlagRecovery, CarryMergeRebuildsTheRecordWithTheInheritedCarry) {
    const std::vector<RecoveryCase> cases{
        {"inc eax", {0xFF, 0xC0}, true},
        {"dec ecx", {0xFF, 0xC9}, true},
        {"inc rax", {0x48, 0xFF, 0xC0}, true},
        {"dec rdx", {0x48, 0xFF, 0xCA}, true},
        {"inc r9d", {0x41, 0xFF, 0xC1}, true},
        {"dec r10", {0x49, 0xFF, 0xCA}, true},
        {"cmp rcx, rdx ; inc eax", {0x48, 0x39, 0xD1, 0xFF, 0xC0}, true, 2},
        {"add eax, ecx ; dec rdx", {0x01, 0xC8, 0x48, 0xFF, 0xCA}, true, 2},
        {"sub rcx, rax ; inc ecx", {0x48, 0x29, 0xC1, 0xFF, 0xC1}, true, 2},
        // The pair as the hot loop latch has it: a load, then the merge.
        {"mov rax, [r8-0x8000] ; inc eax",
         {0x49, 0x8B, 0x80, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xC0}, true, 2},
    };
    for (const RecoveryCase& c : cases) {
        expect_fault_matches(c, {0x4D, 0x89, 0x08}, kData + 0x8000, 1);
        expect_fault_matches(c, {0x4D, 0x8B, 0x10}, kData + 0x8000, 0);
        expect_fault_matches(c, {0x41, 0x51}, kData + 0x8000, 1);
    }
}

// A record read only by the side exit right after it is published on that
// exit's taken edge (FlagDef::exit_deferred); on the fall-through a fault
// before the killer rebuilds it from the plan. The branches here are never
// taken for the seeds (OR 1 is nonzero, R10 is positive, RSI equals no seed
// RAX, R9D + 7 does not carry), so every run reaches the faulting access.
TEST_P(IrX64FlagRecovery, ExitDeferredRecordIsRebuiltAtAFaultOnTheFallThrough) {
    const std::vector<RecoveryCase> cases{
        {"or rcx, 1 ; jz", {0x48, 0x83, 0xC9, 0x01, 0x74, 0x40}, true, 2},
        {"test r10, r10 ; js", {0x4D, 0x85, 0xD2, 0x78, 0x40}, true, 2},
        {"cmp rsi, rax ; je", {0x48, 0x39, 0xC6, 0x74, 0x40}, true, 2},
        {"add r9d, 7 ; jc", {0x41, 0x83, 0xC1, 0x07, 0x72, 0x40}, true, 2},
    };
    for (const RecoveryCase& c : cases) {
        expect_fault_matches(c, {0x4D, 0x89, 0x08}, kData + 0x8000, 1);
        expect_fault_matches(c, {0x4D, 0x8B, 0x10}, kData + 0x8000, 0);
        expect_fault_matches(c, {0x41, 0x51}, kData + 0x8000, 1);
    }
}

// A 64-bit shift sets CF/OF as overlays; the ADD after it owns both flags,
// so its record would retire them. With that record elided under a plan the
// overlays are still dead: the fault handler's rebuild clears them as the
// publication would have. The exception state must be the ADD's flags.
TEST_P(IrX64FlagRecovery, OverlayBeforeARecoveredWriterIsRetiredByTheRebuild) {
    const std::vector<RecoveryCase> cases{
        {"shl r9, 4 ; add r9, r10",
         {0x49, 0xC1, 0xE1, 0x04, 0x4D, 0x01, 0xD1}, true, 2},
        {"shl rax, 4 ; add rax, rcx",
         {0x48, 0xC1, 0xE0, 0x04, 0x48, 0x01, 0xC8}, true, 2},
        {"rol rdx, 3 ; sub rdx, rcx",
         {0x48, 0xC1, 0xC2, 0x03, 0x48, 0x29, 0xCA}, true, 2},
    };
    for (const RecoveryCase& c : cases) {
        expect_fault_matches(c, {0x4D, 0x89, 0x08}, kData + 0x8000, 1);
        expect_fault_matches(c, {0x4D, 0x8B, 0x10}, kData + 0x8000, 0);
    }
}

// Carry merges the planner must leave published.
TEST_P(IrX64FlagRecovery, CarryMergesItCannotRebuildKeepTheRecord) {
    const std::vector<RecoveryCase> cases{
        // The result register is rewritten before the fault.
        {"inc eax ; mov eax, 9", {0xFF, 0xC0, 0xB8, 0x09, 0x00, 0x00, 0x00},
         false, 2},
        // An 8-bit record is not one the handler rebuilds.
        {"inc al", {0xFE, 0xC0}, false},
        // A 16-bit result lives in a field of its register.
        {"dec cx", {0x66, 0xFF, 0xC9}, false},
    };
    for (const RecoveryCase& c : cases)
        expect_fault_matches(c, {0x4D, 0x89, 0x08}, kData + 0x8000, 1);
}

INSTANTIATE_TEST_SUITE_P(MemoryModel, IrX64FlagRecovery, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? std::string("Linear")
                                               : std::string("RegionCache");
                         });

}  // namespace
#endif
