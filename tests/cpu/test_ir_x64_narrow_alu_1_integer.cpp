// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_x64_narrow_alu_1_integer.cpp
/// @brief Long-mode IR emission: narrow integer ALU, INC/DEC, shifts and rotates (M11b).

// These tests execute emitted ARM64, so Intel hosts must not reference the
// native-only IR backend through this translation unit.
#if defined(__aarch64__)
#include "ir_x64_memory_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_x64_memory;

namespace {
TEST_F(IrEmit64NarrowAlu, ByteRegisterFormsAcrossEveryOpcodeAndSpelling) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // /r opcode bases: ADD 00, OR 08, AND 20, SUB 28, XOR 30, CMP 38.
    for (const u8 base : {u8{0x00}, u8{0x08}, u8{0x20}, u8{0x28}, u8{0x30}, u8{0x38}}) {
        for (u8 reg = 0; reg < 8; ++reg) {
            const u8 modrm = static_cast<u8>(0xC0 | (reg << 3) | ((reg + 3) & 7));
            expect_alu_matches_interpreter({base, modrm});                 // r/m8, r8
            expect_alu_matches_interpreter({static_cast<u8>(base + 2), modrm});  // r8, r/m8
            // The REX spelling names SPL/BPL/SIL/DIL for indices 4-7.
            expect_alu_matches_interpreter({0x40, base, modrm});
        }
    }
    // 84 /r: test r/m8, r8 -- no destination write.
    for (u8 reg = 0; reg < 8; ++reg)
        expect_alu_matches_interpreter(
            {0x84, static_cast<u8>(0xC0 | (reg << 3) | ((reg + 5) & 7))});
}

TEST_F(IrEmit64NarrowAlu, WordRegisterForms) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    for (const u8 base : {u8{0x00}, u8{0x08}, u8{0x20}, u8{0x28}, u8{0x30}, u8{0x38}}) {
        for (u8 reg = 0; reg < 8; ++reg) {
            const u8 modrm = static_cast<u8>(0xC0 | (reg << 3) | ((reg + 3) & 7));
            expect_alu_matches_interpreter({0x66, static_cast<u8>(base + 1), modrm});
            expect_alu_matches_interpreter({0x66, static_cast<u8>(base + 3), modrm});
            // REX.R/REX.B reach R8W-R15W.
            expect_alu_matches_interpreter({0x66, 0x45, static_cast<u8>(base + 1), modrm});
        }
    }
    for (u8 reg = 0; reg < 8; ++reg)
        expect_alu_matches_interpreter(
            {0x66, 0x85, static_cast<u8>(0xC0 | (reg << 3) | ((reg + 5) & 7))});
}

TEST_F(IrEmit64NarrowAlu, ImmediateFormsIncludingSignExtendedImm8) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    for (const u8 ext : {u8{0}, u8{1}, u8{4}, u8{5}, u8{6}, u8{7}}) {
        for (const u8 immediate : {u8{0x00}, u8{0x01}, u8{0x7F}, u8{0x80}, u8{0xFF}}) {
            // 80 /n ib: byte r/m8, imm8 -- register and memory destinations.
            expect_alu_matches_interpreter(
                {0x80, static_cast<u8>(0xC1 | (ext << 3)), immediate});
            expect_alu_matches_interpreter(
                {0x80, static_cast<u8>(0x00 | (ext << 3)), immediate});
            // 83 /n ib with 66: word r/m16, imm8 SIGN-EXTENDED to 16 bits.
            // A zero-extending lowering agrees for 0x00-0x7F and diverges above.
            expect_alu_matches_interpreter(
                {0x66, 0x83, static_cast<u8>(0xC1 | (ext << 3)), immediate});
            expect_alu_matches_interpreter(
                {0x66, 0x83, static_cast<u8>(0x00 | (ext << 3)), immediate});
        }
        for (const u16 immediate : {u16{0x0001}, u16{0x7FFF}, u16{0x8000}, u16{0xFFFF}}) {
            const u8 low = static_cast<u8>(immediate);
            const u8 high = static_cast<u8>(immediate >> 8);
            expect_alu_matches_interpreter(
                {0x66, 0x81, static_cast<u8>(0xC1 | (ext << 3)), low, high});
        }
    }
    // A8/A9 and F6 /0, F7 /0: TEST against an accumulator or an immediate.
    for (const u8 immediate : {u8{0x00}, u8{0x55}, u8{0xFF}}) {
        expect_alu_matches_interpreter({0xA8, immediate});
        expect_alu_matches_interpreter({0xF6, 0xC1, immediate});
        expect_alu_matches_interpreter({0xF6, 0x00, immediate});
    }
}

TEST_F(IrEmit64NarrowAlu, MemoryOperandsAtBothWidths) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    for (const u8 base : {u8{0x00}, u8{0x08}, u8{0x20}, u8{0x28}, u8{0x30}, u8{0x38}}) {
        expect_alu_matches_interpreter({base, 0x08});                  // [rax], cl
        expect_alu_matches_interpreter({static_cast<u8>(base + 2), 0x08});  // cl, [rax]
        expect_alu_matches_interpreter({base, 0x28});                  // [rax], ch
        expect_alu_matches_interpreter({0x66, static_cast<u8>(base + 1), 0x08});
        expect_alu_matches_interpreter({0x66, static_cast<u8>(base + 3), 0x08});
    }
    expect_alu_matches_interpreter({0x84, 0x08});        // test [rax], cl
    expect_alu_matches_interpreter({0x66, 0x85, 0x08});  // test [rax], cx
}

// INC/DEC leave CF untouched (Intel SDM Vol. 2A), which the backend gets by
// capturing CF before the merge helper runs. Both carry polarities are driven
// from the block's entry flags, and the operand values sit on the overflow
// boundaries where a wrong record width would show as OF/SF rather than as a
// wrong result.
TEST_F(IrEmit64NarrowAlu, IncDecPreserveCarryAtOverflowBoundaries) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    struct Form { std::vector<u8> bytes; const char* name; };
    const std::vector<Form> forms{
        {{0xFE, 0xC1}, "inc cl"},
        {{0xFE, 0xC9}, "dec cl"},
        {{0xFE, 0xC5}, "inc ch"},
        {{0x66, 0xFF, 0xC1}, "inc cx"},
        {{0x66, 0xFF, 0xC9}, "dec cx"},
        {{0xFE, 0x00}, "inc byte [rax]"},
        {{0xFE, 0x08}, "dec byte [rax]"},
        {{0x66, 0xFF, 0x00}, "inc word [rax]"},
        {{0xF6, 0xD1}, "not cl"},
        {{0x66, 0xF7, 0xD1}, "not cx"},
        {{0xF6, 0x10}, "not byte [rax]"},
    };
    for (const Form& form : forms) {
        SCOPED_TRACE(form.name);
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(form.bytes);
        ASSERT_NE(native, nullptr);
        for (const u64 carry : {0ull, 1ull}) {
            for (const u64 value : {0x00ull, 0x01ull, 0x7Full, 0x80ull, 0xFFull,
                                    0x7FFFull, 0x8000ull, 0xFFFFull}) {
                auto oracle = seed();
                oracle.gpr.set64(0, kData);
                oracle.gpr.rcx = value | 0xAAAA5555AAAA0000ull;
                oracle.flags.rflags = 0x202 | carry;   // CF is the only varying bit
                ASSERT_TRUE(memory.write_u64(kData, value).is_ok());
                auto state = oracle;
                {
                    Interpreter interpreter(oracle, memory);
                    ASSERT_EQ(interpreter.step(), ExecResult::Continue);
                }
                const auto oracle_memory = memory.read_u64(kData);
                ASSERT_TRUE(memory.write_u64(kData, value).is_ok());
                ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
                EXPECT_EQ(memory.read_u64(kData).value_or(1), oracle_memory.value_or(2))
                    << "memory diverged, value=" << std::hex << value;
                for (u8 index = 0; index < 16; ++index)
                    EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                        << "GPR " << unsigned(index) << " value=" << std::hex << value;
                state.materialize_lazy_flags();
                oracle.materialize_lazy_flags();
                EXPECT_EQ(state.flags.rflags, oracle.flags.rflags)
                    << "RFLAGS cf_in=" << carry << " value=" << std::hex << value;
            }
        }
    }
}

// The full-width unary forms through the same recipe. INC/DEC preserve CF
// (both incoming values), NEG sets CF = (source != 0), NOT defines nothing;
// a 32-bit destination clears bits 63:32 of the register. Values sit on the
// 32- and 64-bit overflow and sign boundaries, in registers (including an
// extended one) and in memory at both widths.
TEST_F(IrEmit64NarrowAlu, FullWidthIncDecNegNotAtOverflowBoundaries) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    struct Form { std::vector<u8> bytes; const char* name; };
    const std::vector<Form> forms{
        {{0xFF, 0xC1}, "inc ecx"},
        {{0x48, 0xFF, 0xC1}, "inc rcx"},
        {{0xFF, 0xC9}, "dec ecx"},
        {{0x48, 0xFF, 0xC9}, "dec rcx"},
        {{0xF7, 0xD9}, "neg ecx"},
        {{0x48, 0xF7, 0xD9}, "neg rcx"},
        {{0xF7, 0xD1}, "not ecx"},
        {{0x48, 0xF7, 0xD1}, "not rcx"},
        {{0x49, 0xFF, 0xC1}, "inc r9"},
        {{0x41, 0xFF, 0xC9}, "dec r9d"},
        {{0xFF, 0x00}, "inc dword [rax]"},
        {{0x48, 0xFF, 0x00}, "inc qword [rax]"},
        {{0xFF, 0x08}, "dec dword [rax]"},
        {{0x48, 0xFF, 0x08}, "dec qword [rax]"},
        {{0xF7, 0x18}, "neg dword [rax]"},
        {{0x48, 0xF7, 0x18}, "neg qword [rax]"},
        {{0xF7, 0x10}, "not dword [rax]"},
        {{0x48, 0xF7, 0x10}, "not qword [rax]"},
    };
    for (const Form& form : forms) {
        SCOPED_TRACE(form.name);
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(form.bytes);
        ASSERT_NE(native, nullptr);
        for (const u64 carry : {0ull, 1ull}) {
            for (const u64 value : {0x0ull, 0x1ull, 0x7FFFFFFFull, 0x80000000ull,
                                    0xFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull,
                                    0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull,
                                    0xAAAA55550000000Full}) {
                auto oracle = seed();
                oracle.gpr.set64(0, kData);
                oracle.gpr.rcx = value;
                oracle.gpr.set64(9, value);
                oracle.flags.rflags = 0x202 | carry;   // CF is the only varying bit
                ASSERT_TRUE(memory.write_u64(kData, value).is_ok());
                auto state = oracle;
                {
                    Interpreter interpreter(oracle, memory);
                    ASSERT_EQ(interpreter.step(), ExecResult::Continue);
                }
                const auto oracle_memory = memory.read_u64(kData);
                ASSERT_TRUE(memory.write_u64(kData, value).is_ok());
                ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
                EXPECT_EQ(memory.read_u64(kData).value_or(1), oracle_memory.value_or(2))
                    << "memory diverged, value=" << std::hex << value;
                for (u8 index = 0; index < 16; ++index)
                    EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                        << "GPR " << unsigned(index) << " value=" << std::hex << value;
                state.materialize_lazy_flags();
                oracle.materialize_lazy_flags();
                EXPECT_EQ(state.flags.rflags, oracle.flags.rflags)
                    << "RFLAGS cf_in=" << carry << " value=" << std::hex << value;
            }
        }
    }
    // The carry an INC/DEC inherits from an in-block producer.
    expect_alu_matches_interpreter({0x48, 0x01, 0xD1, 0x48, 0xFF, 0xC1}, 2);  // add rcx,rdx ; inc rcx
    expect_alu_matches_interpreter({0x29, 0xD1, 0xFF, 0xC9}, 2);              // sub ecx,edx ; dec ecx
    expect_alu_matches_interpreter({0x48, 0x29, 0xD1, 0x48, 0xFF, 0x08}, 2);  // sub rcx,rdx ; dec qword [rax]
}

// Narrow shifts and NEG. The matrix requires counts of 0, 1, width-1, width
// and width+1, because each is a different branch of the carry-out rule: at
// the width the last bit out is bit 0 (SHL) or the sign (SAR), and past it
// SHL carries nothing out while SAR keeps carrying the sign.
TEST_F(IrEmit64NarrowAlu, ShiftsAndNegAcrossTheCountBoundaries) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    struct Form { u8 ext; const char* name; };
    const std::vector<Form> shifts{{4, "shl"}, {5, "shr"}, {7, "sar"}};
    for (const Form& form : shifts) {
        for (const u8 count : {u8{0}, u8{1}, u8{7}, u8{8}, u8{9},
                               u8{15}, u8{16}, u8{17}, u8{31}}) {
            SCOPED_TRACE(testing::Message() << form.name << " count " << unsigned(count));
            // Every body is prefixed with `test rbx,rbx`. A masked count of
            // zero modifies NO flags, so it lowers to nothing at all -- which
            // leaves a one-instruction block empty, and more importantly means
            // the TEST's flags must still be the live ones afterwards.
            const auto with_flags = [&](std::vector<u8> tail) {
                std::vector<u8> body{0x48, 0x85, 0xDB};
                body.insert(body.end(), tail.begin(), tail.end());
                // Intel SDM Vol. 2 SAL/SAR/SHL/SHR: AF is UNDEFINED after a
                // shift, and OF is defined only for a count of 1. Those bits
                // are excluded rather than asserted -- a lazy record that
                // leaves an undefined flag alone cannot reproduce the value
                // an eager engine happens to leave, and chasing it would be
                // asserting an artifact. CF is the interesting one and stays
                // compared at every count, as do SF/ZF/PF, all sixteen GPRs
                // and the scratch window.
                const u64 undefined = 0x10u | (count == 1 ? 0u : 0x800u);
                expect_alu_matches_interpreter(body, 2, undefined);
            };
            with_flags({0xC0, static_cast<u8>(0xC1 | (form.ext << 3)), count});
            with_flags({0xC0, static_cast<u8>(0x00 | (form.ext << 3)), count});
            with_flags({0x66, 0xC1, static_cast<u8>(0xC1 | (form.ext << 3)), count});
            with_flags({0x66, 0xC1, static_cast<u8>(0x00 | (form.ext << 3)), count});
            // 32-bit, register and memory.
            with_flags({0xC1, static_cast<u8>(0xC1 | (form.ext << 3)), count});
            with_flags({0xC1, static_cast<u8>(0x00 | (form.ext << 3)), count});
        }
        // D0 /n and D1 /n: the by-1 encodings, which carry no immediate.
        // The by-1 encodings: OF IS defined at a count of one, so only AF is
        // excluded here.
        expect_alu_matches_interpreter({0xD0, static_cast<u8>(0xC1 | (form.ext << 3))}, 1, 0x10u);
        expect_alu_matches_interpreter({0x66, 0xD1, static_cast<u8>(0xC1 | (form.ext << 3))}, 1, 0x10u);
        expect_alu_matches_interpreter({0xD0, static_cast<u8>(0x00 | (form.ext << 3))}, 1, 0x10u);
    }
    // F6 /3 and F7 /3: NEG at both narrow widths, register and memory.
    expect_alu_matches_interpreter({0xF6, 0xD9});          // neg cl
    expect_alu_matches_interpreter({0xF6, 0xDD});          // neg ch
    expect_alu_matches_interpreter({0x66, 0xF7, 0xD9});    // neg cx
    expect_alu_matches_interpreter({0xF6, 0x18});          // neg byte [rax]
    expect_alu_matches_interpreter({0x66, 0xF7, 0x18});    // neg word [rax]
}

// Rotates move CF and OF and leave every other flag ALONE. Each body is
// prefixed with `test rbx,rbx` so the claim is concrete: the TEST's ZF/SF/PF
// must survive the rotate. That is the case a lazy record cannot express --
// only one survives, so a record published here would discard the TEST's and
// ZSP would come from stale RFLAGS. This is the whole reason `SetCf`/`SetOf`
// exist.
TEST_F(IrEmit64NarrowAlu, RotatesForceCarryAndPreserveTheOtherFlags) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    for (const u8 ext : {u8{0}, u8{1}}) {   // ROL, ROR
        for (const u8 count : {u8{0}, u8{1}, u8{2}, u8{7}, u8{8}, u8{9},
                               u8{15}, u8{16}, u8{17}, u8{31}}) {
            SCOPED_TRACE(testing::Message()
                         << (ext == 0 ? "rol" : "ror") << " count " << unsigned(count));
            // OF is UNDEFINED for a rotate count other than 1, so it is
            // excluded there and asserted everywhere else. CF is asserted at
            // every count, and so are ZF/SF/PF/AF.
            const u64 undefined = count == 1 ? 0u : 0x800u;
            const auto with_flags = [&](std::vector<u8> tail) {
                std::vector<u8> body{0x48, 0x85, 0xDB};
                body.insert(body.end(), tail.begin(), tail.end());
                expect_alu_matches_interpreter(body, 2, undefined);
            };
            with_flags({0xC0, static_cast<u8>(0xC1 | (ext << 3)), count});
            with_flags({0xC0, static_cast<u8>(0x00 | (ext << 3)), count});
            with_flags({0x66, 0xC1, static_cast<u8>(0xC1 | (ext << 3)), count});
            with_flags({0x66, 0xC1, static_cast<u8>(0x00 | (ext << 3)), count});
            with_flags({0xC0, static_cast<u8>(0xC5 | (ext << 3)), count});  // ch
            with_flags({0xC1, static_cast<u8>(0xC1 | (ext << 3)), count});  // 32-bit
            with_flags({0xC1, static_cast<u8>(0x00 | (ext << 3)), count});
        }
        // D0 /n and D1 /n: the by-1 encodings, where OF IS defined.
        expect_alu_matches_interpreter({0xD0, static_cast<u8>(0xC1 | (ext << 3))});
        expect_alu_matches_interpreter({0x66, 0xD1, static_cast<u8>(0xC1 | (ext << 3))});
        expect_alu_matches_interpreter({0xD0, static_cast<u8>(0x00 | (ext << 3))});
    }
}

// RCL/RCR: rotate THROUGH the carry. These both READ and WRITE CF, so they
// exercise the flag consumer and the flag overlay in one instruction. The CF
// input is varied by the preceding instruction rather than by the seed, since
// the fixture seeds RFLAGS identically every time -- a sweep that only ever
// entered with CF=1 would pass with the carry input dropped entirely.
//
// The two directions compute OF from DIFFERENT bits (SDM Vol. 2: RCL after the
// rotate, RCR before it), which is invisible whenever the operand's top and
// bottom bits agree -- hence the full seed set and both rotate directions.
TEST_F(IrEmit64NarrowAlu, RotateThroughCarryReadsAndWritesTheCarry) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    // Carry producers: a full-flag writer whose CF depends on the seed, an
    // INC (which PRESERVES CF, so the value comes from further back), and a
    // rotate (whose CF arrives through the overlay, not a record).
    const std::vector<std::vector<u8>> producers = {
        {0x00, 0xD2},        // add dl, dl   -- full writer, seed-dependent CF
        {0x28, 0xF1},        // sub cl, dh?  -- second full writer shape
        {0xFE, 0xC2},        // inc dl       -- preserves CF
        {0xD0, 0xC2},        // rol dl, 1    -- CF through the overlay
    };
    for (const u8 ext : {u8{2}, u8{3}}) {   // RCL, RCR
        for (const u8 count : {u8{0}, u8{1}, u8{2}, u8{7}, u8{8}, u8{9},
                               u8{15}, u8{16}, u8{17}, u8{31}}) {
            // OF is defined only for a masked count of 1.
            const u64 undefined = count == 1 ? 0u : 0x800u;
            for (const auto& producer : producers) {
                SCOPED_TRACE(testing::Message()
                             << (ext == 2 ? "rcl" : "rcr")
                             << " count " << unsigned(count)
                             << " after " << producer.size() << "-byte producer");
                const auto with_carry = [&](std::vector<u8> tail) {
                    std::vector<u8> body = producer;
                    body.insert(body.end(), tail.begin(), tail.end());
                    expect_alu_matches_interpreter(body, 2, undefined);
                };
                with_carry({0xC0, static_cast<u8>(0xC1 | (ext << 3)), count});
                with_carry({0xC0, static_cast<u8>(0xC5 | (ext << 3)), count});  // ch
                with_carry({0xC0, static_cast<u8>(0x00 | (ext << 3)), count});  // m8
                with_carry({0x66, 0xC1, static_cast<u8>(0xC1 | (ext << 3)), count});
                with_carry({0x66, 0xC1, static_cast<u8>(0x00 | (ext << 3)), count});
                with_carry({0xC1, static_cast<u8>(0xC1 | (ext << 3)), count});  // 32
                with_carry({0xC1, static_cast<u8>(0x00 | (ext << 3)), count});
            }
        }
        // D0 /n and D1 /n: the by-1 encodings, where OF IS defined. Driven
        // from each producer so the carry input is both 0 and 1 for them.
        for (const auto& producer : producers) {
            const auto by_one = [&](std::vector<u8> tail) {
                std::vector<u8> body = producer;
                body.insert(body.end(), tail.begin(), tail.end());
                expect_alu_matches_interpreter(body, 2);
            };
            by_one({0xD0, static_cast<u8>(0xC1 | (ext << 3))});
            by_one({0xD0, static_cast<u8>(0x00 | (ext << 3))});
            by_one({0x66, 0xD1, static_cast<u8>(0xC1 | (ext << 3))});
            by_one({0x66, 0xD1, static_cast<u8>(0x00 | (ext << 3))});
        }
    }
    // A 32-bit rotate-through-carry rotates a 33-BIT span, so its shift
    // amounts reach 32 -- which the W-form ARM64 shift encodings cannot
    // express at all. Emitting one is not a wider-than-needed encoding but an
    // ILLEGAL one, and the block dies with SIGILL rather than diverging.
    expect_alu_matches_interpreter({0x00, 0xD2, 0xC1, 0xD1, 0x01}, 2);
    expect_alu_matches_interpreter({0x00, 0xD2, 0xC1, 0xD9, 0x01}, 2);
    // The destination must still be written when the rotate is a full cycle
    // or a masked-to-zero count.
    expect_alu_matches_interpreter({0x00, 0xD2, 0xC0, 0xD1, 0x09}, 2);
    expect_alu_matches_interpreter({0x00, 0xD2, 0xC1, 0xD1, 0x00}, 2);
}
} // namespace
#endif
