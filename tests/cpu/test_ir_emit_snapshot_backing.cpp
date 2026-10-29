// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_emit_snapshot_backing.cpp
/// @brief Tier 1 IR emission parity: PE32 snapshot backing (M9); contract in test_ir_emit.cpp.

#include "ir_emit_test_support.hpp"
#include "ir_tier1_first_compile.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_emit;

namespace {

/// Interpret `prologue`, then run ONLY `body` natively from the resulting
/// state and compare against the interpreter running the same body from the
/// same state. The prologue is what establishes a realistic entry flag
/// representation (overlay or lazy record) that the block then has to consume.
void expect_native_pe32_matches_interpreter(const std::vector<u8>& prologue,
                                            const std::vector<u8>& body,
                                            u32 eax, u32 ecx, u32 edx) {
    std::vector<u8> code = prologue;
    code.insert(code.end(), body.begin(), body.end());
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(0x20000, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    const auto entry = [&](CpuState& cpu) {
        seed_state(cpu, Seed{eax, ecx, edx});
        cpu.gpr.set32(3, 0x8000'0000u);
        Interpreter interp(cpu, memory);
        for (u32 i = 0; i < decode_bytes(prologue).instruction_count; ++i)
            ASSERT_EQ(interp.step(), ExecResult::Continue);
    };

    BasicBlock block = decode_bytes(body);
    block.start_addr = kCode + prologue.size();

    CpuState interp_cpu;
    entry(interp_cpu);
    ASSERT_EQ(interp_cpu.rip, block.start_addr);
    {
        Interpreter interp(interp_cpu, memory);
        for (u32 i = 0; i < block.instruction_count; ++i)
            ASSERT_EQ(interp.step(), ExecResult::Continue);
    }

    CpuState jit_cpu;
    entry(jit_cpu);
    JitCompiler jit(memory);
    ASSERT_TRUE(jit.ir_build_block(block).ok)
        << ir::decline_reason_name(jit.ir_last_result().reason);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_TRUE(jit.ir_emit_active());
    jit.execute(*compiled, jit_cpu);

    jit_cpu.materialize_lazy_flags();
    interp_cpu.materialize_lazy_flags();
    for (u8 r = 0; r < 8; ++r)
        EXPECT_EQ(jit_cpu.gpr.get32(r), interp_cpu.gpr.get32(r))
            << "GPR " << int(r) << " eax=" << std::hex << eax << " edx=" << edx;
    EXPECT_EQ(jit_cpu.flags.rflags, interp_cpu.flags.rflags)
        << "RFLAGS eax=" << std::hex << eax << " ecx=" << ecx << " edx=" << edx;
    EXPECT_EQ(jit_cpu.rip, interp_cpu.rip);
}

}  // namespace

// =============================================================================
// PE32 snapshot backing (M9)
// =============================================================================

// INC/DEC publish Inc32/Dec32, which leave CF untouched (Intel SDM Vol. 2A).
// The backend merges the incoming CF through a helper that clobbers X0-X4, so
// the block has to capture CF before reloading operands. Incoming CF is driven
// from a real interpreted prologue -- an overlay from STC/CLC and a lazy ALU
// record -- rather than a hand-set RFLAGS word, because those are the two
// shapes the merge helper actually has to read.
TEST(IrEmit, Pe32IncDecPreservesIncomingCarry) {
    REQUIRE_IR_GATE();
    struct Case {
        std::vector<u8> body;
        u32 live_merges;  // surviving Inc32/Dec32 records that must merge CF
    };
    const std::vector<Case> cases{
        // INC/DEC last: CF comes from this block's own lazy Add32 record.
        {{0x83,0xC0,5, 0x83,0xC3,5, 0x42}, 1},
        {{0x83,0xC0,5, 0x83,0xC3,5, 0x4A}, 1},
        // No ALU publisher in front: CF comes from the block's entry state.
        {{0x42, 0x4B}, 2},
        {{0x4A, 0x43}, 2},
        // The record is overwritten by a following ADD, so no merge survives;
        // CF must still be the entry value because INC never wrote it.
        {{0x83,0xC0,5, 0x42, 0x83,0xC3,5}, 0},
    };
    for (const Case& test_case : cases) {
        // Mechanism first: without this the differential below would pass on a
        // block that never reaches the CF-preserving publisher at all.
        {
            GuestMemory memory;
            ASSERT_TRUE(memory.map_region(kCode, 0x1000, MemoryProtection::ReadWrite));
            ASSERT_TRUE(memory.write_bytes(kCode, test_case.body.data(),
                                           test_case.body.size()));
            JitCompiler jit(memory);
            BasicBlock block = decode_bytes(test_case.body);
            ASSERT_TRUE(jit.ir_build_block(block).ok)
                << ir::decline_reason_name(jit.ir_last_result().reason);
            u32 live = 0;
            for (const auto& in : jit.ir_block().insts)
                if (in.op == ir::Op::DefFlags && in.operand_count == 3 &&
                    in.flags.repr != ir::FlagRepr::Dead) ++live;
            EXPECT_EQ(live, test_case.live_merges);
        }
        for (const std::vector<u8>& prologue : {
                 std::vector<u8>{0xF8},              // clc: CF=0 overlay
                 std::vector<u8>{0xF9},              // stc: CF=1 overlay
                 std::vector<u8>{0x01,0xC8},         // add eax,ecx: lazy record
             }) {
            for (const u32 edx : {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu}) {
                // EAX/ECX chosen so the ALU prologue produces CF=0 and CF=1.
                for (const auto carry : {std::pair<u32,u32>{1u, 2u},
                                         std::pair<u32,u32>{0xFFFFFFFFu, 3u}}) {
                    expect_native_pe32_matches_interpreter(
                        prologue, test_case.body, carry.first, carry.second, edx);
                }
            }
        }
    }
}

// A PE32 block whose load faults must expose exactly the state the
// interpreter has at that instruction: the committed first ADD, the entry
// flags, the faulting RIP -- and none of the uncommitted values or the work
// that follows the access.
TEST(IrEmit, Pe32FaultExposesOnlyCommittedState) {
    REQUIRE_IR_GATE();
    constexpr GuestAddr kUnmapped = 0x90000000u;
    // add eax,5 / mov edx,[ecx] / add ebx,5
    const std::vector<u8> body{0x83,0xC0,5, 0x8B,0x11, 0x83,0xC3,5};
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(kCode, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_bytes(kCode, body.data(), body.size()));
    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(body);
    ASSERT_TRUE(jit.ir_build_block(block).ok)
        << ir::decline_reason_name(jit.ir_last_result().reason);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_TRUE(jit.ir_emit_active());
    // No storage assertion here: the contract under test is what the FAULT
    // exposes, not which storage the surviving values happened to use.

    CpuState oracle;
    seed_state(oracle, Seed{0x7FFFFFFFu, kUnmapped, 0x1234u});
    oracle.gpr.set32(3, 0xFFFFFFFFu);
    CpuState actual = oracle;
    {
        Interpreter interp(oracle, memory);
        ASSERT_EQ(interp.step(), ExecResult::Continue);  // only the first ADD
    }
    jit.execute(*compiled, actual);
    ASSERT_TRUE(actual.pending_raise.active);
    EXPECT_EQ(actual.pending_raise.code, 0xC0000005u);
    ASSERT_GE(actual.pending_raise.num_args, 2u);
    EXPECT_EQ(actual.pending_raise.args[0], 0u);  // read attempted
    EXPECT_EQ(actual.pending_raise.args[1], kUnmapped);
    EXPECT_EQ(actual.rip, oracle.rip);
    for (u8 r = 0; r < 8; ++r)
        EXPECT_EQ(actual.gpr.get32(r), oracle.gpr.get32(r)) << "GPR " << int(r);
    actual.materialize_lazy_flags();
    oracle.materialize_lazy_flags();
    EXPECT_EQ(actual.flags.rflags, oracle.flags.rflags);
}

TEST(IrEmit64, WidthAwareBlockAllocationAndMixedWidthInvalidation) {
    const vitreo::cpu::jit::test::IrTier1FirstCompile tier1_first_compile;
    if (!JitCompiler::ir_enabled_for(true)) GTEST_SKIP();
    std::vector<std::vector<u8>> bodies;
    for (u8 reg = 0; reg < 16; ++reg) {
        const u8 dest = (reg + 5) & 15;
        const u8 other = (reg + 9) & 15;
        const auto mov = [](std::vector<u8>& code, u8 dst, u8 src, bool wide) {
            code.insert(code.end(), {static_cast<u8>(0x40 | (wide ? 8 : 0) |
                (src >= 8 ? 4 : 0) | (dst >= 8 ? 1 : 0)), 0x89,
                static_cast<u8>(0xC0 | ((src & 7) << 3) | (dst & 7))});
        };
        std::vector<u8> reads;
        mov(reads, dest, reg, true);
        mov(reads, other, reg, true); // same immutable I64 snapshot
        mov(reads, reg, dest, false); // invalidates both widths, zeroes upper half
        mov(reads, other, reg, true); // must reload the changed full register
        mov(reads, dest, reg, false);
        bodies.push_back(reads);
        for (bool wide : {false, true}) {
            for (u8 immediate : {u8{1}, u8{0x7F}, u8{0x80}, u8{0xFF}}) {
                std::vector<u8> code;
                for (u8 extension : {u8{0}, u8{5}, u8{6}}) {
                    code.insert(code.end(), {static_cast<u8>(0x40 | (wide ? 8 : 0) |
                        (reg >= 8)), 0x83,
                        static_cast<u8>(0xC0 | (extension << 3) | (reg & 7)), immediate});
                }
                bodies.push_back(code);
            }
        }
    }
    expect_long_mode_scalar(bodies);
}

TEST(IrEmit64, WidthAwareAllocationPressureSpillsWithParity) {
    const vitreo::cpu::jit::test::IrTier1FirstCompile tier1_first_compile;
    if (!JitCompiler::ir_enabled_for(true)) GTEST_SKIP();
    // Two distinct persistent constants plus a read and result exceed X0-X2.
    const std::vector<u8> code{0x48,0x83,0xC0,1, 0x49,0x83,0xC0,2,
                              0x48,0x83,0xC0,1, 0x49,0x83,0xC0,2};
    expect_long_mode_scalar({code});
}

TEST(IrEmit64, WidthAwarePassesKeepSnapshotsAndConstantsTyped) {
    const vitreo::cpu::jit::test::IrTier1FirstCompile tier1_first_compile;
    ir::IrBlock block;
    block.is_64bit = true;
    block.entry_rip = 0x100001000;
    ir::IrBuilder b(block);
    b.set_rip(block.entry_rip);
    const auto first = b.gpr_read(ir::GuestReg{15, ir::Type::I64});
    b.gpr_write(ir::GuestReg{8, ir::Type::I64}, first);
    b.set_rip(block.entry_rip + 3);
    b.gpr_write(ir::GuestReg{9, ir::Type::I64}, b.gpr_read(ir::GuestReg{15, ir::Type::I64}));
    b.gpr_write(ir::GuestReg{15, ir::Type::I32}, b.constant(ir::Type::I32, 7));
    const auto changed = b.gpr_read(ir::GuestReg{15, ir::Type::I64});
    b.gpr_write(ir::GuestReg{10, ir::Type::I64}, changed);
    ir::PassStats stats;
    EXPECT_EQ(ir::reuse_register_reads(block, stats), 1u);
    EXPECT_EQ(block.insts.back().operands[1].as_value(), changed);
    // Both widths share one instruction, so only the width can keep the
    // I32 and I64 fives apart.
    b.set_rip(block.entry_rip + 20);
    std::vector<ir::ValueId> fives;
    for (auto type : {ir::Type::I32, ir::Type::I64}) {
        for (u8 reg : {u8{11},u8{12}}) {
            const auto five = b.constant(type, 5);
            fives.push_back(five);
            const auto result = b.binary(ir::Op::Add, type,
                b.gpr_read(ir::GuestReg{reg, type}), five);
            b.gpr_write(ir::GuestReg{reg, type}, result);
        }
    }
    EXPECT_EQ(ir::common_scalar_constants(block, stats), 2u);
    std::vector<ir::ValueId> used;
    for (const auto& in : block.insts)
        if (in.op == ir::Op::Add) used.push_back(in.operands[1].as_value());
    ASSERT_EQ(used.size(), 4u);
    EXPECT_EQ(used[0], fives[0]);
    EXPECT_EQ(used[1], fives[0]);
    EXPECT_EQ(used[2], fives[2]);
    EXPECT_EQ(used[3], fives[2]);
    std::string why;
    EXPECT_TRUE(ir::verify(block, &why)) << why;
}

TEST(IrEmit64, RepeatedIndependentSourcesRemainNativeWithSpillBacking) {
    const vitreo::cpu::jit::test::IrTier1FirstCompile tier1_first_compile;
    if (!JitCompiler::ir_enabled_for(true)) GTEST_SKIP();
    std::vector<u8> body;
    for (u32 i = 0; i < 8; ++i)
        body.insert(body.end(), {0x48,0x89,0xC8, 0x48,0x01,0xD0,
                                0x48,0x31,0xD8, 0x48,0x29,0xF8});
    expect_long_mode_scalar({body});
}

// An INC/DEC whose incoming flags were published earlier in the same block
// takes CF from that record's formula (Tier 0's `CarrySource`), not from the
// runtime dispatch over every FlagOp. Mechanism: no load of the record's op
// byte, the dispatch's first instruction. Semantics: CF, and every other flag,
// matches the interpreter for producers leaving CF=0 and CF=1.
TEST(IrEmit, CfMergeTakesAnInBlockProducersCarryFormula) {
    REQUIRE_IR_GATE();
    struct Producer { const char* name; std::vector<u8> bytes; };
    const std::vector<Producer> producers = {
        {"add", {0x01, 0xC8}}, {"sub", {0x29, 0xC8}}, {"cmp", {0x39, 0xC8}},
        {"and", {0x21, 0xC8}}, {"or",  {0x09, 0xC8}}, {"xor", {0x31, 0xC8}},
    };
    const u32 dispatch_load = arm64::ldrb_uoff(REG_SCRATCH3, REG_CPU_STATE,
                                               flag_op_offset());
    for (const Producer& p : producers) {
        for (const u8 incdec : {u8{0x42}, u8{0x4A}}) {   // inc edx / dec edx
            std::vector<u8> code = p.bytes;
            code.push_back(incdec);
            for (const auto ab : {std::pair<u32, u32>{1u, 2u},
                                  std::pair<u32, u32>{0xFFFFFFFFu, 3u},
                                  std::pair<u32, u32>{3u, 9u},
                                  std::pair<u32, u32>{9u, 3u}}) {
                for (const bool cf_in : {false, true}) {
                    SCOPED_TRACE(testing::Message()
                                 << p.name << (incdec == 0x42 ? "; inc" : "; dec")
                                 << " eax=" << ab.first << " ecx=" << ab.second
                                 << " cf_in=" << cf_in);
                    GuestMemory memory;
                    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                                  MemoryProtection::ReadWrite));
                    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));
                    const auto seed = [&](CpuState& cpu) {
                        cpu.init_protected_mode();
                        cpu.gpr.set32(0, ab.first);
                        cpu.gpr.set32(1, ab.second);
                        cpu.gpr.set32(2, 0x7FFFFFFFu);
                        cpu.gpr.set32(4, 0x20800);
                        cpu.flags.set_cf(cf_in);
                        cpu.rip = kCode;
                    };
                    CpuState oracle;
                    seed(oracle);
                    {
                        Interpreter interp(oracle, memory);
                        for (int i = 0; i < 2; ++i)
                            ASSERT_EQ(interp.step(), ExecResult::Continue);
                    }
                    oracle.materialize_lazy_flags();

                    CpuState jit_cpu;
                    seed(jit_cpu);
                    JitCompiler jit(memory);
                    BasicBlock block = decode_bytes(code);
                    ASSERT_TRUE(jit.ir_build_block(block).ok)
                        << ir::decline_reason_name(jit.ir_last_result().reason);
                    CompiledBlock* compiled = jit.compile(block);
                    ASSERT_NE(compiled, nullptr);
                    ASSERT_TRUE(jit.ir_emit_active());
                    const u32* words = static_cast<const u32*>(compiled->code);
                    EXPECT_EQ(std::count(words, words + compiled->code_size / 4u,
                                         dispatch_load), 0)
                        << "CF merge still dispatches on the record's FlagOp";
                    jit.execute(*compiled, jit_cpu);
                    jit_cpu.materialize_lazy_flags();
                    EXPECT_EQ(jit_cpu.gpr.get32(0), oracle.gpr.get32(0));
                    EXPECT_EQ(jit_cpu.gpr.get32(2), oracle.gpr.get32(2));
                    EXPECT_EQ(jit_cpu.flags.rflags, oracle.flags.rflags);
                }
            }
        }
    }
}
