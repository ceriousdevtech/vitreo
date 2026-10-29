// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_x64_narrow_alu_6_shifts_conditions.cpp
/// @brief Long-mode IR emission: count-driven shifts, condition and carry consumers.

// These tests execute emitted ARM64, so Intel hosts must not reference the
// native-only IR backend through this translation unit.
#if defined(__aarch64__)
#include "ir_x64_memory_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_x64_memory;

namespace {
// SHLD/SHRD: a double-precision shift. The second source feeds the bits that
// shift in and never enters the flag rules, which is what lets the ordinary
// shift record describe these -- so the sweep drives distinct, asymmetric
// destination and source values at every count, where a lowering that mixed
// the two up or read CF from the wrong operand cannot agree by accident.
TEST_F(IrEmit64NarrowAlu, DoublePrecisionShiftsAcrossEveryCount) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    struct Form { u8 op; const char* name; };
    for (const Form form : {Form{0xA4, "shld"}, Form{0xAC, "shrd"}}) {
        // From one: a masked count of zero is "no operation", so it has no
        // native lowering to sweep. Its behaviour is covered by
        // JitParity64Test.Shld64ImmCount0PreservesDestAndFlags and
        // Shld32InLongModeCount0IsANoOp.
        for (u8 count = 1; count < 64; ++count) {
            SCOPED_TRACE(testing::Message() << form.name << " count " << unsigned(count));
            // AF is undefined after a shift; OF is defined only at a count of 1.
            const u64 undefined = 0x10u | (count == 1 ? 0u : 0x800u);
            const auto with_flags = [&](std::vector<u8> tail) {
                std::vector<u8> body{0x48, 0x85, 0xDB};   // test rbx,rbx
                body.insert(body.end(), tail.begin(), tail.end());
                expect_alu_matches_interpreter(body, 2, undefined);
            };
            // 16-bit: masked to five bits, and counts at or above the width
            // are architecturally undefined, so only the defined ones run.
            const u8 narrow = count & 31u;   // 16- and 32-bit mask five bits
            if (narrow != 0 && narrow < 16u) {
                with_flags({0x66, 0x0F, form.op, 0xCB, count});   // bx, cx
                with_flags({0x66, 0x0F, form.op, 0x08, count});   // [rax], cx
            }
            if (narrow != 0) {
                with_flags({0x0F, form.op, 0xCB, count});         // ebx, ecx
                with_flags({0x0F, form.op, 0x08, count});         // [rax], ecx
            }
            with_flags({0x48, 0x0F, form.op, 0xCB, count});       // rbx, rcx
            with_flags({0x48, 0x0F, form.op, 0x08, count});       // [rax], rcx
            // REX.R/B reach the extended registers on both operands.
            with_flags({0x4D, 0x0F, form.op, 0xCB, count});       // r11, r9
        }
    }
}

// 64-bit shifts and rotates by a compile-time count, across the whole SIX-bit
// count space. The shifts are the reason this group was open: their record has
// both slots taken by the 64-bit original and result, so CF has to arrive
// through the overlay -- and an overlay published before the record would be
// wiped by it, which is invisible at a count of 1 (where the `_1` record
// defines CF itself) and wrong at every other count.
TEST_F(IrEmit64NarrowAlu, WideShiftsAndRotatesAcrossTheSixBitCountSpace) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    struct Form { u8 ext; const char* name; bool rotate; };
    const std::vector<Form> forms{{4, "shl", false}, {5, "shr", false},
                                  {7, "sar", false}, {0, "rol", true},
                                  {1, "ror", true},  {2, "rcl", true},
                                  {3, "rcr", true}};
    for (const Form& form : forms) {
        for (u32 raw = 0; raw < 68; ++raw) {
            const u8 count = static_cast<u8>(raw < 64 ? raw : (raw - 64) + 252);
            const u8 masked = count & 63u;
            SCOPED_TRACE(testing::Message() << form.name << " imm " << unsigned(count));
            // AF is undefined after both; OF is defined only at a count of 1.
            const u64 undefined = 0x10u | (masked == 1 ? 0u : 0x800u);
            const auto with_flags = [&](std::vector<u8> tail) {
                // `test rbx,rbx` first: a rotate must leave its ZF/SF/PF
                // standing, and a masked count of zero must leave all of them.
                std::vector<u8> body{0x48, 0x85, 0xDB};
                body.insert(body.end(), tail.begin(), tail.end());
                expect_alu_matches_interpreter(body, 2, undefined);
            };
            with_flags({0x48, 0xC1, static_cast<u8>(0xC3 | (form.ext << 3)), count});
            with_flags({0x48, 0xC1, static_cast<u8>(0x00 | (form.ext << 3)), count});
            // REX.B reaches the extended registers.
            with_flags({0x49, 0xC1, static_cast<u8>(0xC1 | (form.ext << 3)), count});
        }
        // 48 D1 /n: the by-1 encoding, where OF is defined.
        expect_alu_matches_interpreter(
            {0x48, 0x85, 0xDB, 0x48, 0xD1, static_cast<u8>(0xC3 | (form.ext << 3))},
            2, 0x10u);
        expect_alu_matches_interpreter(
            {0x48, 0x85, 0xDB, 0x48, 0xD1, static_cast<u8>(0x00 | (form.ext << 3))},
            2, 0x10u);
    }
    // The through-carry pair READS CF, so it is driven from more than one
    // producer -- a carry input that was dropped would still agree with the
    // interpreter whenever the seeded CF happened to match.
    for (const std::vector<u8>& producer : std::vector<std::vector<u8>>{
             {0x00, 0xD2}, {0xFE, 0xC2}, {0xD0, 0xC2}}) {
        for (const u8 ext : {u8{2}, u8{3}}) {
            for (const u8 count : {u8{1}, u8{2}, u8{31}, u8{32}, u8{63}}) {
                std::vector<u8> body = producer;
                body.insert(body.end(),
                            {0x48, 0xC1, static_cast<u8>(0xC3 | (ext << 3)), count});
                expect_alu_matches_interpreter(
                    body, 2, 0x10u | (count == 1 ? 0u : 0x800u));
            }
        }
    }
}

// Rotates by CL, all four directions. The count is a runtime value, so every
// CL value in the five-bit space is driven -- including 0 (no flag written at
// all), the width (a full cycle that still writes CF) and the through-carry
// span (9 and 17, where the modulo is not a power of two). CL is set by an
// immediate MOV rather than the seed so the count is exact rather than
// sampled.
TEST_F(IrEmit64NarrowAlu, RotatesByClAcrossTheWholeCountSpace) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    struct Form { u8 ext; const char* name; bool carry; };
    for (const Form form : {Form{0, "rol", false}, Form{1, "ror", false},
                            Form{2, "rcl", true},  Form{3, "rcr", true}}) {
        for (u8 cl = 0; cl < 34; ++cl) {
            SCOPED_TRACE(testing::Message() << form.name << " cl=" << unsigned(cl));
            const u64 undefined = (cl & 31u) == 1u ? 0u : 0x800u;
            // `mov cl, imm8` then a carry producer, then the rotate.
            const auto by_cl = [&](std::vector<u8> tail) {
                std::vector<u8> body{0xB1, cl, 0x00, 0xD2};
                body.insert(body.end(), tail.begin(), tail.end());
                expect_alu_matches_interpreter(body, 3, undefined);
            };
            by_cl({0xD2, static_cast<u8>(0xC3 | (form.ext << 3))});   // bl
            by_cl({0xD2, static_cast<u8>(0xC7 | (form.ext << 3))});   // bh
            by_cl({0xD2, static_cast<u8>(0x00 | (form.ext << 3))});   // m8
            by_cl({0x66, 0xD3, static_cast<u8>(0xC3 | (form.ext << 3))});
            by_cl({0x66, 0xD3, static_cast<u8>(0x00 | (form.ext << 3))});
            by_cl({0xD3, static_cast<u8>(0xC3 | (form.ext << 3))});
            by_cl({0xD3, static_cast<u8>(0x00 | (form.ext << 3))});
        }
    }
}

// The overlay must also survive being followed by a flag CONSUMER, and be
// cleared by a following full-flag writer -- `set_flag_op` owns CF/OF again.
TEST_F(IrEmit64NarrowAlu, RotateCarryIsVisibleThenSupersededByAFullWriter) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // rol cl,1 ; adc-free: `add dl, dl` is a full writer that must take CF back
    expect_alu_matches_interpreter({0xD0, 0xC1, 0x00, 0xD2}, 2);
    // A rotate between two full writers: the second must own CF and OF again.
    expect_alu_matches_interpreter({0x00, 0xD1, 0xD0, 0xC1, 0x00, 0xD9}, 3);
}

// SETcc: all sixteen conditions, each read back from a different kind of flag
// producer. The condition walk is the whole point -- an `EvalCc` that read the
// wrong record, or read it after a helper had already materialised it, would
// agree with the interpreter for some producers and not others, so every
// condition is driven from a compare, a logic op, an INC (which preserves CF)
// and a rotate (which forces CF through the overlay).
TEST_F(IrEmit64NarrowAlu, SetccReadsEveryConditionFromEveryProducer) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    struct Producer { std::vector<u8> bytes; const char* name; };
    const std::vector<Producer> producers{
        {{0x48, 0x39, 0xD9}, "cmp rcx,rbx"},
        {{0x48, 0x85, 0xD9}, "test rcx,rbx"},
        {{0x00, 0xD1},       "add cl,dl"},
        {{0xFE, 0xC1},       "inc cl"},
        {{0xD0, 0xC1},       "rol cl,1"},
    };
    for (const Producer& producer : producers) {
        for (u8 cond = 0; cond < 16; ++cond) {
            SCOPED_TRACE(testing::Message() << producer.name
                         << " cond " << unsigned(cond));
            std::vector<u8> body = producer.bytes;
            // 0F 90+cc /r, into DL and into byte [rax].
            body.push_back(0x0F);
            body.push_back(static_cast<u8>(0x90 + cond));
            body.push_back(0xC2);
            expect_alu_matches_interpreter(body, 2);

            std::vector<u8> to_memory = producer.bytes;
            to_memory.push_back(0x0F);
            to_memory.push_back(static_cast<u8>(0x90 + cond));
            to_memory.push_back(0x00);
            expect_alu_matches_interpreter(to_memory, 2);
        }
    }
}

// A SETcc destination is a byte, so a register form must MERGE: the rest of
// the architectural register survives, and the high-byte spelling reaches the
// right one.
TEST_F(IrEmit64NarrowAlu, SetccMergesItsByteDestination) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    for (const u8 modrm : {u8{0xC2}, u8{0xC6}, u8{0xC1}}) {  // dl, dh, cl
        expect_alu_matches_interpreter({0x48, 0x39, 0xD9, 0x0F, 0x94, modrm}, 2);
        expect_alu_matches_interpreter({0x48, 0x39, 0xD9, 0x40, 0x0F, 0x94, modrm}, 2);
    }
}

// CMOVcc: all sixteen conditions at all three widths, register and memory
// sources. Both arms are always evaluated -- Intel SDM Vol. 2A says a memory
// source is READ whether or not the condition holds -- and a 32-bit
// destination zeroes bits 63:32 either way, which a select that wrote only on
// the taken path would get wrong.
TEST_F(IrEmit64NarrowAlu, CmovccAtEveryConditionAndWidth) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    for (u8 cond = 0; cond < 16; ++cond) {
        SCOPED_TRACE(testing::Message() << "cmov cond " << unsigned(cond));
        const u8 opcode = static_cast<u8>(0x40 + cond);
        // 32-bit: cmovcc ecx, edx  and  cmovcc ecx, [rax]
        expect_alu_matches_interpreter({0x48, 0x39, 0xD9, 0x0F, opcode, 0xCA}, 2);
        expect_alu_matches_interpreter({0x48, 0x39, 0xD9, 0x0F, opcode, 0x08}, 2);
        // 64-bit
        expect_alu_matches_interpreter({0x48, 0x39, 0xD9, 0x48, 0x0F, opcode, 0xCA}, 2);
        expect_alu_matches_interpreter({0x48, 0x39, 0xD9, 0x48, 0x0F, opcode, 0x08}, 2);
        // 16-bit -- a PARTIAL write, so the rest of the register survives
        expect_alu_matches_interpreter({0x48, 0x39, 0xD9, 0x66, 0x0F, opcode, 0xCA}, 2);
        expect_alu_matches_interpreter({0x48, 0x39, 0xD9, 0x66, 0x0F, opcode, 0x08}, 2);
    }
}

// The not-taken path still zeroes bits 63:32 of a 32-bit destination, and
// still leaves a 16-bit destination's upper bits alone.
TEST_F(IrEmit64NarrowAlu, CmovNotTakenStillObeysTheDestinationWidth) {
    jit = std::make_unique<JitCompiler>(memory);
    // cmp rcx,rcx (ZF=1, so CMOVNZ is NOT taken) ; cmovnz ecx, edx
    auto* native = compile({0x48, 0x39, 0xC9, 0x0F, 0x45, 0xCA});
    ASSERT_NE(native, nullptr);
    auto state = seed();
    state.gpr.rcx = 0xAAAAAAAAFFFFFFFFull;
    state.gpr.rdx = 0x1111111122222222ull;
    ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
    EXPECT_EQ(state.gpr.rcx, 0xFFFFFFFFull)
        << "a 32-bit CMOV zeroes the high half even when not taken";
}

// ADC/SBB read the carry and then redefine every flag from the three-way sum.
// Both carry polarities are driven from a real producer -- a compare that sets
// or clears CF, and a rotate that forces it through the overlay -- because the
// record packs the carry-in INTO its source word and a lowering that lost it
// would still look right whenever CF happened to be 0.
TEST_F(IrEmit64NarrowAlu, AdcSbbConsumeTheCarryFromEveryProducer) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    struct Producer { std::vector<u8> bytes; const char* name; };
    const std::vector<Producer> producers{
        {{0x48, 0x39, 0xD9}, "cmp rcx,rbx"},       // CF from a compare
        {{0x48, 0x39, 0xC9}, "cmp rcx,rcx"},       // CF = 0
        {{0xD0, 0xC1},       "rol cl,1"},          // CF through the overlay
        {{0xFE, 0xC1},       "inc cl"},            // CF preserved by INC
    };
    // 10/18 = r/m8,r8 ; 12/1A = r8,r/m8 ; +1 for the 16/32-bit widths.
    struct Form { std::vector<u8> tail; const char* name; };
    const std::vector<Form> forms{
        {{0x10, 0xD1}, "adc cl,dl"},
        {{0x12, 0xD1}, "adc dl,cl"},
        {{0x18, 0xD1}, "sbb cl,dl"},
        {{0x1A, 0xD1}, "sbb dl,cl"},
        {{0x10, 0x08}, "adc [rax],cl"},
        {{0x18, 0x08}, "sbb [rax],cl"},
        {{0x12, 0x08}, "adc cl,[rax]"},
        {{0x66, 0x11, 0xD1}, "adc cx,dx"},
        {{0x66, 0x19, 0xD1}, "sbb cx,dx"},
        {{0x11, 0xD1}, "adc ecx,edx"},
        {{0x19, 0xD1}, "sbb ecx,edx"},
        {{0x11, 0x08}, "adc [rax],ecx"},
        {{0x80, 0xD1, 0x7F}, "adc cl,0x7f"},
        {{0x80, 0xD9, 0x80}, "sbb cl,0x80"},
        {{0x83, 0xD1, 0xFF}, "adc ecx,-1"},
        {{0x66, 0x83, 0xD9, 0xFF}, "sbb cx,-1"},
        {{0x81, 0xD1, 0x01, 0x00, 0x00, 0x80}, "adc ecx,0x80000001"},
    };
    for (const Producer& producer : producers) {
        for (const Form& form : forms) {
            SCOPED_TRACE(testing::Message() << producer.name << " ; " << form.name);
            std::vector<u8> body = producer.bytes;
            body.insert(body.end(), form.tail.begin(), form.tail.end());
            expect_alu_matches_interpreter(body, 2);
        }
    }
}

// A faulting ADC write-back must publish the PRECEDING flags, exactly as the
// plain ALU forms do -- the carry it read must not have been committed either.
TEST_F(IrEmit64NarrowAlu, FaultingAdcWriteBackPublishesThePrecedingFlags) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::Read));
    const std::vector<u8> bytes{0x48, 0x85, 0xDB, 0x10, 0x08};  // test rbx,rbx ; adc [rax],cl
    auto* native = compile(bytes);
    ASSERT_NE(native, nullptr);
    auto oracle = seed();
    oracle.gpr.rax = kData;
    oracle.gpr.rbx = 0x8000000000000000ull;
    oracle.gpr.rcx = 0x5Aull;
    auto state = oracle;
    {
        Interpreter interpreter(oracle, memory);
        ASSERT_EQ(interpreter.step(), ExecResult::Continue);
    }
    (void)jit->execute(*native, state);
    ASSERT_TRUE(state.pending_raise.active);
    EXPECT_EQ(state.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(state.rip, oracle.rip);
    for (u8 index = 0; index < 16; ++index)
        EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index)) << unsigned(index);
    state.materialize_lazy_flags();
    oracle.materialize_lazy_flags();
    EXPECT_EQ(state.flags.rflags, oracle.flags.rflags);
}

// Shifts by CL. The count is a runtime value, so the zero-count rule -- a
// masked count of zero modifies NO flags, not even ZF -- becomes a runtime
// skip in the lowering. Each body is prefixed with `test rbx,rbx` so that
// skip is observable: at CL==0 the TEST's flags must come out untouched.
TEST_F(IrEmit64NarrowAlu, ShiftsByClIncludingTheZeroCountSkip) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    struct Form { u8 ext; const char* name; };
    const std::vector<Form> forms{{4, "shl"}, {5, "shr"}, {7, "sar"}};
    for (const Form& form : forms) {
        for (const u64 cl : {0ull, 1ull, 7ull, 8ull, 9ull, 15ull, 16ull,
                             17ull, 31ull, 32ull, 33ull, 255ull}) {
            SCOPED_TRACE(testing::Message() << form.name << " cl=" << cl);
            // CL is masked to five bits, so 32 behaves as 0 and 33 as 1 --
            // the cases a lowering that forgot the mask gets wrong.
            const u64 masked = cl & 31u;
            const u64 undefined = masked == 1 ? 0x10u : (0x10u | 0x800u);
            const auto with_flags = [&](std::vector<u8> tail) {
                std::vector<u8> body{0x48, 0x85, 0xDB};
                body.insert(body.end(), tail.begin(), tail.end());
                jit = std::make_unique<JitCompiler>(memory);
                auto* native = compile(body);
                ASSERT_NE(native, nullptr);
                for (const u64 value : {0x01ull, 0x80ull, 0xFFull, 0x8001ull,
                                        0xFFFFFFFFull}) {
                    auto oracle = seed();
                    oracle.gpr.set64(0, kData);
                    oracle.gpr.rcx = cl;              // CL is the count
                    oracle.gpr.rdx = value;
                    oracle.gpr.rbx = value;
                    oracle.flags.rflags = 0xAD7;
                    ASSERT_TRUE(memory.write_u64(kData, value).is_ok());
                    auto state = oracle;
                    {
                        Interpreter interpreter(oracle, memory);
                        for (int step = 0; step < 2; ++step)
                            ASSERT_EQ(interpreter.step(), ExecResult::Continue);
                    }
                    const auto oracle_memory = memory.read_u64(kData);
                    ASSERT_TRUE(memory.write_u64(kData, value).is_ok());
                    ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
                    EXPECT_EQ(memory.read_u64(kData).value_or(1),
                              oracle_memory.value_or(2)) << "memory, value=" << std::hex << value;
                    for (u8 index = 0; index < 16; ++index)
                        EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                            << "GPR " << unsigned(index) << " value=" << std::hex << value;
                    state.materialize_lazy_flags();
                    oracle.materialize_lazy_flags();
                    EXPECT_EQ(state.flags.rflags & ~undefined,
                              oracle.flags.rflags & ~undefined)
                        << "RFLAGS value=" << std::hex << value;
                }
            };
            // D2 /n (byte) and D3 /n (word with 66, dword without), register
            // and memory destinations.
            with_flags({0xD2, static_cast<u8>(0xC2 | (form.ext << 3))});
            with_flags({0xD2, static_cast<u8>(0x00 | (form.ext << 3))});
            with_flags({0x66, 0xD3, static_cast<u8>(0xC2 | (form.ext << 3))});
            with_flags({0xD3, static_cast<u8>(0xC2 | (form.ext << 3))});
            with_flags({0xD3, static_cast<u8>(0x00 | (form.ext << 3))});
        }
    }
}

// A faulting narrow read-modify-write must publish the PRECEDING flags and
// leave both the memory and the destination untouched.
TEST_F(IrEmit64NarrowAlu, FaultingNarrowWriteBackPublishesThePrecedingFlags) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::Read));
    // test rbx,rbx ; add byte [rax], cl  (the store faults on a read-only page)
    const std::vector<u8> bytes{0x48,0x85,0xDB, 0x00,0x08};
    auto* native = compile(bytes);
    ASSERT_NE(native, nullptr);
    auto oracle = seed();
    oracle.gpr.rax = kData;
    oracle.gpr.rbx = 0x8000000000000000ull;
    oracle.gpr.rcx = 0x5Aull;
    auto state = oracle;
    {
        Interpreter interpreter(oracle, memory);
        ASSERT_EQ(interpreter.step(), ExecResult::Continue);  // only the TEST
    }
    const auto before = memory.read_u64(kData);
    (void)jit->execute(*native, state);
    ASSERT_TRUE(state.pending_raise.active);
    EXPECT_EQ(state.pending_raise.code, 0xC0000005u);
    ASSERT_GE(state.pending_raise.num_args, 2u);
    EXPECT_EQ(state.pending_raise.args[0], 1u);
    EXPECT_EQ(state.rip, oracle.rip);
    EXPECT_EQ(memory.read_u64(kData).value_or(~u64{0}), before.value_or(0));
    for (u8 index = 0; index < 16; ++index)
        EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index)) << unsigned(index);
    state.materialize_lazy_flags();
    oracle.materialize_lazy_flags();
    EXPECT_EQ(state.flags.rflags, oracle.flags.rflags)
        << "the attempted narrow ALU must not have published its flags";
}
} // namespace
#endif
